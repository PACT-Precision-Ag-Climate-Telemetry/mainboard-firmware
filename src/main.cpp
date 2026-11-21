#include <Arduino.h>
#include <Wire.h>
#include <sys/time.h>
#include <esp_system.h>  // esp_reset_reason()
#include <esp_task_wdt.h>
#include <LittleFS.h>

#include "config.h"  // all pins / addresses / bauds / intervals live here
#include "device_id.h"
#include "backlog.h"
#include "field_config.h"
#include "i2c_tools.h"
#include "settings.h"
#include "telemetry.h"
#include "devices/ads1115.h"
#include "devices/battery.h"
#include "devices/gnss.h"
#include "devices/methane_mhz9041a.h"
#include "devices/modem.h"
#include "devices/moisture.h"
#include "devices/scd40.h"
#include "devices/sht30.h"
#include "devices/vl53l1x_tof.h"

// Nothing in this file (or the handlers) blocks: setup() only configures,
// loop() advances every handler's state machine. The only waits left are
// inside two vendor libraries' own begin() calls at boot (VL53L1X, MH-Z9041A).

// One handler object per device (see src/devices/).
static VL53L1XTof tof;
static SHT30 sht;
static ADS1115 adc;
static SCD40 co2;
static MethaneMHZ9041A methane;
static Battery battery(adc, cfg::ADC_BATTERY_CHANNEL, cfg::BATT_SENSE_EN_PIN, cfg::SOLAR_DIS_PIN);
static Moisture moisture(adc, cfg::ADC_MOISTURE_CHANNEL);

// UART0 is the USB-serial/programming port, so UART1 and UART2 are free.
static GNSSReceiver gnss(Serial1, cfg::GNSS_RXD_PIN, cfg::GNSS_TXD_PIN);
static Modem modem(Serial2, cfg::MODEM_PWR_PIN, cfg::MODEM_EN_PIN,
                   cfg::MODEM_RXD_PIN, cfg::MODEM_TXD_PIN, cfg::MODEM_BAUD);

static I2CDevice *devices[] = {&tof, &sht, &adc, &co2, &methane};
static const size_t DEVICE_COUNT = sizeof(devices) / sizeof(devices[0]);
static bool gDeviceInit[DEVICE_COUNT];
static bool gSettingsOk = false;

// ---- field configurator ------------------------------------------------------
static FieldConfig console(Serial);
static bool gDevMode = cfg::DEV_MODE_DEFAULT;  // "set DEV_MODE 1": faster sample / upload timing
static bool gLog = true;  // "set LOG 0" silences periodic output while configuring

// Battery saver: 0 = normal, 1 = low, 2 = critical (see config.h). Both periods
// are stretched at the lower levels. Updated from the latest battery reading
// with hysteresis; an unknown battery keeps the current level.
static uint8_t gBattLevel = 0;
static uint8_t battSlowdown() {
  return gBattLevel == 2 ? cfg::BATT_CRITICAL_SLOWDOWN : gBattLevel == 1 ? cfg::BATT_LOW_SLOWDOWN : 1;
}
static uint32_t samplePeriodMs() {
  return (gDevMode ? cfg::DEV_SAMPLE_PERIOD_MS : cfg::TELEMETRY_SAMPLE_PERIOD_MS) * battSlowdown();
}
static uint32_t sessionPeriodMs() {
  return (gDevMode ? cfg::DEV_SESSION_PERIOD_MS : cfg::MODEM_SESSION_PERIOD_MS) * battSlowdown();
}

// ---- telemetry batch ---------------------------------------------------------
static Telemetry::Reading gReadings[cfg::TELEMETRY_MAX_READINGS];
static uint8_t gReadingCount = 0;
static uint32_t gLastSample = 0;
static uint32_t gLastSession = 0;
static bool gSendPending = false;  // a modem session / upload is in progress
static uint8_t gSentCount = 0;     // readings in the frame being uploaded (oldest first)
static bool gSendFromSpill = false;     // that frame came from the flash spill, not the RAM buffer
static uint8_t gInflightSpilled = 0;    // readings of the RAM frame that were pushed to flash mid-upload
static uint8_t gFailCount = 0;          // failed sessions/uploads in a row (drives the retry backoff)
static Telemetry::Reading gChunk[cfg::TELEMETRY_MAX_READINGS];  // oldest readings loaded from the spill
static uint8_t gFrame[24 + (2 + 25) * cfg::TELEMETRY_MAX_READINGS + 8 + 4];  // max frame
static_assert(sizeof(gFrame) <= Modem::MAX_SEND_BYTES, "Modem send buffer too small for max frame");

