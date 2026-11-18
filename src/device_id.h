#pragma once
#include <Arduino.h>

// Device identity, ported unchanged from the old firmware (lib/id.h + tcp.h)
// so a given board keeps the same ID.
//
//   short serial = last 3 bytes of the WiFi STA MAC -> number -> mod 62^4
//                  -> 4 base-62 chars (0-9 A-Z a-z)
//   wire ID      = 10 bytes: model with '-' removed (max 6) + 4-char serial,
//                  padded with '0'. Example: "TESMBOab3Z"
//   display ID   = "<model>-<serial>", e.g. "TES-MBO-ab3Z"

// The model prefix lives in config.h (cfg::DEVICE_MODEL).

String getShortSerial();               // 4 base-62 characters
void buildDeviceId(char outId[10]);    // 10-byte ID for the telemetry header
String getDeviceIdString();            // "TES-MBO-xxxx" for logs / settings
