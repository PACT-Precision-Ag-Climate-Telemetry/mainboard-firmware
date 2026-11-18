#pragma once
#include <Arduino.h>

// Diagnostics kept from bring-up.
void i2cScan();                  // list every responding address
void i2cProbe(uint8_t addr);     // read-only 0x00-0xFF register dump
