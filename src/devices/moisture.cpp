#include "moisture.h"
#include "../config.h"
#include "../settings.h"

bool Moisture::poll() {
  if (!_adc.ok()) return false;
  uint32_t now = millis();

  if (!_waiting) {
    if (_requested || !_everStarted || now - _lastStart >= cfg::PERIOD_MOISTURE_MS) {
      _requested = false;
      _everStarted = true;
      _lastStart = now;
      _ticket = _adc.queue(_channel);
      _t = now;
      _waiting = true;
    }
    return false;
  }

  if (_adc.done(_channel, _ticket)) {
    _waiting = false;
    const Settings::Values &s = Settings::get();
    int16_t r = _adc.raw[_channel];
    volts = _adc.volts[_channel];
    raw = r > 0 ? (uint16_t)r : 0;
    percent = constrain(s.moistureSlope * volts + s.moistureIntercept, 0.0f, 100.0f);
    _lastGood = millis();
    _hasGood = true;
    return true;
  }
  if (now - _t > cfg::BATTERY_ADC_TIMEOUT_MS) _waiting = false;  // retry next period
  return false;
}

void Moisture::print(Print &out) const {
  out.printf("[Moisture] %.1f %%  (%.4f V, raw %u)\n", percent, volts, raw);
}
