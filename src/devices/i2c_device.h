#pragma once
#include <Arduino.h>
#include <Wire.h>
#include "../config.h"

// Base class for every I2C device handler. Nothing here blocks.
//   begin()  -> checks the device ACKs, then runs init()
//   poll()   -> call every loop(). It runs service() every time (finishes
//               work that was started earlier) and update() once per
//               `periodMs` (starts a new measurement). Returns true when new
//               valid data arrived, so the caller can print()/log it.
//
// A device that must wait (a conversion, a command delay) starts the work in
// update(), remembers a state and a timestamp, and finishes it in service()
// on a later loop() pass. No delay() calls.
class I2CDevice {
 public:
  I2CDevice(const char *name, uint8_t addr, uint32_t periodMs)
      : _name(name), _addr(addr), _period(periodMs) {}
  virtual ~I2CDevice() {}

  bool begin() {
    _ok = probe() && init();
    return _ok;
  }

  bool poll() {
    if (!_ok) return false;
    bool fresh = service();
    uint32_t now = millis();
    if (!_started || (now - _last) >= _period) {
      _started = true;
      _last = now;
      if (update()) fresh = true;
    }
    if (fresh) {
      _lastGood = millis();
      _hasGood = true;
    }
    return fresh;
  }

  // Start (or restart, for a retry) one measurement attempt: the next poll()
  // runs update() right away instead of waiting for the period.
  void kick() {
    restart();
    _started = false;
  }

  // Measurement window hooks. Sensors are single-shot: they only measure while
  // a window is open (see main.cpp). windowStart() wakes / starts the sensor,
  // windowEnd() lets it go idle again. busy() is true while the handler still
  // needs poll() after the window closed (e.g. to stop a sensor cleanly).
  virtual void windowStart() {}
  virtual void windowEnd() {}
  virtual bool busy() const { return false; }
  // How long one attempt may take before main.cpp retries it.
  virtual uint32_t attemptTimeoutMs() const { return cfg::SENSOR_ATTEMPT_TIMEOUT_MS; }

  virtual void print(Print &out) const = 0;

  // True if a valid reading arrived within the last maxAgeMs.
  bool fresh(uint32_t maxAgeMs) const {
    return _hasGood && (millis() - _lastGood) <= maxAgeMs;
  }

  const char *name() const { return _name; }
  uint8_t address() const { return _addr; }
  bool ok() const { return _ok; }

 protected:
  virtual bool init() = 0;
  // Start a measurement or fetch data. True if a fresh, valid reading was
  // stored immediately.
  virtual bool update() = 0;
  // Called on every poll(). Finish pending work. True if it stored a fresh,
  // valid reading.
  virtual bool service() { return false; }
  // Drop any half-finished attempt so update() can start a fresh one.
  virtual void restart() {}

  bool probe() {
    Wire.beginTransmission(_addr);
    return Wire.endTransmission() == 0;
  }

  bool writeCmd16(uint16_t cmd) {
    Wire.beginTransmission(_addr);
    Wire.write(cmd >> 8);
    Wire.write(cmd & 0xFF);
    return Wire.endTransmission() == 0;
  }

  bool readBytes(uint8_t *buf, uint8_t n) {
    if (Wire.requestFrom(_addr, n) != n) return false;
    for (uint8_t i = 0; i < n; i++) buf[i] = Wire.read();
    return true;
  }

  // Sensirion CRC-8 (poly 0x31, init 0xFF) used by SHT3x and SCD4x.
  static uint8_t crc8(const uint8_t *d, uint8_t n) {
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < n; i++) {
      crc ^= d[i];
      for (uint8_t b = 0; b < 8; b++)
        crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : (crc << 1);
    }
    return crc;
  }

  const char *_name;
  uint8_t _addr;
  uint32_t _period;
  uint32_t _last = 0;
  bool _started = false;
  bool _ok = false;
  uint32_t _lastGood = 0;
  bool _hasGood = false;
};
