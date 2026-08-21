# bat-monitor

Battery monitor for the Seeed XIAO ESP32-C6 with dual INA219 shunt sensors.
Coulomb-counting fuel gauge with voltage re-anchoring, OLED readout, and the whole
console reachable over BLE. Full design: [DESIGN.md](DESIGN.md).

**Current state: M1–M2 complete and running on hardware, with the fuel gauge, the
display and a BLE console brought forward from later milestones.** Calibration and
gauge state persist in NVS. Verified against a 12 V / 44 A·h lead-acid battery.

Three documents, three jobs:

| | |
|---|---|
| [DESIGN.md](DESIGN.md) | why everything is the way it is |
| [CALIBRATION.md](CALIBRATION.md) | bench procedure: meter readings → firmware constants |
| [CLI.md](CLI.md) | command and protocol reference, sufficient to write a client |

## Milestones

| | Milestone | State |
|---|---|---|
| M1 | I²C + INA219 driver + console | **done**, verified on hardware |
| M2 | PGA auto-range, offset/gain calibration, deadband | **done** — guided two-point flow, persisted |
| M3 | Integrator, SoC, anchor state machine, NVS | **partly done** — gauge and calibration persist; event log and A/B slots outstanding |
| M4 | BLE / JBD emulation | not started — a NUS console exists instead |
| M5 | Battery Service + vendor service | not started; pairing (§8.5) **done** |
| M5b | OLED + button + BME280 + temperature corrections | **OLED and BME280 done**; button and §5.6 corrections outstanding |
| M6 | Low power (tiers down to the light-sleep floor) | not started |
| M7 | OTA, docs | docs in progress |

## What works today

- **Dual INA219**, one per pole, with roles derived from the install mode (§2.10).
- **Calibration**: guided two-point flow (`cal zero` / `cal top`), harness-drop
  correction, all of it written to flash and restored at boot.
- **Fuel gauge**: coulomb counting with I·R-compensated OCV re-anchoring, Peukert
  compensation, capacity learning, full/empty/rest anchors.
- **OLED**: SoC in large digits with volts and amps beside it, plus diagnostic screens.
- **Environmental sensor**: BME280 or BMP280 probed by chip ID, read once a minute,
  surfaced on the console, in the stream and on the panel.
- **BLE console**: the entire command set over Nordic UART Service, **up to three
  centrals at once**, with LE Secure Connections passkey pairing.
- **Live monitoring**: a repainting dashboard (`mon`), and grouped telemetry at three
  independent rates to both transports.

## Design changes since the original document

Three decisions changed once real hardware arrived. DESIGN.md records all three at
length; the short version:

1. **One I²C bus, not two.** The board wires the INA219s, the OLED and the temperature
   sensor together on D4/D5. `LP_I2C` is hard-wired to GPIO6/7 and cannot reach them,
   so **there is no deep-sleep tier** — T2 light sleep is the floor at ~220–400 µA
   instead of 40–90 µA (§2.5, §9.3). Accepted deliberately; two wires would reverse it.
2. **The SoC map needs its own voltage pair.** Appendix B's 1.80 / 2.40 V per cell are
   the discharge cutoff and absorption setpoint — the *operating* window. Used as a
   linear SoC map they read a rested 12.7 V battery as 53 %. So `v_0pct` / `v_100pct`
   hold the **resting-OCV** window and `v_full` drives full detection, separately.
3. **A current-dependent voltage error is not gain.** Two breakout shunts in series put
   ~0.4 V between the VBUS reference and the battery terminals under load. Absorbing
   that into a gain trim is right at one current and wrong everywhere else, so there is
   now an explicit harness-resistance term (`cal vpath`), distinct from the battery's
   internal resistance.

## Build

Requires **ESP-IDF v5.3 or newer** (the ESP32-C6 needs ≥5.1; the `i2c_master` driver
needs ≥5.2). BLE pulls in NimBLE, so the binary is ~769 KB — 57 % of the app partition
still free.