static void pushReading(const Telemetry::Reading &r) {
  if (gReadingCount == cfg::TELEMETRY_MAX_READINGS) {  // full: move the oldest to flash
    Backlog::spillAppend(gReadings[0]);
    memmove(&gReadings[0], &gReadings[1], sizeof(Telemetry::Reading) * (gReadingCount - 1));
    gReadingCount--;
    if (gSendPending && !gSendFromSpill && gSentCount > 0) {
      gSentCount--;  // the oldest one was in the frame being sent: it is in flash now,
      gInflightSpilled++;  // so an ack must remove it from there instead
    }
  }
  gReadings[gReadingCount++] = r;
}

// Unsent readings anywhere: RAM buffer or flash spill.
static bool backlogPending() { return gReadingCount > 0 || Backlog::spillCount() > 0; }

// Wait before the next upload attempt: the normal period, or after failures an
// exponential retry (1, 2, 4, 8 ... min) capped at the normal period.
static uint32_t nextSessionDelayMs() {
  const uint32_t period = sessionPeriodMs();
  if (gFailCount == 0) return period;
  const uint32_t retry = cfg::MODEM_RETRY_BASE_MS << (gFailCount > 10 ? 9 : gFailCount - 1);
  return retry < period ? retry : period;
}

static void noteUploadFailed() {
  if (gFailCount < 255) gFailCount++;
  if (gLog) Serial.printf("[Telemetry] retry in %lu s\n", (unsigned long)(nextSessionDelayMs() / 1000));
}

static bool systemClockSet() { return time(nullptr) > 1700000000; }

// Current UTC: GNSS if it has time (and keep the system clock in step),
// else the system clock, else 0 (not set yet).
static uint64_t nowEpoch() {
  uint64_t t;
  if (gnss.utcEpoch(t)) {
    struct timeval tv = {(time_t)t, 0};
    settimeofday(&tv, nullptr);
    return t;
  }
  return systemClockSet() ? (uint64_t)time(nullptr) : 0;
}

static Telemetry::Location currentLocation() {
  Telemetry::Location loc;
  const Settings::Values &s = Settings::get();
  if (gnss.hasFix()) {
    loc.hasFix = true;
    loc.latitude = gnss.lat();
    loc.longitude = gnss.lon();
  } else if (s.gpsLatitude != 0.0f || s.gpsLongitude != 0.0f) {  // manual fallback
    loc.hasFix = true;
    loc.latitude = s.gpsLatitude;
    loc.longitude = s.gpsLongitude;
  }
  if (modem.hasCell) {  // MCC/MNC learned in the last modem session
    loc.hasCell = true;
    loc.mcc = modem.mcc;
    loc.mnc = modem.mnc;
  }
  if (modem.csq >= 0 && modem.csq <= 31) loc.signalDbm = (int8_t)(-113 + 2 * modem.csq);
  loc.resetReason = (uint8_t)esp_reset_reason();
  return loc;
}

// ---- dev-mode status LED (the single NeoPixel on the dev board) ----------------
// Only lit in dev mode. Priority: result flash > modem on > reading flash > idle tick.
//   white flash 0.3 s   a reading was taken
//   amber steady        modem powered (session / upload running)
//   green 2 s           upload acked by the server
//   red 4 s             modem session or upload failed
//   dim blue tick       idle, alive (50 ms every 2 s)
static uint32_t gLedFlashUntil = 0;
static uint8_t gLedFlashR = 0, gLedFlashG = 0, gLedFlashB = 0;
static uint32_t gLedLast = 0xFFFFFFFF;  // last colour written (0xFFFFFFFF = unknown)

static void ledFlash(uint8_t r, uint8_t g, uint8_t b, uint32_t ms) {
  if (!gDevMode) return;
  gLedFlashR = r; gLedFlashG = g; gLedFlashB = b;
  gLedFlashUntil = millis() + ms;
}

static void ledShow(uint8_t r, uint8_t g, uint8_t b) {
  uint32_t c = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  if (c == gLedLast) return;  // write only on change
  gLedLast = c;
  rgbLedWrite(cfg::DEV_LED_PIN, r, g, b);
}

// Re-evaluate the battery level (call from loop()).
static void updateBatteryLevel() {
  if (!battery.fresh(cfg::PERIOD_BATTERY_MS * 3)) return;
  const float p = battery.percent;
  uint8_t level = gBattLevel;
  if (p < cfg::BATT_CRITICAL_PERCENT) level = 2;
  else if (p < cfg::BATT_LOW_PERCENT && gBattLevel != 2) level = 1;
  else if (gBattLevel == 2 && p >= cfg::BATT_CRITICAL_PERCENT + cfg::BATT_RECOVER_MARGIN)
    level = (p < cfg::BATT_LOW_PERCENT + cfg::BATT_RECOVER_MARGIN) ? 1 : 0;
  else if (gBattLevel == 1 && p >= cfg::BATT_LOW_PERCENT + cfg::BATT_RECOVER_MARGIN) level = 0;
  if (level != gBattLevel) {
    gBattLevel = level;
    if (gLog) {
      static const char *names[] = {"normal", "LOW", "CRITICAL"};
      Serial.printf("[Battery] %.0f%% -> %s: reading every %lu s, upload every %lu s\n", p,
                    names[level], (unsigned long)(samplePeriodMs() / 1000),
                    (unsigned long)(sessionPeriodMs() / 1000));
    }
  }
}

