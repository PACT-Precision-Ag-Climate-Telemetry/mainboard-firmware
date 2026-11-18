#include "device_id.h"
#include "config.h"
#include <esp_mac.h>

static const char BASE62[] =
    "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

String getShortSerial() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);

  uint32_t val = ((uint32_t)mac[3] << 16) | ((uint32_t)mac[4] << 8) | mac[5];
  val = val % 14776336;  // 62^4, so it fits in 4 base-62 characters

  String serial = "";
  for (int i = 0; i < 4; i++) {
    serial = BASE62[val % 62] + serial;  // prepend
    val /= 62;
  }
  return serial;
}

void buildDeviceId(char outId[10]) {
  size_t pos = 0;

  // Up to 6 bytes from the model (dashes skipped) ...
  const char *model = cfg::DEVICE_MODEL;
  for (size_t i = 0; model[i] != '\0' && pos < 6; ++i) {
    if (model[i] == '-') continue;
    outId[pos++] = model[i];
  }

  // ... then the 4-char short serial ...
  String serial = getShortSerial();
  for (uint8_t i = 0; i < 4 && pos < 10; ++i) outId[pos++] = serial[i];

  // ... padded with '0' to exactly 10 bytes.
  while (pos < 10) outId[pos++] = '0';
}

String getDeviceIdString() { return String(cfg::DEVICE_MODEL) + "-" + getShortSerial(); }