### Option A — Docker (no local IDF install)

Wrappers in `tools/` run the official `espressif/idf` image with the project
bind-mounted at `/project`, so `build/`, `sdkconfig` and all artefacts land on the host.
The first run pulls ~2.5 GB.

**Windows (PowerShell):**

```powershell
.\tools\idf.ps1 set-target esp32c6     # once
.\tools\idf.ps1 build
.\tools\idf.ps1 menuconfig             # pins, shunt value, display, BLE
.\tools\flash.ps1 -Port COM14 -Monitor
.\tools\monitor.ps1 -Port COM14
```

**Linux / macOS / Git Bash:**

```sh
./tools/idf.sh set-target esp32c6
./tools/idf.sh build
BATMON_PORT=/dev/ttyACM0 ./tools/idf.sh flash monitor   # not on Windows, see below
```

> **Flashing is not done from the container on Windows.** Docker Desktop has no
> COM-port passthrough. Build in Docker, flash from the host with `tools\flash.ps1`,
> which needs only pip packages:
>
> ```powershell
> python -m pip install esptool esp-idf-monitor
> ```
>
> It reads `build/flash_project_args` — written by the build, listing every image and
> its offset — so the partition layout can change without touching the script. Works
> with esptool 4.x and 5.x (5.0 hyphenated the subcommands; the script detects which).

Pin a different IDF version with `$env:BATMON_IDF_IMAGE` / `BATMON_IDF_IMAGE`.

### Option B — local ESP-IDF

```sh
. $IDF_PATH/export.sh          # Windows: %IDF_PATH%\export.bat
idf.py set-target esp32c6
idf.py build flash monitor
```

Exit the monitor with `Ctrl-]`.

## Toolchain troubleshooting

Every one of these was hit for real, and none of the errors names its true cause.

**Two builds at once corrupt the tree.** Symptoms are `file too short`,
`ELF section name out of range`, or `objdump: file format not recognized` — all of
which read like a broken toolchain. They mean two ninja processes wrote the same
archive. `tools/idf.ps1` now refuses to start if a container is already building this
project, using a per-project label rather than a lock file so a Ctrl-C leaves nothing
stale to clean up.

**`Could not open COM14, the port is busy or doesn't exist`** conflates two very
different faults. `tools/flash.ps1` now checks enumeration before invoking esptool and
says which it is, listing the ports that do exist. If the board is plugged in and
absent from that list, the USB-Serial-JTAG peripheral is wedged: hold BOOT, tap RESET,
release BOOT to reach the ROM bootloader, which always enumerates.

**`docker: invalid reference format`** — the `-v` mount path contains Windows
backslashes. The message points at the *image reference*. Use forward slashes; the
wrappers convert for you.

**CMake `configure_file: No such file or directory`** on
`build/CMakeFiles/<ver>/CMakeCCompiler.cmake` — a transient write failure on Docker
Desktop's Windows bind mount. The giveaway is that the *sibling* files were written
correctly, which rules out permissions or a bad path. Delete `build/` and rebuild.

**`%lld` prints the literal letters, or crashes the board.** `CONFIG_NEWLIB_NANO_FORMAT`
drops `%ll` support entirely, and it does not merely print wrong — it consumes the
wrong number of vararg bytes, so a following `%s` dereferences garbage. That boot-looped
this firmware once with a load-access fault. Narrow 64-bit values to 32 bits before
printing; `main/fixed_fmt.h` explains the convention.

## Wiring

| INA219 | XIAO ESP32-C6 | Note |
|---|---|---|
| VCC | 3V3 | |
| GND | GND | **tie all grounds** — see below |
| SDA | **GPIO22 (D4)** | `CONFIG_BATMON_I2CA_SDA_GPIO` |
| SCL | **GPIO23 (D5)** | `CONFIG_BATMON_I2CA_SCL_GPIO` |
| VIN+/VIN− | across the shunt | Kelvin connection |
| Vbus | pack + (≤26 V) | via a divider above 26 V |

