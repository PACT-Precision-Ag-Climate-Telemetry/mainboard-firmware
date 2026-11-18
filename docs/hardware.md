# Hardware Reference

What is wired to what, and how much of it has been checked on the real board.
Source of truth for the code is [src/config.h](../src/config.h).

Sources used:

- Schematic: `pact\mainboard-master\mainboard-master.kicad_sch` (KiCad 10).
  GPIO numbers were worked out from net-label positions against the DevKit
  symbol's pin offsets, not read from a netlist.
- Old firmware: `pact\master` (previous board revision). Used for the modem
  power-up sequence, the settings, and calibration formulas.
- Bench runs on the real board (serial logs).

"Verified" below means seen working on the board. "From schematic" means read
from the schematic only.

## MCU

ESP32-S3-DevKitC-1 N16R8 (16 MB flash, 8 MB PSRAM), Arduino framework
(pioarduino platform). UART0 is the USB-serial port and also the J10 PROG pads
(use one at a time).

## Pin map

| Function | Net name | GPIO | Notes | Status |
|----------|----------|------|-------|--------|
| I2C SDA | SDA / SDA_MCU | 8 | via 100 R (R7), 4.7 k pull-up to 3V3 | Verified |
| I2C SCL | SCL / SCL_MCU | 9 | via 100 R (R8), 4.7 k pull-up to 3V3 | Verified |
| GNSS, ESP transmits | GNSS_RXD | 15 | named from the GNSS module's side (owner-confirmed) | Verified |
| GNSS, ESP receives | GNSS_TXD | 16 | | Verified |
| Modem power (MOSFET) | MODEM_PWR | 5 | HIGH = modem powered, LOW = off. Pull-down keeps it off at boot | Verified |
| Modem power key | MODEM_EN | 4 | LOW 1 s then HIGH, copied from the old firmware | Verified |
| Modem, ESP transmits | MODEM_RXD | 17 | named from the modem's side (bench-confirmed in old firmware) | Verified |
| Modem, ESP receives | MODEM_TXD | 18 | | Verified |
| Battery sense enable | BATT_SENSE_EN | 10 | HIGH while measuring | From schematic |
| Solar panel disable | SOLAR_DIS | 11 | HIGH while measuring | From schematic |

Net name convention: UART net names (`GNSS_RXD`, `MODEM_RXD`, ...) are named
for the **peripheral's** pin, so the ESP transmits on the `*_RXD` net.

## Connectors

| Ref | Value | Purpose |
|-----|-------|---------|
| J1 | SCD40 | CO2 sensor, I2C (XH2.5 4-pin) |
| J2 | SHT30 | Temperature / humidity, I2C (XH2.5 4-pin) |
| J3 | TOF | VL53L1X distance, I2C (XH2.5 4-pin) |
| J4 | MHZ9041A | Methane sensor (PH2.0), wired to SDA/SCL (owner-confirmed). The note on the schematic still says "MH-Z19 CO2": that text is outdated, the wiring is correct |
| J5 | Soil Sensor | Capacitive soil moisture, analog |
| J6 | ADS1115 | ADC breakout, 10-pin socket |
| J7 | MODEM | A7670E modem, XH2.5 8-pin |
| J8 | GPS | GNSS receiver (NEO-7M), 5-pin socket |
| J9 | POWER | From the power board, XH2.5 6-pin: 5 V, 3V3, battery sense, solar control. No I2C |
| J10 | PROG | Programming pads, UART0 + EN + BOOT, FTDI order |
| J11 | 3V3_LINK | Shunt fitted = ESP32 runs from board 3V3. Remove it when the DevKit is on USB |

## UART allocation

| UART | Use | Settings |
|------|-----|----------|
| UART0 | Debug log and field-configurator console (USB / J10) | 115200 8N1 |
| UART1 (`Serial1`) | GNSS | baud auto-detected from 9600, 38400, 115200, 57600, 4800 |
| UART2 (`Serial2`) | Modem | 115200 8N1 |

## I2C devices

See [i2c-addresses.md](i2c-addresses.md).

## Modem (A7670E): on-demand power

The modem is **off by default** (`MODEM_PWR` LOW). It is powered only for a
session: to send data, to pull status / cell info / network time, or to run a
console `at` command. Everything is a non-blocking state machine
(`devices/modem.cpp`).

**Power-up**, from the old firmware's `initModem()` (which worked on the
previous board), with the waits turned into states:

1. `MODEM_PWR` (GPIO5) HIGH: the MOSFET connects the modem's 5 V rail.
2. 200 ms for the rail to settle.
3. `MODEM_EN` (GPIO4): LOW for 1 s, then HIGH (power-key pulse).
4. Send `AT` every 300 ms until `OK` (gives up after 20 s; the old firmware
   waited a fixed 15 s).
