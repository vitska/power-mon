# CLI reference and client protocol

The same command interface is reachable two ways: over USB serial, and over BLE as a
Nordic UART Service. **The protocol is byte-identical on both** — the same echo, the
same line endings, the same exit status and terminator, including on the error paths.
Both transports call one execution function; only the sink differs.

This document is written to be sufficient for building a client — an Android app, a
script, a logger — without reading the firmware. Anything configurable from the local
console is configurable over BLE, with one documented exception (`mon`).

Protocol version **3**. Check it with `ver` before anything else.

> **Changed in 3:** telemetry is split into three groups with independent rates, each
> emitted as its own prefixed record — `f` fast, `c` calculated, `e` environmental. The
> single wide CSV row of protocol 2 is gone. Up to **three BLE centrals** may connect at
> once; a command's reply is unicast to the client that sent it, while the stream is
> broadcast to every subscriber.
>
> **Changed in 2:** the CSV stream gained temperature, humidity and pressure, and `env`
> was added.

---

## 1. Transports

### BLE (what an app will use)

| | |
|---|---|
| Advertised name | `batmon-XXXX` — last two bytes of the BT MAC |
| Advertising interval | 500 ms, connectable, general discoverable |
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` (Nordic UART) |
| RX — write commands here | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` — Write, Write-No-Response |
| TX — subscribe for output | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` — Notify |
| Connections | **up to 3 at once** |
| Preferred ATT MTU | 512 |

The 128-bit service UUID is in the **scan response**, not the advertisement — a
16-byte UUID plus the name does not fit in 31 bytes. Scan actively, or filter on name.

**Subscribe to TX before sending anything.** Output to an unsubscribed client is
discarded, not buffered; the command still executes. `ble` reports connection and
subscriber counts separately for exactly this reason.

**Multiple centrals share one console.** A phone can watch telemetry while a laptop
configures. Two rules make that safe, and a client should rely on both:

- **Replies are unicast.** A command's echo, output and `exit` line go only to the
  connection that sent it. You will never receive another client's `help` output.
- **The stream is broadcast.** Every subscriber receives every telemetry record. If two
  clients set different rates, the last setting wins for everyone — the rates are device
  state, not per-connection state.

Each connection has its own line-assembly buffer, so simultaneous writes cannot splice
into one corrupt command. MTU is negotiated per connection and chunking follows each
one's own value.

On subscribing you receive an unsolicited greeting:

```
\r\nbat-monitor console over BLE. 'help' lists commands.\r\n
```

Notifications are chunked to `ATT_MTU − 3` bytes, re-read per write, so a response
arrives as several notifications. **Reassemble by scanning for the terminator, never by
assuming one notification is one line.**

### USB serial

115200 8N1 on the ESP32-C6's native USB (USB-Serial-JTAG).

Adds, on top of the identical protocol: a `batmon> ` prompt, line editing, history and
tab completion. Those are *local conveniences layered over* the same framing, not a
different protocol — a command typed here produces exactly the bytes it would produce
over the air, so a USB transcript is a valid reference for a BLE client.

One consequence to expect: your typed characters are echoed locally by the line editor
**and** the protocol emits its own `> command` echo. That duplication is the price of
identical framing, and it is deliberate.

If the far end cannot handle escape sequences — a raw pipe, a script driving the port —
the console says so once and disables editing:

```
Terminal does not support escape sequences; line editing disabled.
```

The framing is unaffected. A script may drive USB exactly as a BLE client would; the
prompt string `batmon> ` is the only extra output, and it carries no CR/LF, so it
appears immediately before the next echo line.

---

## 2. Framing

A command is an ASCII line terminated by `\n` or `\r`. Both are accepted; CRLF is fine.

Every command, **on either transport**, produces exactly this, in order:

```
> <the command as received>\r\n      <- echo
<zero or more output lines>\r\n
exit <n>\r\n
\x04                                 <- EOT, one byte
```

**Read until `0x04`.** That byte is the only reliable end-of-response marker; a
quiet-period timeout will misfire precisely when the device is slow, which is when a
command is doing something interesting. `0x04` is invisible in a terminal, so the same
stream stays readable by a human.

`exit <n>` is always present, including on success:

| `n` | meaning |
|---|---|
| `0` | success |
| `>0` | the command ran and reported failure — usually a rejected argument |
| `-1` | the console failed to execute it |
| `-2` | unknown command |
| `-3` | empty command |

Other framing rules:

- **Output is capped at 4 KB per command**, the same on both transports. On overflow
  the response ends with `[output truncated]` before `exit`. Only `help` comes close,
  at ~1.5 KB.
- **Lines are capped at 160 characters.** An overlong line is discarded whole, with
  `line too long, discarded` — never executed truncated, because a shortened `zero 512`
  would silently become `zero 5`.
- `\b` and `\x7F` delete the previous character; a bare newline is ignored.
- Firmware log output (`I (1234) tag: ...`) is **not** sent over BLE. It only appears on
  USB serial, and it can interleave with command output there.

### One command at a time

Commands run on a single worker task and sensor access is behind one mutex. Send the
next command only after the `0x04` of the previous one. Pipelining is not rejected — it
is queued — but a second command that needs the sensor will wait, and some commands wait
a long time:

Durations below are **measured**, not estimated. Anything that averages is dominated by
the INA219's conversion time, so compute a timeout from the sample count rather than
hard-coding one:

| | cost per sample | why |
|---|---|---|
| one sensor | **~137 ms** | one triggered conversion at 128× hardware averaging |
| both sensors | **~270 ms** | current and voltage devices read in turn |

| command | samples (default) | measured |
|---|---|---|
| `ver`, `help`, and every setter | — | **< 100 ms** |
| `read` | 1 both | **0.4 s** |
| `env` | 1 forced conversion | **~12 ms** |
| `cal top i\|v`, `cal vpath` | 64 both | **~17 s** |
| `cal zero i` | 256 one | **~35 s** (8.8 s at 64) |
| `cal zero v` | 256 both | **~68 s** (8.5 s at 32) |
| `detect` | 32 both | ~9 s, scales with the count |
| `mon` | — | local terminal only — **refuses remotely** with `exit 1` |

Allow generous headroom: a client timeout of `samples × per-sample × 2 + 2 s` will not
misfire. **Do not** infer a hang from silence — averaging commands emit a `.` every four
samples and nothing else until they finish.

`mon` is the one command that cannot be transport-agnostic: it repaints until a keypress
arrives on the console it was started from, and a remote caller's keystrokes are not on
that stdin. Rather than hang or behave differently depending on the wire, it refuses with
`exit 1` and names the alternative. Clients use `stream csv`, which carries the same
values and needs no terminal.

---

## 3. Handshake

Send `ver` first. Output is stable, fixed order, one `key value` pair per line — the
only command designed for machine parsing rather than human reading:

```
protocol 3
firmware 0.4.0-m2
idf v5.3.5-1161-g6d0016c3c1f
chip esp32c6 rev2 cores1
mac CC:8D:A2:F2:DC:FA
built Aug 21 2026 18:33:21
units micro
```

`protocol` is bumped when an existing command's output shape changes, an argument's
meaning changes, or the framing changes. **Adding** a command does not bump it. A client
should refuse to drive a `protocol` it does not know rather than guess.

---

## 4. Units — the thing to get right first

**Every numeric argument is an integer in micro-units. There are no decimal points
anywhere in the interface.**

| Quantity | Unit | Example |
|---|---|---|
| current | µA | `2013400` = 2.0134 A |
| voltage | µV | `12654300` = 12.6543 V |
| resistance | µΩ | `100000` = 100 mΩ |
| capacity | µAh | `44000000` = 44 Ah |
| gain | ppm | `1000000` = ×1.000000 |
| divider | Q16.16 | `196608` = ×3.0 (ratio × 65536) |
| SoC | per mille | `500` = 50.0 % |
| Peukert k | Q8 | `294` ≈ 1.15 |

Outputs, by contrast, are **formatted decimal**: volts to 3 decimals, amps to 4, watts
to 3, millivolts to 3. A client that round-trips a value must convert back to
micro-units itself.

Sign convention: **current > 0 is charge into the battery**, < 0 is discharge.

---

## 5. Telemetry — three groups, three rates

Quantities change at genuinely different speeds, so they are sent at different rates as
three record types. Sending temperature at the current-sampling rate wastes airtime;
sending current at the temperature rate loses the event you were watching for.

| group | prefix | default | contents |
|---|---|---|---|
| fast | `f` | **100 ms (10 Hz)** | voltage, current — the measurement itself |
| calculated | `c` | **500 ms (2 Hz)** | power, SoC, charge, state, OCV, Peukert |
| environmental | `e` | **10 000 ms** | temperature, humidity, pressure |

```
stream csv                enable, CSV grouped records, re-emit headers
stream text               human one-liner instead, at the calc rate
stream fast <ms|off>      20..60000
stream calc <ms|off>      20..60000
stream env  <ms|off>      20..600000, and it also sets how often the sensor is read
stream on | off           all groups
stream                    show the current state
```

A group set to `off` (0 ms) stops without disturbing the others — a client that only
wants temperature need not receive 10 Hz of current to get it.

### Records

Header lines are emitted once when the stream is enabled and **start with `#`**, so a
parser can either use them or skip them on that one character.

