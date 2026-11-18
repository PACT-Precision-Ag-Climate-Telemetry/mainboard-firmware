#include "modem.h"
#include "../config.h"
#include "../settings.h"
#include "../timeutil.h"

static const size_t MAX_RESPONSE = 512;
static const uint8_t INIT_STAGE_MODEL = 2;  // index of AT+CGMM in initCmd()

void Modem::begin() {
  pinMode(_pinPwr, OUTPUT);
  digitalWrite(_pinPwr, LOW);  // modem off (the board's pull-down holds it off before this)
  pinMode(_pinEn, INPUT);      // released until the power-key pulse
  _state = State::OFF;
}

const char *Modem::stateName() const {
  switch (_state) {
    case State::OFF: return "off";
    case State::POWER_SETTLE: return "power-settle";
    case State::KEY_PULSE: return "key-pulse";
    case State::BOOT_WAIT: return "booting";
    case State::INIT: return "init";
    case State::READY: return _job == Job::NONE ? "ready" : "working";
    case State::SHUTDOWN: return "shutting-down";
  }
  return "?";
}

bool Modem::requestRaw(const String &cmd) {
  if (_rawPending || _job == Job::RAW) return false;
  _rawCmd = cmd;
  _rawPending = true;
  return true;
}

// ---- command engine (non-blocking) -------------------------------------------
void Modem::startCmd(const String &cmd, uint32_t timeoutMs) {
  while (_port.available()) _port.read();  // drop stale bytes
  _port.print(cmd);
  _port.print("\r\n");
  _resp = "";
  _cmdT0 = millis();
  _cmdTimeout = timeoutMs;
  _cmdActive = true;
}

int Modem::cmdPoll() {
  if (!_cmdActive) return -1;
  while (_port.available() && _resp.length() < MAX_RESPONSE) _resp += (char)_port.read();

  if (_resp.indexOf("\r\nOK\r\n") >= 0 || _resp.startsWith("OK\r\n")) {
    _cmdActive = false;
    return 1;
  }
  if (_resp.indexOf("ERROR") >= 0) {  // also matches "+CME ERROR"
    _cmdActive = false;
    return -1;
  }
  if (millis() - _cmdT0 >= _cmdTimeout) {
    _cmdActive = false;
    return -1;
  }
  return 0;
}

String Modem::initCmd(uint8_t stage) const {
  switch (stage) {
    case 0: return "ATE0";             // no command echo
    case 1: return "AT+CSCLK=0";       // no DTR-gated sleep (DTR is not wired)
    case 2: return "AT+CGMM";          // model
    case 3: return "AT+CTZU=1";        // network time update
    case 4: return "AT+COPS=3,2";      // operator reported as numeric MCC+MNC
    case 5: {
      const String &apn = Settings::get().apn;
      if (apn.length() == 0) return "";
      return "AT+CGDCONT=1,\"IP\",\"" + apn + "\"";
    }
    default: return "";
  }
}

void Modem::cutPower() {
  _port.end();
  pinMode(_pinEn, INPUT);  // don't drive a pin into an unpowered modem
  digitalWrite(_pinPwr, LOW);
  _cmdActive = false;
  _job = Job::NONE;
  _shutdownStarted = false;
}

void Modem::failStart(const char *why, Event &ev) {
  lastError = why;
  cutPower();
  _state = State::OFF;
  _sessionRequested = false;
  if (_rawPending) {
    _rawPending = false;
    if (_rawSink) _rawSink->printf("\r\n[at] ERR modem did not start (%s)\r\n", why);
  }
  ev = Event::SESSION_FAILED;
}

