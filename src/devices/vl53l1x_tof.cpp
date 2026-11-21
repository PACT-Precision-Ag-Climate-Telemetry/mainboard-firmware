#include "vl53l1x_tof.h"

bool VL53L1XTof::init() {
  _sensor.setBus(&Wire);
  _sensor.setTimeout(500);
  if (!_sensor.init()) return false;
  // Narrowest field of view (4x4 SPADs, the minimum) aimed at the array centre
  // (SPAD 199), so only the spot straight ahead is measured.
  _sensor.setROISize(4, 4);
  _sensor.setROICenter(199);
  _sensor.setDistanceMode(VL53L1X::Long);
  _sensor.setMeasurementTimingBudget(50000);  // 50 ms
  // Single-shot: nothing runs until update() starts one measurement.
  _sensor.stopContinuous();
  return true;
}

bool VL53L1XTof::update() {
  if (!_measuring) {
    _sensor.readSingle(false);  // start one ranging, do not wait
    _measuring = true;
    return false;
  }
  if (!_sensor.dataReady()) return false;  // asked again on the next period
  _measuring = false;
  distanceMm = _sensor.read(false);
  rangeValid = (_sensor.ranging_data.range_status == VL53L1X::RangeValid);
  return true;
}

void VL53L1XTof::print(Print &out) const {
  out.printf("[%s 0x%02X] distance=%u mm  (%s)\n", _name, _addr, distanceMm,
             rangeValid ? "valid" : "range status not valid");
}
