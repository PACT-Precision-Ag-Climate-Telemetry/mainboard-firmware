# Development Log

What was done, what was found, what was decided, and what is still open.
Newest information last within each section. Facts here come from bench runs,
the schematic, the old firmware, or the owner; anything that is an assumption
says so.

## 1. Timeline

1. **I2C scanner.** Replaced the blank PlatformIO project with a scanner for
   every 7-bit address. The first build failed on overlapping `case` ranges in
   the address-hint table (my error); fixed. The hint table was later removed
   because it caused a wrong identification (see 2.1).
2. **Bus scan on the board:** 0x29, 0x34, 0x44, 0x48, 0x62.
3. **Identified 0x34** as the methane sensor (section 2.1).
4. **One handler class per device**, with a shared base class
   (`devices/i2c_device.h`). The VL53L1X uses Pololu's library; the SHT30,
   ADS1115 and SCD40 are written directly over `Wire` from their datasheet
   command sets; the methane sensor uses DFRobot's library.
5. **GNSS handler.** Schematic gave GPIO15/16 on J8. Owner confirmed the
   RXD/TXD names are from the GNSS module's side. The old firmware confirmed a
   NEO-7M at 9600 baud on the same pins. The handler auto-detects the baud rate.
6. **Modem handler.** First version only toggled `MODEM_PWR` and talked AT.
   Owner pointed at the old firmware in `pact\master`, which showed that
   `MODEM_EN` (GPIO4) is the power-key pulse (LOW 1 s, then HIGH) and that the
   UART runs at 115200. The handler was rewritten around that sequence and
   worked on the first run.
7. **Single config file** (`src/config.h`) for every pin, address, baud rate and
   interval.
8. **Device ID** ported from the old firmware (section 2.4).
9. **Settings store, field-configurator console and telemetry frame builder**
   (sections 2.5 and 2.6), with documentation.
10. **Battery** reading with `BATT_SENSE_EN` / `SOLAR_DIS`, **moisture** reading
    in percent, and **status bit 5** for moisture failure.
11. Documentation written (this file, the README, hardware.md).
12. **Non-blocking rewrite, on-demand modem, cell block** (sections 2.8 to 2.10):
    every `delay()` removed from the firmware; the modem is powered only for a
    session; MCC/MNC are read from the network and put in the frame's cell
    block. J4 confirmed correctly wired by the owner.
13. **Telemetry v2 fields and TCP sender** (sections 2.11 and 2.12): added
    `esp_temperature` and a 2-byte trailer (signal strength, reset reason) to the
    frame, and a modem SEND job that uploads the frame and waits for the ack.
14. **Intervals and dev mode** (open item 10, section 2.13): 5 min / 30 min
    normally, 1 min / 5 min with `set DEV_MODE 1`. Dev-mode status NeoPixel.
15. **Battery saver** (section 2.14): slower reading and upload when the battery
    is low.

## 2. Findings and decisions

### 2.1 The 0x34 device

See [i2c-addresses.md](i2c-addresses.md). Short version: a wrong "power
management chip" guess, corrected by a register dump and the owner. Lesson kept:
do not label an address from a table of common assignments.

### 2.2 Old firmware as a reference

`pact\master` (previous board) was used for: modem power-up sequence, UART
settings, NVS key names, calibration formulas (moisture, battery SOC),
telemetry field meanings (`offset`, methane ppm = %LEL x 500), device ID, and
the settings names. Differences worth knowing:

- Old board: I2C on GPIO15/16, modem power pin GPIO21/4. New board: I2C on
  GPIO8/9, GNSS on GPIO15/16, modem power on GPIO5 plus a key pulse on GPIO4.
- Old payload used 34/42-byte records; the new format (telemetry.md) uses
  21-byte bodies (25 with RAW) since section 2.11 (it was 19/23 before).

### 2.3 Modem