5. Init: `ATE0` (no echo), `AT+CSCLK=0` (no DTR-gated sleep, DTR is not wired),
   `AT+CGMM` (model, reads back `A7670E-LASE`), `AT+CTZU=1` (network time
   update), `AT+COPS=3,2` (report the operator as numeric MCC+MNC), then the
   APN if `MODEM_APN` is set. The APN is read on every wake, so a changed
   `MODEM_APN` takes effect at the next wake.

**Status pull session** (`AT+CPIN?`, `CREG?`/`CEREG?` until registered or 60 s,
`CSQ`, `CFUN?`, `COPS?`, `CCLK?`):

- The numeric operator, e.g. `52003`, is split into MCC `520` and MNC `3`
  (first 3 digits, then the rest). This fills the telemetry **cell block**.
- `AT+CCLK?` gives the network time (local time plus a timezone offset in
  quarter hours), converted to UTC. Dates before 2024 are rejected (an unsynced
  module reports 1970/1980).
- Cell info and time are kept in RAM between wakes; they are lost at reset.
- If there is no SIM, the session skips the registration wait.

**Shutdown:** after 5 s with nothing to do, send `AT+CPOF`, then `MODEM_PWR`
LOW and `MODEM_EN` released to an input (so it cannot back-feed the unpowered
modem). A failed start (no answer to `AT` in 20 s) also cuts power and reports
a failed session; the next session period tries again.

**Not verified:** `AT+CPOF` and `AT+COPS=3,2` have not been run on this module.
The registration problem (`CREG=0`) means a real MCC/MNC has not been read yet.

The A7670E has no GNSS engine of its own (`AT+CGNSSPWR=1` answers `ERROR` in
the old firmware), which is why there is a separate GNSS receiver on J8.

## Battery measurement

`BATT_SENSE_EN` (GPIO10) and `SOLAR_DIS` (GPIO11) are driven HIGH, the divider
settles for 5 s, ADS1115 AIN0 is read, and both pins go LOW again. This is the
sequence the owner specified. The old firmware drove only its `SOLAR_DIS` pin
for 10 ms.

It is a non-blocking state machine: the 5 s settle is timed with `millis()`
while the rest of the firmware keeps running. Because solar is disconnected for
the whole settle time, a reading is started only every 60 s
(`PERIOD_BATTERY_MS`), so the panel is off about 8 % of the time.

```text
battery volts = ADC volts x (R1 + R2) / R2      R1 = 220 k, R2 = 47 k (settings)
```

At the ADS1115's +/-4.096 V range this measures batteries up to about 22 V
(the ADS1115 is powered from 3.3 V, so its input must stay under about 3.6 V:
a 15 V battery gives 2.64 V).

## ADC sharing (ADS1115)

One ADS1115 serves three users: the periodic 4-channel scan, the battery
reading and the moisture reading. Conversions run one at a time from a queue.
A user calls `queue(channel)` and gets a ticket; `done(channel, ticket)` is true
only for a conversion that **started after** `queue()`. That matters for the
battery: a conversion already in flight when the sense pins were raised would
read the wrong voltage and is ignored.

## ADC channels (ADS1115)

| Channel | Use | Basis |
|---------|-----|-------|
| AIN0 | Battery sense | Old firmware; J6 pin 8 carries `BATT_SENSE_ADC` |
| AIN1 | Soil moisture | Old firmware; J6 pin 9 carries `MOISTURE_ADC` |
| AIN2, AIN3 | unused | read about 0.12 V, floating |

**Assumption, not verified:** J6 is a generic 10-pin symbol with no pin names,
so which ADS1115 input pins 8 and 9 reach depends on the breakout's pin order.
The assumption fits the old firmware and the first bench scan (AIN0 about 0 V
with sense off, AIN1 drifting like a floating input). Confirm with a battery
connected: the voltage at the ADC pin should follow the battery. If not, change
`ADC_BATTERY_CHANNEL` / `ADC_MOISTURE_CHANNEL` in `config.h`.

## Observations from the bench

- **AIN1 decays** from 1.49 V to 0.25 V over about 5 minutes with nothing
  connected. This is a floating input discharging, not a signal. Expected for a
  moisture input with no sensor plugged in.
- **Methane sensor** reads 0.00 %LEL, module temperature about 33 C, error 0.
- **CO2 / temperature:** the SCD40 settles slowly (temperature fell from 26.8 C
  to 24.4 C over the run); the SHT30 stayed near 23.6 C. The SCD40 warms itself.
- **Modem:** `A7670E-LASE`, SIM ready, CSQ 13-14 (about -85 to -87 dBm),
  registration status 0 (not registered, not searching).
- **GNSS:** NMEA sentences arrive and parse; no fix indoors.
