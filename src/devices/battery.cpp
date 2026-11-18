#include "battery.h"
#include "../config.h"
#include "../settings.h"

void Battery::begin() {
  pinMode(_pinSense, OUTPUT);
  pinMode(_pinSolar, OUTPUT);
  release();
}

void Battery::release() {
  digitalWrite(_pinSense, LOW);  // sense off
  digitalWrite(_pinSolar, LOW);  // solar panel connected
}

bool Battery::poll() {
  if (!_adc.ok()) return false;
  uint32_t now = millis();

  switch (_st) {
    case St::IDLE:
      if (_requested || !_everStarted || now - _lastStart >= cfg::PERIOD_BATTERY_MS) {
        _requested = false;
        _everStarted = true;
        _lastStart = now;
        digitalWrite(_pinSolar, HIGH);  // disconnect the solar panel
        digitalWrite(_pinSense, HIGH);  // connect the divider to the ADC
        _t = now;
        _st = St::SETTLING;
      }
      return false;

    case St::SETTLING:
      if (now - _t >= cfg::BATTERY_SETTLE_MS) {
        _ticket = _adc.queue(_channel);  // conversion that starts from now on
        _t = now;
        _st = St::CONVERTING;
      }
      return false;

    case St::CONVERTING:
      if (_adc.done(_channel, _ticket)) {
        release();
        _st = St::IDLE;
        return finish();
      }
      if (now - _t > cfg::BATTERY_ADC_TIMEOUT_MS) {  // always restore the pins
        release();
        _st = St::IDLE;
      }
      return false;
  }
  return false;
}

bool Battery::finish() {
  const Settings::Values &s = Settings::get();
  float v = _adc.volts[_channel];
  float vbat = v * ((s.batteryDividerR1 + s.batteryDividerR2) / s.batteryDividerR2);
  float mv = vbat * 1000.0f;
  adcVolts = v;
  if (mv < cfg::BATTERY_PLAUSIBLE_MIN_MV || mv > cfg::BATTERY_PLAUSIBLE_MAX_MV) return false;

  volts = vbat;
  millivolts = (uint16_t)lroundf(mv);
  float soc = ((vbat + cfg::BATTERY_LOAD_COMP_V) - cfg::BATTERY_V_EMPTY) /
              (cfg::BATTERY_V_FULL - cfg::BATTERY_V_EMPTY) * 100.0f;
  percent = constrain(soc, 0.0f, 100.0f);
  _lastGood = millis();
  _hasGood = true;
  return true;
}

void Battery::print(Print &out) const {
  out.printf("[Battery] %.2f V (%u mV, ADC pin %.4f V)  SOC %.0f %%\n", volts,
             millivolts, adcVolts, percent);
}
