#include "sht30.h"

static const uint16_t CMD_SOFT_RESET = 0x30A2;
static const uint16_t CMD_SINGLE_HIGH = 0x2400;  // high repeatability, no clock stretch
static const uint32_t RESET_MS = 2;     // after soft reset
static const uint32_t MEASURE_MS = 16;  // max time for high repeatability

bool SHT30::init() {
  writeCmd16(CMD_SOFT_RESET);
  _st = St::RESETTING;
  _t = millis();
  return true;
}

bool SHT30::update() {
  if (_st != St::IDLE) return false;  // still resetting / measuring
  if (!writeCmd16(CMD_SINGLE_HIGH)) return false;
  _st = St::MEASURING;
  _t = millis();
  return false;  // the result arrives in service()
}

bool SHT30::service() {
  uint32_t now = millis();
  if (_st == St::RESETTING) {
    if (now - _t >= RESET_MS) _st = St::IDLE;
    return false;
  }
  if (_st != St::MEASURING || now - _t < MEASURE_MS) return false;
  _st = St::IDLE;

  uint8_t b[6];
  if (!readBytes(b, 6)) return false;
  if (crc8(b, 2) != b[2] || crc8(b + 3, 2) != b[5]) return false;

  uint16_t rawT = (b[0] << 8) | b[1];
  uint16_t rawH = (b[3] << 8) | b[4];
  temperatureC = -45.0f + 175.0f * rawT / 65535.0f;
  humidityRH = 100.0f * rawH / 65535.0f;
  return true;
}

void SHT30::print(Print &out) const {
  out.printf("[%s 0x%02X] T=%.2f C  RH=%.1f %%\n", _name, _addr, temperatureC,
             humidityRH);
}