- Model `A7670E-LASE`. SIM reports ready. Signal CSQ 13-14 (about -85 dBm).
- **Open:** registration stays at `CREG=0, CEREG=0` (not registered, not
  searching). It showed `CREG=2` (searching) once, early. The status line now
  also prints `CFUN` and `COPS` to help find why; those values have not been
  seen yet. Possible causes to check: functionality level, manual operator
  selection, SIM not activated, wrong or missing APN.
- A bug fixed on the way: `digitalWrite()` on GPIO5 before `pinMode()` logged
  `IO 5 is not set as GPIO`. Harmless, removed.

### 2.4 Device ID

Ported unchanged: last 3 MAC bytes (WiFi STA) -> number mod 62^4 -> 4 base-62
characters; wire ID is `TESMBO` + those 4 characters (10 bytes, `0`-padded);
display form `TES-MBO-xxxx`. A board with the same MAC gets the same ID as under
the old firmware. Model prefix is `cfg::DEVICE_MODEL`.

### 2.5 Settings and the field configurator

- Text console on UART0 (`get`, `set`, `list`, `help`, `reset`, `at`).
- Settings are stored in NVS with the **same namespace and keys as the old
  firmware**, so existing values survive a reflash. Every successful `set`
  writes to flash immediately; the old firmware chained writes with `&&`, which
  silently skipped everything after the first failure (NVS keys over 15
  characters), so each setting is written independently here.
- `OFFSET` (the `offset` field of every telemetry record) is captured when a
  reading is taken, so changing it in the field affects only new readings.
- Full reference: [field-configurator.md](field-configurator.md).

### 2.6 Telemetry frame

- Built by `Telemetry::build()` from [telemetry.md](telemetry.md).
- Checked on a PC (not on the board): the builder was compiled with stub
  headers and fed the two readings of the worked example. It matched the
  documented hex exactly. The first run differed only in the GPS block, because
  latitude/longitude were `float` (about 9 cm of error at 1e-7 degree
  resolution); they are `double` now.
- The check was against the example as it stood before the status-bit-5 update;
  that change does not alter the example.
- Status byte: bit 0 battery, 1 depth (ToF), 2 temperature, 3 methane, 4 CO2,
  5 moisture. A failed sensor sends 0 for its value with its bit set.

### 2.7 Battery and moisture

- Battery: `SOLAR_DIS` + `BATT_SENSE_EN` HIGH, 5 s settle, read AIN0, both LOW.
  Now a non-blocking state machine (section 2.8); started every 60 s because
  the solar panel is disconnected during the whole settle.
- Moisture: `percent = SLOPE x volts + INTERCEPT`, clamped to 0-100. Defaults
  (slope 1, intercept 0) are placeholders; each installation needs calibration.
- The ADC channel mapping is an assumption, see hardware.md.
- The console getters `BATTERY_MV` / `MOISTURE_PERCENT` now answer at once with
  the last reading and its age, and start a new reading in the background (the
  old blocking `measure()` is gone, a 5 s wait cannot sit inside a command).

### 2.8 Non-blocking design

Goal: no `delay()` anywhere, so the console, GNSS and sensors stay responsive
while the battery settles, the ADC converts or the modem boots.

- `I2CDevice::poll()` now runs `service()` on every call (finishes work started
  earlier) and `update()` once per period (starts new work). Devices that must
  wait keep a state and a `millis()` timestamp.
- **SHT30:** command in `update()`, result read 16 ms later in `service()`.
- **SCD40:** the 500 ms after `stop_periodic_measurement` and the 1 ms command
  delays are states.
- **ADS1115:** a conversion queue. The periodic scan, the battery and the
  moisture reading all queue channels; one conversion runs at a time. A ticket
  system makes `done()` true only for a conversion that started after the
  request, which matters for the battery (pins must be high before the conversion
  starts). This replaced the old `readChannel()` that spun in a polling loop.
- **Battery / moisture:** small state machines on top of the ADC queue.
- **GNSS:** the baud search is stepped from `poll()` (1.5 s per candidate rate)
  instead of blocking in `begin()`.
