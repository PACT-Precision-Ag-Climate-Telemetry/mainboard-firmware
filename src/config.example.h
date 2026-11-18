#pragma once
// Copy this file to config.h and fill in your own values. config.h is gitignored.
// ============================================================================
//  Mainboard firmware: single source of truth for hardware and tuning values.
//  Pins come from the KiCad schematic (mainboard-master.kicad_sch); I2C
//  addresses are as scanned on the real board (docs/i2c-addresses.md).
//  Change a value HERE and nothing else.
// ============================================================================
#include <Arduino.h>

namespace cfg {

// ---- Identity --------------------------------------------------------------
// Model prefix of the device ID: "TES-MBO" -> wire ID "TESMBO" + 4-char serial.
constexpr const char *DEVICE_MODEL = "TES-MBO";

// ---- Debug serial (UART0, USB) --------------------------------------------
constexpr uint32_t SERIAL_BAUD = 115200;
constexpr uint32_t SERIAL_ATTACH_DELAY_MS = 1500;  // let the monitor attach

// ---- I2C bus ---------------------------------------------------------------
// GPIO8/9 via 100R (R7/R8), 4.7k pull-ups to 3V3 (R1/R2).
constexpr int I2C_SDA_PIN = 8;
constexpr int I2C_SCL_PIN = 9;
constexpr uint32_t I2C_FREQ_HZ = 100000;

// ---- I2C device addresses (7-bit) -------------------------------------------
constexpr uint8_t ADDR_VL53L1X = 0x29;  // ToF distance      (J3)
constexpr uint8_t ADDR_MHZ9041A = 0x34;  // methane           (J4)
constexpr uint8_t ADDR_SHT30 = 0x44;     // temp / humidity   (J2)
constexpr uint8_t ADDR_ADS1115 = 0x48;   // ADC               (J6)
constexpr uint8_t ADDR_SCD40 = 0x62;     // CO2               (J1)

// ---- Sensor read intervals (ms) ---------------------------------------------
constexpr uint32_t PERIOD_VL53L1X_MS = 500;
constexpr uint32_t PERIOD_MHZ9041A_MS = 1000;
constexpr uint32_t PERIOD_SHT30_MS = 2000;
constexpr uint32_t PERIOD_ADS1115_MS = 1000;
constexpr uint32_t PERIOD_SCD40_MS = 1000;  // new data only every ~5 s

// ---- GNSS receiver (J8, UART1, NMEA) -----------------------------------------
// Net names are from the GNSS module's side: the ESP transmits on GNSS_RXD
// and receives on GNSS_TXD.
constexpr int GNSS_RXD_PIN = 15;  // module RX = ESP TX
constexpr int GNSS_TXD_PIN = 16;  // module TX = ESP RX
constexpr uint32_t GNSS_BAUDS[] = {9600, 38400, 115200, 57600, 4800};  // tried in order
constexpr uint32_t GNSS_DETECT_MS = 1500;   // listen time per baud rate
constexpr uint32_t GNSS_PRINT_MS = 1000;

// ---- Cellular modem (J7, SIMCom A7670E, UART2, AT commands) -----------------
// UART net names are from the modem's side: the ESP transmits on MODEM_RXD
// and receives on MODEM_TXD.
constexpr int MODEM_PWR_PIN = 5;    // MOSFET: HIGH = modem powered, LOW = off
constexpr int MODEM_EN_PIN = 4;     // power-key pulse (LOW 1 s, then HIGH)
constexpr int MODEM_RXD_PIN = 17;   // module RX = ESP TX
constexpr int MODEM_TXD_PIN = 18;   // module TX = ESP RX
constexpr uint32_t MODEM_BAUD = 115200;
constexpr uint32_t MODEM_RAIL_SETTLE_MS = 200;   // after MODEM_PWR high
constexpr uint32_t MODEM_EN_PULSE_MS = 1000;     // EN held LOW
constexpr uint32_t MODEM_BOOT_TIMEOUT_MS = 20000;  // wait for first "OK"
constexpr uint32_t MODEM_AT_RETRY_MS = 300;        // between "AT" probes while booting
// The modem is OFF by default. It is woken only for a session (send data, or
// pull status / cell info / network time) and powered down again afterwards.
constexpr uint32_t MODEM_LINGER_MS = 5000;         // stay on after the last job, for follow-ups
constexpr uint32_t MODEM_REGISTER_TIMEOUT_MS = 60000;  // give up waiting for the network
constexpr uint32_t MODEM_REGISTER_POLL_MS = 2000;  // between registration checks
constexpr uint32_t MODEM_SHUTDOWN_WAIT_MS = 3000;  // for AT+CPOF to answer
constexpr uint32_t MODEM_RAW_TIMEOUT_MS = 5000;    // console "at" command
constexpr uint32_t MODEM_SESSION_PERIOD_MS = 120000;  // wake to send. Bench value; field: 5-120 min

// ---- Firmware ----------------------------------------------------------------
constexpr const char *FW_VERSION = "0.1.0";

// ---- Field configurator console (get/set commands) --------------------------
// The configurator talks over UART0 (USB / the J10 PROG pads), the same port as
// the debug log, using plain text lines. See docs/field-configurator.md.
constexpr size_t CONSOLE_MAX_LINE = 128;

// ---- Settings defaults (used until a value is set from the configurator) ------
constexpr const char *DEFAULT_SERVER_IP = "0.0.0.0";  // TODO: your server IP
constexpr uint16_t DEFAULT_SERVER_PORT = 0;  // TODO: your server port
constexpr uint8_t DEFAULT_MAGIC_BYTE_1 = 0x00;  // must match the server config
constexpr uint8_t DEFAULT_MAGIC_BYTE_2 = 0x00;
constexpr bool DEFAULT_TELEMETRY_RAW = true;
constexpr float DEFAULT_OFFSET_MM = 0.0f;
constexpr float DEFAULT_MOISTURE_SLOPE = 1.0f;       // percent per volt
constexpr float DEFAULT_MOISTURE_INTERCEPT = 0.0f;   // percent
constexpr float DEFAULT_BATTERY_DIVIDER_R1 = 220000.0f;  // ohms
constexpr float DEFAULT_BATTERY_DIVIDER_R2 = 47000.0f;   // ohms
constexpr float DEFAULT_GPS_LATITUDE = 0.0f;
constexpr float DEFAULT_GPS_LONGITUDE = 0.0f;
constexpr const char *DEFAULT_APN = "";

// ---- Telemetry ---------------------------------------------------------------
constexpr uint8_t TELEMETRY_MAX_READINGS = 24;       // max records per frame
constexpr uint32_t TELEMETRY_SAMPLE_PERIOD_MS = 30000;  // bench value; field: 5 min
constexpr float METHANE_PPM_PER_LEL_PERCENT = 500.0f;   // LEL = 5 % vol = 50,000 ppm

// ADS1115 channels (copied from the old firmware; CONFIRM against the board).
constexpr uint8_t ADC_BATTERY_CHANNEL = 0;   // AIN0, via the R1/R2 divider
constexpr uint8_t ADC_MOISTURE_CHANNEL = 1;  // AIN1, capacitive soil sensor
// Battery sense switches (nets BATT_SENSE_EN / SOLAR_DIS on the DevKit).
// Both are driven HIGH only while a battery reading is taken, then LOW again:
//   BATT_SENSE_EN HIGH = connect the divider to the ADC input
//   SOLAR_DIS     HIGH = disconnect the solar panel so it can't skew the reading
constexpr int BATT_SENSE_EN_PIN = 10;
constexpr int SOLAR_DIS_PIN = 11;
constexpr uint32_t BATTERY_SETTLE_MS = 5000;    // wait after raising the pins
constexpr uint32_t BATTERY_ADC_TIMEOUT_MS = 500;  // give up on an ADC conversion
// Solar is disconnected during the whole settle time, so keep readings sparse.
constexpr uint32_t PERIOD_BATTERY_MS = 60000;   // time between battery readings
constexpr uint32_t PERIOD_MOISTURE_MS = 2000;   // time between soil moisture readings
// Battery readings outside this window count as a failed read.
constexpr float BATTERY_PLAUSIBLE_MIN_MV = 1000.0f;
constexpr float BATTERY_PLAUSIBLE_MAX_MV = 20000.0f;
// 12 V AGM pack state-of-charge curve (old firmware).
constexpr float BATTERY_V_FULL = 12.85f;   // 100 % resting voltage
constexpr float BATTERY_V_EMPTY = 11.50f;  // 0 % resting voltage
constexpr float BATTERY_LOAD_COMP_V = 0.03f;  // compensation for sensor load
// A sensor reading older than this counts as failed in the status byte.
constexpr uint32_t SENSOR_STALE_MS = 15000;

}  // namespace cfg
