#pragma once
#include <Arduino.h>
#include "telemetry.h"

// Flash copy of the unsent readings, so a failed upload followed by a reset or
// power loss does not lose data. The RAM buffer in main.cpp stays the working
// copy; this just mirrors it.
namespace Backlog {

bool begin();  // mounts LittleFS (formats it on first use)

// Replace the stored backlog with `count` readings (oldest first).
// Writes a temp file then renames, so a power cut keeps the previous copy.
bool save(const Telemetry::Reading *readings, uint8_t count);

// Load up to `capacity` readings. Returns how many (0 if none / corrupt).
uint8_t load(Telemetry::Reading *out, uint8_t capacity);

// ---- spill queue ---------------------------------------------------------------
// Readings that no longer fit in the RAM buffer (24) go here instead of being
// dropped, oldest first, up to cfg::BACKLOG_MAX_READINGS. Everything in the
// spill is OLDER than everything in the RAM buffer, so it is uploaded first.
uint32_t spillCount();
bool spillAppend(const Telemetry::Reading &r);   // drops the oldest chunk if full
uint8_t spillPeek(Telemetry::Reading *out, uint8_t maxCount);  // oldest first
bool spillDrop(uint32_t n);                       // remove the n oldest

}  // namespace Backlog
