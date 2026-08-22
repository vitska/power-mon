# Calibration procedure (M1/M2)

You have a trustworthy meter. This is how its readings become the firmware's
constants.

Everything is entered through the console — over USB, or over BLE if that is easier
where the shunt is mounted. Nothing here needs a rebuild, and **the device records
every value to flash itself** — there is nothing to write down and nothing to type
back after a reboot.

---

## 1. The one thing that catches everybody: units

**Every value is an integer in micro-units.** There are no decimal points anywhere in
the console. Convert before typing.

| You measured | You type | Command |
|---|---|---|
| 2.0134 A | `2013400` | `cal top i 2013400` |
| −0.5 A (discharge) | `-500000` | `cal top i -500000` |
| 12.6543 V | `12654300` | `cal top v 12654300` |
| 10 mΩ shunt | `10000` | `shunt 10000` |
| 100 mΩ shunt | `100000` | `shunt 100000` |
| +0.25 % gain | `1002500` | `curve i gain 1002500` |
| ×3.0 divider | `196608` | `curve v divider 196608` |

Amps and volts → **µA and µV**: multiply by 1 000 000.
Gain → **ppm**: 1 000 000 is unity, so +1 % is 1 010 000.
Divider → **Q16.16**: multiply the ratio by 65 536 (3.0 × 65536 = 196608).

A misplaced factor of ten here is the single most common calibration mistake, and
the firmware cannot detect it — `2013400` and `201340` are both plausible currents.

---

## 2. Two points per channel

Each channel is a straight line, so it needs two points:

| Point | What you do | What it fixes |
|---|---|---|
| **zero** | Apply nothing — no current, no voltage | the **offset** |
| **top** | Apply a known value and read your meter | the **gain** |

`cal` walks those two points and does the arithmetic for you.

```
cal                     show which points are set
cal zero i [n]          no current flowing      -> current offset
cal zero v [n]          no voltage on VBUS      -> voltage offset
cal top i <uA> [n]      known current           -> current gain
cal top v <uV> [n]      known voltage           -> voltage gain
cal reset [i|v]         back to zero offset, unity gain
cal save                write to flash (zero/top do this for you)
cal forget              erase the stored calibration
```

Do **zero before top** on each channel: gain is solved assuming the offset is already
right. Running them the other way is not refused — it is just a worse fit — because
re-trimming one term deliberately is a legitimate thing to want.

`n` is the sample count: 256 by default for a zero point, 64 for a top point, both
overridable. More samples, less noise, longer wait.

Each successful point writes itself to flash and is restored at the next boot:

```
Applied. Now: cal top i <uA> with a known current flowing.
Saved to flash -- restored automatically at every boot.
```

`cal` reports storage state on its first line, so a board that is only calibrated in
RAM says so.

---

## 3. Before you start

1. **Warm up.** Power the board for 10 minutes. The INA219's offset drifts while the
   board self-heats, and calibrating cold bakes in a value you will not see again.
2. **Set the shunt resistance to its nominal value** — the marked value, or the
   measured DC resistance if you have a four-wire meter:
   ```
   shunt 10000
   ```
   Gain trim then absorbs the remaining error. Do not try to "calibrate" by editing
   the shunt value instead of the gain; that number also drives full scale and the
   low-side headroom warning.
3. **Set the divider ratio** if there is one — `65536` when the pack connects
   straight to VBUS:
   ```
   curve v divider 65536
   ```
4. **Confirm both sensors and the shunt location:**
   ```
   scan
   shunt loc n        # or p — whichever lead the shunt is actually in
   ```
5. **Fix the sign before touching gain.** Apply a charging current and check that it
   reads positive:
   ```
   read
   sense sign invert  # only if charging reads negative
   ```
   `cal top i` refuses a sign mismatch rather than silently inverting the gain, so
   this must be right first.

---

## 4. Current channel

### Zero point — no current

**Disconnect the load and the charger.** The firmware cannot verify this, and any
current flowing now becomes a permanent part of the offset.

```
cal zero i 512
```

Averages 512 samples at PGA/1 — the finest range, for 8× better resolution on the
offset — then reports:

```
  offset  -0.0031 A
  stddev   0.0001 A
Applied. Now: cal top i <uA> with a known current flowing.
```

If it prints `REJECTED: too noisy -- current was flowing`, something is still drawing
current. That rejection is the check working; find the load rather than lowering the
sample count.

### Top point — a known current

