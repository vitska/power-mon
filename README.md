# bat-monitor

BLE battery monitor for the Seeed XIAO ESP32-C6 with an INA219 shunt sensor.
Coulomb-counting fuel gauge, JBD/Xiaoxiang-compatible BLE, OLED + button UI,
temperature-corrected. Full design: [DESIGN.md](DESIGN.md).

**Current state: M1 (bring-up) — I²C, INA219 driver, bring-up console.**
Nothing is integrated, persisted or transmitted yet.

## Milestones

| | Milestone | State |
|---|---|---|
| M1 | I²C + INA219 driver + console dump | **done, builds clean; not yet run on hardware** |
| M2 | PGA auto-range, offset/gain calibration, deadband | driver support present, procedure not automated |
| M3 | Integrator, SoC, anchor state machine, NVS | not started |
| M4 | BLE / JBD emulation | not started |
| M5 | Battery Service + vendor service | not started |
| M5b | OLED + button + BME280 + temperature corrections | not started |
| M6 | Low power (tiers down to the light-sleep floor, measurement) | not started |
| M7 | OTA, docs | not started |

## Build

Requires **ESP-IDF v5.3 or newer** (the ESP32-C6 needs ≥5.1; the `i2c_master`
driver used here needs ≥5.2).

### Option A — Docker (no local IDF install)

Wrappers in `tools/` run the official `espressif/idf` image with the project
bind-mounted at `/project`, so `build/`, `sdkconfig` and all artefacts land on
the host and persist between runs. The first run pulls ~2.5 GB.

**Windows (PowerShell):**

```powershell
.\tools\idf.ps1 set-target esp32c6     # once
.\tools\idf.ps1 build
.\tools\idf.ps1 menuconfig             # pins, shunt value, address
.\tools\flash.ps1 -Port COM5 -Monitor
.\tools\monitor.ps1 -Port COM5
```

**Linux / macOS / Git Bash:**

```sh
./tools/idf.sh set-target esp32c6
./tools/idf.sh build
./tools/idf.sh menuconfig
BATMON_PORT=/dev/ttyACM0 ./tools/idf.sh flash monitor   # not on Windows, see below
```

> **Flashing is not done from the container on Windows.** Docker Desktop has no
> COM-port passthrough, so `idf.py flash` inside the container cannot see the
> board. Build in Docker, flash from the host with `tools\flash.ps1`, which needs
> only pip packages:
>
> ```powershell
> python -m pip install esptool esp-idf-monitor
> ```
>
> It reads `build/flash_project_args` — written by the build, listing every image
> and its offset — so the partition layout can change without touching the script.
> Works with esptool 4.x and 5.x (5.0 hyphenated the subcommands; the script
> detects the version).
>
> On Linux and macOS a serial device *can* be passed in: set `BATMON_PORT` and
> `tools/idf.sh` adds `--device`, so `flash` and `monitor` work in the container.

Raw equivalent, if you would rather not use the wrappers:

```powershell
docker run --rm -it -v "C:/Temp/personal/esp32/bat-monitor:/project" -w /project `
    espressif/idf:release-v5.3 idf.py build
