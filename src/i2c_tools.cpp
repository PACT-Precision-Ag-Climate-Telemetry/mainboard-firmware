#include "i2c_tools.h"
#include <Wire.h>

void i2cScan() {
  uint8_t found[128];
  uint8_t count = 0;

  Serial.println("\nScanning I2C bus...");
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    uint8_t err = Wire.endTransmission();
    if (err == 0) {
      found[count++] = addr;
      Serial.printf("  FOUND  0x%02X  (%3u)\n", addr, addr);
    } else if (err == 4) {
      Serial.printf("  ERROR  unknown error at 0x%02X\n", addr);
    }
  }

  if (count == 0) {
    Serial.println("No I2C devices found. Check wiring, pull-ups and power.");
    return;
  }
  Serial.printf("Summary: %u device(s):", count);
  for (uint8_t i = 0; i < count; i++) Serial.printf(" 0x%02X", found[i]);
  Serial.println();
}

void i2cProbe(uint8_t addr) {
  Serial.printf("\n=== Register probe @ 0x%02X ===\n", addr);
  Serial.print("     ");
  for (uint8_t c = 0; c < 16; c++) Serial.printf(" %X ", c);
  Serial.println();
  for (uint16_t row = 0; row < 16; row++) {
    Serial.printf("0x%X0:", (unsigned)row);
    for (uint16_t col = 0; col < 16; col++) {
      uint8_t reg = (row << 4) | col;
      Wire.beginTransmission(addr);
      Wire.write(reg);
      bool ok = (Wire.endTransmission(false) == 0) &&
                (Wire.requestFrom(addr, (uint8_t)1) == 1);
      if (ok) Serial.printf(" %02X", Wire.read());
      else {
        Wire.endTransmission(true);
        Serial.print(" --");
      }
    }
    Serial.println();
  }
  Serial.println("=== end probe ===");
}
