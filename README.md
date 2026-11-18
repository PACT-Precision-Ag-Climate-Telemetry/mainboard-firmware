# Mainboard Firmware

Firmware for the PACT mainboard: an ESP32-S3-DevKitC-1 (N16R8) that reads
environmental sensors, a battery, a GNSS receiver and a cellular modem, and
formats the readings into the binary telemetry frame the ingest server expects.
Settings (server address, offset, calibration) are changed in the field over a
text console.

> **Status:** bring-up firmware. The five I2C sensors, GNSS and the modem's
> basic power-up were run on the real board with the previous (blocking)
> version of the code. The current non-blocking version, the on-demand modem
> sessions, the cell block, the settings console, battery, moisture, device ID
> and frame builder compile but have **not yet been run on the board**. There
> is no TCP sender yet. See [Status](#status).

## Documentation

| Document | What it covers |
|----------|----------------|
| [docs/hardware.md](docs/hardware.md) | Pin map, connectors, power sequences, ADC mapping, what is verified |
| [docs/i2c-addresses.md](docs/i2c-addresses.md) | Every device on the I2C bus and its address |
| [docs/field-configurator.md](docs/field-configurator.md) | The `get` / `set` console: commands, every setting, every read-only value |
| [docs/telemetry.md](docs/telemetry.md) | The binary telemetry frame format |
| [docs/development-log.md](docs/development-log.md) | What was found, decided and fixed so far, and open items |

## Build, flash, monitor

PlatformIO, environment `esp32-s3-devkitc1-n16r8` ([platformio.ini](platformio.ini)).

```text
platformio run                                  build
platformio run --target upload --target monitor flash and open the serial monitor
```

On Windows the PlatformIO executable is
`C:\Users\<you>\.platformio\penv\Scripts\platformio.exe`. The system Python
must not be 3.14 for the command-line `pio`; the VS Code PlatformIO buttons
use their own Python and work regardless.

Libraries (downloaded automatically): `pololu/VL53L1X`,
`dfrobot/DFRobot_MHZ9041A`, `mikalhart/TinyGPSPlus`.

Serial monitor: 115200 baud on the DevKit's UART port.

## What the firmware does

**Nothing blocks.** There are no `delay()` calls in the firmware. Every
handler is a state machine that `loop()` advances a little on each pass, so
the console, GNSS and sensors keep running while the battery settles, the ADC
converts, or the modem boots. The only waits left are inside two vendor
libraries: `DFRobot_MHZ9041A` (about 5 ms per read, roughly 15 ms a second,
plus 100 ms in its `begin()`) and `VL53L1X`'s `begin()` at boot.

**At boot** (`setup()` in [src/main.cpp](src/main.cpp)) everything is only
configured, and returns immediately:

1. Open persistent settings (NVS).
2. Start I2C (SDA GPIO8, SCL GPIO9) and initialise the five I2C sensors.
3. Set the battery switch pins LOW, start the GNSS baud search in the
   background, and set the modem power pin LOW (modem **off**).
4. Register the console commands.

After `SERIAL_ATTACH_DELAY_MS` (1.5 s, measured with `millis()`, not a wait) a
boot report is printed: settings, device ID, I2C scan, device status. The
delay only gives a serial monitor time to attach after a reset.

**In the loop** (`loop()`):

- Serve the console (`get` / `set` commands).
- Advance every handler and print readings (when `LOG` is 1).
- Every `TELEMETRY_SAMPLE_PERIOD_MS` (30 s for bench use; the old firmware
  used 5 min) take one reading of everything and add it to a batch of up to 24.
- Every `MODEM_SESSION_PERIOD_MS` (2 min for bench use) **wake the modem** for
  a session (below), then build the telemetry frame, including the cell block.
  The frame is printed as hex and also available from the console as
  `get PAYLOAD_HEX`. There is no TCP sender yet, so it is **not sent**.

**The modem is off unless needed.** It is woken for a send, a status pull, or
a console `at` command, and then shuts itself down:

```text
off -> power rail on -> power-key pulse -> wait for AT -> init -> READY
READY: run the job (status pull, or raw AT)  ->  5 s idle  ->  AT+CPOF  ->  power rail off
```

A status pull checks the SIM, waits for network registration (up to 60 s),
reads signal strength, and reads the operator to get the **MCC and MNC** for
the telemetry cell block, and the network time. Cell info and time are kept in
RAM between wakes. `set MODEM_WAKE 1` runs a session on demand.

## Source layout

```text
src/
  main.cpp              wiring: objects, boot sequence, loop, console bindings
  config.h              single source of truth: pins, addresses, bauds, intervals,
                        defaults, timeouts   (namespace cfg::)
  settings.{h,cpp}      persistent field-editable settings in NVS
  field_config.{h,cpp}  the get/set text console
  binding.h             "named value with getter/setter" used by the console
  telemetry.{h,cpp}     builds the binary frame (docs/telemetry.md)
  device_id.{h,cpp}     device ID from the MAC (ported from the old firmware)
  i2c_tools.{h,cpp}     bus scanner and register dump (bring-up helpers)
  timeutil.h            date/time -> Unix seconds (GNSS and modem time)
  devices/
    i2c_device.h        base class: begin(), poll() = service() + update(), fresh(), print()
    vl53l1x_tof.*       distance sensor              (I2C 0x29)
    methane_mhz9041a.*  methane sensor               (I2C 0x34)
    sht30.*             temperature and humidity     (I2C 0x44)
    ads1115.*           4-channel ADC                (I2C 0x48)
    scd40.*             CO2                          (I2C 0x62)
    battery.*           battery voltage via the ADC, switches the sense pins (state machine)
    moisture.*          soil moisture in percent via the ADC (state machine)
    gnss.*              GNSS receiver, NMEA over UART1, background baud search
    modem.*             A7670E modem, on-demand power + AT command state machine
```

Each device has its own file and handler class. Everything that is a number
about the hardware lives in [src/config.h](src/config.h), so a pin or address
change is a one-line edit there.

## Status

| Area | State |
|------|-------|
| I2C scan, 5 sensors (VL53L1X, SHT30, ADS1115, SCD40, MH-Z9041A) | Read plausible values on the board with the **old blocking code**. The non-blocking rewrite (SHT30, SCD40, ADS1115 state machines) compiles but is **not yet run** |
| GNSS (NEO-7M, UART1) | NMEA received on the board with the old code (no fix indoors). Non-blocking baud search compiles, not yet run |
| Modem (A7670E) power-up and AT | Ran on the board with the old blocking code: powers up, answers AT, SIM ready, signal OK. **Not registering on the network** (`CREG=0`), cause unknown |
| Modem on-demand sessions, shutdown (`AT+CPOF`), cell info (MCC/MNC), network time | Written, compiles, **not yet run**. Registration is the open problem: without it there is no cell info |
| Device ID | Compiled, not yet seen on the board |
| Settings store and field-configurator console | Compiled, not yet run on the board |
| Battery reading (`BATT_SENSE_EN` / `SOLAR_DIS`) | Compiled, not yet run. ADC channel is an assumption (see hardware.md) |
| Moisture reading | Compiled, not yet run. Needs calibration |
| Telemetry frame builder, cell block | Compiled. The frame was checked on a PC against the worked example in telemetry.md (byte-for-byte match); the cell block is filled from the modem session but never seen on the board |
| TCP sender, ack handling, retry buffer | **Not written**. The modem is woken and the frame built, but nothing is sent |
| Sleep / power management of the MCU | **Not written** |

Open questions and next steps are in [docs/development-log.md](docs/development-log.md).