// ---- main state machine -----------------------------------------------------
Modem::Event Modem::service() {
  Event ev = Event::NONE;
  uint32_t now = millis();

  switch (_state) {
    case State::OFF:
      if (_sessionRequested || _rawPending) {
        _port.end();
        _port.begin(_baud, SERIAL_8N1, /*rx*/ _pinTxd, /*tx*/ _pinRxd);
        digitalWrite(_pinPwr, HIGH);  // MOSFET on: modem gets its 5 V rail
        _t0 = now;
        _state = State::POWER_SETTLE;
      }
      break;

    case State::POWER_SETTLE:
      if (now - _t0 >= cfg::MODEM_RAIL_SETTLE_MS) {
        pinMode(_pinEn, OUTPUT);
        digitalWrite(_pinEn, LOW);  // power-key pulse starts
        _t0 = now;
        _state = State::KEY_PULSE;
      }
      break;

    case State::KEY_PULSE:
      if (now - _t0 >= cfg::MODEM_EN_PULSE_MS) {
        digitalWrite(_pinEn, HIGH);
        _bootT0 = now;
        _lastTry = 0;
        _cmdActive = false;
        _state = State::BOOT_WAIT;
      }
      break;

    case State::BOOT_WAIT:
      if (now - _bootT0 >= cfg::MODEM_BOOT_TIMEOUT_MS) {
        failStart("no answer to AT", ev);
        break;
      }
      if (_cmdActive) {
        int r = cmdPoll();
        if (r == 1) {
          _initStage = 0;
          _state = State::INIT;
        } else if (r == -1) {
          _lastTry = now;
        }
      } else if (_lastTry == 0 || now - _lastTry >= cfg::MODEM_AT_RETRY_MS) {
        startCmd("AT", 800);
      }
      break;

    case State::INIT:
      if (!_cmdActive) {
        String c = initCmd(_initStage);
        if (c.length() == 0) {  // script finished
          _state = State::READY;
          _job = Job::NONE;
          _lastActivity = now;
          lastError = "";
        } else {
          startCmd(c, 2500);
        }
      } else {
        int r = cmdPoll();
        if (r != 0) {
          if (_initStage == INIT_STAGE_MODEL && r == 1) {
            String m = _resp;
            m.replace("AT+CGMM", "");
            m.replace("OK", "");
            m.trim();
            model = m;
          }
          _initStage++;  // optional commands: carry on even if one failed
        }
      }
      break;

    case State::READY:
      runReady(now, ev);
      break;

    case State::SHUTDOWN:
      if (!_shutdownStarted) {
        startCmd("AT+CPOF", cfg::MODEM_SHUTDOWN_WAIT_MS);  // graceful power-off
        _shutdownStarted = true;
      } else if (cmdPoll() != 0) {  // answered, failed or timed out: cut power anyway
        cutPower();
        _state = State::OFF;
      }
      break;
  }
  return ev;
}

void Modem::runReady(uint32_t now, Event &ev) {
  if (_job == Job::NONE) {
    if (_rawPending) {
      _rawPending = false;
      _job = Job::RAW;
      startCmd(_rawCmd, cfg::MODEM_RAW_TIMEOUT_MS);
    } else if (_sessionRequested) {
      _sessionRequested = false;
      _job = Job::PULL;
      _stage = Stage::CPIN;
      _jobT0 = now;
      _cmdActive = false;
    } else if (now - _lastActivity >= cfg::MODEM_LINGER_MS) {
      _shutdownStarted = false;
      _state = State::SHUTDOWN;
    }
    return;
  }

  if (_job == Job::RAW) {
    if (cmdPoll() != 0) {
      if (_rawSink) {
        String r = _resp;
        r.trim();
        _rawSink->printf("\r\n[at] %s\r\n", r.length() ? r.c_str() : "(no reply)");
      }
      _job = Job::NONE;
      _lastActivity = now;
    }
    return;
  }

  stepPull(now, ev);
}

// ---- PULL session -----------------------------------------------------------
void Modem::stepPull(uint32_t now, Event &ev) {
  if (_stage == Stage::REG_WAIT) {  // waiting between registration checks
    if (now - _stageT >= cfg::MODEM_REGISTER_POLL_MS) _stage = Stage::CREG;
    return;
  }
  if (_stage == Stage::DONE) {
    _job = Job::NONE;
    _lastActivity = now;
    sessions++;
    ev = Event::SESSION_DONE;
    return;
  }

  if (!_cmdActive) {
    switch (_stage) {
      case Stage::CPIN: startCmd("AT+CPIN?", 3000); break;
      case Stage::CREG: startCmd("AT+CREG?", 2000); break;
      case Stage::CEREG: startCmd("AT+CEREG?", 2000); break;
      case Stage::CSQ: startCmd("AT+CSQ", 2000); break;
      case Stage::CFUN: startCmd("AT+CFUN?", 2000); break;
      case Stage::COPS: startCmd("AT+COPS?", 5000); break;
      case Stage::CCLK: startCmd("AT+CCLK?", 2000); break;
      default: break;
    }
    return;
  }

  int r = cmdPoll();
  if (r == 0) return;
  if (r == 1) parsePull(_stage);

  switch (_stage) {
    case Stage::CPIN:
      // No point waiting for the network without a SIM.
      _stage = simReady ? Stage::CREG : Stage::CSQ;
      break;
    case Stage::CREG: _stage = Stage::CEREG; break;
    case Stage::CEREG:
      if (registered() || now - _jobT0 >= cfg::MODEM_REGISTER_TIMEOUT_MS) {
        _stage = Stage::CSQ;
      } else {
        _stageT = now;
        _stage = Stage::REG_WAIT;  // check again in MODEM_REGISTER_POLL_MS
      }
      break;
    case Stage::CSQ: _stage = Stage::CFUN; break;
    case Stage::CFUN: _stage = Stage::COPS; break;
    case Stage::COPS: _stage = Stage::CCLK; break;
    case Stage::CCLK: _stage = Stage::DONE; break;
    default: _stage = Stage::DONE; break;
  }
}