```

Note the **forward slashes** in the mount path. Docker's `-v` parser rejects
Windows backslashes with the thoroughly misleading error
`docker: invalid reference format`, which reads as though the image name were
the problem.

Pin a different IDF version with `$env:BATMON_IDF_IMAGE` / `BATMON_IDF_IMAGE`.

### Docker troubleshooting

Both of these were hit while bringing the wrappers up, and neither error names its
real cause.

**`docker: invalid reference format`** — the `-v` mount path contains Windows
backslashes. The message points at the *image reference*, so it reads as though
the tag were wrong. Use forward slashes: `C:/Temp/...:/project`. The wrappers do
this conversion for you.

**CMake fails during configure with `configure_file: No such file or directory`,
usually on `build/CMakeFiles/<ver>/CMakeCCompiler.cmake`** — a transient write
failure on Docker Desktop's Windows bind mount. The giveaway is that the *sibling*
files (`CMakeCXXCompiler.cmake`, `CMakeASMCompiler.cmake`) were written correctly
into the same directory, which rules out permissions, a missing template or a bad
path. Delete the build directory and build again:

```powershell
Remove-Item build -Recurse -Force
.\tools\idf.ps1 build
```

If it recurs often, move the build directory off the bind mount into a Docker
named volume and copy out only what flashing needs (`bootloader/bootloader.bin`,
`partition_table/partition-table.bin`, `ota_data_initial.bin`, `bat-monitor.bin`,
`flash_project_args`, plus `bat-monitor.elf` for the monitor). Windows bind mounts
are also markedly slower than a volume for CMake and ninja, which touch thousands
of small files.

**Build is verified.** A clean containerised build of the M1 tree completes at
1023/1023 targets with no warnings from any file in `main/` or
`components/ina219/`; `bat-monitor.bin` comes out at ~242 KB, 87 % of the app
partition free.

### Option B — local ESP-IDF

```sh
. $IDF_PATH/export.sh          # Windows: %IDF_PATH%\export.bat
idf.py set-target esp32c6
idf.py menuconfig
idf.py build flash monitor
```

Exit the monitor with `Ctrl-]`.

## Wiring (M1)

| INA219 | XIAO ESP32-C6 | Note |
|---|---|---|
| VCC | 3V3 | |
| GND | GND | |
| SDA | **GPIO22 (D4)** | `CONFIG_BATMON_I2CA_SDA_GPIO` |
| SCL | **GPIO23 (D5)** | `CONFIG_BATMON_I2CA_SCL_GPIO` |
| VIN+/VIN− | across the shunt | Kelvin connection |
| Vbus | pack + (≤26 V) | via a divider above 26 V |

> **One shared bus.** D4/D5 carry the INA219s *and* the OLED and temperature
> sensor — the schematic wires them together, so the firmware does too
> (DESIGN.md §2.5). Addresses keep them apart: 0x40, 0x41, 0x3C, 0x76.
>
> The cost is the deep-sleep tier. `LP_I2C` is hard-wired to GPIO6/7 and cannot
> reach GPIO22/23, so nothing can keep counting with the main core off; the power
> floor is light sleep at ~220–400 µA instead of 40–90 µA (DESIGN.md §9.3). That
> is an accepted trade, not a bug. Moving just the two INA219 lines to GPIO6/7
> would buy it back.

### ⚠ Low-side wiring — read before connecting anything

The shunt goes in the pack **negative** lead (DESIGN.md §2.9). Two consequences that
will otherwise cost you a day:

1. **Size the shunt for a ≤100 mV drop, not 320 mV.** The INA219's inputs cannot go
   more than 0.3 V below their own ground, and a bidirectional low-side shunt always
   drives one input negative in one current direction. That caps the usable current
   range at about a third of the high-side figure — a 10 mΩ shunt covers ±10 A here,
   not ±32 A. `sense` and `shunt` both print the real limit and warn if you exceed it.

2. **The shunt must be the *only* connection between battery negative and system
   ground.** Any second path — chassis bond, a second charger, a shared negative —
   carries current *around* the shunt, and that current is simply never counted. There
   is no error for this; the readings look perfectly plausible and are wrong.

   **This includes the USB cable you are about to plug in for the console.** If the
   laptop's ground reaches the pack negative through anything else (a bench supply, a
   mains charger), the console cable becomes a parallel path across your shunt. During
   M1/M2, either use a USB isolator, or power the board from the pack and check the
   readings against a clamp meter.

Select the topology in `menuconfig` → bat-monitor → INA219 → *Shunt position*. The
default is low-side/L1, which keeps the monitor's own draw inside the measured loop and
applies the exact bus-voltage correction of §2.9.5.

Internal pull-ups are enabled by default so a bare sensor works out of the box.
They are weak (~45 kΩ) and marginal at 400 kHz; fit 4.7 kΩ externals and disable
`CONFIG_BATMON_I2CA_INTERNAL_PULLUPS` before trusting any measurement.

Set `CONFIG_BATMON_SHUNT_UOHM` to match your hardware. Most INA219 breakout
boards ship with **100 mΩ = 100000**; the design recommends a 10 mΩ external
shunt = `10000` (DESIGN.md §2.3). Getting this wrong scales every current reading
by the ratio and is the most likely cause of a confusing first run.

## Console

Connect at 115200 (or over native USB, which is the default) and press Enter.

| Command | Purpose |
|---|---|
| `scan` | I²C bus scan, with hints for misplaced devices |
| `read` | One sample, with raw registers and the active range |
| `stream <on\|off\|ms>` | Periodic one-line dump |
| `stats [reset]` | Mean / stddev / min / max over the window, plus error counters |
| `zero [n]` | Zero-current calibration (§5.5) — **load disconnected** |
| `shunt [uohm]` | Show or set shunt resistance; prints resulting range and resolution |
| `gain [ppm]` | Gain trim, restricted to ±10 % |
| `offset [uA]` | Show or set the current offset directly |
| `pga <auto\|1\|2\|4\|8>` | Lock or release the range |
| `sense` | Low-side settings: current sign, bus-voltage compensation, PGA ceiling |
| `profile <continuous\|triggered>` | Switch sampling profile (§4.1) |
| `shunt loc <p\|n\|single\|auto>` | Which lead the shunt is in (§2.10.3) |
| `curve` | Show both conversion curves; see below |
| `disp [on\|off\|screen n\|contrast v]` | OLED debug screens (§9.11) |
| `ble` | BLE console status, MTU, traffic counters |

Nothing set from the console persists across a reboot. Persistence arrives with
the NVS layout in M3; until then, write your calibration values down.

### Shunt location

Two things about a shunt are adjustable and they now live under one command:

```
shunt              # resistance, full scale, resolution, and the location
shunt 100000       # set resistance to 100 mOhm
shunt loc n        # shunt is in the negative lead (low-side)
shunt loc p        # ... the positive lead (high-side)
shunt loc auto     # resolve by observation -- needs a load, then run `detect`
```

`sensors mode <...>` still works and does exactly the same thing; both drive one
piece of state.

### Conversion curve

Both channels convert as `raw -> scale -> offset -> gain`. `curve` shows and sets
every term:

```
curve                        # show both channels
curve i offset -1400         # current zero, in uA (or use `zero`, which measures it)
curve i gain 1002500         # +0.25%, in ppm
curve i ref 2000000          # solve gain so the present reading equals 2.000000 A
curve v divider 196608       # 3.0:1 external divider, Q16.16
curve v offset 12000         # bus-voltage zero trim, in uV
curve v ref 12600000         # solve gain against a 12.600000 V bench reference
curve reset [i|v]            # back to unity gain and zero offset
```

`ref` takes one reference point and solves gain, averaging 64 samples (override with
a trailing count). It deliberately refuses three cases rather than fitting nonsense:
a reference near zero, where offset error dominates the ratio; a sign mismatch
between reading and reference; and a solved gain outside ±10 %, which means the shunt
resistance or the divider ratio is wrong, not the gain.

The intended order is **`zero` first, then `curve i ref`**: `zero` fixes the offset at
the origin where it can be measured properly, and `ref` then fixes the slope at a
real working point. That is a two-point calibration with each point taken where it is
trustworthy.

## BLE console

The same console is exposed over Bluetooth as a Nordic UART Service, so a shunt
bolted into an awkward corner of a pack is still reachable. Connect with nRF Connect,
Serial Bluetooth Terminal, or anything that speaks NUS.

| | |
|---|---|
| Advertised name | `batmon-XXXX` (last two MAC bytes) |
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` |
| RX (write commands here) | `6E400002-...` |
| TX (subscribe for output) | `6E400003-...` |

