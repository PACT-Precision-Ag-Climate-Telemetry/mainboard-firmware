#pragma once
#include <VL53L1X.h>  // Pololu library
#include "../config.h"
#include "i2c_device.h"

// ST VL53L1X time-of-flight distance sensor, address 0x29.
class VL53L1XTof : public I2CDevice {
 public:
  VL53L1XTof() : I2CDevice("VL53L1X", cfg::ADDR_VL53L1X, cfg::PERIOD_VL53L1X_MS) {}
  void print(Print &out) const override;

  uint16_t distanceMm = 0;
  bool rangeValid = false;

 protected:
  bool init() override;
  bool update() override;

 private:
  VL53L1X _sensor;
};
