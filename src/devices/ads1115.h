#pragma once
#include "../config.h"
#include "i2c_device.h"

// TI ADS1115 16-bit ADC, address 0x48 (ADDR pin to GND).
// Single-ended inputs, +/-4.096 V range (3.3 V supply).
//
// Non-blocking. Conversions are queued and run one at a time:
//   * update() queues all four channels (periodic scan, AIN0..AIN3 in volts)
//   * other code can queue one channel with queue(ch) and later ask
//     done(ch, ticket). The ticket guarantees the conversion STARTED after
//     queue() was called (so a switch raised before queue() has had its effect).
class ADS1115 : public I2CDevice {
 public:
  ADS1115() : I2CDevice("ADS1115", cfg::ADDR_ADS1115, cfg::PERIOD_ADS1115_MS) {}
  void print(Print &out) const override;

  // Queue one fresh conversion of `channel` (0-3). Returns a ticket for done().
  uint32_t queue(uint8_t channel);
  // True once a conversion that started after queue() returned `ticket` finished;
  // raw[channel] / volts[channel] then hold that result.
  bool done(uint8_t channel, uint32_t ticket) const {
    return channel < 4 && (int32_t)(_doneCnt[channel] - ticket) >= 0;
  }

  int16_t raw[4] = {0, 0, 0, 0};
  float volts[4] = {NAN, NAN, NAN, NAN};

 protected:
  bool init() override;
  bool update() override;
  bool service() override;

 private:
  bool startConversion(uint8_t channel);

  bool _busy = false;
  uint8_t _ch = 0;
  uint32_t _t0 = 0;
  uint8_t _queued = 0;       // channels waiting to start (bit per channel)
  uint8_t _scanPending = 0;  // channels of the periodic scan still to finish
  uint32_t _startCnt[4] = {0, 0, 0, 0};
  uint32_t _doneCnt[4] = {0, 0, 0, 0};
};