static void updateLed(uint32_t now) {
  const uint8_t L = cfg::DEV_LED_LEVEL;
  if (!gDevMode) {
    ledShow(0, 0, 0);
    gLedFlashUntil = 0;
    return;
  }
  const bool flashing = (int32_t)(gLedFlashUntil - now) > 0;
  const bool resultFlash = flashing && (gLedFlashR != gLedFlashG || gLedFlashG != gLedFlashB);  // coloured, not white
  if (resultFlash) ledShow(gLedFlashR, gLedFlashG, gLedFlashB);
  else if (modem.state() != Modem::State::OFF) ledShow(L, L * 2 / 3, 0);  // amber
  else if (flashing) ledShow(gLedFlashR, gLedFlashG, gLedFlashB);        // white: reading taken
  else if (now % 2000 < 50) ledShow(0, 0, L / 3);                         // dim blue tick
  else ledShow(0, 0, 0);
}

// Builds one Reading from the latest sensor values. A sensor with no recent
// valid reading sets its bit in `status` and sends 0.
static Telemetry::Reading collectReading() {
  using namespace Telemetry;
  Reading r;
  r.timestamp = nowEpoch();
  r.offsetMm = currentOffsetMm();  // OFFSET from the field configurator
  r.espTemperatureC = temperatureRead();  // ESP die temperature

  if (sht.fresh(cfg::SENSOR_STALE_MS)) {
    r.temperatureC = sht.temperatureC;
    r.humidityRH = sht.humidityRH;
  } else {
    r.status |= Status::TEMPERATURE_READ_FAILED;
  }

  if (co2.fresh(cfg::SENSOR_STALE_MS)) r.co2Ppm = co2.co2ppm;
  else r.status |= Status::CO2_READ_FAILED;

  if (methane.fresh(cfg::SENSOR_STALE_MS) && !isnan(methane.ch4PercentLEL)) {
    float ppm = methane.ch4PercentLEL * cfg::METHANE_PPM_PER_LEL_PERCENT;
    r.methanePpm = (uint16_t)constrain(lroundf(ppm), 0L, 65535L);
    r.methaneRaw = (uint16_t)constrain(lroundf(methane.ch4PercentLEL * 100.0f), 0L, 65535L);  // %LEL x 100
  } else {
    r.status |= Status::METHANE_READ_FAILED;
  }

  if (tof.fresh(cfg::SENSOR_STALE_MS) && tof.rangeValid) r.depthMm = tof.distanceMm;
  else r.status |= Status::ULTRASONIC_READ_FAILED;

  if (moisture.fresh(cfg::SENSOR_STALE_MS)) {
    r.moisturePct = moisture.percent;  // already calibrated + clamped to 0-100
    r.moistureRaw = moisture.raw;
  } else {
    r.status |= Status::MOISTURE_READ_FAILED;
  }

  // Battery comes from its own handler (it switches BATT_SENSE_EN / SOLAR_DIS).
  if (battery.fresh(cfg::PERIOD_BATTERY_MS * 3)) {
    r.batteryMv = battery.millivolts;
    r.batteryPct = battery.percent;
  } else {
    r.status |= Status::BATTERY_READ_FAILED;
  }
  return r;
}

// ---- sensor measurement window -------------------------------------------------
// Sensors are not polled continuously. A window opens when a telemetry sample is
// due (or on demand from the configurator), every sensor is kicked once, and the
// window closes when all of them delivered a new value or after
// SENSOR_WINDOW_TIMEOUT_MS. A window opened for a sample then stores the reading.
static bool gMeasuring = false;
static bool gSampleWanted = false;   // store a reading when the window closes
static uint32_t gMeasureStart = 0;
// Per-sensor progress inside one window. A sensor is "settled" once it delivered a
// value or used up SENSOR_MAX_ATTEMPTS tries; an attempt that gets no result within
// the sensor's attempt timeout is restarted.
struct Progress {
  bool got = false;
  uint8_t attempts = 0;
  uint32_t attemptStart = 0;
};
static Progress gProg[DEVICE_COUNT];
static Progress gBattProg, gMoistProg;
static const uint32_t kBattAttemptMs = cfg::BATTERY_SETTLE_MS + cfg::BATTERY_ADC_TIMEOUT_MS + 1000;

static void startMeasure(bool forSample) {
  if (forSample) gSampleWanted = true;
  if (gMeasuring) return;
  gMeasuring = true;
  gMeasureStart = millis();
  const uint32_t now = gMeasureStart;
  for (size_t i = 0; i < DEVICE_COUNT; i++) {
    gProg[i] = Progress{false, 1, now};
    devices[i]->windowStart();
    devices[i]->kick();
  }
  gBattProg = gMoistProg = Progress{false, 1, now};
  battery.request();
  moisture.request();
}

