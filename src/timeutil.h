#pragma once
#include <stdint.h>

// Unix seconds for a UTC calendar date/time (no timezone, no leap seconds).
// Howard Hinnant's days_from_civil. Used for GNSS and modem (AT+CCLK) time.
inline uint64_t epochFromUtc(int year, unsigned month, unsigned day, unsigned hour,
                             unsigned minute, unsigned second) {
  int y = year - (month <= 2 ? 1 : 0);
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = (int64_t)era * 146097 + (int64_t)doe - 719468;
  return (uint64_t)(days * 86400 + (int64_t)hour * 3600 + (int64_t)minute * 60 + second);
}