**Enable notifications on TX** — without that, commands run and the output goes
nowhere. `ble` on the USB console reports whether a central is connected *and*
subscribed, precisely because those are different states that look the same.

> **There is no pairing.** Anything in radio range can run every command, including
> `zero` and `curve`. That is fine for a bench milestone and is not fine in the
> field; bonding is DESIGN.md §8.5 at M5. Turn it off in menuconfig for anything
> resembling a real installation.

Commands run on a worker task, not on the BLE host task, so a slow command such as
`zero 512` cannot drop the connection while it runs. Output is chunked to the
negotiated MTU and truncated at 2.5 KB per command, with a marker when that happens.

## M1 acceptance procedure

Exit criterion (DESIGN.md §11): **V and I match a bench meter within 1 % over
±2 A.**

1. `scan` — confirm **both** sensors answer: 0x40 (positive pole) and 0x41
   (negative pole). One address only means a mis-strapped A0/A1 or a dead part;
   the firmware can run single-sensor but says so (DESIGN.md §2.10.7). The OLED
   at 0x3C and the temperature sensor at 0x76/0x77 may also appear — they share
   this bus and are not yet driven.
2. `shunt` — confirm the printed full scale and resolution match your hardware.
3. With the load disconnected: `zero 512`. Expect a rejection if anything is
   still drawing current; that rejection is the check working.
4. `stats reset`, wait a minute, `stats`. With no load, the mean should be within
   a few counts of zero and the stddev should be about one count. A larger stddev
   means noise pickup — check the Kelvin connection and the shunt wiring before
   going further.
5. Apply a known load. Compare `read` against a series-connected bench meter at
   roughly ±0.1 A, ±0.5 A and ±2 A, in both directions.
   - A constant error at all currents is **offset** → re-run `zero`.
   - An error proportional to current is **gain**, i.e. the shunt value is wrong.
     Compute the true resistance from the ratio and set `shunt`; only use `gain`
     for the residual trim after that.
6. Compare the bus voltage against the meter at two points.
7. `stats` — the range-discard count should be small relative to the sample
   count, and bus errors should be zero.

Record the resulting shunt, offset and gain. M3 will read them from NVS instead.

## Layout

```
CMakeLists.txt          top-level project
partitions.csv          DESIGN.md §6.2 layout, fixed now so it never moves
sdkconfig.defaults      target, partition table, console device
main/
  main.c                init, sampler task, statistics, zero calibration
  console_cmds.c        bring-up console
  app_ctx.h             shared M1 state
  fixed_fmt.h           integer fixed-point formatting (no floats, §4.3)
  Kconfig.projbuild     pins, shunt, bring-up options
components/
  ina219/               register-level driver, PGA auto-ranging, raw→SI
tools/
  idf.ps1  idf.sh       run idf.py in the espressif/idf container
  flash.ps1             host-side esptool flash (Windows has no COM passthrough)
  monitor.ps1           host-side serial console
```

Components still to come — `fuelgauge`, `nvstore`, `ble_svc`, `lp_gauge`,
`bme280`, `ui`, `config` — are specified in DESIGN.md §3.2 and are deliberately
absent rather than stubbed. An empty module is a claim that its interface is
settled, and none of them are.