// Poll the sensors while a window is open and retry the ones that do not answer.
// The battery and moisture handlers (and any device that is still busy, such as the
// SCD40 being stopped) keep being polled after the window closed: the battery
// reading switches BATT_SENSE_EN / SOLAR_DIS and must always finish and restore
// them, which needs the ADS1115 polled too.
static void pollSensors(uint32_t now) {
  const bool adcNeeded = battery.busy() || moisture.busy();
  for (size_t i = 0; i < DEVICE_COUNT; i++) {
    I2CDevice *d = devices[i];
    if (!gMeasuring && !d->busy() && !(d == &adc && adcNeeded)) continue;
    if (d->poll()) {
      gProg[i].got = true;
      if (gLog) d->print(Serial);
    }
    // No result within this sensor's attempt timeout: try again.
    if (gMeasuring && d->ok() && !gProg[i].got && gProg[i].attempts < cfg::SENSOR_MAX_ATTEMPTS &&
        now - gProg[i].attemptStart >= d->attemptTimeoutMs()) {
      gProg[i].attempts++;
      gProg[i].attemptStart = now;
      d->kick();
      if (gLog) Serial.printf("[%s] no reading, retry %u/%u\n", d->name(), gProg[i].attempts,
                              (unsigned)cfg::SENSOR_MAX_ATTEMPTS);
    }
  }

  if (gMeasuring || battery.busy()) {
    if (battery.poll()) {
      gBattProg.got = true;
      if (gLog) battery.print(Serial);
    }
    if (gMeasuring && adc.ok() && !gBattProg.got && !battery.busy() &&
        gBattProg.attempts < cfg::SENSOR_MAX_ATTEMPTS && now - gBattProg.attemptStart >= kBattAttemptMs) {
      gBattProg.attempts++;
      gBattProg.attemptStart = now;
      battery.request();
      if (gLog) Serial.printf("[Battery] no reading, retry %u/%u\n", gBattProg.attempts,
                              (unsigned)cfg::SENSOR_MAX_ATTEMPTS);
    }
  }
  if (gMeasuring || moisture.busy()) {
    if (moisture.poll()) {
      gMoistProg.got = true;
      if (gLog) moisture.print(Serial);
    }
    if (gMeasuring && adc.ok() && !gMoistProg.got && !moisture.busy() &&
        gMoistProg.attempts < cfg::SENSOR_MAX_ATTEMPTS &&
        now - gMoistProg.attemptStart >= cfg::SENSOR_ATTEMPT_TIMEOUT_MS) {
      gMoistProg.attempts++;
      gMoistProg.attemptStart = now;
      moisture.request();
      if (gLog) Serial.printf("[Moisture] no reading, retry %u/%u\n", gMoistProg.attempts,
                              (unsigned)cfg::SENSOR_MAX_ATTEMPTS);
    }
  }
}

// A sensor is settled once it has a value, or its last attempt has timed out.
static bool settled(const Progress &p, uint32_t attemptMs, uint32_t now) {
  return p.got || (p.attempts >= cfg::SENSOR_MAX_ATTEMPTS && now - p.attemptStart >= attemptMs);
}

// True when every working sensor is settled, or the hard timeout passed.
static bool measureDone(uint32_t now) {
  if (now - gMeasureStart >= cfg::SENSOR_WINDOW_TIMEOUT_MS) return true;
  for (size_t i = 0; i < DEVICE_COUNT; i++)
    if (devices[i]->ok() && !settled(gProg[i], devices[i]->attemptTimeoutMs(), now)) return false;
  if (adc.ok() && (!settled(gBattProg, kBattAttemptMs, now) ||
                   !settled(gMoistProg, cfg::SENSOR_ATTEMPT_TIMEOUT_MS, now)))
    return false;
  return true;
}

static String hexString(const uint8_t *b, size_t n) {
  static const char *H = "0123456789abcdef";
  String s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    s += H[b[i] >> 4];
    s += H[b[i] & 0xF];
  }
  return s;
}

// Frame for the readings collected so far, as hex ("get PAYLOAD_HEX").
static String payloadHex() {
  size_t n = Telemetry::build(gReadings, gReadingCount, currentLocation(), gFrame, sizeof(gFrame));
  return n ? hexString(gFrame, n) : String("(no readings yet)");
}

// ---- read-only values exposed to the field configurator -----------------------
static String ageText(uint32_t ms) { return " (age " + String(ms / 1000) + " s)"; }

