#include "settings.h"
#include <IPAddress.h>
#include <Preferences.h>
#include "config.h"

namespace Settings {

namespace {

// Same namespace and keys as the old firmware. NVS keys max 15 characters.
constexpr const char *NS = "device_settings";
constexpr const char *K_SERVER_IP = "server_ip";
constexpr const char *K_SERVER_PORT = "server_port";
constexpr const char *K_MAGIC1 = "magic_byte_1";
constexpr const char *K_MAGIC2 = "magic_byte_2";
constexpr const char *K_MOIST_SLOPE = "moisture_slope";
constexpr const char *K_MOIST_INT = "moist_int";
constexpr const char *K_BATT_R1 = "battery_div_r1";
constexpr const char *K_BATT_R2 = "battery_div_r2";
constexpr const char *K_OFFSET = "us_offset";  // old name kept for compatibility
constexpr const char *K_GPS_LAT = "gps_lat";
constexpr const char *K_GPS_LON = "gps_lon";
constexpr const char *K_RAW = "tele_raw";  // new
constexpr const char *K_APN = "modem_apn";  // new

Values g;
Preferences prefs;
bool ready = false;

// ---- parsing / storage helpers ---------------------------------------------
bool parseU(const String &s, unsigned long lo, unsigned long hi, unsigned long &out) {
  String t = s;
  t.trim();
  if (t.length() == 0) return false;
  char *end = nullptr;
  // Decimal, or hex with a 0x prefix (so "08080" is 8080, not a bad octal).
  bool hex = t.startsWith("0x") || t.startsWith("0X");
  unsigned long v = strtoul(t.c_str(), &end, hex ? 16 : 10);
  if (end == t.c_str() || *end != '\0' || v < lo || v > hi) return false;
  out = v;
  return true;
}

bool parseF(const String &s, float lo, float hi, float &out) {
  String t = s;
  t.trim();
  if (t.length() == 0) return false;
  char *end = nullptr;
  float v = strtof(t.c_str(), &end);
  if (end == t.c_str() || *end != '\0' || isnan(v) || v < lo || v > hi) return false;
  out = v;
  return true;
}

bool saveU(const char *key, uint32_t v) { return ready && prefs.putUInt(key, v) == sizeof(uint32_t); }
bool saveF(const char *key, float v) {
  String s = String(v, 6);
  return ready && prefs.putString(key, s) == s.length();
}

// ---- server ip / port ------------------------------------------------------
String getServerIp() { return g.serverIp; }
bool setServerIp(const String &v) {
  String t = v;
  t.trim();
  if (!isValidServerHost(t)) return false;
  if (!ready || prefs.putString(K_SERVER_IP, t) != t.length()) return false;
  g.serverIp = t;
  return true;
}

String getServerPort() { return String(g.serverPort); }
bool setServerPort(const String &v) {
  unsigned long n;
  if (!parseU(v, 1, 65535, n) || !saveU(K_SERVER_PORT, n)) return false;
  g.serverPort = (uint16_t)n;
  return true;
}

// ---- telemetry header ------------------------------------------------------
String getMagic1() { return String(g.magicByte1); }
bool setMagic1(const String &v) {
  unsigned long n;
  if (!parseU(v, 0, 255, n) || !saveU(K_MAGIC1, n)) return false;
  g.magicByte1 = (uint8_t)n;
  return true;
}

String getMagic2() { return String(g.magicByte2); }
bool setMagic2(const String &v) {
  unsigned long n;
  if (!parseU(v, 0, 255, n) || !saveU(K_MAGIC2, n)) return false;
  g.magicByte2 = (uint8_t)n;
  return true;
}

String getRaw() { return String(g.telemetryRaw ? 1 : 0); }
bool setRaw(const String &v) {
  String t = v;
  t.trim();
  t.toLowerCase();
  bool b;
  if (t == "1" || t == "true" || t == "on") b = true;
  else if (t == "0" || t == "false" || t == "off") b = false;
  else return false;
  if (!saveU(K_RAW, b ? 1 : 0)) return false;
  g.telemetryRaw = b;
  return true;
}

// ---- float settings --------------------------------------------------------
#define FLOAT_SETTING(NAME, FIELD, KEY, LO, HI)                      \
  String get##NAME() { return String(g.FIELD, 6); }                  \
  bool set##NAME(const String &v) {                                  \
    float f;                                                         \
    if (!parseF(v, LO, HI, f) || !saveF(KEY, f)) return false;       \
    g.FIELD = f;                                                     \
    return true;                                                     \
  }

FLOAT_SETTING(Offset, offsetMm, K_OFFSET, 0.0f, 65535.0f)  // uint16 on the wire
FLOAT_SETTING(MoistSlope, moistureSlope, K_MOIST_SLOPE, -1e9f, 1e9f)
FLOAT_SETTING(MoistIntercept, moistureIntercept, K_MOIST_INT, -1e9f, 1e9f)
FLOAT_SETTING(BattR1, batteryDividerR1, K_BATT_R1, 1.0f, 1e9f)
FLOAT_SETTING(BattR2, batteryDividerR2, K_BATT_R2, 1.0f, 1e9f)
FLOAT_SETTING(GpsLat, gpsLatitude, K_GPS_LAT, -90.0f, 90.0f)
FLOAT_SETTING(GpsLon, gpsLongitude, K_GPS_LON, -180.0f, 180.0f)

// ---- modem apn -------------------------------------------------------------
String getApn() { return g.apn; }
bool setApn(const String &v) {
  String t = v;
  t.trim();
  if (t.length() > 63 || t.indexOf('"') >= 0 || t.indexOf(' ') >= 0) return false;
  if (!ready) return false;
  if (t.length() == 0) prefs.remove(K_APN);
  else if (prefs.putString(K_APN, t) != t.length()) return false;
  g.apn = t;
  return true;
}

// ---- defaults, as the strings "set" would take ------------------------------
String defServerIp() { return cfg::DEFAULT_SERVER_IP; }
String defServerPort() { return String(cfg::DEFAULT_SERVER_PORT); }
String defMagic1() { return String(cfg::DEFAULT_MAGIC_BYTE_1); }
String defMagic2() { return String(cfg::DEFAULT_MAGIC_BYTE_2); }
String defRaw() { return String(cfg::DEFAULT_TELEMETRY_RAW ? 1 : 0); }
String defOffset() { return String(cfg::DEFAULT_OFFSET_MM, 6); }
String defSlope() { return String(cfg::DEFAULT_MOISTURE_SLOPE, 6); }
String defIntercept() { return String(cfg::DEFAULT_MOISTURE_INTERCEPT, 6); }
String defR1() { return String(cfg::DEFAULT_BATTERY_DIVIDER_R1, 6); }
String defR2() { return String(cfg::DEFAULT_BATTERY_DIVIDER_R2, 6); }
String defLat() { return String(cfg::DEFAULT_GPS_LATITUDE, 6); }
String defLon() { return String(cfg::DEFAULT_GPS_LONGITUDE, 6); }
String defApn() { return cfg::DEFAULT_APN; }

struct Entry {
  Binding binding;
  String (*def)();
};

// Order = order of "list". ULTRASONIC_OFFSET is the old name of OFFSET.
const Entry kEntries[] = {
    {{"SERVER_IP", "Telemetry server IPv4 address or hostname", getServerIp, setServerIp}, defServerIp},
    {{"SERVER_PORT", "Telemetry server TCP port (1-65535)", getServerPort, setServerPort}, defServerPort},
    {{"TELEMETRY_MAGIC_BYTE_1", "Frame magic byte 1, 0-255 (must match server)", getMagic1, setMagic1}, defMagic1},
    {{"TELEMETRY_MAGIC_BYTE_2", "Frame magic byte 2, 0-255 (must match server)", getMagic2, setMagic2}, defMagic2},
    {{"TELEMETRY_RAW", "1 = send methane_raw + moisture_raw in each record (RAW flag), 0 = omit", getRaw, setRaw}, defRaw},
    {{"OFFSET", "Telemetry 'offset' field in mm (0-65535), set per installation", getOffset, setOffset}, defOffset},
    {{"ULTRASONIC_OFFSET", "Old name of OFFSET (same value)", getOffset, setOffset}, defOffset},
    {{"MOISTURE_SLOPE", "Soil moisture percent = SLOPE * volts + INTERCEPT", getMoistSlope, setMoistSlope}, defSlope},
    {{"MOISTURE_INTERCEPT", "Soil moisture calibration intercept (percent)", getMoistIntercept, setMoistIntercept}, defIntercept},
    {{"BATTERY_DIVIDER_R1", "Battery sense divider, top resistor in ohms", getBattR1, setBattR1}, defR1},
    {{"BATTERY_DIVIDER_R2", "Battery sense divider, bottom resistor in ohms", getBattR2, setBattR2}, defR2},
    {{"GPS_LATITUDE", "Manual fallback latitude, -90..90 (0/0 = not set)", getGpsLat, setGpsLat}, defLat},
    {{"GPS_LONGITUDE", "Manual fallback longitude, -180..180 (0/0 = not set)", getGpsLon, setGpsLon}, defLon},
    {{"MODEM_APN", "Cellular APN, no spaces or quotes (empty = none)", getApn, setApn}, defApn},
};
constexpr size_t N = sizeof(kEntries) / sizeof(kEntries[0]);

Binding kBindings[N];  // plain Binding array handed to the console

}  // namespace