// Parses "+CREG: <n>,<stat>" / "+CEREG: <n>,<stat>"; returns stat or -1.
int Modem::parseStat(const String &resp, const char *tag) {
  int i = resp.indexOf(tag);
  if (i < 0) return -1;
  int comma = resp.indexOf(',', i);
  if (comma < 0) return -1;
  return resp.substring(comma + 1).toInt();
}

void Modem::parsePull(Stage stage) {
  switch (stage) {
    case Stage::CPIN:
      simReady = _resp.indexOf("READY") >= 0;
      break;
    case Stage::CREG:
      creg = parseStat(_resp, "+CREG:");
      break;
    case Stage::CEREG:
      cereg = parseStat(_resp, "+CEREG:");
      break;
    case Stage::CSQ: {
      int i = _resp.indexOf("+CSQ:");
      if (i >= 0) csq = _resp.substring(i + 5).toInt();
      break;
    }
    case Stage::CFUN: {
      int i = _resp.indexOf("+CFUN:");
      if (i >= 0) cfun = _resp.substring(i + 6).toInt();
      break;
    }
    case Stage::COPS: {
      // "+COPS: 0,2,"52003",7"  -> numeric PLMN = MCC (3 digits) + MNC (2-3 digits)
      int i = _resp.indexOf("+COPS:");
      if (i < 0) break;
      int e = _resp.indexOf('\r', i);
      cops = _resp.substring(i + 6, e < 0 ? _resp.length() : e);
      cops.trim();
      int q1 = _resp.indexOf('"', i);
      int q2 = (q1 < 0) ? -1 : _resp.indexOf('"', q1 + 1);
      if (q1 < 0 || q2 < 0) break;  // no operator (not registered): keep the last cell
      String plmn = _resp.substring(q1 + 1, q2);
      bool digits = plmn.length() == 5 || plmn.length() == 6;
      for (size_t k = 0; digits && k < plmn.length(); k++)
        if (plmn[k] < '0' || plmn[k] > '9') digits = false;
      if (digits) {
        mcc = (uint16_t)plmn.substring(0, 3).toInt();
        mnc = (uint16_t)plmn.substring(3).toInt();
        hasCell = true;
      }
      break;
    }
    case Stage::CCLK: {
      // +CCLK: "24/07/13,10:15:30+32"  (local time + offset in quarter hours)
      int a = _resp.indexOf("+CCLK: \"");
      if (a < 0) break;
      a += 8;
      int b = _resp.indexOf('"', a);
      if (b < 0) break;
      String ts = _resp.substring(a, b);
      int yy, mo, dd, hh, mi, ss, tzq;
      char sign;
      if (sscanf(ts.c_str(), "%d/%d/%d,%d:%d:%d%c%d", &yy, &mo, &dd, &hh, &mi, &ss, &sign,
                 &tzq) != 8)
        break;
      if (sign == '-') tzq = -tzq;
      // An unsynced module reports an epoch placeholder (80 or 70): reject.
      if (yy < 24 || yy > 50) break;
      int64_t local = (int64_t)epochFromUtc(2000 + yy, mo, dd, hh, mi, ss);
      networkEpoch = (uint64_t)(local - (int64_t)tzq * 15 * 60);  // local -> UTC
      networkTimeAtMs = millis();
      networkTimeValid = true;
      break;
    }
    default: break;
  }
}

void Modem::print(Print &out) const {
  out.printf("[Modem %s] %s  SIM=%s  ", stateName(), model.length() ? model.c_str() : "?",
             simReady ? "ready" : "not ready");
  if (csq == 99) out.print("signal=unknown  ");
  else out.printf("CSQ=%d (%d dBm)  ", csq, -113 + 2 * csq);
  out.printf("CREG=%d CEREG=%d CFUN=%d COPS=%s  ", creg, cereg, cfun, cops.c_str());
  if (hasCell) out.printf("cell MCC=%u MNC=%u  ", mcc, mnc);
  else out.print("cell=unknown  ");
  out.printf("sessions=%lu\n", (unsigned long)sessions);
}