```
#f,ms,volts,amps
#c,ms,watts,soc_pct,charge_ah,state,ocv_v,peukert
#e,ms,temp_c,humid_pct,press_hpa
f,7073,13.113,-0.0006
c,7213,-0.007,90.2,39.696,RESTING,13.117,1.000
e,7033,26.97,48.9,1001.64
```

Records are **never duplicated**: a group emits only when the underlying sample is new.
Ask for 10 Hz from a 7.3 Hz sensor and you get 7.3 Hz of distinct records rather than
10 Hz with repeats — so a client may derive the true rate from the `ms` field and trust
it.

### The sample-rate ceiling

The fast group cannot outrun the ADC. Both channels convert sequentially, so the pair
rate is what matters:

| profile | averaging | pair | ceiling |
|---|---|---|---|
| `continuous` (default) | 128× | 136.2 ms | **7.3 Hz** |
| `fast` | 64× | 68.1 ms | **14.6 Hz** |
| `triggered` | on demand | — | low-power tier (§9.4) |

**For a genuine 10 Hz, send `profile fast`.** It costs about 40 % more noise per sample,
which is a real trade against the 3 mA integration deadband, so it is opt-in rather than
the default. Measured on hardware: `stream fast 100` with `profile fast` delivers
**~9.2 Hz**; with the default profile the same request yields ~7.3 Hz of non-duplicated
records.

