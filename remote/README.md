# batmon-remote

A touch display for the battery monitor, running on the **ESP32-2432S028** ("Cheap Yellow
Display": classic ESP32, 2.8" 320×240 ILI9341 or ST7789, XPT2046 resistive touch) or the
**GUITION JC2432W328R** (the same ESP32-WROOM-32, ST7789 and XPT2046, wired differently),
or without touch on the **GUITION JC1732W319** (the same ESP32, a 1.9" 320×170 ST7789). It
connects to a `batmon-*` board over BLE and shows what the phone app's Monitor tab shows,
on a screen that can live next to the battery. A separate firmware from the monitor in the
repository root: different chip, different BLE role, its own `version.txt`.

## Screens

**Dashboard.** State of charge (large, with a ten-brick gauge), voltage,
current, power, the mode (CHARGING / DISCHARGING / IDLE / FULL / EMPTY) with the gauge's
own state, the battery chemistry and its capacity under it, and the time to empty or to
full. Below, the
monitor's SoC history, tap-cycled between full span, half and a quarter, with the
board's temperature,
humidity and pressure on the line above it. **Tap the graph** to change the span, and
**tap the header** for Settings.

- **Which one shows (to empty vs. to full) follows the instantaneous current**, the same
  reading the CHARGING/DISCHARGING label uses, so the two never disagree. The *duration*
  uses a one-minute moving average when it agrees on direction, so a load switching on
  and off doesn't make the number itself jump around; right after the direction changes,
  it falls back to the instantaneous reading rather than showing a stale, wrong-signed
  estimate for the ~60 s the average takes to catch up.
  - *To empty* is remaining charge ÷ current.
  - *To full* is (capacity − charge) ÷ current, using the learned capacity from `config`.
    It is optimistic near the end of a charge, when the current tapers.
  - Currents under 5 mA read as idle. The format is exact, `Dd HH:MM:SS` (e.g.
    `0d 03:14:07`), with no cap on days -- an implausible estimate from a near-zero
    current is then visibly implausible rather than rounded away.
- **Capacity** reads `CAP 44.0AH (43.97)`: the capacity the pack is *set* to
  (`soc.cap_uah`, its nameplate) and, in brackets, what the monitor has *measured* it to
  be (`soc.learned_uah`). The gap between them is the pack's state of health.
  - Until the gauge has measured the pack even once (`soc.learn_count` is 0) it reads
    `CAP 45.0AH (learning)`. Until then the "learned" figure is the nameplate copied,
    and printing it as a measurement invites the one reading it cannot support -- that
    the pack was checked and came out at exactly its rating.
  - The measured value carries two decimals and the set one a single decimal, matching
    the phone's Configure tab exactly: the two part company by a fraction of an Ah at
    first, and a coarser display calls them identical for the whole early life of a pack.
  - Both are computed on the monitor and only reported here -- two devices watching one
    pack must not quote two capacities.
- **The gauge is ten bricks, one per 10 %**, and the brick the pack is part way into
  **blinks until it is earned**: through 70.0-74.9 % the eighth flashes, from 75 % it is
  solid. A brick fades in across its band rather than appearing at one edge of it, so
  seven-and-a-blink says what the digits would otherwise have to be read for.
  - Solid bricks follow the nearest tenth: the top goes out below 95 %, the next below
    85, down to the first below 5. Truncating instead would drop the tenth brick at
    99 %, and a pack one percent off full showing nine tenths reads as a fault.
  - Below 5 % the first brick blinks rather than sitting solid -- a nearly flat pack
    should not claim a whole tenth -- and at a true zero nothing lights at all.
  - Unlit bricks stay visible as dim slots, so the lit ones read as a proportion rather
    than as a bar of unknown length.
  A mark through the middle of the frame splits it at 50 %: which side of the middle
  the lit bricks end on is faster to see than counting five of them, and half is the
  threshold most decisions about a lead-acid pack are made against.
- **The SoC colour is about the pack**, not the link: green from 50 %, yellow from
  30 %, red below. The number and the bar always take the same colour. Below **10 %**
  both blink, half a second each way -- by then the reading is a thing to act on, and
  a static red carries no more urgency across a room than a static green.
- Values turn grey when no telemetry has arrived for 5 seconds.
- **Communications, top right.** A Bluetooth icon, always blue, flashes white on every
  notification received and yellow on every command sent. With telemetry streaming it flickers
  steadily, and when it stops, the data has stopped. The link's state itself is the
  status text's colour instead (blue while searching, yellow while connecting or in
  setup, orange while pairing or with no data, green or cyan when it is working).
  - Beside it, two lines: **this display's own version** on top in grey, and under it
    **the monitor's firmware version followed by the packet rate**, e.g. `0.11.6 9/s`,
    in the link's colour. Position tells them apart -- there is no room to label two
    versions and a rate. A stalled stream shows `0/s`, where a resting battery's
    unchanging numbers alone would not show it, and which monitor firmware is answering
    decides what half of this screen can show at all (time estimates need 0.11.3, the
    history 0.9.0).
  - A working link carries no status *word*: the packets ticking up say it. Encrypted
    versus merely connected is the colour -- **green** authenticated, **cyan** not.
  - A five-bar signal-strength antenna sits between the status text and the icon, from
    this connection's own RSSI (read locally every second, not a round trip to the
    board): 5 bars ≥ −60 dBm down to 1 bar at −100 dBm.

