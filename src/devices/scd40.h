#pragma once
#include "../config.h"
#include "i2c_device.h"

// Sensirion SCD40 CO2 / temperature / humidity, address 0x62.
// Choose the chip with cfg::CO2_SENSOR_IS_SCD41. The SCD41 uses its native
// single-shot command. The SCD40 has none, so single-shot is emulated: periodic measurement is started when a measurement window opens, the
// first sample (about 5 s later) is read, and periodic mode is stopped again.
// Non-blocking: every command that needs a wait is finished in service() on a
// later loop pass.
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
  void restart() override;

 public:
  void windowStart() override { _wantRun = true; }
  void windowEnd() override { _wantRun = false; }
  bool busy() const override { return _running || _st != St::IDLE; }
  uint32_t attemptTimeoutMs() const override { return 8000; }  // first sample after start ~5 s

 private:
  enum class St : uint8_t { STOPPING, ASC_SET, IDLE, WAIT_READY, WAIT_DATA, STOP_WAIT };
  St _st = St::IDLE;
  uint32_t _t = 0;
  bool _wantRun = false;  // a measurement window is open
  bool _running = false;  // periodic measurement is running
};
