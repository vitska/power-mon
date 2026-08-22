# power-mon Android client

A phone client for the battery monitor, speaking the Nordic-UART console described in
[`../CLI.md`](../CLI.md) — protocol **3**. Everything the console exposes is reachable
from here: live telemetry, the fuel-gauge configuration, shunt topology, calibration,
the display, BLE pairing, and a raw command line for whatever is left.

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

- Kotlin 2.0.21, AGP 8.7.2, Gradle 8.11.1, Compose BOM 2024.10.01
- `minSdk 26`, `compileSdk`/`targetSdk 35`

## Layout

| File | Role |
|---|---|
| `ble/Protocol.kt` | The wire contract, transcribed from CLI.md: UUIDs, records, framing, micro-unit conversion, per-command timeouts. No Android APIs, so it is the part that can be reasoned about on its own. |
| `ble/BatmonClient.kt` | One GATT connection. Scan → connect → MTU → discover → subscribe, then commands serialised through a channel. |
| `model/MonitorViewModel.kt` | Folds the four record types into one `Telemetry`, runs the handshake, keeps a bounded transcript. |
| `ble/DeviceStore.kt` | The boards this phone has talked to, and which one it used last. |
| `ui/MonitorScreen.kt` | Live SoC, volts, amps, watts, gauge, diagnostics, environment. |
| `ui/DeviceSheet.kt` | Device picker: remembered boards merged with scan results. |
| `ui/ConfigureScreen.kt` | Every setter, grouped as CLI.md groups them, with guarded calibration. |
| `ui/ConsoleScreen.kt` | Raw command entry and transcript. |

## Several boards

The app talks to one board at a time but remembers all of them. The Bluetooth button in
the top bar opens the picker, which merges the boards this phone has connected to before
with whatever the current scan turns up; tapping one switches to it, dropping the open
link. A saved board with no signal reading is listed as "not seen in this scan", which is
the honest statement — it may be powered down, or just out of range.

On startup the app reconnects to the board it used last. It does that by **scanning for
that address** rather than connecting to it directly: a scan hit proves the board is
powered and in range, where a direct connect to an absent one stalls until the stack
gives up. A board is remembered only once it reaches a usable link, so one that fails at
service discovery does not become the thing the app chases on every launch. With nothing
remembered yet — a fresh install — the first board found is taken, since there is no
prior choice to respect.

Switching clears the panel. A voltage from the previous board displayed under a new
board's name would be worse than an empty readout, so telemetry and the handshake reset
and the next connection re-reads everything.

Identity is the MAC address, not the name. `batmon-DCFA` is derived from the MAC so it is
stable in practice, but the name is cached only so the picker can label a saved board
before a scan has found it.

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
and `cal zero v` takes 68 seconds, which is a lot of records to swallow. `StreamParser`
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

## Pairing

The device defaults to `ble pair open`, which has no pairing at all: anything in range
can run every command, calibration included. That is a bench setting. `ble pair bonded`
switches to LE Secure Connections with a six-digit passkey shown on the device's OLED;
it persists and drops the current link, so the app confirms before doing it.
