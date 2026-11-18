#include "vl53l1x_tof.h"

bool VL53L1XTof::init() {
  _sensor.setBus(&Wire);
  _sensor.setTimeout(500);
  if (!_sensor.init()) return false;
  _sensor.setDistanceMode(VL53L1X::Long);
  _sensor.setMeasurementTimingBudget(50000);  // 50 ms
  _sensor.startContinuous(50);
  return true;
}

bool VL53L1XTof::update() {
  if (!_sensor.dataReady()) return false;
  distanceMm = _sensor.read(false);
  rangeValid = (_sensor.ranging_data.range_status == VL53L1X::RangeValid);
  return true;
}

void VL53L1XTof::print(Print &out) const {
  out.printf("[%s 0x%02X] distance=%u mm  (%s)\n", _name, _addr, distanceMm,
             rangeValid ? "valid" : "range status not valid");
}
