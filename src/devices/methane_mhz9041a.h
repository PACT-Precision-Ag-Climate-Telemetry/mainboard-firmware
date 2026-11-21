#pragma once
#include <DFRobot_MHZ9041A.h>  // DFRobot SEN0654 library (I2C mode, 0x34)
#include "../config.h"
#include "i2c_device.h"

// DFRobot MH-Z9041A laser methane sensor (SEN0654), address 0x34.
// The sensor's dip switch must be on the I2C side. The library has no sleep or
// single-shot command: the sensor measures on its own and passive mode only reads
// the latest value on request, which is what the measurement window does.
class MethaneMHZ9041A : public I2CDevice {
 public:
  MethaneMHZ9041A()
      : I2CDevice("MH-Z9041A", cfg::ADDR_MHZ9041A, cfg::PERIOD_MHZ9041A_MS),
        _ch4(&Wire, cfg::ADDR_MHZ9041A) {}
  void print(Print &out) const override;

  float ch4PercentLEL = NAN;  // %LEL (percent of lower explosive limit)
  float temperatureC = NAN;
  int errorCode = 0;

 protected:
  bool init() override;
  bool update() override;

 private:
  DFRobot_MHZ9041A_I2C _ch4;
};
