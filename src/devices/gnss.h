#pragma once
#include <Arduino.h>
#include <TinyGPSPlus.h>

// GNSS receiver on UART1 (NMEA output), socket J8.
// Schematic: GNSS_RXD net = GPIO15, GNSS_TXD net = GPIO16, named from the
// GNSS module's side: the ESP transmits on GPIO15 and receives on GPIO16.
//
// Non-blocking. begin() starts a baud-rate search; poll() steps through the
// candidate rates (cfg::GNSS_BAUDS), listening GNSS_DETECT_MS on each, until a
// checksum-valid NMEA sentence is heard. status() reports DETECTING / OK /
// FAILED meanwhile.
class GNSSReceiver {
 public:
  enum class Status : uint8_t { DETECTING, OK, FAILED };

  GNSSReceiver(HardwareSerial &port, int pinGnssRxd, int pinGnssTxd)
      : _port(port), _pinRxd(pinGnssRxd), _pinTxd(pinGnssTxd) {}

  void begin();                  // start detecting (returns immediately)
  bool poll();                   // feed the parser; true about once per second
  void print(Print &out) const;

  Status status() const { return _status; }
  bool ok() const { return _status == Status::OK; }
  bool hasFix() const { return _gps.location.isValid(); }
  double lat() const { return _gps.location.lat(); }
  double lon() const { return _gps.location.lng(); }
  double altitudeM() const { return _gps.altitude.meters(); }
  uint32_t satellites() const { return _gps.satellites.value(); }
  uint32_t baud() const { return _baud; }

  // Current UTC time from the receiver as Unix seconds. False until the
  // receiver has a valid date and time (can happen before a position fix).
  bool utcEpoch(uint64_t &out) const;

 private:
  void startListen();

  HardwareSerial &_port;
  int _pinRxd, _pinTxd;  // schematic net names GNSS_RXD / GNSS_TXD
  mutable TinyGPSPlus _gps;  // library getters are non-const
  TinyGPSPlus _probe;        // parser used only during baud detection
  Status _status = Status::FAILED;
  uint8_t _idx = 0;
  uint32_t _t0 = 0;
  uint32_t _last = 0;
  uint32_t _baud = 0;
  int _espRx = -1, _espTx = -1;
};
