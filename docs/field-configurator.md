# Field Configurator Protocol

How a technician, or a field-configurator device, reads and changes the
mainboard's settings. It is a plain-text, line-based `get` / `set` protocol.
Implementation: `src/field_config.cpp` (console), `src/settings.cpp` (values).

## 1. Connection

| Item | Value |
|------|-------|
| Port | UART0: the DevKit USB-UART, or the J10 PROG pads (they share UART0, use one at a time) |
| Settings | 115200 baud, 8N1 (`cfg::SERIAL_BAUD` in `src/config.h`) |
| Line ending | send `\n`, `\r` or `\r\n`; any works |
| Max line | 128 characters (`cfg::CONSOLE_MAX_LINE`) |

The debug log uses the same port. While configuring, silence it with
`set LOG 0` so log lines don't interleave with replies. `LOG` resets to `1`
at boot.

## 2. Commands

Commands are case-insensitive. Setting names are case-insensitive too.

| Command | What it does | Success reply |
|---------|--------------|---------------|
| `help` | List the commands | command list |
| `help NAME` | Describe one setting and say if it is read/write | name, access, description |
| `list` | Print every setting and live value | one `NAME=value` per line (`(ro)` marks read-only) |
| `get NAME` | Read one value | `OK NAME=value` |
| `set NAME VALUE` | Change a setting and save it to flash | `OK NAME=value` (the value read back) |
| `set NAME ""` | Set an empty value (only `MODEM_APN` accepts it) | `OK NAME=` |
| `reset NAME` | Restore one setting to its default | `OK reset NAME` |
| `reset ALL` | Restore every setting to its default (clears the saved settings) | `OK reset ALL` |
| `at COMMAND` | Queue a raw AT command; the modem is woken if off (see "The modem and the `at` command") | `OK queued...`, then the reply later |

### Replies

- Every reply line starts with `OK` or `ERR`. After each command the device
  prints `\r\n> ` as a prompt.
- The device **echoes** the characters it receives, so a machine client sees
  its own line before the reply. Skip to the line that starts with `OK` or `ERR`.
- `set` saves to flash immediately and survives power loss. A rejected value
  changes nothing.

| Error | Meaning |
|-------|---------|
| `ERR unknown command: x` | not one of the commands above |
| `ERR unknown setting: NAME` | no setting with that name |
| `ERR setting is read-only: NAME` | live values can't be set |
| `ERR invalid value for NAME` | wrong format, out of range, or flash write failed |
| `ERR usage: ...` | missing arguments |
| `ERR command too long` | line over 128 characters; it is discarded |
| `ERR AT passthrough not available` | the modem handler is not registered |

### Example session

```text
> set LOG 0
OK LOG=0

> get SERVER_IP
OK SERVER_IP=103.99.11.209

> set SERVER_IP 203.0.113.10
OK SERVER_IP=203.0.113.10

> set SERVER_PORT 46096
OK SERVER_PORT=46096

> set OFFSET 350
OK OFFSET=350.000000

> set SERVER_PORT 70000
ERR invalid value for SERVER_PORT

> get BATTERY_MV
OK BATTERY_MV=12410 (age 14 s)
```

## 3. Writable settings

All of these are saved in flash (NVS namespace `device_settings`). The storage
layout is identical to the old firmware, so a board that already holds a server
IP, offset or calibration keeps them after flashing this firmware.

| Name | Type and range | Default | Effect |
|------|----------------|---------|--------|
| `SERVER_IP` | IPv4 address or hostname, up to 253 chars, no spaces or `/` | `103.99.11.209` | Telemetry server address |
| `SERVER_PORT` | integer 1 to 65535 | `46096` | Telemetry server TCP port |
| `TELEMETRY_MAGIC_BYTE_1` | integer 0 to 255 (`0x1A` also accepted) | `26` (`0x1A`) | First magic byte of every frame. Must match the server's `magic_byte_1` |
| `TELEMETRY_MAGIC_BYTE_2` | integer 0 to 255 | `43` (`0x2B`) | Second magic byte. Must match the server's `magic_byte_2` |
| `TELEMETRY_RAW` | `1`/`0` (also `true`/`false`, `on`/`off`) | `1` | `1` sets the RAW flag, so each record also carries `methane_raw` and `moisture_raw` (+4 bytes per record). `0` omits them |
| `OFFSET` | number 0 to 65535 (mm) | `0` | The `offset` field of every telemetry record. Set it per installation (see section 5) |
| `ULTRASONIC_OFFSET` | same as `OFFSET` | | Old name of `OFFSET`. Reads and writes the same value |
| `MOISTURE_SLOPE` | number | `1.0` | Soil moisture percent = `SLOPE` x volts + `INTERCEPT` |
| `MOISTURE_INTERCEPT` | number | `0.0` | See above |
| `BATTERY_DIVIDER_R1` | ohms, 1 to 1e9 | `220000` | Top resistor of the battery-sense divider |
| `BATTERY_DIVIDER_R2` | ohms, 1 to 1e9 | `47000` | Bottom resistor. Battery volts = ADC volts x (R1+R2) / R2 |
| `GPS_LATITUDE` | -90 to 90 | `0` | Manual fallback latitude. Used only when the GNSS receiver has no fix. `0`/`0` means not set |
| `GPS_LONGITUDE` | -180 to 180 | `0` | Manual fallback longitude |
| `MODEM_APN` | up to 63 chars, no spaces or quotes. `""` clears it | empty | Cellular APN, applied to the modem (`AT+CGDCONT`) every time it wakes. A change takes effect at the next wake (`set MODEM_WAKE 1`) |