bool isValidServerHost(const String &value) {
  String t = value;
  t.trim();
  if (t.length() == 0 || t.length() > 253) return false;
  if (t.indexOf(' ') >= 0 || t.indexOf('/') >= 0 || t.indexOf('\\') >= 0) return false;

  IPAddress ip;
  if (ip.fromString(t)) return true;  // IPv4 literal

  if (t.startsWith(".") || t.endsWith(".")) return false;
  int labelStart = 0;
  for (int i = 0; i <= (int)t.length(); ++i) {
    if (i == (int)t.length() || t.charAt(i) == '.') {
      int len = i - labelStart;
      if (len == 0 || len > 63) return false;
      if (t.charAt(labelStart) == '-' || t.charAt(i - 1) == '-') return false;
      for (int j = labelStart; j < i; ++j) {
        char c = t.charAt(j);
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
      }
      labelStart = i + 1;
    }
  }
  return true;
}

static void loadDefaults() {
  g.serverIp = cfg::DEFAULT_SERVER_IP;
  g.serverPort = cfg::DEFAULT_SERVER_PORT;
  g.magicByte1 = cfg::DEFAULT_MAGIC_BYTE_1;
  g.magicByte2 = cfg::DEFAULT_MAGIC_BYTE_2;
  g.telemetryRaw = cfg::DEFAULT_TELEMETRY_RAW;
  g.offsetMm = cfg::DEFAULT_OFFSET_MM;
  g.moistureSlope = cfg::DEFAULT_MOISTURE_SLOPE;
  g.moistureIntercept = cfg::DEFAULT_MOISTURE_INTERCEPT;
  g.batteryDividerR1 = cfg::DEFAULT_BATTERY_DIVIDER_R1;
  g.batteryDividerR2 = cfg::DEFAULT_BATTERY_DIVIDER_R2;
  g.gpsLatitude = cfg::DEFAULT_GPS_LATITUDE;
  g.gpsLongitude = cfg::DEFAULT_GPS_LONGITUDE;
  g.apn = cfg::DEFAULT_APN;
}