static const Binding kLiveBindings[] = {
    {"ID", "Device ID (model + serial from the MAC)", +[]() { return getDeviceIdString(); }, nullptr},
    {"FW_VERSION", "Firmware version", +[]() { return String(cfg::FW_VERSION); }, nullptr},
    {"ESP_CHIP", "Chip model, revision, cores",
     +[]() -> String {
       return String(ESP.getChipModel()) + " rev " + String(ESP.getChipRevision()) + ", " +
              String(ESP.getChipCores()) + " cores";
     }, nullptr},
    {"ESP_CPU_MHZ", "CPU frequency, MHz", +[]() -> String { return String(ESP.getCpuFreqMHz()); }, nullptr},
    {"ESP_MAC", "Base MAC address",
     +[]() -> String {
       char b[18];
       uint64_t m = ESP.getEfuseMac();  // low 6 bytes, first octet in the lowest byte
       snprintf(b, sizeof(b), "%02X:%02X:%02X:%02X:%02X:%02X", (uint8_t)m, (uint8_t)(m >> 8),
                (uint8_t)(m >> 16), (uint8_t)(m >> 24), (uint8_t)(m >> 32), (uint8_t)(m >> 40));
       return String(b);
     }, nullptr},
    {"ESP_SDK", "ESP-IDF / Arduino core version",
     +[]() -> String { return String(ESP.getSdkVersion()) + " / " + String(ESP_ARDUINO_VERSION_MAJOR) + "." +
                    String(ESP_ARDUINO_VERSION_MINOR) + "." + String(ESP_ARDUINO_VERSION_PATCH); },
     nullptr},
    {"ESP_UPTIME", "Time since boot, seconds", +[]() -> String { return String(millis() / 1000); }, nullptr},
    {"ESP_RESET_REASON", "Why the chip last reset (power-on, software, panic, watchdog, brownout...)",
     +[]() -> String {
       static const char *names[] = {"unknown", "power-on", "external pin", "software", "panic",
                                     "interrupt watchdog", "task watchdog", "other watchdog",
                                     "deep sleep wake", "brownout", "SDIO", "USB", "JTAG", "eFuse",
                                     "power glitch", "CPU lockup"};
       uint8_t r = (uint8_t)esp_reset_reason();
       return String(r < sizeof(names) / sizeof(names[0]) ? names[r] : "?") + " (" + String(r) + ")";
     }, nullptr},
    {"ESP_TEMPERATURE", "ESP die temperature, C", +[]() -> String { return String(temperatureRead(), 1); }, nullptr},
    {"ESP_HEAP", "Heap: free / minimum ever free / total, bytes",
     +[]() -> String {
       return String(ESP.getFreeHeap()) + " / " + String(ESP.getMinFreeHeap()) + " / " +
              String(ESP.getHeapSize());
     }, nullptr},
    {"ESP_PSRAM", "PSRAM: free / total, bytes (0 / 0 = none found)",
     +[]() -> String { return String(ESP.getFreePsram()) + " / " + String(ESP.getPsramSize()); }, nullptr},
    {"ESP_FLASH", "Flash chip size, bytes and speed MHz",
     +[]() -> String { return String(ESP.getFlashChipSize()) + " B @ " + String(ESP.getFlashChipSpeed() / 1000000) + " MHz"; },
     nullptr},
    {"ESP_FS", "Flash filesystem (LittleFS): used / total bytes",
     +[]() -> String { return String(LittleFS.usedBytes()) + " / " + String(LittleFS.totalBytes()); }, nullptr},
    {"BACKLOG", "Unsent readings: in RAM buffer / in flash",
     +[]() -> String { return String(gReadingCount) + " / " + String(Backlog::spillCount()); }, nullptr},
    {"LOG", "1 = periodic debug output on, 0 = off (not saved; resets at boot)",
     +[]() { return String(gLog ? 1 : 0); },
     +[](const String &v) {
       if (v != "0" && v != "1") return false;
       gLog = (v == "1");
       return true;
     }},
    {"DEV_MODE", "1 = dev timing (reading every 1 min, upload every 5 min), 0 = normal (5 min / 30 min). Not saved; resets at boot",
     +[]() { return String(gDevMode ? 1 : 0); },
     +[](const String &v) {
       if (v != "0" && v != "1") return false;
       gDevMode = (v == "1");
       return true;
     }},
    {"SHT30_TEMPERATURE", "Air temperature, C", +[]() { return String(sht.temperatureC, 2); }, nullptr},
    {"SHT30_HUMIDITY", "Relative humidity, percent", +[]() { return String(sht.humidityRH, 1); }, nullptr},
    {"CO2_PPM", "SCD40 CO2, ppm", +[]() { return String(co2.co2ppm); }, nullptr},
    {"METHANE_LEL", "Methane, percent of lower explosive limit", +[]() { return String(methane.ch4PercentLEL, 2); }, nullptr},
    {"DISTANCE_MM", "VL53L1X distance, mm (sent as depth)", +[]() { return String(tof.distanceMm); }, nullptr},
    {"MEASURE", "set 1 = read all sensors now (sensors are otherwise read only once per sample period). Reads back 1 while measuring",
     +[]() { return String(gMeasuring ? 1 : 0); },
     +[](const String &v) {
       if (v != "1") return false;
       startMeasure(false);
       return true;
     }},
    {"MOISTURE_PERCENT", "Soil moisture, percent 0-100. Returns the last reading and starts a new one",
     +[]() {
       startMeasure(false);
       return moisture.hasReading() ? String(moisture.percent, 1) + ageText(moisture.ageMs())
                                    : String("no reading yet");
     }, nullptr},
    {"MOISTURE_VOLTS", "Soil moisture sensor voltage at the ADC pin (last reading)",
     +[]() { return String(moisture.volts, 4); }, nullptr},
    {"MOISTURE_RAW", "Soil moisture raw ADC count (last reading)",
     +[]() { return String(moisture.raw); }, nullptr},
    {"BATTERY_MV", "Battery voltage, mV. Returns the last reading and starts a new one (about 5 s)",
     +[]() {
       startMeasure(false);
       return battery.hasReading() ? String(battery.millivolts) + ageText(battery.ageMs())
                                   : String("no reading yet (measuring, takes about 5 s)");
     }, nullptr},
    {"BATTERY_PERCENT", "Battery state of charge, percent (from the last reading)",
     +[]() { return String(battery.percent, 0); }, nullptr},
    {"ADC_0", "ADS1115 AIN0 volts", +[]() { return String(adc.volts[0], 4); }, nullptr},
    {"ADC_1", "ADS1115 AIN1 volts", +[]() { return String(adc.volts[1], 4); }, nullptr},
    {"ADC_2", "ADS1115 AIN2 volts", +[]() { return String(adc.volts[2], 4); }, nullptr},
    {"ADC_3", "ADS1115 AIN3 volts", +[]() { return String(adc.volts[3], 4); }, nullptr},
    {"GPS_FIX", "GNSS fix: 'lat,lon' or 'none'",
     +[]() { return gnss.hasFix() ? String(gnss.lat(), 6) + "," + String(gnss.lon(), 6) : String("none"); }, nullptr},
    {"MODEM_STATE", "Modem state: off, booting, ready, working, shutting-down...",
     +[]() { return String(modem.stateName()); }, nullptr},
    {"MODEM_WAKE", "set 1 = wake the modem now and run a status/cell/time session. Reads back the modem state",
     +[]() { return String(modem.stateName()); },
     +[](const String &v) {
       if (v != "1") return false;
       modem.request();
       return true;
     }},
    {"MODEM_HOLD", "1 = wake the modem and keep it powered until set to 0 (then it powers down). Not saved; resets at boot",
     +[]() -> String { return String(modem.held() ? 1 : 0) + " (" + modem.stateName() + ")"; },
     +[](const String &v) {
       if (v != "0" && v != "1") return false;
       modem.hold(v == "1");
       return true;
     }},
    {"MODEM_CELL", "Cell MCC,MNC from the last session, or 'unknown'",
     +[]() { return modem.hasCell ? String(modem.mcc) + "," + String(modem.mnc) : String("unknown"); }, nullptr},
    {"PAYLOAD_HEX", "Telemetry frame built from the readings so far, hex", +[]() { return payloadHex(); }, nullptr},
};