Apply a **steady** current, ideally 50–80 % of your normal working maximum: a top
point near the bottom of the range makes the slope more sensitive to noise. Read your
meter, then hand the firmware that number in µA:

```
cal top i 2013400
```

```
measured 1.9782, reference 2.0134 -> gain 1017794 ppm
Applied. Verify at a THIRD point ...
```

---

## 5. Voltage channel

Identical shape. The zero point is available here too — you cannot disconnect a
voltage the way you disconnect a load, but you *can* disconnect the pack so that VBUS
sits at 0 V.

### Zero point — VBUS at ground

**"Disconnected" is not the same as "at 0 V."** VBUS is measured against system
ground, so an ungrounded input floats up to near the 3.3 V rail — steady, plausible,
and completely wrong. Tie the VBUS node to GND before running this.

```
cal zero v
```

```
  offset   0.008 V
  spread   0.000 V
Applied. Now: cal top v <uV> with a known voltage applied.
Saved to flash -- restored automatically at every boot.
```

An offset of *zero counts* is also a perfectly normal outcome, not a failure: the bus
channel is 4 mV/LSB, so anything smaller is invisible to the hardware and nothing is
lost by leaving it at zero.

> **The refusal is worth trusting.** On this project's own board the command rejected
> a rock-steady 3.448 V — which turned out to be the sensor grounds not being tied
> together. The tell was the two sensors disagreeing: `read` showed `pack voltage
> 3.448 V` beside `load voltage 0.000 V`. Identically wired sensors that differ mean
> a wiring fault, not a calibration problem. Absorbing that 3.4 V into the offset
> would have hidden the fault permanently.

### Top point — a known voltage

```
cal top v 12654300
```

Use a voltage in the range you actually operate at. For a 4S Li-ion pack, calibrate
near 14–16 V rather than at 5 V.

---

## 6. When it refuses

| Message | What it means |
|---|---|
| `too noisy -- current was flowing` | The load or charger is still connected. |
| `that is a real voltage, not an offset` | VBUS is not at ground. Anything beyond ±0.5 V is a voltage, not an offset — and a reading near 3.3 V usually means the grounds are not tied together. |
| `must both exceed 10000 uA` | Below 10 mA the ratio is dominated by offset error. Use a larger current; the zero point is the tool for the small end. |
| `must both exceed 500000 uV` | The same, for voltage: pick a top point above 0.5 V. |
| `sign mismatch` | Meter and device disagree on direction. `sense sign invert`, or fix the sense leads. |
| `solved gain … is outside +/-10%` | Not a gain error. The shunt resistance or the divider ratio is wrong — see §3. Trimming gain would hide a 10 % error rather than fix it. |

---

## 7. Verify at a third point

A two-point fit always passes through its own two points. The only honest check is a
third:

```
stats reset
  ... apply a different known current, wait ~30 s ...
stats
cal
```

Compare the mean against your meter. The M1 exit criterion (DESIGN.md §11) is **±1 %
over ±2 A**, and you should comfortably beat it.

Check at the PGA range you will actually run at, not just the one the zero point
used. The gain is range-independent — the shunt register is 10 µV/LSB whatever the
range — but the *offset* is not perfectly so, because the PGA contributes a little of
its own. A residual that is worse at PGA/4 than at PGA/1 is exactly that effect.

---

## 8. The device keeps it

Calibration lives in NVS and is restored before the first sample is taken, so a
reboot changes nothing:

```
I (209) config: restored: shunt 10000 uOhm, i offset -3100 uA gain 1017794 ppm,
                   v offset 8000 uV gain 1005249 ppm
I (209) main: calibration restored from NVS ('cal' to review)
```

What is stored: shunt resistance, current offset/gain/sign, divider ratio, voltage
offset/gain, the **range ceiling** and bus-comp mode, and the **shunt install mode** —
that last because roles have to resolve before any of the rest can be applied.

The range ceiling is in there because it changes what a reading *is*, not just how
precise it is. The INA219's PGA (CONFIG bits 12:11) selects ±40 / 80 / 160 / 320 mV
full scale; `sense pgamax 8` sets PG=11 for the widest. The shunt register stays
10 µV/LSB in every range, so widening it costs no resolution — it only relaxes the
low-side input budget of DESIGN.md §2.9.2, which is a hardware-protection choice
rather than a limit of the part.