static void loadFromNvs() {
  float f;
  unsigned long n;
  String s;

  s = prefs.getString(K_SERVER_IP, "");
  if (isValidServerHost(s)) g.serverIp = s;
  if (prefs.isKey(K_SERVER_PORT)) {
    n = prefs.getUInt(K_SERVER_PORT, g.serverPort);
    if (n >= 1 && n <= 65535) g.serverPort = (uint16_t)n;
  }
  if (prefs.isKey(K_MAGIC1)) g.magicByte1 = (uint8_t)prefs.getUInt(K_MAGIC1, g.magicByte1);
  if (prefs.isKey(K_MAGIC2)) g.magicByte2 = (uint8_t)prefs.getUInt(K_MAGIC2, g.magicByte2);
  if (prefs.isKey(K_RAW)) g.telemetryRaw = prefs.getUInt(K_RAW, g.telemetryRaw) != 0;

  struct F { const char *key; float *dst; float lo, hi; };
  const F floats[] = {
      {K_OFFSET, &g.offsetMm, 0.0f, 65535.0f},
      {K_MOIST_SLOPE, &g.moistureSlope, -1e9f, 1e9f},
      {K_MOIST_INT, &g.moistureIntercept, -1e9f, 1e9f},
      {K_BATT_R1, &g.batteryDividerR1, 1.0f, 1e9f},
      {K_BATT_R2, &g.batteryDividerR2, 1.0f, 1e9f},
      {K_GPS_LAT, &g.gpsLatitude, -90.0f, 90.0f},
      {K_GPS_LON, &g.gpsLongitude, -180.0f, 180.0f},
  };
  for (const F &e : floats) {
    s = prefs.getString(e.key, "");
    if (parseF(s, e.lo, e.hi, f)) *e.dst = f;
  }

  s = prefs.getString(K_APN, "");
  if (s.length() > 0 && s.length() <= 63) g.apn = s;
}

bool begin() {
  for (size_t i = 0; i < N; i++) kBindings[i] = kEntries[i].binding;
  loadDefaults();
  ready = prefs.begin(NS, false);
  if (ready) loadFromNvs();
  return ready;
}

const Values &get() { return g; }

const Binding *bindings(size_t &count) {
  count = N;
  return kBindings;
}

bool resetToDefault(const String &name) {
  if (!ready) return false;
  if (name.equalsIgnoreCase("ALL")) {
    prefs.clear();
    loadDefaults();
    return true;
  }
  for (size_t i = 0; i < N; i++) {
    if (name.equalsIgnoreCase(kEntries[i].binding.name))
      return kEntries[i].binding.setter(kEntries[i].def());
  }
  return false;
}

}  // namespace Settings
