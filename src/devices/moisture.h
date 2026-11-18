#pragma once
#include <Arduino.h>
#include "ads1115.h"

// Capacitive soil-moisture sensor on an ADS1115 input (AIN1, net MOISTURE_ADC).
// Non-blocking: poll() queues an ADC conversion and collects it on a later pass.
//
//   percent = MOISTURE_SLOPE * volts + MOISTURE_INTERCEPT, clamped to 0-100
//
// SLOPE and INTERCEPT are field-configurator settings. The percentage is
// computed from the stored settings on every reading, so a change applies to
// the next reading without a reboot.
class Moisture {
 public:
  Moisture(ADS1115 &adc, uint8_t channel) : _adc(adc), _channel(channel) {}

  void request() { _requested = true; }   // start a reading as soon as idle
  bool poll();                            // true when a NEW valid reading is ready
  bool busy() const { return _waiting; }
  void print(Print &out) const;

  bool hasReading() const { return _hasGood; }
  uint32_t ageMs() const { return _hasGood ? millis() - _lastGood : 0; }
  bool fresh(uint32_t maxAgeMs) const { return _hasGood && ageMs() <= maxAgeMs; }

  float volts = NAN;             // voltage at the ADC pin
  uint16_t raw = 0;              // raw ADC count (0 if negative)
  float percent = NAN;           // 0-100 after calibration

 private:
  ADS1115 &_adc;
  uint8_t _channel;
  uint32_t _ticket = 0, _t = 0, _lastStart = 0, _lastGood = 0;
  bool _waiting = false, _requested = false, _everStarted = false, _hasGood = false;
};