> **One shared bus.** D4/D5 carry both INA219s *and* the OLED and temperature sensor.
> Addresses keep them apart: 0x40, 0x41, 0x3C, 0x76. The cost is the deep-sleep tier
> (§9.3) — see *Design changes* above.

**Tie the grounds.** An ungrounded VBUS floats to near the 3.3 V rail and reads as a
steady, plausible, completely wrong voltage — 3.448 V on this bench. The tell is the two
sensors disagreeing: `read` showing `pack voltage 3.448 V` beside `load voltage
0.000 V`. Identically wired sensors that differ mean a wiring fault, not a calibration
problem, and `cal zero v` refuses to absorb it.

**Check which shunt the chip is measuring.** Most INA219 breakouts carry their own
100 mΩ shunt across the VIN terminals. If that is the one in circuit, `shunt 100000` is
correct and 2 A produces 195 mV — which overruns the default `/4` ceiling and needs
`sense pgamax 8`. A saturated reading is reported as such rather than silently clipped.

### ⚠ Low-side wiring — read before connecting anything

The shunt goes in the pack **negative** lead (§2.9). Two consequences that will
otherwise cost you a day:

1. **Size the shunt for a ≤100 mV drop, not 320 mV.** The INA219's inputs cannot go
   more than 0.3 V below their own ground, and a bidirectional low-side shunt always
   drives one input negative in one direction. That caps usable current at about a third
   of the high-side figure. `sense` and `shunt` both print the real limit and warn when
   you exceed it — the `/8` ceiling is available and the warning is not idle.

2. **The shunt must be the *only* connection between battery negative and system
   ground.** Any second path — chassis bond, a second charger, a shared negative —
   carries current *around* the shunt, and that current is never counted. There is no
   error for this; the readings look plausible and are wrong.

   **This includes the USB cable for the console.** If the laptop's ground reaches pack
   negative any other way, that cable becomes a parallel path across your shunt. Use a
   USB isolator, or power the board from the pack and cross-check with a clamp meter.

Internal pull-ups are on by default so a bare sensor works out of the box. They are weak
(~45 kΩ) and marginal at 400 kHz; fit 4.7 kΩ externals and disable
`CONFIG_BATMON_I2CA_INTERNAL_PULLUPS` before trusting any measurement.

## Console

Connect at 115200 over native USB and press Enter. The same commands work over BLE —
see [CLI.md](CLI.md) for the framing a programmatic client needs.

| Command | Purpose |
|---|---|
| `ver` | Protocol and firmware version — machine-parseable |
| `mon [ms]` | Live repainting dashboard of the whole device state |
| `read` | One sample, with raw registers and the active range |
| `env` | Temperature, pressure and humidity from the BME/BMP280 |
| `stream [on\|off\|csv\|text\|fast <ms>\|calc <ms>\|env <ms>]` | Grouped telemetry, one rate per group |
| `profile <continuous\|fast\|triggered>` | Sampling profile, and therefore the rate ceiling |
| `stats [reset]` | Mean / σ / min / max over the window, plus error counters |
| `scan` | I²C bus scan, with hints for unexpected devices |
| `options` | Everything currently set, in one place |
| `soc [...]` | State of charge, endpoints, Peukert, capacity learning |
| `cal <zero\|top> <i\|v>` | Guided two-point calibration, saved to flash |
| `cal vpath <uV>` | Harness resistance, from a loaded terminal reading |
| `curve` | Every conversion term numerically |
| `shunt [uohm \| loc <p\|n\|single\|auto>]` | Resistance, or which lead it is in |
| `sense` | Sign, bus-voltage compensation, PGA ceiling |
| `detect [n]` | Resolve the topology by observation — needs a load |
| `profile <continuous\|triggered>` | Sampling profile (§4.1) |
| `disp [on\|off\|screen n\|contrast v]` | OLED screens (§9.11) |
| `ble [pair\|passkey\|bonds\|unpair\|disconnect]` | Link, pairing and bonds |