**Settings** (tap the header). Seven rows:

- **Board**: opens Devices, to connect to a different monitor.
- **Zero current**: `cal zero i`. The load must be disconnected. About 35 s.
- **Zero voltage**: `cal zero v`. VBUS must be tied to ground, not just disconnected.
  About 70 s.
- **Measured current**: type what your meter reads, and the board solves the current gain
  (`cal top i`). At least 10 mA, measured in the same direction as the board. About 17 s.
- **Measured voltage**: the same for voltage (`cal top v`). Take the reading at rest, with
  no load. About 17 s.
- **Firmware update**: puts this display in range of the phone app (see below).
- **Battery log**: the pack's black box, as the monitor has recorded it -- chemistry and
  cells, the nameplate against the measured capacity, state of health, equivalent full
  cycles, and the lifetime amp-hours in and out. Every figure is the monitor's, kept in
  its flash across every kind of restart; nothing here is computed on the remote, since
  two devices watching one pack must not arrive at two histories. Needs monitor firmware
  0.11.15 for the cycle and throughput counters.

The measured values use a keypad with a decimal point and a sign toggle. **USE** fills in
the board's own live reading as a starting point to type over. Every calibration goes
through a confirmation screen that states its precondition, because the firmware cannot
check any of them, and a zero point taken with current flowing ruins the offset.

At the bottom, the Settings screen shows the running command with its elapsed time, then
the board's answer. A refusal is shown verbatim (for example "too noisy -- current was
flowing"), since it names the physical cause. Calibration results are saved on the board
automatically. The board sends a command's output only once it has finished, so there is
no live progress, just the elapsed time against the expected duration.

**Devices.** The boards in range, strongest first, with signal strength and which one is
saved and connected. Tap one to switch to it; it is remembered. **FORGET** (tap twice)
drops the saved board and every pairing bond. **BACK** returns to Settings.

**Status LED** (JC2432W328R), at a tenth of full brightness (PWM, 10 % duty: full drive glares). **Red** while there is no connection to the monitor. **Blue**
blinks with Bluetooth traffic, like the header's icon: a short blink for every notification
received, a longer one for every command sent. A steady blue flicker means a working link.

**Pairing keypad.** Appears by itself when the board asks for pairing: type the six digits
the board shows on its OLED.

### Without touch: the JC1732W319

The 1.9" board has 170 rows and no touch, so none of the touch screens are compiled in.
Everything they do is on the console (below).