Changes to `SERVER_IP` and `SERVER_PORT` are stored and readable now, but the
firmware has no TCP sender yet, so nothing connects to that address.

## 4. Read-only values

| Name | Value |
|------|-------|
| `ID` | Device ID, `TES-MBO-xxxx` (model plus 4 base-62 characters from the MAC) |
| `FW_VERSION` | Firmware version |
| `SHT30_TEMPERATURE` | Air temperature, degrees C |
| `SHT30_HUMIDITY` | Relative humidity, percent |
| `CO2_PPM` | SCD40 CO2, ppm |
| `METHANE_LEL` | Methane in percent of the lower explosive limit (1 %LEL = 500 ppm) |
| `DISTANCE_MM` | VL53L1X distance in mm (sent as `depth`) |
| `MOISTURE_PERCENT` | Soil moisture, 0 to 100 %, `SLOPE` x ADC volts + `INTERCEPT`, clamped. Replies with the last reading and its age, e.g. `42.5 (age 1 s)`, and starts a new one in the background. Calibrate it with `MOISTURE_SLOPE` and `MOISTURE_INTERCEPT` (section 3) |
| `MOISTURE_VOLTS` | Voltage at the moisture sensor's ADC pin (ADS1115 AIN1) from the last reading |
| `MOISTURE_RAW` | Raw ADC count from the last reading (negative counts show as 0) |
| `BATTERY_MV` | Battery voltage in mV. Replies with the last reading and its age, e.g. `12410 (age 14 s)`, and starts a new one in the background: it drives `BATT_SENSE_EN` and `SOLAR_DIS` HIGH, waits 5 s, reads ADS1115 AIN0, then drives both LOW. So the console never waits: ask twice, 5-6 seconds apart, to see the fresh value. A reading under 1 V or over 20 V is rejected and the old value stays |
| `BATTERY_PERCENT` | State of charge from the last battery reading (12 V AGM curve: 11.50 V = 0 %, 12.85 V = 100 %) |
| `ADC_0` to `ADC_3` | ADS1115 inputs in volts. These do **not** switch the battery pins, so `ADC_0` reads about 0 V between battery readings |
| `GPS_FIX` | `lat,lon` when the receiver has a fix, else `none` |
| `MODEM_STATE` | `off`, `power-settle`, `key-pulse`, `booting`, `init`, `ready`, `working` or `shutting-down`. The modem is normally `off` |
| `MODEM_CELL` | `MCC,MNC` learned in the last modem session (e.g. `520,3`), or `unknown` |
| `PAYLOAD_HEX` | The telemetry frame for the readings so far, as hex. Handy for checking what the server would receive |

Two names in this list are writable. Their values are **not saved** (they are
runtime controls, not settings):

| Name | `set` value | Effect |
|------|-------------|--------|
| `LOG` | `0` / `1` | `0` mutes the periodic debug output, `1` brings it back (resets to 1 at boot) |
| `DEV_MODE` | `0` / `1` | `1` = dev timing: a reading every 1 min and an upload every 5 min. `0` = normal (5 min / 30 min). Not saved, resets to `0` at boot. In dev mode the dev board's NeoPixel shows status (see development-log 2.13) |
| `MODEM_WAKE` | `1` | Wake the modem now and run a status session (SIM, registration, signal, MCC/MNC, network time). `get MODEM_WAKE` returns the modem state |

### The modem and the `at` command

The modem is off unless it is needed. `at COMMAND` therefore does not answer
immediately: it replies `OK queued. ...` at once, then wakes the modem if it is
off (about 4-25 s to boot), runs the command, and prints the modem's reply on
its own line, for example:

```text
> at AT+CSQ
OK queued. The modem wakes if needed; the reply is printed when it arrives
> 
[at] +CSQ: 14,99

OK
```

The reply is printed between prompts, so a machine client should keep reading
until it sees a line starting with `[at]`. Only one `at` command can be pending
(`ERR another AT command is still pending`). If the modem fails to start, the
reply is `[at] ERR modem did not start (...)`. The modem stays on for about 5 s
after the last command, so follow-up commands are fast.

## 5. How `OFFSET` reaches the telemetry frame

The `offset` field in each telemetry record (`docs/telemetry.md`, sensor body
byte +8) comes from the `OFFSET` setting. It is read **when a reading is taken**
(`Telemetry::currentOffsetMm()`), so:

- Changing `OFFSET` in the field affects readings taken after the change.
  Readings already waiting in the batch keep the offset they were taken with.
- The value is rounded to the nearest whole mm and clamped to 0 to 65535.

The frame itself is built by `Telemetry::build()` in `src/telemetry.cpp`.
Besides `OFFSET`, three settings change the header: `TELEMETRY_MAGIC_BYTE_1`,
`TELEMETRY_MAGIC_BYTE_2` and `TELEMETRY_RAW`. GPS comes from the receiver, or
from `GPS_LATITUDE` / `GPS_LONGITUDE` when there is no fix.

To see the result without a server:

```text
> get PAYLOAD_HEX
OK PAYLOAD_HEX=1a2ba0544553...
```

## 6. Adding a new setting

1. Add a default to `cfg::` in `src/config.h`.
2. In `src/settings.cpp`: add a field to `Settings::Values`, an NVS key
   (15 characters maximum, or the write silently fails), a getter and setter
   (the `FLOAT_SETTING` macro covers numbers), a default function, and one line
   in `kEntries`.
3. Add a row to the table in section 3 of this file.
