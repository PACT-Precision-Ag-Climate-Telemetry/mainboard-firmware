#include "telemetry.h"
#include <math.h>
#include "config.h"
#include "device_id.h"
#include "settings.h"

namespace Telemetry {

namespace {

// ---- little-endian writers --------------------------------------------------
void putU8(uint8_t *b, size_t &o, uint8_t v) { b[o++] = v; }
void putU16(uint8_t *b, size_t &o, uint16_t v) {
  b[o++] = v & 0xFF;
  b[o++] = v >> 8;
}
void putI16(uint8_t *b, size_t &o, int16_t v) { putU16(b, o, (uint16_t)v); }
void putI32(uint8_t *b, size_t &o, int32_t v) {
  uint32_t u = (uint32_t)v;
  for (int i = 0; i < 4; i++) b[o++] = (u >> (8 * i)) & 0xFF;
}
void putU64(uint8_t *b, size_t &o, uint64_t v) {
  for (int i = 0; i < 8; i++) b[o++] = (v >> (8 * i)) & 0xFF;
}

// ---- scaling ----------------------------------------------------------------
// Round to nearest BEFORE scaling's cast, never truncate (28.499 -> 2850).
int16_t scaleI16x100(float v) {
  if (isnan(v)) return 0;
  long n = lroundf(v * 100.0f);
  return (int16_t)constrain(n, -32768L, 32767L);
}
uint16_t scaleU16x100(float v, float maxValue) {  // percent -> 0..100.00
  if (isnan(v)) return 0;
  v = constrain(v, 0.0f, maxValue);
  return (uint16_t)lroundf(v * 100.0f);
}
int32_t scaleCoord(double deg) { return (int32_t)llround(deg * 10000000.0); }

size_t bodySize(bool raw) { return 21 + (raw ? 4 : 0); }

void putBody(uint8_t *b, size_t &o, const Reading &r, bool raw) {
  putI16(b, o, scaleI16x100(r.temperatureC));
  putU16(b, o, scaleU16x100(r.humidityRH, 100.0f));
  putU16(b, o, r.methanePpm);
  putU16(b, o, r.co2Ppm);
  putU16(b, o, r.offsetMm);
  putU16(b, o, r.depthMm);
  putU16(b, o, scaleU16x100(r.moisturePct, 100.0f));
  putU16(b, o, scaleU16x100(r.batteryPct, 100.0f));
  putU16(b, o, r.batteryMv);
  putI16(b, o, scaleI16x100(r.espTemperatureC));
  if (raw) {
    putU16(b, o, r.methaneRaw);
    putU16(b, o, r.moistureRaw);
  }
  putU8(b, o, r.status);
}

// Seconds from `older` to `newer`, clamped to uint16 (0 if out of order).
uint16_t gapSeconds(uint64_t newer, uint64_t older) {
  if (newer <= older) return 0;
  uint64_t d = newer - older;
  return d > 0xFFFF ? 0xFFFF : (uint16_t)d;
}

}  // namespace

uint16_t currentOffsetMm() {
  float v = Settings::get().offsetMm;
  if (isnan(v) || v < 0) return 0;
  return v > 65535.0f ? 65535 : (uint16_t)lroundf(v);
}

size_t frameSize(uint8_t count, bool raw, bool gps, bool cell) {
  // 24 + (2 + body) * count + gps + cell   (docs/telemetry.md section 4)
  return 24 + (2 + bodySize(raw)) * count + (gps ? GPS_BLOCK_SIZE : 0) +
         (cell ? CELL_BLOCK_SIZE : 0);
}

size_t maxFrameSize() { return frameSize(cfg::TELEMETRY_MAX_READINGS, true, true, true); }

size_t build(const Reading *readings, uint8_t count, const Location &loc,
             uint8_t *out, size_t capacity) {
  if (readings == nullptr || out == nullptr || count == 0) return 0;

  const Settings::Values &s = Settings::get();
  const bool raw = s.telemetryRaw;
  const bool gps = loc.hasFix;
  const bool cell = loc.hasCell;

  const size_t total = frameSize(count, raw, gps, cell);
  if (total > capacity) return 0;

  size_t o = 0;

  // -- header (14 B): magic(2) flags(1) device_id(10) count(1)
  putU8(out, o, s.magicByte1);
  putU8(out, o, s.magicByte2);
  uint8_t flags = PAYLOAD_VERSION & FLAG_VERSION_MASK;
  if (raw) flags |= FLAG_RAW;
  if (cell) flags |= FLAG_CELL;
  if (gps) flags |= FLAG_GPS;
  putU8(out, o, flags);

  char id[10];
  buildDeviceId(id);
  for (int i = 0; i < 10; i++) putU8(out, o, (uint8_t)id[i]);
  putU8(out, o, count);

  // -- historical records: timedif(2) + body. timedif = seconds to the NEXT
  //    newer reading.
  for (uint8_t i = 0; i + 1 < count; i++) {
    putU16(out, o, gapSeconds(readings[i + 1].timestamp, readings[i].timestamp));
    putBody(out, o, readings[i], raw);
  }

  // -- newest record: timedif(2, gap to previous older) + timestamp(8) + body
  const Reading &newest = readings[count - 1];
  uint16_t prevGap = (count > 1) ? gapSeconds(newest.timestamp, readings[count - 2].timestamp) : 0;
  putU16(out, o, prevGap);
  putU64(out, o, newest.timestamp);
  putBody(out, o, newest, raw);

  // -- optional blocks, in this order
  if (gps) {
    putI32(out, o, scaleCoord(loc.latitude));
    putI32(out, o, scaleCoord(loc.longitude));
  }
  if (cell) {
    putU16(out, o, loc.mcc);
    putU16(out, o, loc.mnc);
  }

  // -- trailer (2 B), always last
  putU8(out, o, (uint8_t)loc.signalDbm);
  putU8(out, o, loc.resetReason);

  return (o == total) ? o : 0;  // sanity: must match the documented length
}

}  // namespace Telemetry
