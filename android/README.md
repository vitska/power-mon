# Battery monitor — Android client

**Install:** download `battery-monitor-X.Y.Z.apk` from the newest `app-v…` release on
[GitHub](https://github.com/vitska/power-mon/releases) on the phone and open it. It
installs over an earlier version and keeps the saved boards.

A phone client for the battery monitor, speaking the Nordic-UART console described in
[`../CLI.md`](../CLI.md) — protocol **3**. Everything the console exposes is reachable
from here, in four tabs:

- **Monitor:** SoC labelled with the battery chemistry, charge remaining written as a
  fraction of the capacity the percentage is taken against (`34.33 of 45.00 Ah` --
  alone, that first figure reads as the pack's capacity, which is the one thing it is
  not), the monitor's SoC history
  (full span / half / quarter chart, ruled at every 25 % and on whole hours taken from
  the recording interval; gaps stay gaps), volts, amps, watts, gauge state,
  diagnostics, environment. The history is fetched on connect and every two minutes.
- **Configure:** a menu of nine pages, one screen each -- calibration, shunt and
  topology, battery, fuel gauge, SoC history, telemetry, display, BLE, read state.
  Every row carries a line of what the device currently holds for that page, so
  "is this board set up" is answered without opening any of them. One long scroll
  meant passing the calibration buttons twice on the way to the passkey.
  The Fuel gauge page states what has to happen before capacity is measured again --
  where the open span started, how much has come out of it, and the SoC the next
  reference has to be at or below ([CAPACITY.md](../CAPACITY.md)).
- **Console:** a raw command line for whatever is left.
- **Firmware:** the board's version and slot, the newest GitHub release, and the update
  over BLE, with rollback if the new firmware does not come back.

## Build

Needs JDK 17 (the Android Studio JBR works) and the Android SDK with platform 35.

```powershell
cd android
# once, if local.properties is absent:
"sdk.dir=C:/Users/<you>/AppData/Local/Android/Sdk" | Set-Content local.properties
$env:JAVA_HOME = "C:\Program Files\Android\Android Studio\jbr"
.\gradlew.bat assembleDebug
```

The APK lands in `app/build/outputs/apk/debug/app-debug.apk`. Or just open the `android`
folder in Android Studio.

To build, install on a USB-attached phone and launch in one go, from the repo root:

```powershell
.\tools\android.ps1                 # finds the JDK, SDK and adb itself
.\tools\android.ps1 -Serial <id>    # when several devices are attached
.\tools\android.ps1 -Logcat         # then follow the app's log
.\tools\android.ps1 -Reinstall      # signature mismatch: uninstall first (clears saved boards)
```

- Kotlin 2.0.21, AGP 8.7.2, Gradle 8.11.1, Compose BOM 2024.10.01
- `minSdk 26`, `compileSdk`/`targetSdk 35`

## Icon, name, releases

The launcher shows **Battery monitor** with a batteries icon. `art/ic_launcher_source.png`
is the source, and `art/make_icons.py` regenerates every density. It writes an adaptive
foreground scaled into the 66 dp safe circle, so no launcher mask crops it, plus legacy
square and round icons.

`.\tools\release.ps1 -App -Bump minor` publishes the APK:
- It bumps `versionName` and `versionCode`, builds the release APK, and reads the version
  back out of it with `aapt`.
- It publishes as `app-vX.Y.Z`, never as GitHub's "latest": that spot belongs to the
  monitor firmware, which the app itself looks up.
- Release builds are signed with this machine's debug key, so a downloaded APK installs
  over a development build without an uninstall (which would erase the saved boards).
  That's fine for a personal tool. Use a dedicated keystore before giving the app to
  anyone else.

## Layout

| File | Role |
|---|---|
| `ble/Protocol.kt` | The wire contract, transcribed from CLI.md: UUIDs, records, framing, micro-unit conversion, per-command timeouts. No Android APIs, so it is the part that can be reasoned about on its own. |
| `ble/BatmonClient.kt` | One GATT connection. Scan → connect → MTU → discover → subscribe, then commands serialised through a channel. |
| `model/MonitorViewModel.kt` | Folds the four record types into one `Telemetry`, runs the handshake, keeps a bounded transcript. |
| `ble/DeviceStore.kt` | The boards this phone has talked to, and which one it used last. |
| `ui/MonitorScreen.kt` | Live SoC, volts, amps, watts, gauge, diagnostics, environment. |
| `ui/DeviceSheet.kt` | Device picker: remembered boards merged with scan results. |
| `ui/ConfigureScreen.kt` | Calibration first, then every other setter, grouped as CLI.md groups them. |
| `ui/ConsoleScreen.kt` | Raw command entry and transcript. |
| `ble/Firmware.kt` | Firmware versions, reading an image's identity out of the `.bin`, and the GitHub release lookup. |
| `ui/FirmwareScreen.kt` | What the board runs, what is published, and the update itself. |
| `ui/HistoryChart.kt` | The SoC history from `hist`, placed by age, with gaps as breaks. |

## Several boards

The app talks to one board at a time but remembers all of them. The Bluetooth button in
the top bar opens the picker, which merges the boards this phone has connected to before
with whatever the current scan turns up; tapping one switches to it, dropping the open
link. A saved board with no signal reading is listed as "not seen in this scan", which is
the honest statement — it may be powered down, or just out of range.

Tapping a board connects to it **immediately**: any scan stops and the app dials that
address directly. Opening the picker does not scan either -- it lists the saved boards at
once, and scans only when there is nothing to list. Scan is for finding a board that is
not listed yet.

**The app keeps the chosen board connected.** If the link drops -- the board reboots after
a firmware update, you walk out of range, the stack hiccups -- it redials the same board,
backing off to one attempt every five seconds, until the board answers or you disconnect
or pick another. On startup it dials the board used last the same way. Only a fresh
install, with nothing remembered, scans, and then takes the first board found. A board is
remembered once it reaches a usable link, so one that fails at service discovery does not
become the thing the app chases on every launch.

Switching clears the panel. A voltage from the previous board displayed under a new
board's name would be worse than an empty readout, so telemetry and the handshake reset
and the next connection re-reads everything.

Identity is the MAC address, not the name. `batmon-DCFA` is derived from the MAC so it is
stable in practice, but the name is cached only so the picker can label a saved board
before a scan has found it.

## Calibrating from the phone

Calibration sits at the top of the Configure tab, because it is the reason to open that
tab while standing at the bench with a meter in hand; everything else there is set once
and left alone. The panel is laid out in the order the work happens:

1. **What the device reads now** — voltage, current, and raw shunt drop, live from the
   stream, so the meter reading has something to be compared against without leaving the
   screen. A saturated shunt channel says so here, since calibrating current against a
   range limit solves for the limit.
2. **Zero points** — `cal zero i` and `cal zero v`, the offsets. These come first: the
   offset is subtracted before the gain is applied, so solving a gain against an
   uncorrected offset bakes the offset into it.
3. **Known values** — type what the meter reads and the device solves the gain: current
   and voltage, each from one instant reading, applied immediately on Set.
4. **Save, refresh, erase** — and the device's own `cal` output underneath, re-read after
   every action rather than inferred from what the command said.

Every one of these is behind a dialog naming its physical precondition — load
disconnected, VBUS at ground, reading taken at rest, at least 0.5 A flowing. The firmware
cannot check any of them, and `cal zero i` run with current flowing poisons the offset
permanently.

## Three decisions worth knowing about

**Scanning filters by name, not by service UUID.** The 128-bit NUS UUID lives in the
*scan response* rather than the advertisement — a 16-byte UUID plus the device name does
not fit in 31 bytes — and a `ScanFilter` on service UUID misses scan-response-only UUIDs
on a good number of Android stacks. So the scan is unfiltered and matches the `batmon`
name prefix in the callback.

**`Ready` means subscribed, not connected.** Output to an unsubscribed client is
discarded, not buffered, and the command still executes — so the link is not reported
usable until the CCCD write has actually completed. Commands sent before that return
`null` rather than silently vanishing.

**Records route by prefix first, then to the in-flight command.** CLI.md offers the
simpler rule "while a command is in flight, everything up to the next `0x04` belongs to
that command", but taken literally that mis-files telemetry: the stream is asynchronous
and physically interleaves with a command's own output regardless of how long it runs.
`StreamParser`
checks for `f,` `c,` `d,` `e,` `#` first — which is CLI.md's own fallback rule — and
hands anything else to the pending response. Unknown *record* prefixes are dropped
rather than treated as errors, honouring CLI.md's promise that new record types may
appear without a protocol bump.

## What the UI insists on

Three things CLI.md §7 says to build in from the start, which are structural here rather
than cosmetic:

- **`sat=1` is not a reading.** A saturated shunt channel raises a banner saying the
  current figure is a range limit, not a measurement.
- **Calibration is never automatic.** Every `cal` action goes through a dialog that
  states its physical precondition — load disconnected, VBUS at ground, reading taken at
  rest — because the firmware cannot check those and a poisoned offset is permanent.
- **Configuration is never cached across connections.** Another client may have changed
  it, so the setter chips carry no selected state and the handshake re-reads on every
  connect.

Command output is shown verbatim. Apart from `ver` and the CSV stream, the console
speaks prose, and a refusal always names a physical cause (`too noisy -- current was
flowing`, `that is a real voltage, not an offset`) that is more useful to read than to
pattern-match.

## Battery chemistry

Configure → **Battery** picks the chemistry (flooded, AGM and gel lead-acid, LiFePO4,
Li-ion, LiPo, LTO, NiMH) and the number of cells in series, and sends `battery <chem>
[cells]` (CLI.md §6). Nothing is sent until Apply, behind a dialog, because applying
restarts the charge count. Left blank, the cell count is guessed on the board from the
present voltage. The Monitor tab labels the SoC with the chemistry it is computed for.

## Firmware updates

The Firmware tab compares the board's version with the newest GitHub release of
`vitska/power-mon` and offers the update only when the release is strictly newer;
reinstalling or going back is possible but behind a dialog. A local
`build/bat-monitor.bin` can be flashed too. Either way the image is checked before a byte
is sent: ESP-IDF images carry their chip ID and app descriptor (version, project name) at
fixed offsets, so a wrong file is refused on the phone, not discovered on the board.

**Remote displays too.** A remote display in its Firmware update screen appears in the
device list as `batmon-remote-XXXX`, labelled as such, and is never connected to
automatically. Connected to one, the Firmware tab looks in the remote's release series
(`remote-vX.Y.Z`, `batmon-remote.bin`) rather than the monitor's. It checks an image for
the classic ESP32 and the `batmon-remote` project, and the update runs exactly as for a
monitor. The other tabs say that a remote has nothing to show or configure. Releases are
found by listing them, not by asking for GitHub's "latest", which only ever points at the
monitor series.

The transfer is CLI.md §6: `ota begin` with size and SHA-256, the image in acknowledged
writes to the OTA characteristic, each prefixed with its offset, then `ota end` and
`reboot`. The app then finds the board again by address, and once the handshake succeeds
-- the evidence that the new firmware's radio and console both work -- sends
`ota confirm`. If it never gets that far, the board rolls itself back.

Every GATT write now waits for its acknowledgement behind one lock. Android refuses a
second write while one is in flight, and commands and firmware chunks share the link.

## Pairing

The device defaults to `ble pair open`, which has no pairing at all: anything in range
can run every command, calibration included. That is a bench setting. `ble pair bonded`
switches to LE Secure Connections with a six-digit passkey shown on the device's OLED;
it persists and drops the current link, so the app confirms before doing it.
