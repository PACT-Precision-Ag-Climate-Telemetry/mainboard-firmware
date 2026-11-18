#pragma once
#include <Arduino.h>

// Builds the binary telemetry frame described in docs/telemetry.md.
// Pure formatting: no sensors, no network. Feed it readings, get bytes.
//
// What comes from the field configurator (see settings.h):
//   * magic bytes     -> TELEMETRY_MAGIC_BYTE_1 / _2
//   * RAW flag        -> TELEMETRY_RAW (adds methane_raw + moisture_raw)
//   * `offset` field  -> OFFSET (mm). Capture it per reading with
//                        Telemetry::currentOffsetMm() when you take a sample,
//                        so a change in the field only affects new readings.
// The device ID comes from buildDeviceId() (device_id.h).
namespace Telemetry {

// Flag byte (header offset 2).
constexpr uint8_t PAYLOAD_VERSION = 0;
constexpr uint8_t FLAG_VERSION_MASK = 0x0F;
constexpr uint8_t FLAG_RAW = 1 << 5;
constexpr uint8_t FLAG_CELL = 1 << 6;
constexpr uint8_t FLAG_GPS = 1 << 7;

// status byte bits: a set bit means that sensor's read failed.
namespace Status {
constexpr uint8_t OK = 0;
constexpr uint8_t BATTERY_READ_FAILED = 1 << 0;
constexpr uint8_t ULTRASONIC_READ_FAILED = 1 << 1;  // the depth sensor (ToF)
constexpr uint8_t TEMPERATURE_READ_FAILED = 1 << 2;
constexpr uint8_t METHANE_READ_FAILED = 1 << 3;
constexpr uint8_t CO2_READ_FAILED = 1 << 4;
constexpr uint8_t MOISTURE_READ_FAILED = 1 << 5;
}  // namespace Status

constexpr size_t HEADER_SIZE = 14;
constexpr size_t GPS_BLOCK_SIZE = 8;
constexpr size_t CELL_BLOCK_SIZE = 4;

// One sample, in natural units. Scaling to the wire format happens in build().
struct Reading {
  uint64_t timestamp = 0;      // Unix seconds, UTC
  float temperatureC = 0;
  float humidityRH = 0;        // percent
  uint16_t methanePpm = 0;
  uint16_t co2Ppm = 0;
  uint16_t offsetMm = 0;       // from the OFFSET setting at sample time
  uint16_t depthMm = 0;        // sent as `depth`
  float moisturePct = 0;
  float batteryPct = 0;
  uint16_t batteryMv = 0;
  uint16_t methaneRaw = 0;     // only sent when RAW is on
  uint16_t moistureRaw = 0;    // only sent when RAW is on
  uint8_t status = Status::OK;
};

// Shared by the whole frame (refreshed far less often than sensors).
struct Location {
  bool hasFix = false;
  double latitude = 0;   // double: float loses ~9 cm at 1e-7 degree resolution
  double longitude = 0;
  bool hasCell = false;
  uint16_t mcc = 0;
  uint16_t mnc = 0;
};

// The OFFSET setting rounded to the wire type (uint16, clamped).
uint16_t currentOffsetMm();

// Exact frame length for these options (see docs/telemetry.md section 4).
size_t frameSize(uint8_t count, bool raw, bool gps, bool cell);

// Largest frame build() can produce (24 readings, RAW, GPS, CELL).
size_t maxFrameSize();

// Serializes `count` readings (oldest first) into `out`. Returns bytes
// written, or 0 if count is 0 / out is too small / a pointer is null.
size_t build(const Reading *readings, uint8_t count, const Location &location,
             uint8_t *out, size_t capacity);

}  // namespace Telemetry