// ---- boot report (deferred so a serial monitor opened after reset sees it) ------
static bool gBootReported = false;

static void printBootReport() {
  Serial.printf("Settings %s\n", gSettingsOk ? "loaded" : "FAILED (using defaults, not saved)");
  Serial.printf("Mainboard  I2C SDA=GPIO%d SCL=GPIO%d %lu Hz\n", cfg::I2C_SDA_PIN,
                cfg::I2C_SCL_PIN, (unsigned long)cfg::I2C_FREQ_HZ);

  char wireId[11] = {0};
  buildDeviceId(wireId);
  Serial.printf("Device ID: %s   (wire: %s)   FW %s\n", getDeviceIdString().c_str(), wireId,
                cfg::FW_VERSION);

  i2cScan();

  Serial.println("\nDevices:");
  for (size_t i = 0; i < DEVICE_COUNT; i++)
    Serial.printf("  %-10s 0x%02X  %s\n", devices[i]->name(), devices[i]->address(),
                  gDeviceInit[i] ? "OK" : "FAILED");
  Serial.println("  Battery    switches BATT_SENSE_EN / SOLAR_DIS only while measuring");
  Serial.println("  GNSS       detecting baud rate in the background");
  Serial.println("  Modem      off; wakes only to send data or pull status");

  Serial.println("\nField configurator ready: type 'help'.");
  console.begin();
}