- **Boot:** `setup()` only configures. The boot report is printed after 1.5 s
  by comparing `millis()`, so a monitor that attaches after a reset still sees it.
- **Not removable:** `DFRobot_MHZ9041A` calls `delay(5)` inside each read (three
  reads per second, about 15 ms a second) and `delay(100)` in `setMode()` at
  boot; `VL53L1X::init()` waits with its own timeout. These are inside the
  vendor libraries. Replacing the methane library with direct register reads
  would remove them, but the register map is not documented here.
- Checked by `grep`: no `delay(` calls remain in `src/` (only a comment saying so).

### 2.9 Modem on demand

- Old behaviour: powered at boot, kept on, polled every 10 s.
- New: off by default. A session is requested (`modem.request()`) when a send is
  due (`MODEM_SESSION_PERIOD_MS`, now 30 min), by `set MODEM_WAKE 1`, or
  by a console `at` command. The state machine is described in hardware.md.
- After the work, the modem idles 5 s (`MODEM_LINGER_MS`), then `AT+CPOF`, then
  the rail is cut.
- A failed start (no `AT` answer in 20 s) cuts power, reports
  `SESSION_FAILED`, and the next period tries again. Without that, a dead modem
  would block all later sends.
- Console `at` is asynchronous: it queues the command and the reply is printed
  when it arrives (see field-configurator.md).
- **Honest limits:** `AT+CPOF`, `AT+COPS=3,2` and the whole session are
  untested on the board. The earlier run could power the modem and get `OK` to
  `AT`, but registration never happened.
- **The send** is described in section 2.12. (Earlier versions only printed the
  frame with `TCP send is not implemented yet`.)

### 2.10 Cell block

- Source: `AT+COPS?` with the numeric format set by `AT+COPS=3,2`, e.g.
  `+COPS: 0,2,"52003",7`. The quoted PLMN is 5 or 6 digits: the first 3 are the
  MCC, the rest the MNC (so `52003` gives MCC 520, MNC 3).
- Only read while the modem is in a session. Kept in RAM; the frame uses the
  last known values and sets the CELL flag (bit 6) once they are known. Before
  the first successful session the flag stays clear.
- If the network never registers (the current bench state), there is no operator
  in the reply and the cell block stays absent.
- Network time (`AT+CCLK?`) is also taken in the session and used to set the
  system clock when neither GNSS nor an earlier sync has set it.

### 2.11 Telemetry v2 fields

- `docs/telemetry.md` was updated by the owner: the sensor body gained
  `esp_temperature` (int16, degrees C x 100, signed, after `battery_voltage`),
  and the frame gained a 2-byte trailer, always present and always last, after the
  optional GPS and cell blocks: `signal_strength` (int8 dBm) and `reset_reason`
  (uint8, `esp_reset_reason_t`). Frame length is now `24 + (2 + body) x count +
  gps + cell`, with `body = 21` (25 with RAW).
- Firmware: `Reading.espTemperatureC` is filled from `temperatureRead()`;
  `Location.signalDbm` comes from the modem's CSQ (`-113 + 2 x csq`) and
  `Location.resetReason` from `esp_reset_reason()`.
- **Assumption:** when CSQ is 99 (unknown) `signal_strength` is sent as 0. The
  document does not define an "unknown" value; confirm with the server side.
- **Doc nit (open):** the `reset_reason` row in telemetry.md has `power*on`,
  `external*pin` and `unknown*<code>`; the underscores were mangled into
  asterisks by a formatter.
- Not yet re-checked against the documented hex example on a PC, and the
  worked example in telemetry.md should be checked for the new fields.

### 2.12 TCP sender

- New `Job::SEND` in the modem handler (non-blocking, same command engine, which
  can now wait for a given reply such as `>` instead of only `OK`). Sequence:
  `AT+NETOPEN` (`+NETOPEN: 0`, or "already opened"), `AT+CIPOPEN=0,"TCP",host,port`
  (`+CIPOPEN: 0,0`), `AT+CIPSEND=0,len` (wait for `>`), the frame bytes in one
  write (`+CIPSEND:`), then the ack, then `AT+CIPCLOSE=0` and `AT+NETCLOSE`.
  Cleanup runs after a failure too.
