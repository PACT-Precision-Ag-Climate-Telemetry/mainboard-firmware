#include <Arduino.h>
#include <Wire.h>
#include <sys/time.h>

#include "config.h"  // all pins / addresses / bauds / intervals live here
#include "device_id.h"
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
static bool gLog = true;  // "set LOG 0" silences periodic output while configuring

// ---- telemetry batch ---------------------------------------------------------
static Telemetry::Reading gReadings[cfg::TELEMETRY_MAX_READINGS];
static uint8_t gReadingCount = 0;
static uint32_t gLastSample = 0;
static uint32_t gLastSession = 0;
static bool gSendPending = false;  // a modem session was started to send data

static void pushReading(const Telemetry::Reading &r) {
  if (gReadingCount == cfg::TELEMETRY_MAX_READINGS) {  // full: drop the oldest
    memmove(&gReadings[0], &gReadings[1], sizeof(Telemetry::Reading) * (gReadingCount - 1));
    gReadingCount--;
  }
  gReadings[gReadingCount++] = r;
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
  return loc;
}

// Builds one Reading from the latest sensor values. A sensor with no recent
// valid reading sets its bit in `status` and sends 0.
static Telemetry::Reading collectReading() {
  using namespace Telemetry;
  Reading r;
  r.timestamp = nowEpoch();
  r.offsetMm = currentOffsetMm();  // OFFSET from the field configurator

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
    r.methaneRaw = (uint16_t)constrain(lroundf(methane.ch4PercentLEL), 0L, 65535L);
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
  static uint8_t frame[22 + (2 + 23) * cfg::TELEMETRY_MAX_READINGS + 8 + 4];  // max frame
  size_t n = Telemetry::build(gReadings, gReadingCount, currentLocation(), frame, sizeof(frame));
  return n ? hexString(frame, n) : String("(no readings yet)");
}

// ---- read-only values exposed to the field configurator -----------------------
static String ageText(uint32_t ms) { return " (age " + String(ms / 1000) + " s)"; }

static const Binding kLiveBindings[] = {
    {"ID", "Device ID (model + serial from the MAC)", +[]() { return getDeviceIdString(); }, nullptr},
    {"FW_VERSION", "Firmware version", +[]() { return String(cfg::FW_VERSION); }, nullptr},
    {"LOG", "1 = periodic debug output on, 0 = off (not saved; resets at boot)",
     +[]() { return String(gLog ? 1 : 0); },
     +[](const String &v) {
       if (v != "0" && v != "1") return false;
       gLog = (v == "1");
       return true;
     }},
    {"SHT30_TEMPERATURE", "Air temperature, C", +[]() { return String(sht.temperatureC, 2); }, nullptr},
    {"SHT30_HUMIDITY", "Relative humidity, percent", +[]() { return String(sht.humidityRH, 1); }, nullptr},
    {"CO2_PPM", "SCD40 CO2, ppm", +[]() { return String(co2.co2ppm); }, nullptr},
    {"METHANE_LEL", "Methane, percent of lower explosive limit", +[]() { return String(methane.ch4PercentLEL, 2); }, nullptr},
    {"DISTANCE_MM", "VL53L1X distance, mm (sent as depth)", +[]() { return String(tof.distanceMm); }, nullptr},
    {"MOISTURE_PERCENT", "Soil moisture, percent 0-100. Returns the last reading and starts a new one",
     +[]() {
       moisture.request();
       return moisture.hasReading() ? String(moisture.percent, 1) + ageText(moisture.ageMs())
                                    : String("no reading yet");
     }, nullptr},
    {"MOISTURE_VOLTS", "Soil moisture sensor voltage at the ADC pin (last reading)",
     +[]() { return String(moisture.volts, 4); }, nullptr},
    {"MOISTURE_RAW", "Soil moisture raw ADC count (last reading)",
     +[]() { return String(moisture.raw); }, nullptr},
    {"BATTERY_MV", "Battery voltage, mV. Returns the last reading and starts a new one (about 5 s)",
     +[]() {
       battery.request();
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
}

// Modem session finished: log it, take the network time if our clock is not
// set, and (if it was started to send) build the frame with the fresh cell info.
static void onModemSession(Modem::Event ev) {
  if (ev == Modem::Event::SESSION_FAILED) {
    if (gLog) Serial.printf("[Modem] session failed: %s\n", modem.lastError.c_str());
    gSendPending = false;  // try again at the next session period
    return;
  }

  if (modem.networkTimeValid && !systemClockSet() && !gnss.hasFix()) {
    struct timeval tv = {(time_t)modem.networkEpoch, 0};
    settimeofday(&tv, nullptr);
  }
  if (gLog) modem.print(Serial);

  if (gSendPending) {
    gSendPending = false;
    if (gReadingCount > 0 && gLog) {
      Serial.printf("[Telemetry] frame ready: %u reading(s), cell block %s\n", gReadingCount,
                    modem.hasCell ? "yes" : "no");
      Serial.println(payloadHex());
      Serial.println("[Telemetry] TCP send is not implemented yet: frame NOT sent");
    }
  }
}

void loop() {
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
  if (ev != Modem::Event::NONE) onModemSession(ev);

  for (size_t i = 0; i < DEVICE_COUNT; i++) {
    if (devices[i]->poll() && gLog) devices[i]->print(Serial);
  }
  if (battery.poll() && gLog) battery.print(Serial);
  if (moisture.poll() && gLog) moisture.print(Serial);

  if (!gBootReported) return;  // timers start after the boot report

  // Take a reading and add it to the batch.
  if (now - gLastSample >= cfg::TELEMETRY_SAMPLE_PERIOD_MS) {
    gLastSample = now;
    pushReading(collectReading());
    if (gLog) Serial.printf("[Telemetry] %u reading(s) buffered, status=0x%02X\n", gReadingCount,
                            gReadings[gReadingCount - 1].status);
  }

  // Wake the modem only when there is data to send.
  if (!gSendPending && !modem.busy() && gReadingCount > 0 &&
      now - gLastSession >= cfg::MODEM_SESSION_PERIOD_MS) {
    gLastSession = now;
    gSendPending = true;
    modem.request();
    if (gLog) Serial.println("[Modem] waking to send data");
  }
}
