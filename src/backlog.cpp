#include "backlog.h"
#include <LittleFS.h>
#include "config.h"

namespace Backlog {

namespace {
constexpr const char *PATH = "/backlog.bin";
constexpr const char *TMP = "/backlog.tmp";
constexpr uint32_t MAGIC = 0x42434B31;  // "BCK1"; bump if Reading changes

struct Header {
  uint32_t magic;
  uint16_t recordSize;  // sizeof(Reading): rejects a file from another layout
  uint16_t count;
};

bool gMounted = false;

// ---- spill file: SpillHeader, then fixed-size records, oldest first ----------
constexpr const char *SPILL = "/spill.bin";
constexpr const char *SPILL_TMP = "/spill.tmp";
constexpr uint32_t SPILL_MAGIC = 0x53504C31;  // "SPL1"; bump if Reading changes
constexpr size_t REC = sizeof(Telemetry::Reading);

struct SpillHeader {
  uint32_t magic;
  uint32_t recordSize;
};

uint32_t gSpillCount = 0;  // cached so the loop never touches flash to ask

// Copy records [skip, end) to a new spill file (whole records only), then swap.
bool rewriteSpill(uint32_t skip) {
  File in = LittleFS.open(SPILL, FILE_READ);
  if (!in) return false;
  size_t total = in.size() >= sizeof(SpillHeader) ? (in.size() - sizeof(SpillHeader)) / REC : 0;
  if (skip >= total) {
    in.close();
    LittleFS.remove(SPILL);
    gSpillCount = 0;
    return true;
  }
  File out = LittleFS.open(SPILL_TMP, FILE_WRITE);
  if (!out) {
    in.close();
    return false;
  }
  SpillHeader h = {SPILL_MAGIC, (uint32_t)REC};
  bool ok = out.write((const uint8_t *)&h, sizeof(h)) == sizeof(h);
  in.seek(sizeof(SpillHeader) + skip * REC);
  uint8_t buf[REC * 8];
  uint32_t left = total - skip;
  while (ok && left > 0) {
    uint32_t n = left > 8 ? 8 : left;
    ok = in.read(buf, REC * n) == REC * n && out.write(buf, REC * n) == REC * n;
    left -= n;
  }
  in.close();
  out.close();
  if (!ok) {
    LittleFS.remove(SPILL_TMP);
    return false;
  }
  LittleFS.remove(SPILL);
  if (!LittleFS.rename(SPILL_TMP, SPILL)) return false;
  gSpillCount = total - skip;
  return true;
}
}  // namespace

bool begin() {
  gMounted = LittleFS.begin(true);
  gSpillCount = 0;
  if (!gMounted) return false;

  File f = LittleFS.open(SPILL, FILE_READ);
  if (!f) return true;
  SpillHeader h;
  bool good = f.read((uint8_t *)&h, sizeof(h)) == sizeof(h) && h.magic == SPILL_MAGIC &&
              h.recordSize == REC;
  size_t size = f.size();
  f.close();
  if (!good) {
    LittleFS.remove(SPILL);  // other firmware layout or corrupt: cannot trust it
    return true;
  }
  gSpillCount = (size - sizeof(h)) / REC;
  if ((size - sizeof(h)) % REC != 0) rewriteSpill(0);  // torn last record: trim it
  return true;
}

bool save(const Telemetry::Reading *readings, uint8_t count) {
  if (!gMounted) return false;
  if (count == 0) {
    LittleFS.remove(PATH);
    return true;
  }
  File f = LittleFS.open(TMP, FILE_WRITE);
  if (!f) return false;
  Header h = {MAGIC, (uint16_t)sizeof(Telemetry::Reading), count};
  size_t want = sizeof(h) + sizeof(Telemetry::Reading) * count;
  size_t got = f.write((const uint8_t *)&h, sizeof(h)) +
               f.write((const uint8_t *)readings, sizeof(Telemetry::Reading) * count);
  f.close();
  if (got != want) {
    LittleFS.remove(TMP);
    return false;
  }
  LittleFS.remove(PATH);
  return LittleFS.rename(TMP, PATH);
}

uint8_t load(Telemetry::Reading *out, uint8_t capacity) {
  if (!gMounted) return 0;
  File f = LittleFS.open(PATH, FILE_READ);
  if (!f) return 0;
  Header h;
  if (f.read((uint8_t *)&h, sizeof(h)) != sizeof(h) || h.magic != MAGIC ||
      h.recordSize != sizeof(Telemetry::Reading) || h.count == 0) {
    f.close();
    return 0;
  }
  uint8_t n = h.count > capacity ? capacity : (uint8_t)h.count;
  // Keep the newest `n` if the file holds more than fits.
  f.seek(sizeof(h) + sizeof(Telemetry::Reading) * (h.count - n));
  size_t bytes = sizeof(Telemetry::Reading) * n;
  size_t got = f.read((uint8_t *)out, bytes);
  f.close();
  return got == bytes ? n : 0;
}

uint32_t spillCount() { return gSpillCount; }

bool spillAppend(const Telemetry::Reading &r) {
  if (!gMounted) return false;
  if (gSpillCount >= cfg::BACKLOG_MAX_READINGS) spillDrop(cfg::TELEMETRY_MAX_READINGS);  // oldest go first
  bool fresh = !LittleFS.exists(SPILL);
  File f = LittleFS.open(SPILL, FILE_APPEND);
  if (!f) return false;
  bool ok = true;
  if (fresh) {
    SpillHeader h = {SPILL_MAGIC, (uint32_t)REC};
    ok = f.write((const uint8_t *)&h, sizeof(h)) == sizeof(h);
  }
  ok = ok && f.write((const uint8_t *)&r, REC) == REC;
  f.close();
  if (ok) gSpillCount++;
  return ok;
}

uint8_t spillPeek(Telemetry::Reading *out, uint8_t maxCount) {
  if (!gMounted || gSpillCount == 0) return 0;
  File f = LittleFS.open(SPILL, FILE_READ);
  if (!f) return 0;
  uint8_t n = gSpillCount > maxCount ? maxCount : (uint8_t)gSpillCount;
  f.seek(sizeof(SpillHeader));
  size_t bytes = REC * n;
  size_t got = f.read((uint8_t *)out, bytes);
  f.close();
  return got == bytes ? n : 0;
}

bool spillDrop(uint32_t n) {
  if (!gMounted || n == 0 || gSpillCount == 0) return true;
  return rewriteSpill(n);
}

}  // namespace Backlog
