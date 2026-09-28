# batmon-remote

A touch display for the battery monitor, running on the **ESP32-2432S028** ("Cheap Yellow
Display": classic ESP32, 2.8" 320×240 ILI9341 or ST7789, XPT2046 resistive touch). It
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
- **The gauge is ten bricks, one per 10 %.** A brick lights only when its whole tenth
  is in the pack -- a gauge that rounds up strands people -- so 76.9 % lights seven
  and the digits carry the rest. Anything above zero keeps one brick lit, because a
  pack at 9 % must not look identical to a flat one. Unlit bricks stay visible as dim
  slots, so the lit ones read as a proportion rather than as a bar of unknown length.
- **The SoC colour is about the pack**, not the link: green from 50 %, yellow from
  30 %, red below. The number and the bar always take the same colour. Below **10 %**
  both blink, half a second each way -- by then the reading is a thing to act on, and
  a static red carries no more urgency across a room than a static green.
- Values turn grey when no telemetry has arrived for 5 seconds.
- **Communications, top right.** A Bluetooth icon, always blue, flashes white on every
  notification received and yellow on every command sent. At 10 Hz telemetry it flickers
  steadily, and when it stops, the data has stopped. The link's state itself is the
  status text's colour instead (blue while searching, yellow while connecting or in
  setup, orange while pairing or with no data, green or cyan when it is working).
  - Beside it is the packet rate and **the monitor's firmware version**, e.g.
    `9/s 0.11.6`. A stalled stream shows `0/s`, where a resting battery's unchanging
    numbers alone would not show it. Which firmware is answering decides what half of
    this screen can show at all -- time estimates need 0.11.3, the history 0.9.0 -- and
    it is otherwise only findable from the phone app. The remote's *own* version is on
    its FIRMWARE UPDATE screen.
  - A working link carries no status *word*: the packets ticking up say it. Encrypted
    versus merely connected is the colour -- **green** authenticated, **cyan** not.
  - A five-bar signal-strength antenna sits between the status text and the icon, from
    this connection's own RSSI (read locally every second, not a round trip to the
    board): 5 bars ≥ −60 dBm down to 1 bar at −100 dBm.

**Settings** (tap the header). Five rows:

- **Board**: opens Devices, to connect to a different monitor.
- **Zero current**: `cal zero i`. The load must be disconnected. About 35 s.
- **Zero voltage**: `cal zero v`. VBUS must be tied to ground, not just disconnected.
  About 70 s.
- **Measured current**: type what your meter reads, and the board solves the current gain
  (`cal top i`). At least 10 mA, measured in the same direction as the board. About 17 s.
- **Measured voltage**: the same for voltage (`cal top v`). Take the reading at rest, with
  no load. About 17 s.

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

**Pairing keypad.** Appears by itself when the board asks for pairing: type the six digits
the board shows on its OLED.

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

```powershell
.\tools\idf.ps1 --project-dir remote build     # Docker, like the monitor
.\tools\flash.ps1 -Remote                      # finds the CYD's CH340
.\tools\idf.ps1 --project-dir remote menuconfig   # "batmon remote display"
```

`menuconfig` → **batmon remote display** has the settings that differ between boards:

- **Display controller.** ILI9341 for the original single-micro-USB board. ST7789 for the
  later board with USB-C plus micro-USB. The wrong one gives a mirrored or negative picture.
- **Rotate 180°.** Touch follows the picture.
- **Touch calibration.** Raw XPT2046 limits, and an option to log every touch, raw and
  mapped, on the serial console. If taps land in the wrong place, turn the logging on, tap
  the corners, and put the raw values in.

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
   `remote/build/batmon-remote.bin`.

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
- **The first time needs USB.** Remote firmware 0.1.0 had one app slot. `flash.ps1 -Remote`
  once installs the two-slot partition table and the rollback bootloader. The remembered
  board and pairing bonds survive it.

## Versions and releases

`remote/version.txt` is the remote's version, separate from the monitor's. It shows on
the boot splash and in the serial log. A release is tagged `remote-vX.Y.Z`, with
`batmon-remote.bin` attached:

```powershell
.	oolselease.ps1 -Remote -Bump patch -DryRun   # bump, build, verify; publish nothing
.	oolselease.ps1 -Remote -Bump minor           # commit, tag, push, publish
```

Remote releases are never marked "latest" on GitHub; that spot belongs to the monitor
firmware. The phone app finds the newest release of whichever series fits the connected
device, and installs it over BLE as described above.

## Board wiring (fixed on the PCB)

| | Pins |
|---|---|
| Display (SPI2) | SCLK 14, MOSI 13, MISO 12, CS 15, DC 2, backlight 21 |
| Touch (SPI3) | CLK 25, MOSI 32, MISO 39, CS 33 (IRQ 36 unused; pressure is read instead) |

## Layout

| File | Role |
|---|---|
| `main/link.c` | BLE central: scan, connect, discover NUS, subscribe, pair, the command conversation, telemetry parsing into one model |
| `main/ui.c` | The three screens; every field redraws only when its text changes |
| `main/lcd.c` | Panel init and drawing (rectangles, 5×7 text scaled, strip-rendered bitmaps). No framebuffer: 150 KB is too much of the ESP32's RAM beside the BLE stack |
| `main/touch.c` | XPT2046: pressure check, median of five conversions, taps on the press edge |
| `main/rcon.c` | The peripheral side: advertising in update mode, the small console (`ver`, `ota`, `reboot`) and the OTA characteristic |
| `../main/ota.c` | The OTA session and probation, shared with the monitor |