void setup() {
  Serial.begin(cfg::SERIAL_BAUD);
  gSettingsOk = Settings::begin();
  Wire.begin(cfg::I2C_SDA_PIN, cfg::I2C_SCL_PIN, cfg::I2C_FREQ_HZ);

  for (size_t i = 0; i < DEVICE_COUNT; i++) gDeviceInit[i] = devices[i]->begin();
  battery.begin();  // BATT_SENSE_EN / SOLAR_DIS as outputs, LOW
  gnss.begin();     // starts the baud search, returns immediately
  modem.begin();    // modem power off until a session is requested
  modem.setRawSink(&Serial);

  // Readings that were not uploaded before the last reset come back from flash.
  if (Backlog::begin()) gReadingCount = Backlog::load(gReadings, cfg::TELEMETRY_MAX_READINGS);

  // Field configurator: writable settings first, then read-only live values.
  size_t n = 0;
  const Binding *settingBindings = Settings::bindings(n);
  console.addBindings(settingBindings, n);
  console.addBindings(kLiveBindings, sizeof(kLiveBindings) / sizeof(kLiveBindings[0]));
  console.setAtHandler(+[](const String &cmd) {
    return modem.requestRaw(cmd)
               ? String("OK queued. The modem wakes if needed; the reply is printed when it arrives")
               : String("ERR another AT command is still pending");
  });

  // Hardware watchdog, armed last: the begin() calls above can legitimately take a while.
  // If loop() stops running for WDT_TIMEOUT_S the chip resets; the cause then shows up
  // as the reset-reason byte in the next telemetry frame.
  esp_task_wdt_config_t wdt = {};
  wdt.timeout_ms = cfg::WDT_TIMEOUT_S * 1000;
  wdt.idle_core_mask = 0;  // watch only loop(), not the idle tasks
  wdt.trigger_panic = true;
  if (esp_task_wdt_init(&wdt) == ESP_ERR_INVALID_STATE) esp_task_wdt_reconfigure(&wdt);
  esp_task_wdt_add(nullptr);
}

// Pick the oldest unsent readings (flash spill first, then the RAM buffer), build
// the frame with the freshest cell info / signal strength and queue the TCP
// upload. On success a send is pending; false if nothing could be queued.
static bool queueUpload() {
  const Telemetry::Reading *src = gReadings;
  uint8_t cnt = gReadingCount;
  gSendFromSpill = Backlog::spillCount() > 0;
  if (gSendFromSpill) {
    cnt = Backlog::spillPeek(gChunk, cfg::TELEMETRY_MAX_READINGS);
    src = gChunk;
    if (cnt == 0) {  // flash read failed: do not get stuck behind it, send the RAM buffer
      if (gLog) Serial.println("[Telemetry] could not read the flash backlog");
      gSendFromSpill = false;
      src = gReadings;
      cnt = gReadingCount;
    }
  }
  gSentCount = cnt;
  gInflightSpilled = 0;
  size_t n = cnt ? Telemetry::build(src, cnt, currentLocation(), gFrame, sizeof(gFrame)) : 0;
  const Settings::Values &s = Settings::get();
  if (n == 0 || !modem.requestSend(gFrame, n, s.serverIp, s.serverPort)) {
    gSentCount = 0;
    gSendFromSpill = false;
    if (gLog) Serial.println("[Telemetry] could not queue the frame");
    return false;
  }
  gSendPending = true;
  if (gLog) {
    Serial.printf("[Telemetry] sending %u %s reading(s), %u bytes to %s:%u\n", cnt,
                  gSendFromSpill ? "backlog" : "buffered", (unsigned)n, s.serverIp.c_str(),
                  (unsigned)s.serverPort);
    Serial.println(hexString(gFrame, n));
  }
  return true;
}

// Modem session finished: log it, take the network time if our clock is not
// set, and (if it was started to send) build the frame with the fresh cell info.
static void onModemSession(Modem::Event ev) {
  if (ev == Modem::Event::SESSION_FAILED) {
    if (gLog) Serial.printf("[Modem] session failed: %s\n", modem.lastError.c_str());
    if (gSendPending) noteUploadFailed();  // retry sooner than the normal period
    gSendPending = false;
    ledFlash(cfg::DEV_LED_LEVEL, 0, 0, 4000);
    return;
  }

  if (modem.networkTimeValid && !systemClockSet() && !gnss.hasFix()) {
    struct timeval tv = {(time_t)modem.networkEpoch, 0};
    settimeofday(&tv, nullptr);
  }
  if (gLog) modem.print(Serial);

  if (gSendPending) {
    gSendPending = false;  // queueUpload() sets it again once the frame is queued
    queueUpload();
  }
}