- **The dashboard is the same down to the readings.** Below them, the SoC graph is
  replaced by the **mode line**, the full width of the screen. It shows the same history
  as the graph, but each column only takes the colour of the gauge's state at that time:
  green charging, orange discharging, cyan full, red empty, grey resting. Hour divisions
  are gaps in it. The label above it reads `MODES 24h /6h`.
- **Pairing** shows a screen saying where the digits go: `passkey 123456` on the console.
- **Firmware update** opens with `update on` and closes with `update off`. KEEP and ROLL
  BACK are `ota confirm` and `ota rollback`.

## Console

Every board has a console on its USB serial port (115200, `.\tools\monitor.ps1 -Port COMn`).
The same commands also run over the remote's BLE console, the one the phone app uses for
updates. On a touch board it is a second way to do what the screens do; on the JC1732W319
it is the only way.

| Command | Does |
|---|---|
| `status` | the link and the latest reading |
| `battery` | the pack's black box, as the monitor keeps it |
| `scan [seconds]` | lists the boards in range, numbered (default 5 s) |
| `connect <n \| name \| address>` | uses that board and remembers it |
| `forget` | drops the remembered board and every bond |
| `passkey <digits>` / `passkey cancel` | answers or refuses the monitor's pairing request |
| `monitor <command ...>` | runs a command on the monitor (CLI.md) and prints its reply (up to its last 16 lines), e.g. `monitor cal zero i`. Calibration waits up to two minutes. |
| `update on` / `update off` | firmware update mode: visible to the phone app |
| `pin <gpio> <0\|1>` | drives a GPIO, for finding a board's wiring (refused on the flash and UART pins) |
| `ver`, `ota [...]`, `reboot`, `help` | as on the monitor (CLI.md §6) |

The calibrations behind `monitor cal ...` have the same preconditions as the Settings
screen states: no load for `cal zero i`, VBUS tied to ground for `cal zero v`, a meter
reading for `cal top i|v <value>`. The firmware cannot check any of them.

## BLE

The remote is a BLE central speaking the monitor's console protocol (CLI.md), like the
app. It subscribes, then sends `ver`, `config` and `stream csv`, and parses the `f` and `c`
records. It re-reads `config` every five minutes, because capacity is learned over time. It
changes nothing on the board except the calibration you run from Settings.

- **Which board.** On first boot it takes the first `batmon-*` it finds and remembers it.
  After that, switching is explicit (Devices). A dropped link is re-established by scanning
  for the remembered address.
- **Pairing.** With the monitor in `ble pair bonded` mode, the monitor refuses commands from
  an unpaired link. The remote then starts LE Secure Connections pairing. Its IO capability
  is keyboard-only and the monitor's is display-only, so the remote shows its keypad and the
  monitor shows its passkey. The bond is stored in NVS, so this happens once per board. If
  the monitor forgets the bond (`ble unpair`), the remote drops its own copy and pairs again.
- **Several displays.** The monitor accepts three connections, so a remote and the phone
  app can watch together. The stream is broadcast, and `stream csv` from any of them turns it
  on for all.

## SoC history

The graph is the monitor's own history (`hist`, CLI.md §6): 288 points at whatever
interval the monitor is set to -- 5 minutes covering 24 h by default -- which it keeps in flash. The
span comes from the reply (`interval_s` x `capacity`), so changing the interval on the
monitor changes the axis here with no update to this firmware.
- The remote fetches it on connect and every two minutes, so the graph is full as soon as
  it connects, whether the remote has just booted or not.
- The right edge is now, and each point sits where its age puts it. Gaps, from reboots or
  an unseeded gauge, stay gaps.
- Against a monitor older than 0.9.0, which has no `hist`, the graph says so.

## Environment

The `e` records supply temperature, humidity and pressure. They appear right-aligned
above the graph, e.g. `24.1C 46.2% 1003.5hPa`. A channel the board's sensor lacks is left
out (a BMP280 has no humidity), and nothing is shown without a sensor.

