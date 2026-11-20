#pragma once
#include <Arduino.h>

// SIMCom A7670E cellular modem on UART2 (AT commands), connector J7.
//
// The modem is OFF by default and only woken when something needs it. Nothing
// here blocks: service() is called from loop() and advances a state machine.
//
//   OFF -> POWER_SETTLE -> KEY_PULSE -> BOOT_WAIT -> INIT -> READY -> SHUTDOWN -> OFF
//
//   POWER_SETTLE  MODEM_PWR (GPIO5) HIGH: MOSFET connects the 5 V rail, wait 200 ms
//   KEY_PULSE     MODEM_EN (GPIO4) LOW for 1 s then HIGH (power key, from the old firmware)
//   BOOT_WAIT     send "AT" every 300 ms until "OK" (give up after 20 s)
//   INIT          ATE0, AT+CSCLK=0, AT+CGMM, AT+CTZU=1, AT+COPS=3,2, APN
//   READY         runs jobs; after MODEM_LINGER_MS with nothing to do -> SHUTDOWN
//   SHUTDOWN      AT+CPOF, then MODEM_PWR LOW and MODEM_EN released
//
// Jobs (queue one with request() / requestRaw()):
//   PULL session  SIM state, network registration (waits up to 60 s), signal
//                 (CSQ), functionality (CFUN), operator -> MCC/MNC for the
//                 telemetry cell block, and network time (AT+CCLK?)
//   SEND job      TCP upload of one telemetry frame (docs/telemetry.md):
//                 NETOPEN, CIPOPEN, CIPSEND, wait for the 1-byte ack 0x00,
//                 CIPCLOSE, NETCLOSE. Queue with requestSend() after a PULL
//                 session (the modem lingers MODEM_LINGER_MS for follow-ups).
//   RAW command   one AT command typed in the field configurator
//
// UART: 115200 8N1. Net names are from the MODEM's side, so the ESP transmits
// on MODEM_RXD (GPIO17) and receives on MODEM_TXD (GPIO18).
class Modem {
 public:
  enum class State : uint8_t { OFF, POWER_SETTLE, KEY_PULSE, BOOT_WAIT, INIT, READY, SHUTDOWN };
  enum class Event : uint8_t { NONE, SESSION_DONE, SESSION_FAILED, SEND_DONE, SEND_FAILED };
  static constexpr size_t MAX_SEND_BYTES = 768;  // >= largest telemetry frame

  Modem(HardwareSerial &port, int pinPwr, int pinEn, int pinModemRxd,
        int pinModemTxd, uint32_t baud = 115200)
      : _port(port), _pinPwr(pinPwr), _pinEn(pinEn), _pinRxd(pinModemRxd),
        _pinTxd(pinModemTxd), _baud(baud) {}

  void begin();                         // MODEM_PWR LOW (modem off), EN released

  void request() { _sessionRequested = true; }   // wake and run a PULL session
  // Queue one AT command; its reply is printed to the raw sink when it
  // finishes. False if a raw command is already pending.
  bool requestRaw(const String &cmd);
  void setRawSink(Print *sink) { _rawSink = sink; }

  // Queue one frame for TCP upload to host:port. The bytes are copied. False if
  // a send is already queued/running or the frame is empty / too large. The
  // result arrives as Event::SEND_DONE (server acked 0x00) or SEND_FAILED
  // (reason in lastError). If the modem was off it is woken first.
  bool requestSend(const uint8_t *data, size_t len, const String &host, uint16_t port);

  // Call every loop(). Returns SESSION_DONE when a PULL session just finished
  // (fresh results below), SESSION_FAILED if the modem could not be started.
  Event service();

  State state() const { return _state; }
  const char *stateName() const;
  bool busy() const { return _state != State::OFF || _sessionRequested || _rawPending || _sendRequested; }
  bool registered() const { return creg == 1 || creg == 5 || cereg == 1 || cereg == 5; }
  void print(Print &out) const;

  // Results, kept in RAM between wakes.
  String model;
  int csq = 99;            // 0-31 valid, 99 = unknown
  bool simReady = false;
  int creg = -1;           // 2G/3G registration (0-5), -1 unknown; 1 or 5 = registered
  int cereg = -1;          // LTE registration
  int cfun = -1;           // functionality level (1 = full)
  String cops;             // raw "+COPS:" reply
  bool hasCell = false;    // mcc / mnc valid (learned from the registered network)
  uint16_t mcc = 0;
  uint16_t mnc = 0;
  bool networkTimeValid = false;
  uint64_t networkEpoch = 0;       // Unix seconds, from AT+CCLK? (UTC)
  uint32_t networkTimeAtMs = 0;    // millis() when networkEpoch was read
  String lastError;
  uint32_t sessions = 0;           // completed PULL sessions
  uint32_t sendsOk = 0;            // frames acked by the server

 private:
  enum class Job : uint8_t { NONE, PULL, RAW, SEND };
  enum class SendStage : uint8_t { NETOPEN, IPOPEN, IPSEND, DATA, ACK, CIPCLOSE, NETCLOSE };
  enum class Stage : uint8_t { CPIN, CREG, CEREG, REG_WAIT, CSQ, CFUN, COPS, CCLK, DONE };

  // Send `cmd` and return immediately. cmdPoll() then reports success on "OK",
  // or, if `expect` is set, once `expect` plus `extra` more bytes have arrived.
  void startCmd(const String &cmd, uint32_t timeoutMs, const char *expect = "", size_t extra = 0);
  void startData(const uint8_t *d, size_t n, uint32_t timeoutMs, const char *expect);
  int cmdPoll();           // 0 = pending, 1 = OK, -1 = ERROR / timeout / none running
  String initCmd(uint8_t stage) const;
  void runReady(uint32_t now, Event &ev);
  void stepPull(uint32_t now, Event &ev);
  void stepSend(uint32_t now, Event &ev);
  void sendFailed(const char *why);
  void parsePull(Stage stage);
  void cutPower();
  void failStart(const char *why, Event &ev);
  static int parseStat(const String &resp, const char *tag);

  HardwareSerial &_port;
  int _pinPwr, _pinEn, _pinRxd, _pinTxd;
  uint32_t _baud;

  State _state = State::OFF;
  Job _job = Job::NONE;
  Stage _stage = Stage::CPIN;
  bool _sessionRequested = false;
  bool _rawPending = false;
  bool _sendRequested = false;
  SendStage _sendStage = SendStage::NETOPEN;
  bool _sendOk = false;
  uint8_t _txBuf[MAX_SEND_BYTES];
  size_t _txLen = 0;
  String _sendHost;
  uint16_t _sendPortNo = 0;
  String _rawCmd;
  Print *_rawSink = nullptr;

  // command engine
  String _resp;
  String _expect;
  size_t _expectExtra = 0;
  bool _cmdActive = false;
  uint32_t _cmdT0 = 0, _cmdTimeout = 0;

  uint32_t _t0 = 0, _bootT0 = 0, _lastTry = 0, _lastActivity = 0, _jobT0 = 0, _stageT = 0;
  uint8_t _initStage = 0;
  bool _shutdownStarted = false;
};