// TCP upload finished. Drop the readings that were sent only if the server acked.
static void onModemSend(Modem::Event ev) {
  gSendPending = false;
  const bool acked = (ev == Modem::Event::SEND_DONE);
  ledFlash(acked ? 0 : cfg::DEV_LED_LEVEL, acked ? cfg::DEV_LED_LEVEL : 0, 0, acked ? 2000 : 4000);

  if (!acked) {
    noteUploadFailed();
    if (gLog)
      Serial.printf("[Telemetry] send failed: %s (keeping %u buffered + %lu in flash)\n",
                    modem.lastError.c_str(), gReadingCount, (unsigned long)Backlog::spillCount());
    if (gLog && modem.lastRx.length()) {
      Serial.print("[Modem] received before the failure: ");
      for (size_t i = 0; i < modem.lastRx.length(); i++) {
        const uint8_t c = (uint8_t)modem.lastRx[i];
        if (c >= 0x20 && c < 0x7F) Serial.print((char)c);
        else Serial.printf("<%02X>", c);  // CR/LF and binary bytes shown in hex
      }
      Serial.println();
    }
    gSentCount = 0;
    gInflightSpilled = 0;
    gSendFromSpill = false;
    return;
  }

  gFailCount = 0;
  const bool wasSpill = gSendFromSpill;
  if (wasSpill) {
    Backlog::spillDrop(gSentCount);  // the oldest chunk, now delivered
  } else {
    if (gSentCount > gReadingCount) gSentCount = gReadingCount;
    memmove(&gReadings[0], &gReadings[gSentCount],
            sizeof(Telemetry::Reading) * (gReadingCount - gSentCount));
    gReadingCount -= gSentCount;
    if (gInflightSpilled) Backlog::spillDrop(gInflightSpilled);  // sent, but had moved to flash
    Backlog::save(gReadings, gReadingCount);
  }
  gSentCount = 0;
  gInflightSpilled = 0;
  gSendFromSpill = false;
  if (gLog)
    Serial.printf("[Telemetry] server acked, %u reading(s) left in buffer, %lu in flash\n",
                  gReadingCount, (unsigned long)Backlog::spillCount());

  // Catching up after an outage: send the next chunk in this same modem session
  // (it is still powered) rather than waiting a whole period. Not on a weak battery.
  if (wasSpill && gBattLevel < 2 && backlogPending() && !queueUpload()) {
    gLastSession = millis() - sessionPeriodMs();  // modem busy: upload at the next chance
  }
}

void loop() {
  esp_task_wdt_reset();  // proof loop() is still running
  uint32_t now = millis();

  if (!gBootReported && now >= cfg::SERIAL_ATTACH_DELAY_MS) {
    gBootReported = true;
    printBootReport();
  }
  if (gBootReported) console.poll();

  // GNSS: report once when the baud search finishes, then print fixes.
  static GNSSReceiver::Status lastGnss = GNSSReceiver::Status::DETECTING;
  if (gnss.poll() && gLog) gnss.print(Serial);
  if (gnss.status() != lastGnss) {
    lastGnss = gnss.status();
    if (gBootReported) {
      if (lastGnss == GNSSReceiver::Status::OK) Serial.printf("[GNSS] NMEA found at %lu baud\n", (unsigned long)gnss.baud());
      else if (lastGnss == GNSSReceiver::Status::FAILED) Serial.println("[GNSS] no NMEA heard at any baud rate");
    }
  }

  // Modem: log state changes, handle a finished session.
  static Modem::State lastModem = Modem::State::OFF;
  Modem::Event ev = modem.service();
  if (modem.state() != lastModem) {
    lastModem = modem.state();
    if (gLog) Serial.printf("[Modem] -> %s\n", modem.stateName());
  }
  if (ev == Modem::Event::SEND_DONE || ev == Modem::Event::SEND_FAILED) onModemSend(ev);
  else if (ev != Modem::Event::NONE) onModemSession(ev);

  updateBatteryLevel();
  updateLed(now);

  if (!gBootReported) return;  // timers start after the boot report

  // Open a measurement window when a sample is due, and poll the sensors only
  // while it is open.
  if (!gMeasuring && now - gLastSample >= samplePeriodMs()) {
    gLastSample = now;
    startMeasure(true);
  }
  pollSensors(now);

  // Window finished: take the reading and add it to the batch.
  if (gMeasuring && measureDone(now)) {
    gMeasuring = false;
    for (size_t i = 0; i < DEVICE_COUNT; i++) devices[i]->windowEnd();
    if (gSampleWanted) {
      gSampleWanted = false;
      pushReading(collectReading());
      Backlog::save(gReadings, gReadingCount);  // survives a reset while offline
      ledFlash(cfg::DEV_LED_LEVEL, cfg::DEV_LED_LEVEL, cfg::DEV_LED_LEVEL, 300);
      if (gLog) Serial.printf("[Telemetry] %u reading(s) buffered, status=0x%02X\n", gReadingCount,
                              gReadings[gReadingCount - 1].status);
    }
  }

  // Wake the modem only when there is data to send.
  if (!gSendPending && (!modem.busy() || modem.idleHeld()) && backlogPending() &&
      now - gLastSession >= nextSessionDelayMs()) {
    gLastSession = now;
    gSendPending = true;
    gSentCount = 0;
    gInflightSpilled = 0;
    gSendFromSpill = false;
    modem.request();
    if (gLog) Serial.println("[Modem] waking to send data");
  }
}