**Calibration, gauge state, the sampling profile and BLE pairing mode persist in flash**
and are restored before the first sample is taken. Everything else — stream rates,
screen selection — is RAM-only until the full config layer lands in M3. `options` says
which is which.

### Calibration in one paragraph

Every numeric argument is an integer in **micro-units**: 2.0134 A is `2013400`. Each
channel needs two points — `cal zero` fixes the offset with nothing applied, `cal top`
fixes the gain against a meter reading. Both write themselves to flash. The full
procedure, including the two-point algebra if you want to check it, is in
[CALIBRATION.md](CALIBRATION.md).

### Environmental sensor

`env` does a fresh forced-mode read and prints all three channels:

```
chip        BME280 at 0x76
temperature 27.02 C
pressure    1001.65 hPa
humidity    48.6 %RH
```

Either part works — the chip ID is probed (0x60 BME280, 0x58 BMP280) and humidity is
reported only if a BME280 answered. The address defaults to probing 0x76 then 0x77,
because SDO strapping decides between them and breakouts disagree about which they use.

The sampler reads it **once a minute** (§4.4 — thermal mass makes anything faster
pointless) and caches the value for the stream and the display; `env` bypasses the cache
so a person asking gets the current value rather than one up to a minute old.

> **It measures the board, not the cells** (§2.7). Every temperature correction in §5.6
> inherits that error, which is why each of them is individually switchable. A pack in a
> separate enclosure may be better served by no correction than by this one.

Absence is a configuration, not a fault: the gauge runs without it and the §5.6
corrections stay disabled rather than being guessed from the die sensor.

### Telemetry groups

Quantities change at different speeds, so they are streamed at different rates as three
prefixed record types:

| group | prefix | default | contents |
|---|---|---|---|
| fast | `f` | 100 ms (10 Hz) | voltage, current |
| calculated | `c` | 500 ms (2 Hz) | power, SoC, charge, state, OCV, Peukert |
| diagnostics | `d` | 1 s **and on change** | raw shunt drop, range, saturation |
| environmental | `e` | 10 s | temperature, humidity, pressure |

```
stream csv                 grouped records, headers re-emitted
stream fast 100            10 Hz voltage and current
stream calc 500            2 Hz derived values
stream diag 1000           1 s diagnostics, plus immediately on any change
stream env 10000           10 s environmental; also sets the sensor read cadence
stream env off             disable one group without touching the others
```

Records are never duplicated — a group emits only when its underlying sample is new — so
the true rate can be derived from the timestamps and trusted.

**A genuine 10 Hz needs `profile fast`.** Both ADC channels convert sequentially, so the
default 128× averaging caps the pair rate at 7.3 Hz; 64× averaging doubles it to 14.6 Hz
at roughly 40 % more noise per sample, which is a real trade against the 3 mA
integration deadband rather than a free speed-up. Measured: `stream fast 100` with
`profile fast` delivers ~9.2 Hz.

Full record schemas and the client protocol are in [CLI.md](CLI.md).

### Fuel gauge

`soc` shows and sets everything:

```
soc                     # SoC, state, charge, SoH, learning, Peukert, endpoints
soc cap 44000000        # 44 Ah
soc v0 11800000         # resting OCV at 0 %
soc v100 12700000       # resting OCV at 100 %
soc vfull 14400000      # absorption voltage for full detection
soc rint 6000           # internal resistance, for I*R -> OCV
soc peukert 294         # k = 1.15 in Q8; 256 disables
soc rest 600            # idle seconds before OCV is trusted
```