`profile` prints the ceiling for the current setting.

### Fields

| field | meaning |
|---|---|
| `ms` | milliseconds since boot. **Not a wall clock** — the device has no idea what time it is. Stamp arrival time on the client. |
| `volts` | pack voltage, calibrated and harness-corrected |
| `amps` | current, > 0 charging |
| `watts` | volts × amps |
| `soc_pct` | state of charge, one decimal |
| `charge_ah` | accumulated charge, 3 decimals |
| `state` | `UNKNOWN` \| `COUNTING` \| `RESTING` \| `FULL` \| `EMPTY` |
| `ocv_v` | I·R-compensated open-circuit estimate — what the SoC map actually uses |
| `peukert` | the discharge multiplier in force right now; `1.000` while charging |
| `temp_c` | board temperature, °C to 2 dp. **Empty when no sensor is fitted** |
| `humid_pct` | relative humidity, 1 dp. **Empty on a BMP280**, which has no humidity channel |
| `press_hpa` | pressure, hPa to 2 dp. Empty when no sensor is fitted |

The environmental fields are **empty rather than zero** when unavailable: 0.00 °C is a
plausible temperature and would be indistinguishable from a real reading. The field
count never changes, so a positional parser stays valid — it just sees `,,`.

Raw diagnostics — `shunt_mv`, `pga`, `sat` — are no longer in the stream. They belong to
a single measurement rather than to telemetry, and `read` reports them on demand. A
client that needs to know whether the shunt channel is saturated should call `read`.

**The stream reaches both transports**, emitted by the sampler task directly to USB
and to the BLE TX characteristic, so a phone gets it without polling. Lines end
`CR LF` on both.

**It is asynchronous and sits outside the framing.** Stream records arrive between
command responses, with no echo, no `exit` and no EOT of their own. A client must
attribute each incoming line to one or the other; the simplest workable rule is: while a
command is in flight, everything up to the next `0x04` belongs to that command, and
anything arriving with no command pending is stream data.

Because records are prefixed, the fallback is also easy: a line starting `f,`, `c,`, `e,`
or `#` is telemetry.

Because `stream_enabled` is one global flag, enabling the stream from BLE also enables it
on USB, and vice versa. There is one stream, not one per transport.

Disable the stream before running a long calibration if you want a clean transcript;
`cal` and `detect` suppress it for their duration anyway and restore it afterwards.

---

## 6. Command reference

Grouped by what they touch. "Persists" means it survives a power cycle.

### Identity and overview

