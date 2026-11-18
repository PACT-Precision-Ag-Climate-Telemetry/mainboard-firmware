#pragma once
#include "../config.h"
#include "i2c_device.h"

// Sensirion SHT30 temperature / humidity, address 0x44.
// Non-blocking: update() sends the measure command, service() reads the
// result 16 ms later.
class SHT30 : public I2CDevice {
 public:
  SHT30() : I2CDevice("SHT30", cfg::ADDR_SHT30, cfg::PERIOD_SHT30_MS) {}
  void print(Print &out) const override;

  float temperatureC = NAN;
  float humidityRH = NAN;

 protected:
  bool init() override;
  bool update() override;
  bool service() override;

 private:
  enum class St : uint8_t { RESETTING, IDLE, MEASURING };
  St _st = St::IDLE;
  uint32_t _t = 0;
};
