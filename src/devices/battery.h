#pragma once
#include <Arduino.h>
#include "ads1115.h"

// Battery voltage through the divider on an ADS1115 input. Non-blocking.
//
// One reading is a small state machine driven by poll():
//   1. drive BATT_SENSE_EN and SOLAR_DIS HIGH (sense on, solar panel off)
//   2. wait BATTERY_SETTLE_MS (loop() keeps running meanwhile)
//   3. queue an ADC conversion and wait for it
//   4. drive both pins LOW again, compute the result
// A new reading starts every PERIOD_BATTERY_MS, or on request().
//
//   battery volts = ADC volts * (R1 + R2) / R2
// R1 / R2 are the BATTERY_DIVIDER_R1 / _R2 settings (field configurator).
class Battery {
 public:
  Battery(ADS1115 &adc, uint8_t channel, int pinSenseEn, int pinSolarDis)
      : _adc(adc), _channel(channel), _pinSense(pinSenseEn), _pinSolar(pinSolarDis) {}

  void begin();                // both pins output, LOW (sense off, solar on)
  void request() { _requested = true; }  // start a reading as soon as idle
  bool poll();                 // advance the state machine; true when a NEW valid reading is ready
  bool busy() const { return _st != St::IDLE; }
  void print(Print &out) const;

  bool hasReading() const { return _hasGood; }
  uint32_t ageMs() const { return _hasGood ? millis() - _lastGood : 0; }
  bool fresh(uint32_t maxAgeMs) const { return _hasGood && ageMs() <= maxAgeMs; }

  float adcVolts = NAN;        // voltage at the ADC pin
  float volts = NAN;           // battery voltage
  uint16_t millivolts = 0;
  float percent = 0;           // state of charge from the resting-voltage curve

 private:
  enum class St : uint8_t { IDLE, SETTLING, CONVERTING };
  void release();              // sense off, solar on
  bool finish();               // compute from the ADC result

  ADS1115 &_adc;
  uint8_t _channel;
  int _pinSense, _pinSolar;
  St _st = St::IDLE;
  uint32_t _t = 0, _ticket = 0, _lastStart = 0, _lastGood = 0;
  bool _requested = false, _everStarted = false, _hasGood = false;
};