| Command | Notes |
|---|---|
| `ver` | §3. Machine-parseable. |
| `help` | All commands with hints. ~1.5 KB, near the truncation cap. |
| `options` | Everything currently set, in labelled sections. Human-oriented; use the specific commands for parsing. |

### Measurement

| Command | Notes |
|---|---|
| `read` | One blocking sample. ~0.4 s. Prose output, 4–7 lines. |
| `env` | Temperature, pressure, humidity — a **fresh** forced-mode read, ~12 ms |
| `stats` | Mean / σ / min / max over the window, plus error counters. |
| `stats reset` | Clear the window. |
| `scan` | I²C bus scan; identifies expected devices, flags unexpected ones. |
| `profile <continuous\|fast\|triggered>` | Sampling profile and therefore the rate ceiling; prints it |
| `mon [refresh_ms]` | Interactive dashboard. **Not for clients.** |

### Shunt and sensor topology

| Command | Args | Notes |
|---|---|---|
| `shunt` | — | Resistance, full scale, resolution, location |
| `shunt <uohm>` | 1000–1000000 | Persists |
| `shunt loc <p\|n\|single\|auto>` | | Which lead the shunt is in. Persists |
| `sensors [mode <p\|n\|single\|auto>]` | | Same state as `shunt loc` |
| `detect [samples]` | 8–1024 | Resolve the topology by observation. **Needs a load** — refuses at zero current |
| `sense sign <normal\|invert>` | | Charging must read positive. Persists |
| `sense vbuscomp <none\|add\|sub>` | | Low-side reference correction. Persists |
| `sense pgamax <1\|2\|4\|8>` | | Range ceiling. Persists |

### Calibration

Guided two-point flow. Everything here persists automatically.

| Command | Args | Notes |
|---|---|---|
| `cal` | — | Which points are set, and whether stored |
| `cal zero i [n]` | 16–4096, default 256 | Current offset. **Load disconnected.** ~35 s |
| `cal zero v [n]` | 16–4096, default 256 | Voltage offset. **VBUS at ground**, not merely disconnected. ~68 s |
| `cal top i <uA> [n]` | n 8–1024, default 64 | Current gain from a meter reading. ~17 s |
| `cal top v <uV> [n]` | | Voltage gain. Use a reading taken **at rest** |
| `cal vpath <uV>` | | Harness resistance, from a **loaded** terminal reading. Needs ≥ 0.5 A |
| `cal save` | | Persist values set via `curve` |
| `cal forget` | | Erase the stored calibration |
| `cal reset [i\|v]` | | Clear live **and** stored trims |
| `curve` | | Every term numerically, with the raw µ integers in parentheses |
| `curve i offset <uA>` / `gain <ppm>` / `ref <uA> [n]` | gain 900000–1100000 | Direct set |
| `curve v offset <uV>` / `gain <ppm>` / `ref <uV> [n]` / `divider <q16>` | divider ≥ 65536 | Direct set |
| `zero [n]` | | Older alias of `cal zero i` |
| `gain [ppm]`, `offset [uA]`, `pga <auto\|1\|2\|4\|8>` | | Individual legacy accessors |

Refusals a client should expect and surface verbatim — each names a physical cause:

- `must both exceed 10000 uA` / `500000 uV` — reference too small to solve a slope
- `sign mismatch` — meter and device disagree on direction
- `solved gain … is outside +/-10%` — the shunt value or divider ratio is wrong, not the gain
- `the shunt channel is SATURATED` — the reading is a range limit; sense wiring
- `too noisy -- current was flowing` — load still connected during a zero point
- `that is a real voltage, not an offset` — VBUS not at ground
- `sensor busy -- try again` — another acquisition held the mutex

### Fuel gauge

| Command | Args | Notes |
|---|---|---|
| `soc` | — | SoC, state, charge, SoH, learning, Peukert, endpoints, all config |
| `soc set <permille>` | 0–1000 | Force SoC |
| `soc full` | | Declare the pack full now |
| `soc reset` | | Forget the count; re-seed from voltage |
| `soc cap <uAh>` | | Design capacity |
| `soc v0 <uV>` / `v100 <uV>` | v0 < v100 | Resting-OCV endpoints of the SoC map |
| `soc vfull <uV>` | ≥ v100 | Absorption voltage for full detection |
| `soc rint <uOhm>` | | Battery internal resistance, for I·R → OCV |
| `soc taper <uA>` | | Charge current below which full can latch |
| `soc rest <s>` | | Idle time before OCV is trusted |
| `soc peukert <q8>` | 256–512 | k; 256 = disabled |
| `soc irated <uA>` | | Rate the nameplate capacity assumes |
| `soc depth <permille>` | 100–1000 | Discharge depth required to learn capacity |