## Build and flash

The board is compiled in, so each one has its own build. `-Board` picks it:

| `-Board` | Board | Panel | Backlight | Touch bus |
|---|---|---|---|---|
| `cyd` | ESP32-2432S028R, single micro-USB | ILI9341 | 21 | its own (SPI3) |
| `cyd2usb` | ESP32-2432S028R, USB-C + micro-USB | ST7789 | 21 | its own (SPI3) |
| `jc2432w328r` | GUITION JC2432W328R | ST7789 | 27 | shared with the panel (SPI2) |
| `jc1732w319` | GUITION JC1732W319, 1.9" 320×170 | ST7789 | 21 | none |

```powershell
.\tools\idf.ps1 -Board jc2432w328r build        # Docker, like the monitor
.\tools\flash.ps1 -Board jc2432w328r            # finds the CH340
.\tools\flash.ps1 -Remote                       # the board read off the device itself
.\tools\idf.ps1 -Board jc2432w328r menuconfig   # "batmon remote display"
```

- **Each board builds separately**, into `remote/build/<board>/`, with its settings in
  `remote/sdkconfig.<board>`. A board's fragment, `boards/<board>.defaults`, picks it.
  `--project-dir remote` without `-Board` builds `cyd`.
- **`flash.ps1` can detect the board.** Without `-Board`, it resets the board and reads
  `board: <name>` from the boot log of the remote firmware already on it. That is the
  board the image was *built* for, so the first flash of each board needs `-Board`: a
  new board, one running other firmware, or one running remote firmware from 0.4.24 or
  older, which does not say. A CYD image runs on a JC2432W328R (dark, still logging
  `ILI9341 panel up`), so its panel line identifies the image, not the board.
- **`flash.ps1` checks the image.** It refuses a build whose sdkconfig was switched to
  another board in menuconfig: the wrong image gives a dark screen or dead touch.

`menuconfig` → **batmon remote display**:

- **Board.** Normally set by `-Board`. The pins and panel settings for each board are in
  `main/board.h`.
- **Rotate 180°.** Touch follows the picture.
- **Panel overrides.** *Invert colours the other way* fixes a negative picture, and *swap
  red and blue* fixes orange showing as blue. Both apply on top of the board's settings.
- **Touch calibration.** Raw XPT2046 limits, and an option to log every touch, raw and
  mapped, on the serial console. If taps land in the wrong place, turn the logging on, tap
  the corners, and put the raw values in. If touch is mirrored on one axis, swap that
  axis's MIN and MAX.

The serial console (`.\tools\monitor.ps1 -Port COMn`, 115200) logs what the dashboard
shows every 30 seconds:

```
I (31402) ui: batmon-DCFA live: 71.5 % 12.424 V -0.0091 A (avg -0.0091) RESTING, cap 44000 mAh, flooded 6S
```

## Firmware update over BLE

The remote updates from the phone app the same way the monitor does. Settings →
**FIRMWARE UPDATE** opens a screen that makes the remote visible as `batmon-remote-XXXX`.
In the app:
1. Tap the Bluetooth button and pick `batmon-remote-XXXX`. It is labelled "remote display —
   firmware update", and the app never connects to one on its own.
2. On the Firmware tab, tap **Update to remote-vX.Y.Z**, or flash a local
   `remote/build/<board>/batmon-remote.bin`.

The remote shows the transfer's progress itself. It restarts into the new image, reopens
this screen, and the app reconnects and confirms it.

- **Visible only on that screen.** Leaving it stops advertising, so the remote is not in
  every device list and takes no firmware from anyone nearby the rest of the time. A
  phone already connected stays connected.
- **Probation and rollback.** A new image boots on probation, straight into the update
  screen, so the app can find it again. It is kept when the app confirms it or when you
  tap **KEEP**. **ROLL BACK** returns to the previous image. So does a reset before
  either, and so does ten minutes without a confirmation.
