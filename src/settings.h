#pragma once
#include <Arduino.h>
#include "binding.h"

// Persistent, field-editable settings (stored in NVS).
//
// NVS layout is identical to the old firmware (namespace "device_settings",
// same keys, ints as u32, floats as strings), so a board that already holds
// a server IP / offset / calibration keeps them after flashing this firmware.
//
// Every successful "set" is written to flash immediately. A rejected value
// (out of range, bad format) changes nothing.
namespace Settings {

struct Values {
  String serverIp;
  uint16_t serverPort;
  uint8_t magicByte1;
  uint8_t magicByte2;
  bool telemetryRaw;       // RAW flag in the telemetry header
  float offsetMm;          // `offset` field of every telemetry record (mm)
  float moistureSlope;     // moisture % = slope * volts + intercept
  float moistureIntercept;
  float batteryDividerR1;  // ohms, top resistor
  float batteryDividerR2;  // ohms, bottom resistor
  float gpsLatitude;       // manual fallback position (0/0 = not set)
  float gpsLongitude;
  String apn;              // cellular APN ("" = none)
};

bool begin();                 // open NVS and load; defaults for anything missing
const Values &get();          // current values (read-only view)

// Writable settings, in the order "list" prints them.
const Binding *bindings(size_t &count);

// Restore one setting (by name) or all ("ALL") to its compiled-in default.
bool resetToDefault(const String &name);

// Plausibility check used by "set SERVER_IP" (IPv4 literal or hostname).
bool isValidServerHost(const String &value);

}  // namespace Settings