All of it persists. A rejected config prints the constraint that failed:
`v0 < v100 <= vfull`, non-zero capacity, `peukert` 256–512, `depth` 100–1000.

**`soc` output includes a trust flag a client must respect.** When SoC comes from
voltage with no count behind it, the line reads:

```
SoC          67.9 %  (from VOLTAGE only -- no count behind it yet)
```

and `last anchor never since boot` means a restored count has not been verified against
voltage since power-up. Present these as uncertainty, not as a precise number.

### Display

| Command | Notes |
|---|---|
| `disp` | State, pinned screen, screen count |
| `disp on` / `off` | Blank or restore the panel |
| `disp screen <n\|auto>` | Screen 0 is the large volts/amps/SoC readout |
| `disp contrast <1-255>` | 0x40 default; warns above 0x80 |

### BLE and pairing

| Command | Notes |
|---|---|
| `ble` | Connection and subscriber counts, security of the **weakest** link, bonds, MTU, traffic |
| `ble pair <open\|bonded>` | **Persists.** Switching to `bonded` drops the current link |
| `ble passkey <random\|NNNNNN>` | Random per pairing is the default |
| `ble bonds` | List bonded peers |
| `ble unpair` | Forget all bonds; drops the link |
| `ble disconnect` | Drop **every** link; advertising resumes |

**Pairing model.** The device is `DISPLAY_ONLY` with MITM protection and LE Secure
Connections. In `bonded` mode it initiates security on connect; the phone prompts for a
**six-digit passkey which the device shows on its OLED** and logs on USB serial. Bonds
persist in NVS, so this happens once per phone.

In `bonded` mode a write to RX from an unauthenticated link is rejected with ATT error
`0x05` (Insufficient Authentication) — encryption alone is not enough, authentication is
required too. Handle that error by initiating pairing rather than retrying.

`open` mode has **no pairing at all**: anything in range can run every command,
including calibration. It is the default and it is a bench setting.

---

## 7. Recommended client sequence

```
scan / connect to batmon-XXXX
subscribe to TX                     <- before anything else
send  ver                           -> check protocol == 1
send  soc                           -> capacity, endpoints, state
send  curve                         -> calibration state, to show "uncalibrated"
send  stream csv                    -> live telemetry begins
send  stream 1000                   -> pick a cadence
```

Then keep the connection open and consume CSV lines. Only issue further commands on
user action, one at a time, waiting for `0x04`.

Things worth building in from the start:

- **Treat `sat=1` as "no reading".** It is a range limit, not a measurement.
- **Surface the SoC trust flag.** A voltage-only or un-anchored SoC should look
  different in the UI from a counted one.
- **Never auto-run calibration.** `cal zero i` with a load connected permanently
  poisons the offset, and the firmware cannot detect that condition.
- **Expect long commands.** Show progress; the device emits `.` characters during
  averaging, one per four samples.
- **Reconnect and re-handshake** after any disconnect; do not cache calibration across
  connections, since another client may have changed it.

---

## 8. Known limitations

Stated plainly, because a client author will hit them.

1. **Most output is prose, not a data format.** `ver` and `stream csv` are the only
   machine-oriented surfaces. Everything else is designed to be read by a person, and
   parsing it means matching on label text. A structured mode (JSON or TLV) is the
   obvious next protocol version; DESIGN.md §7.8 specifies a binary telemetry
   characteristic that supersedes CSV.
2. **No push telemetry outside the stream.** There is no notification on state change,
   and no way to subscribe to one value rather than all of them. A client leaves the
   stream on and filters.
3. **No wall clock.** Timestamps are milliseconds since boot and reset on reboot.
4. **No authentication in `open` mode**, which is the default.
5. **The JBD/Xiaoxiang protocol of DESIGN.md §7 is not implemented yet.** This NUS
   console is the whole interface today; an app written against it will need to change
   when the binary service lands.
6. **Log output on USB serial can interleave** with command output — it is written by
   whichever task logged it, outside the framing. BLE never carries log lines, so a
   client sees a clean stream; a USB transcript may contain `I (1234) tag: …` lines
   belonging to no command. Filter by prefix if you parse USB captures.
7. **`mon` is local-only** — the single command whose behaviour differs by transport.
   It refuses remotely rather than degrading silently.