- Ack: the server's 1-byte `0x00` is read from the `+IPD1` notification. Verified
  on the A7670E-LASE: received data arrives as `RECV FROM:<ip>:<port>`, `+IPD<len>`,
  then the data bytes (not `+RECEIVE,<link>,<len>`, which this firmware first
  assumed and which never matched). Any other byte, or nothing within 20 s, is a
  failure.
- Flow in `main.cpp`: a PULL session finishes, the frame is built with the fresh
  cell info and signal strength, `modem.requestSend()` queues it (the modem is
  still in its 5 s linger), and `SEND_DONE` / `SEND_FAILED` comes back. On
  `SEND_DONE` the readings that were in the frame are removed from the buffer;
  readings taken during the upload stay. On failure nothing is removed and the
  next session retries. If the buffer is full during an upload, the sent count is
  reduced when the oldest reading is dropped.
- Server host and port come from the `SERVER_IP` / `SERVER_PORT` settings.
- **Not verified.** Written from knowledge of the A7670E AT manual, not run.
  Neither the build nor a PC check was possible (PlatformIO would not run on
  Python 3.14, and is not installed for 3.13). The URC formats are the main risk.
- **Differences from the old firmware:** the old one read the ack with
  `AT+CIPRXGET` and kept a retry cache; this uses the unsolicited `+RECEIVE` and
  simply keeps unsent readings in RAM (lost on reset, no flash cache).

### 2.13 Dev mode

- Normal timing: reading every 5 min, upload every 30 min (open item 10).
  `set DEV_MODE 1` switches to a reading every 1 min and an upload every 5 min
  (`DEV_SAMPLE_PERIOD_MS`, `DEV_SESSION_PERIOD_MS` in `config.h`).
- Not saved to flash, like `LOG`: a reset returns to normal timing, so a unit
  cannot be left in dev mode in the field by accident. `DEV_MODE_DEFAULT` in
  `config.h` makes a bench build start in dev mode.
- Takes effect at once; the next reading / upload is measured from the last one.
- **Status LED:** in dev mode the dev board's single NeoPixel shows what the
  firmware is doing (off outside dev mode): white flash 0.3 s = reading taken,
  steady amber = modem powered, green 2 s = upload acked, red 4 s = session or
  upload failed, dim blue tick every 2 s = idle. Uses the core's
  `neopixelWrite()`, no library; brightness is `DEV_LED_LEVEL` (24/255).
- **Assumption:** the pin is `RGB_BUILTIN` from the board package (GPIO48 on
  DevKitC-1 v1.0, GPIO38 on v1.1); change `DEV_LED_PIN` in `config.h` if the
  LED stays dark. The production board has no such LED.
- Not built or run.

### 2.14 Battery saver

- A low battery stretches both periods: below 30 % (`BATT_LOW_PERCENT`) they are
  multiplied by 2 (normal: reading every 10 min, upload every 60 min); below
  15 % (`BATT_CRITICAL_PERCENT`) by 4 (20 min / 120 min). Applies in dev mode
  too, so it can be tested on the bench.
- Hysteresis: to leave a level the battery must rise 3 % above its threshold
  (`BATT_RECOVER_MARGIN`). A level change is logged with the new periods.
- No fresh battery reading (read failed) keeps the current level; the start
  level is normal.
- The battery handler reads about once a minute; the level follows its latest
  value, and a new period takes effect from the last reading / upload.
- **Caution:** with no battery connected the reading may be near 0 %, which
  would put a bench unit in CRITICAL (x4). Battery channel and calibration are
  still open (item 2).
- Thresholds and multipliers are guesses, not from the owner or the power
  budget. Not built or run.

## 3. Bench results to date