Defaults are seeded for a 12 V / 44 A·h flooded lead-acid. Three anchors restore
absolute reference: full charge (absorption voltage at taper current, held), empty
(compensated OCV at the 0 % endpoint under load), and resting OCV re-sync — which
*blends* at 25 % rather than snapping, so the display does not jump when a load goes
away.

The gauge is explicit about what it does not know. A SoC derived from voltage alone is
flagged `(from VOLTAGE only)` on the console and with a `?` on the panel, and a count
restored across a power cut reads `last anchor never since boot` until a rest period
re-syncs it.

> **Lead-acid surface charge is the trap here.** Straight off a charger a 12 V battery
> reads 13 V+ and takes hours to settle. With `soc rest` at ten minutes the gauge will
> anchor on that transient and drift toward 100 %. Raise it to hours for a real
> installation.

## BLE console

The whole console over Nordic UART Service, so a shunt bolted into an awkward corner is
still reachable. Connect with nRF Connect, Serial Bluetooth Terminal, or any NUS client.

| | |
|---|---|
| Advertised name | `batmon-XXXX` (last two MAC bytes) |
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX (write commands) | `6E400002-...` |
| TX (subscribe for output) | `6E400003-...` |

**Enable notifications on TX** — without that, commands run and the output goes nowhere.
`ble` reports connection and subscriber counts separately, precisely because those are
different states that look identical.

**Up to three centrals at once** — a phone watching telemetry while a laptop configures.
A command's reply is unicast to the client that sent it, so you never receive someone
else's `help` output; the telemetry stream is broadcast to every subscriber. Each
connection has its own line buffer, so simultaneous writes cannot splice into one
corrupt command. Stream rates are device state, not per-connection: if two clients set
different rates, the last one wins for both.

Responses are framed for machine use: an echo line, the output, `exit <n>`, then a
`0x04` terminator. The CSV stream reaches BLE as well as USB. Commands run on a worker
task, not the BLE host task, so a 35-second `cal zero i 512` cannot drop the link.

**Pairing** is LE Secure Connections with a six-digit passkey shown on the OLED
(`ble pair bonded`); bonds persist. The default is `open`, which means **no pairing at
all** — anything in range can run every command including calibration. That is a bench
setting, and `ble pair bonded` is one command away.

## Layout

```
CMakeLists.txt          top-level project
partitions.csv          §6.2 layout, fixed now so it never moves
sdkconfig.defaults      target, partitions, console, NimBLE
main/
  main.c                init, sampler task, statistics, zero calibrations
  console_cmds.c        the console: measurement, calibration, gauge, dashboard
  cal_store.c/.h        NVS-backed calibration (§6.1's cfg namespace, early)
  display_debug.c/.h    screens, including the large SoC readout
  ble_console.c/.h      bridges the console onto the BLE transport
  app_ctx.h             shared state, sensor lock, protocol constants
  fixed_fmt.h           integer fixed-point formatting (no floats, §4.3)
  Kconfig.projbuild     pins, shunt, display, BLE
components/
  ina219/               register-level driver, PGA auto-ranging, raw→SI, trims
  sensors/              dual-sensor roles, harness-drop correction
  fuelgauge/            counting, anchors, Peukert, capacity learning
  bme280/               BME280/BMP280, chip-ID probe, Bosch integer compensation
  ssd1306/              128×32 OLED, 6×8 and pixel-doubled text
  ble_serial/           NUS transport, pairing, bond store
tools/
  idf.ps1  idf.sh       run idf.py in the espressif/idf container
  flash.ps1             host-side esptool flash (Windows has no COM passthrough)
  monitor.ps1           host-side serial console
```

Still to come — `nvstore` (A/B slots, event log), `ble_svc` (JBD emulation),
`ui` (button gestures), `config` — are specified in DESIGN.md §3.2 and are deliberately
absent rather than stubbed. An empty module is a claim that its interface is settled.
`lp_gauge` will never exist: §9.3 records why.
