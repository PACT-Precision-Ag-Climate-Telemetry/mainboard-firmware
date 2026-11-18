#include "methane_mhz9041a.h"

bool MethaneMHZ9041A::init() {
  if (!_ch4.begin()) return false;
  _ch4.setMode(ePassivityMode);  // we poll; sensor does not push data
  return true;
}

bool MethaneMHZ9041A::update() {
  ch4PercentLEL = _ch4.getCH4Concentration();
  temperatureC = _ch4.getTemperature();
  errorCode = (int)_ch4.getErrorMsg();
  return true;
}

void MethaneMHZ9041A::print(Print &out) const {
  out.printf("[%s 0x%02X] CH4=%.2f %%LEL  T=%.2f C  err=%d\n", _name, _addr,
             ch4PercentLEL, temperatureC, errorCode);
}