| Check | Result |
|-------|--------|
| I2C scan | 5 devices, as expected |
| VL53L1X | tracks movement, 0-230 mm seen, one "range not valid" |
| SHT30 | about 23.6 C, 41 % RH |
| SCD40 | about 850-890 ppm, temperature settling down |
| MH-Z9041A | 0.00 %LEL, 33 C, error 0 |
| ADS1115 | AIN0 about 0 V; AIN1 decaying (floating input); AIN2/3 about 0.12 V |
| GNSS | NMEA parsed, 0 satellites, no fix indoors |
| Modem | boots, answers AT, SIM ready, CSQ 13-14, not registered |

Those results are from the **earlier blocking version** of the code. Not yet
run on the board: the non-blocking rewrite, on-demand modem sessions and
shutdown, cell block, device ID print, settings console, battery, moisture,
frame builder, the v2 fields (2.11) and the TCP sender (2.12).

One thing verified on a PC for the rewrite: the date-to-epoch function used for
GNSS and modem time (`timeutil.h`) matches Python's `calendar.timegm` for six
dates, including a leap day and the year-2038 boundary.

## 4. Open items

1. **Modem not registering.** Flash and read the `CFUN` / `COPS` values in the
   session log (`set MODEM_WAKE 1`). Without registration there is no cell
   block and no network time.
2. **Battery ADC channel** and the sense/solar pins: confirm with a battery
   connected (hardware.md).
3. **Moisture calibration:** needs dry and wet voltage readings for each
   sensor; a two-point `set` command could be added.
4. **TCP sender:** written (2.12) but never built or run. Needs a build, a test
   against the real server, and a check of the module's reply formats. No
   persistent retry cache (unsent readings are lost on reset).
   Also: the APN default is empty; without a working APN `NETOPEN` fails.
   Also: decide the `signal_strength` value for "unknown" (2.11) and fix the
   mangled underscores in telemetry.md.
5. **Run the whole new code on the board.** The non-blocking rewrite, modem
   shutdown (`AT+CPOF`), the cell block and the session timing have only been
   compiled. Check in particular that the SHT30, SCD40 and ADS1115 still read
   the same values as before.
6. **MCU sleep and power management** are not written; the board runs
   continuously (the modem is the only part powered down).
7. **J4 schematic text** still says "MH-Z19 CO2"; the wiring is correct
   (owner-confirmed), only the note is stale.
8. **Vendor-library delays** (methane sensor reads, `VL53L1X::init()`) still
   block briefly (section 2.8).
9. **Hardcoded values in comments:** code reads everything from `config.h`,
   but several comments quote GPIO numbers and addresses; update them if a pin
   changes.
10. **Intervals.** Set by the owner: one reading every 5 min
    (`TELEMETRY_SAMPLE_PERIOD_MS`) and one upload every 30 min
    (`MODEM_SESSION_PERIOD_MS`), so about 6 readings per frame (buffer holds 24,
    about 2 h of offline data). The first upload comes 30 min after boot. Power
    budget not checked.
11. The AIN1 drain and the AIN2/AIN3 level (about 0.12 V, stable) are explained
    only as floating inputs; not investigated further.

## Offline resilience (backlog, retry, watchdog)

- **Flash backlog.** Unsent readings survive a reset (`/backlog.bin`, mirror of the
  24-reading RAM buffer). When RAM is full the oldest reading moves to a flash spill
  queue (`/spill.bin`, up to `BACKLOG_MAX_READINGS`, default 2016 = 7 days at 5 min)
  instead of being dropped. Uploads send the spill first, 24 readings per frame; after
  an ack the next chunk goes out in the same modem session until the backlog is empty.
  Past the cap the oldest 24 are dropped.
- **Retry backoff.** A failed session or upload retries after `MODEM_RETRY_BASE_MS`
  (1 min), doubling each failure (2, 4, 8 ...) up to the normal session period. Any ack
  resets it.
- **Watchdog.** Hardware task watchdog on `loop()`, `WDT_TIMEOUT_S` (60 s). A hang
  resets the chip; the cause appears as the reset-reason byte in the next frame.
- Not yet tested on hardware.