- **Same protocol as the monitor.** The remote serves the monitor's Nordic UART service,
  framing, OTA characteristic and `ver` / `ota ...` / `reboot` commands (CLI.md §6). It
  also runs the monitor's own `ota.c`, which checks an image against this firmware's
  project name, so a monitor image is refused here, and a remote image there.
- **Each board takes only its own image.** Every image carries its board's name right
  after its app descriptor (`main/ota.h`, BOARD TAG), and the remote reports its board in
  `ver` (`board jc2432w328r`). The app downloads `batmon-remote-<board>.bin` for that
  board, and refuses a file built for another one. The remote's `ota.c` refuses it too,
  since the wrong board's image boots into a dark or negative screen. A remote running
  0.4.29 or older names no board: it was a CYD image, and the app gives it plain
  `batmon-remote.bin`, the `cyd` image.
- **The first time needs USB.** Remote firmware 0.1.0 had one app slot. `flash.ps1 -Remote`
  once installs the two-slot partition table and the rollback bootloader. The remembered
  board and pairing bonds survive it.

## Versions and releases

`remote/version.txt` is the remote's version, separate from the monitor's. It shows on
the boot splash and in the serial log. A release is tagged `remote-vX.Y.Z`. It builds
every board and attaches one image per board, `batmon-remote-<board>.bin`, plus
`batmon-remote.bin`, a copy of the `cyd` image for apps from before per-board images.
Each image's version, project and board tag are checked before anything is published:

```powershell
.\tools\release.ps1 -Remote -Bump patch -DryRun   # bump, build, verify; publish nothing
.\tools\release.ps1 -Remote -Bump minor           # commit, tag, push, publish
```

Remote releases are never marked "latest" on GitHub; that spot belongs to the monitor
firmware. The phone app finds the newest release of whichever series fits the connected
device, and installs it over BLE as described above.

## Board wiring (fixed on the PCB)

| | ESP32-2432S028R (`cyd`, `cyd2usb`) | JC2432W328R (`jc2432w328r`) | JC1732W319 (`jc1732w319`) |
|---|---|---|---|
| Display (SPI2) | SCLK 14, MOSI 13, MISO 12, CS 15, DC 2 | the same | the same; 170 rows, 35 down in the controller |
| Backlight | 21 | 27 | 21 |
| Touch | SPI3: CLK 25, MOSI 32, MISO 39, CS 33 | on the display's SPI2, CS 33 | none |
| RGB LED (common anode) | not used | red 4, green 16, blue 17 | none |

No pin list for the JC1732W319 is published. Its headers break out every usable GPIO
except 2, 13, 14 and 15, which is the GUITION panel bus. Its backlight was found with the
console's `pin` command.

IRQ (36) is not used on either board; the firmware reads the pressure instead.

## Layout

| File | Role |
|---|---|
| `main/link.c` | BLE central: scan, connect, discover NUS, subscribe, pair, the command conversation, telemetry parsing into one model |
| `main/ui.c` | The three screens; every field redraws only when its text changes |
| `main/lcd.c` | Panel init and drawing (rectangles, 5×7 text scaled, strip-rendered bitmaps). No framebuffer: 150 KB is too much of the ESP32's RAM beside the BLE stack |
| `main/board.h` | What differs between the boards: panel controller, MADCTL, inversion, backlight pin, touch bus |
| `main/console.c` | The console's commands, for the serial port and the BLE console alike |
| `main/led.c` | The status LED, on boards that have one: red while disconnected, blue blinking with BLE traffic |
| `main/touch.c` | XPT2046: pressure check, median of five conversions, taps on the press edge |
| `main/rcon.c` | The peripheral side: advertising in update mode, the small console (`ver`, `ota`, `reboot`) and the OTA characteristic |
| `../main/ota.c` | The OTA session and probation, shared with the monitor |