| Command | |
|---|---|
| `cal save` | Persist whatever is set now. `cal zero` and `cal top` do this for you; use it after poking values in with `curve`. |
| `cal forget` | Erase the stored copy. Live values are untouched until the next reboot. |
| `cal reset` | Clear the live values **and** the stored copy — a reset that undid itself overnight would be worse than none. |

Two details worth knowing:

- The version key is written **last**, so a power cut mid-save leaves a store that is
  rejected wholesale rather than one that loads half a calibration.
- The shunt value and divider ratio also exist as Kconfig defaults
  (`BATMON_SHUNT_UOHM`, `BATMON_VBUS_DIVIDER_Q16`) for baking into a build. Gain and
  offset deliberately have no Kconfig entry: they are properties of *one physical
  board*, and a per-board constant in a shared config file is a constant that ends up
  on the wrong board.

This is DESIGN.md §6.1's `cfg` namespace brought forward from M3 — typed keys, as the
design specifies, so M3's configuration layer absorbs this namespace rather than
replacing it. Everything else set from the console (stream period, screen selection,
sampling profile) is still RAM-only until then; `options` shows the whole picture.

---

## 9. The equations, if you want to check the arithmetic

Current (`components/ina219/ina219.c`, `convert()`):

```
i_ua = v_shunt_uv × 1e6 / r_shunt_uohm      // raw, from the 10 µV/LSB register
i_ua = i_ua − i_offset_ua                   // offset FIRST
i_ua = i_ua × i_gain_ppm / 1e6              // then gain
i_ua = −i_ua                                // only if `sense sign invert`
```

Voltage:

```
v_uv = raw_uv × vbus_divider_q16 / 65536    // hardware divider
v_uv = v_uv − v_offset_uv                   // offset FIRST
v_uv = v_uv × v_gain_ppm / 1e6              // then gain
v_uv = v_uv ± v_shunt_uv                    // only if `sense vbuscomp add|sub`
```

**Offset is subtracted before gain is applied.** That ordering is why `cal zero`
forces unity gain while it measures: an offset measured through a non-unity gain
would be stored in the wrong domain and come back out scaled by 1/gain.

`vbuscomp` is **not** a calibration term. It adds or subtracts the *measured* shunt
drop to correct the low-side reference (DESIGN.md §2.9.5). It is exact, and it is
applied after your trims so they never scale it.

### Solving both terms by hand

If you would rather compute the fit yourself — or want a two-point fit on the current
channel instead of zero + top — clear the trims first so the readings are raw:

```
curve reset i        # or: curve reset v
```

Then with `m` = what the device reports and `r` = what your meter says, in the same
micro-units:

```
gain_ppm = 1000000 × (r2 − r1) / (m2 − m1)

offset   = m1 − r1 × (m2 − m1) / (r2 − r1)
```

**Worked example.** Meter 6.000000 V and 24.000000 V; device reports 5.968000 V and
23.874000 V.

```
r1 = 6000000    m1 = 5968000
r2 = 24000000   m2 = 23874000

gain_ppm = 1000000 × (24000000 − 6000000) / (23874000 − 5968000)
         = 1000000 × 18000000 / 17906000
         = 1005249

offset   = 5968000 − 6000000 × 17906000 / 18000000
         = 5968000 − 5968666.67
         = −667          (round; the fraction is far below the 4 mV bus LSB)
```

```
curve v offset -667
curve v gain 1005249
```

Check both ends — `(m − offset) × gain`:

```
(5968000  + 667) × 1.005249 =  5 999 996.5   ->  5.999997 V   (3.5 µV low)
(23874000 + 667) × 1.005249 = 23 999 985.1   -> 23.999985 V   ( 15 µV low)
```

Both residuals are far below the bus channel's own 4 mV LSB, so the fit is as exact
as the hardware can express.

---

## Quick reference

```
shunt 10000                 # shunt resistance, micro-ohms
shunt loc n                 # which lead the shunt is in
curve v divider 65536       # hardware divider, Q16.16
sense sign invert           # charging must read positive

cal zero i 512              # current offset, load DISCONNECTED
cal top i 2013400           # current gain, from a 2.0134 A reference
cal zero v                  # voltage offset, pack DISCONNECTED
cal top v 12654300          # voltage gain, from a 12.6543 V reference

cal                         # which points are set, and whether stored
cal save                    # persist values set via `curve`
cal forget                  # erase the stored calibration
curve                       # every term, numerically
curve reset [i|v]           # back to unity gain, zero offset
stats reset ; stats         # averaged reading, for a manual fit or a check
options                     # everything, in one place
```
