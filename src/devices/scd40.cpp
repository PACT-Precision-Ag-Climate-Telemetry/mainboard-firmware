#include "scd40.h"

static const uint16_t CMD_STOP_PERIODIC = 0x3F86;
static const uint16_t CMD_START_PERIODIC = 0x21B1;
static const uint16_t CMD_SET_ASC = 0x2416;  // automatic self-calibration on/off
static const uint16_t CMD_GET_DATA_READY = 0xE4B8;
static const uint16_t CMD_READ_MEASUREMENT = 0xEC05;
static const uint32_t STOP_MS = 500;  // after stop_periodic_measurement
static const uint32_t CMD_MS = 2;     // execution time of the 1 ms commands

bool SCD40::init() {
  writeCmd16(CMD_STOP_PERIODIC);  // harmless if not running
  _st = St::STOPPING;             // service() starts periodic mode in 500 ms
  _t = millis();
  return true;
}

bool SCD40::update() {
  if (_st != St::IDLE) return false;
  if (!writeCmd16(CMD_GET_DATA_READY)) return false;
  _st = St::WAIT_READY;
  _t = millis();
  return false;
}

bool SCD40::service() {
  uint32_t now = millis();
  switch (_st) {
    case St::STOPPING:
      if (now - _t >= STOP_MS) {
        // set_automatic_self_calibration_enabled(0). Only accepted while idle,
        // so it goes between stop and start. Re-sent at every boot.
        const uint8_t off[2] = {0x00, 0x00};
        Wire.beginTransmission(_addr);
        Wire.write(CMD_SET_ASC >> 8);
        Wire.write(CMD_SET_ASC & 0xFF);
        Wire.write(off, 2);
        Wire.write(crc8(off, 2));
        Wire.endTransmission();
        _st = St::ASC_SET;
        _t = now;
      }
      return false;

    case St::ASC_SET:
      if (now - _t >= CMD_MS) {
        writeCmd16(CMD_START_PERIODIC);
        _st = St::IDLE;
      }
      return false;

    case St::WAIT_READY: {
      if (now - _t < CMD_MS) return false;
      _st = St::IDLE;
      uint8_t r[3];
      // Low 11 bits non-zero = a measurement is waiting.
      if (!readBytes(r, 3) || crc8(r, 2) != r[2]) return false;
      if ((((r[0] << 8) | r[1]) & 0x07FF) == 0) return false;
      if (!writeCmd16(CMD_READ_MEASUREMENT)) return false;
      _st = St::WAIT_DATA;
      _t = now;
      return false;
    }

    case St::WAIT_DATA: {
      if (now - _t < CMD_MS) return false;
      _st = St::IDLE;
      uint8_t b[9];
      if (!readBytes(b, 9)) return false;
      for (uint8_t i = 0; i < 9; i += 3)
        if (crc8(b + i, 2) != b[i + 2]) return false;
      co2ppm = (b[0] << 8) | b[1];
      uint16_t rawT = (b[3] << 8) | b[4];
      uint16_t rawH = (b[6] << 8) | b[7];
      temperatureC = -45.0f + 175.0f * rawT / 65535.0f;
      humidityRH = 100.0f * rawH / 65535.0f;
      return true;
    }

    default:
      return false;
  }
}

void SCD40::print(Print &out) const {
  out.printf("[%s 0x%02X] CO2=%u ppm  T=%.2f C  RH=%.1f %%\n", _name, _addr,
             co2ppm, temperatureC, humidityRH);
}
