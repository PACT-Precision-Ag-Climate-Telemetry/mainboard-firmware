# I2C Addresses

Bus: SDA = GPIO8, SCL = GPIO9, 100 kHz, 4.7 k pull-ups to 3V3. Scanned on the
real board with `i2cScan()` (`src/i2c_tools.cpp`), which runs at every boot.
Addresses are defined in [src/config.h](../src/config.h) (`cfg::ADDR_*`).

| 7-bit address | Decimal | Device | Connector | Handler | Identified by |
|---------------|---------|--------|-----------|---------|---------------|
| 0x29 | 41 | VL53L1X distance (ToF) | J3 | `devices/vl53l1x_tof` | owner |
| 0x34 | 52 | MH-Z9041A methane (DFRobot SEN0654) | J4 | `devices/methane_mhz9041a` | owner, matches the register dump |
| 0x44 | 68 | SHT30 temperature / humidity | J2 | `devices/sht30` | owner |
| 0x48 | 72 | ADS1115 ADC | J6 | `devices/ads1115` | owner |
| 0x62 | 98 | SCD40 CO2 | J1 | `devices/scd40` | owner |

Five devices, no address conflicts. All five read plausible values on the board.

## How 0x34 was identified

The scan showed five addresses but the owner's parts list had four sensors.

1. A first guess of "power-management chip" (AXP192/AXP2101) was made from a
   table of common address assignments. It was **wrong**: nothing on the board
   or in the schematic points to a PMIC, and a guess like that should not have
   been written down as a fact.
2. The schematic showed no I2C ICs on the mainboard, so the device had to be on
   a plugged-in module.
3. A register dump (`i2cProbe(0x34)`) showed ASCII text in registers 0x20-0x4C:
   `,A+000.00 +32.8 0650.70 +19.6 B+056.0 00 1D` and zeros elsewhere. That is a
   sensor serving text, not a register-map IC.
4. The owner identified it as the DFRobot MH-Z9041A methane sensor. DFRobot's
   `DFRobot_MHZ9041A` library supports I2C at 0x34, so the handler uses it.

The sensor's dip switch must be on the I2C side (it is: it answers at 0x34).
J4's wiring to SDA/SCL is correct (owner-confirmed). The schematic note on J4
still says "MH-Z19 CO2": that text is outdated, the wiring is right.

## Tools

- `i2cScan()` lists every responding address (runs at boot).
- `i2cProbe(addr)` prints a read-only 0x00-0xFF register dump of one address.
  It is not called at boot any more; call it from `setup()` to investigate a
  device. It is not a normal runtime function.

## Non-blocking reads

Each I2C handler starts a measurement in `update()` and finishes it in
`service()` on a later `loop()` pass (`devices/i2c_device.h`): the SHT30 reads
16 ms after its command, the SCD40 finishes its 500 ms stop/start and 1 ms
commands the same way, and the ADS1115 runs queued conversions. The VL53L1X and
MH-Z9041A use vendor libraries whose calls block briefly (about 15 ms per second
for the methane sensor, see the README).
