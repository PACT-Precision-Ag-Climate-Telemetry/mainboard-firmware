#include "gnss.h"
#include "../config.h"
#include "../timeutil.h"

static const uint8_t BAUD_COUNT = sizeof(cfg::GNSS_BAUDS) / sizeof(cfg::GNSS_BAUDS[0]);

void GNSSReceiver::startListen() {
  // Schematic net names are from the GNSS MODULE's side (confirmed by owner):
  // GNSS_RXD = module RX = ESP TX (GPIO15), GNSS_TXD = module TX = ESP RX (GPIO16).
  _espRx = _pinTxd;
  _espTx = _pinRxd;
  _port.end();
  _port.begin(cfg::GNSS_BAUDS[_idx], SERIAL_8N1, _espRx, _espTx);
  _probe = TinyGPSPlus();
  _t0 = millis();
}

void GNSSReceiver::begin() {
  _idx = 0;
  _status = Status::DETECTING;
  startListen();
}

bool GNSSReceiver::poll() {
  uint32_t now = millis();

  if (_status == Status::DETECTING) {
    while (_port.available()) _probe.encode(_port.read());
    if (_probe.passedChecksum() > 0) {  // valid NMEA: keep this baud rate
      _baud = cfg::GNSS_BAUDS[_idx];
      _status = Status::OK;
      _last = now;
    } else if (now - _t0 >= cfg::GNSS_DETECT_MS) {
      if (++_idx >= BAUD_COUNT) {
        _status = Status::FAILED;
        _port.end();
      } else {
        startListen();
      }
    }
    return false;
  }

  if (_status != Status::OK) return false;
  while (_port.available()) _gps.encode(_port.read());
  if (now - _last < cfg::GNSS_PRINT_MS) return false;
  _last = now;
  return true;
}

bool GNSSReceiver::utcEpoch(uint64_t &out) const {
  if (!_gps.date.isValid() || !_gps.time.isValid() || _gps.date.year() < 2024) return false;
  out = epochFromUtc(_gps.date.year(), _gps.date.month(), _gps.date.day(),
                     _gps.time.hour(), _gps.time.minute(), _gps.time.second());
  return true;
}

void GNSSReceiver::print(Print &out) const {
  if (!_gps.location.isValid()) {
    out.printf("[GNSS UART1 rx=GPIO%d tx=GPIO%d %lu baud] no fix  sats=%lu  "
               "sentences=%lu  checksum-fail=%lu\n",
               _espRx, _espTx, (unsigned long)_baud,
               (unsigned long)_gps.satellites.value(),
               (unsigned long)_gps.passedChecksum(),
               (unsigned long)_gps.failedChecksum());
    return;
  }
  out.printf("[GNSS] lat=%.6f lon=%.6f alt=%.1f m  sats=%lu  hdop=%.1f  "
             "speed=%.1f km/h  UTC %02d:%02d:%02d\n",
             _gps.location.lat(), _gps.location.lng(), _gps.altitude.meters(),
             (unsigned long)_gps.satellites.value(), _gps.hdop.hdop(),
             _gps.speed.kmph(), _gps.time.hour(), _gps.time.minute(),
             _gps.time.second());
}
