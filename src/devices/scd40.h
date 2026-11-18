#pragma once
#include "../config.h"
#include "i2c_device.h"

// Sensirion SCD40 CO2 / temperature / humidity, address 0x62.
// Runs in periodic mode (a new sample every ~5 s). Non-blocking: every
// command that needs a wait is finished in service() on a later loop pass.
class SCD40 : public I2CDevice {
 public:
  SCD40() : I2CDevice("SCD40", cfg::ADDR_SCD40, cfg::PERIOD_SCD40_MS) {}
  void print(Print &out) const override;

  uint16_t co2ppm = 0;
  float temperatureC = NAN;
  float humidityRH = NAN;

 protected:
  bool init() override;
  bool update() override;
  bool service() override;

 private:
  enum class St : uint8_t { STOPPING, IDLE, WAIT_READY, WAIT_DATA };
  St _st = St::IDLE;
  uint32_t _t = 0;
};
