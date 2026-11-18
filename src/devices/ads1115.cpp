#include "ads1115.h"

static const uint8_t REG_CONVERSION = 0x00;
static const uint8_t REG_CONFIG = 0x01;
static const float LSB_VOLTS = 0.125e-3f;        // +/-4.096 V range -> 125 uV/bit
static const uint32_t CONVERSION_MIN_MS = 9;     // 128 SPS = 7.8 ms
static const uint32_t CONVERSION_TIMEOUT_MS = 100;

bool ADS1115::init() {
  // The config register must read back (proves reads work, not just ACKs).
  Wire.beginTransmission(_addr);
  Wire.write(REG_CONFIG);
  if (Wire.endTransmission(false) != 0) return false;
  return Wire.requestFrom(_addr, (uint8_t)2) == 2 && (Wire.read(), Wire.read(), true);
}

bool ADS1115::startConversion(uint8_t ch) {
  // OS=1 (start) | MUX=100+ch (AINch vs GND) | PGA=001 (+/-4.096 V)
  // | MODE=1 (single shot) | DR=100 (128 SPS) | comparator disabled (11)
  uint16_t cfg = 0x8000 | ((0x4 | ch) << 12) | 0x0200 | 0x0100 | 0x0080 | 0x0003;
  Wire.beginTransmission(_addr);
  Wire.write(REG_CONFIG);
  Wire.write(cfg >> 8);
  Wire.write(cfg & 0xFF);
  return Wire.endTransmission() == 0;
}

uint32_t ADS1115::queue(uint8_t channel) {
  if (channel > 3) return 0;
  _queued |= (1 << channel);
  // The next conversion of this channel to start is number _startCnt + 1,
  // even if one is in flight right now (that one began too early to count).
  return _startCnt[channel] + 1;
}

bool ADS1115::update() {
  _scanPending = 0x0F;  // periodic scan: all four channels
  _queued |= 0x0F;
  return false;         // service() reports when the scan completes
}

bool ADS1115::service() {
  bool scanFinished = false;
  uint32_t now = millis();

  if (_busy && now - _t0 >= CONVERSION_MIN_MS) {
    Wire.beginTransmission(_addr);
    Wire.write(REG_CONFIG);
    bool got = Wire.endTransmission(false) == 0 && Wire.requestFrom(_addr, (uint8_t)2) == 2;
    if (got) {
      uint16_t c = (Wire.read() << 8) | Wire.read();
      if (c & 0x8000) {  // OS bit set: conversion finished
        Wire.beginTransmission(_addr);
        Wire.write(REG_CONVERSION);
        if (Wire.endTransmission(false) == 0 && Wire.requestFrom(_addr, (uint8_t)2) == 2) {
          raw[_ch] = (int16_t)((Wire.read() << 8) | Wire.read());
          volts[_ch] = raw[_ch] * LSB_VOLTS;
          _doneCnt[_ch]++;
          if (_scanPending & (1 << _ch)) {
            _scanPending &= ~(1 << _ch);
            if (_scanPending == 0) scanFinished = true;
          }
        }
        _busy = false;
      }
    }
    if (_busy && now - _t0 > CONVERSION_TIMEOUT_MS) {  // give up on this one
      _busy = false;
      _scanPending &= ~(1 << _ch);
    }
  }

  if (!_busy && _queued) {
    uint8_t ch = 0;
    while (!(_queued & (1 << ch))) ch++;  // lowest queued channel
    _queued &= ~(1 << ch);
    if (startConversion(ch)) {
      _busy = true;
      _ch = ch;
      _t0 = now;
      _startCnt[ch]++;
    } else {
      _scanPending &= ~(1 << ch);
    }
  }
  return scanFinished;
}

void ADS1115::print(Print &out) const {
  out.printf("[%s 0x%02X] AIN0=%.4f V  AIN1=%.4f V  AIN2=%.4f V  AIN3=%.4f V\n",
             _name, _addr, volts[0], volts[1], volts[2], volts[3]);
}
