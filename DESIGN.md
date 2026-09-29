# BLE Battery Monitor — Firmware Design Document

**Target:** Seeed Studio XIAO ESP32-C6
**Sensor:** INA219 (I²C, shunt-based V/I measurement)
**Function:** Coulomb-counting fuel gauge with non-volatile accumulators, BLE telemetry + configuration
**Client:** Android — off-the-shelf Play Store apps, plus an optional custom app
**Status:** Draft v1.0

---

## 1. Scope and Goals

### 1.1 Functional requirements

| # | Requirement |
|---|---|
| F1 | Continuously measure pack voltage and bidirectional current via INA219 |
| F2 | Integrate current over time to maintain **accumulated charge** (coulomb count) |
| F3 | Integrate power over time to maintain **accumulated energy** in and out (Wh) |
| F4 | Derive State of Charge (%), time-to-empty / time-to-full, cycle count |
| F5 | Learn actual full capacity (State of Health) across full↔empty excursions |
| F6 | Persist all accumulators to non-volatile memory; survive power loss and brownout |
| F7 | Expose telemetry over BLE, usable by **existing Play Store apps without a custom app** |
| F8 | Allow configuration over BLE — capacity, shunt, calibration, and explicitly the **0 % and 100 % battery voltages** (§8.6) |
| F9 | Broadcast a compact state summary in the advertisement (connectionless read) |
| F10 | **Minimise average power consumption** — the monitor must not be a meaningful load on the pack it measures (§9) |
| F11 | Show SoC on a 0.91" OLED **on button press only**; display and its rail fully off otherwise (§2.6, §9.10, Appendix D) |
| F12 | Measure ambient temperature (BME280/BMP280) and apply **temperature correction** to capacity, voltage endpoints and sensor offset (§5.6) |

### 1.2 Non-goals

- Not a BMS: no cell balancing, no protection FET switching, no per-cell measurement.
  (The chosen protocol *can* carry cell voltages; the hardware does not measure them.)
- No safety-critical disconnect. This is an instrument, not a protection device.
- No cloud connectivity in v1. The C6's Wi-Fi radio is reserved for OTA later.

### 1.3 Design drivers

1. **Coulomb counting drifts.** The whole design is organised around bounding that drift
   and periodically re-anchoring it (§5.4). Everything else is bookkeeping.
2. **Flash endurance vs. power-loss tolerance.** The persistence cadence is the central
   trade-off (§6).
3. **App compatibility beats protocol elegance.** Emulating an existing, widely-supported
   BMS protocol yields a working Android UI on day one (§7).
4. **Sampling rate vs. self-consumption is a direct conflict.** A coulomb counter wants to
   watch continuously; a low-power device wants to sleep. §9 resolves this with a tiered
   power model whose floor is light sleep, rather than by picking one side.

---

## 2. Hardware Context

### 2.1 XIAO ESP32-C6

| Item | Value | Notes |
|---|---|---|
| SoC | ESP32-C6 (RISC-V 160 MHz, BLE 5.3 + 802.15.4) | ESP-IDF ≥ 5.1 required |
| RAM | 512 KB HP SRAM + 16 KB LP SRAM | LP SRAM is retained across reset and sleep; it holds the hot state mirror (§6.1) |
| Flash | 4 MB | NVS partition sized in §6.2 |
| I²C bus — everything | **D4 = GPIO22 (SDA) / D5 = GPIO23 (SCL)** — both INA219s, the OLED and the temperature sensor | One shared bus, as the board is wired (§2.5) |
| Button | D2 = GPIO2 | Wake from light sleep and screen cycling (§2.6) |
| Display rail gate | D3 = GPIO21 → high-side load switch (§2.5) | Any HP GPIO |
| LP core | Present, but **unusable for the gauge**: `LP_I2C` is fixed to GPIO6/7 and cannot reach sensors on GPIO22/23 | This is what sets the power floor at T2 (§9.3) |
| Antenna | GPIO3 = RF switch enable (drive LOW), GPIO14 = select (LOW = onboard, HIGH = u.FL) | Must be driven at boot or RF performance is degraded |
| Power | Onboard charger + BAT pads | Prefer powering the monitor from the monitored pack (§2.8) |
| Board parasitics | Power LED, charge LED, onboard regulator quiescent | A significant — no longer dominant — term at the T2 floor, §9.7 |

### 2.2 INA219

| Parameter | Value | Consequence |
|---|---|---|
| Shunt ADC | 12-bit, ±40 / 80 / 160 / 320 mV (PGA /1 /2 /4 /8) | LSB = 10 µV always; PGA sets range, not LSB |
| Bus ADC | 12-bit, 0–26 V, LSB = 4 mV | Pack voltage ceiling — see §2.3 |
| Shunt offset | ±100 µV max over temperature (±10 µV typ) | **Dominant error source** — §5.5 |
| Gain error | ±0.5 % max | Corrected by user gain calibration |
| Conversion time | 84 µs (9-bit) … 68.1 ms (12-bit, 128× avg) | Mode is power-state dependent — §9.4 |
| Supply current, converting | ~1 mA typ | **Comparable to the whole rest of the budget** — must be duty-cycled (§9.4) |
| Supply current, power-down | ~6 µA typ | Reached via `MODE = 000`, or automatically after a triggered conversion |
| Triggered mode | `MODE = 001/010/011` — one conversion, then auto power-down | The basis of the low-power sampling scheme |
| I²C address | 0x40–0x4F (A0/A1 strapping) | 0x40 default |

**Registers used:** `0x00` Config, `0x01` Shunt Voltage (signed), `0x02` Bus Voltage,
`0x05` Calibration. The `0x03` Power and `0x04` Current registers are deliberately *not*
used — current and power are computed in firmware from raw shunt/bus readings so that
calibration, PGA auto-ranging and offset correction all live in one place (§4.3).

### 2.3 Shunt sizing

Size the shunt so peak current lands near ±320 mV full scale, then let PGA auto-ranging
recover resolution at low current.

| Shunt | Full scale (PGA/8) | Current LSB | Offset-induced error (±100 µV) |
|---|---|---|---|
| 100 mΩ | ±3.2 A | 100 µA | ±1 mA → ±24 mAh/day |
| 10 mΩ | ±32 A | 1 mA | ±10 mA → ±240 mAh/day |
| 5 mΩ | ±64 A | 2 mA | ±20 mA → ±480 mAh/day |
| 1 mΩ | ±320 A | 10 mA | ±100 mA → ±2.4 Ah/day |

**Implication:** below ~10 mΩ, the INA219's offset makes unattended long-term coulomb
counting unreliable without frequent re-anchoring. If the application needs >30 A *and*
week-scale drift-free counting, the INA219 is the wrong part — see §12.

**Recommended default:** 10 mΩ, ≥2 W, 4-terminal (Kelvin) shunt, low-side, in the pack
negative lead so that *all* load and charge current — including the monitor's own
draw — passes through it.

> **The full-scale figures above are the high-side numbers.** Putting the shunt in the
> negative lead caps the usable shunt drop at roughly 100 mV rather than 320 mV, which
> cuts the current range by ~3×. **Read §2.9 before choosing the shunt** — for a low-side
> build it, not the table above, sets the value.

> **Voltage limit:** the INA219 bus input tops out at 26 V. For 24 V nominal packs
> (29.4 V charged) the bus pin needs a divider, or bus measurement moves to an ESP32 ADC
> input with its own divider. Config field `vbus_divider_q16` (§8.3) covers this;
> default 1.0.

### 2.4 Peripherals

All four I²C devices share one bus on **GPIO22/23 (D4/D5)**; addresses are what
distinguishes them, and they do not collide.

| Device | Address | Role | Idle draw |
|---|---|---|---|
| INA219 **A** | **0x40** | Positive-pole sensor (§2.10) | ~6 µA (triggered mode, §9.4) |
| INA219 **B** | **0x41** | Negative-pole sensor (§2.10) | ~6 µA |
| SSD1306 0.91" 128×32 OLED | 0x3C | SoC display on demand | **0 µA — rail hard-gated** (§2.5) |
| BME280 or BMP280 | 0x76 (or 0x77) | Ambient temperature | ~0.1 µA (sleep mode) |
| Momentary button | — (GPIO2) | Wake + screen cycling | ~0 (1 MΩ pull-up) |

**BME280 vs BMP280.** Either works; the firmware probes the chip ID (`0x60` = BME280,
`0x58` = BMP280) and adapts. Only temperature is required, and both give ±0.5–1.0 °C,
which is ample — the corrections in §5.6 are all first-order and none of them need better
than ±1 °C. The BME280's humidity channel is used only for an optional condensation
warning; pressure is read from neither. **Pick the BMP280 if choosing fresh** — cheaper,
identical for our purposes, and one fewer unused subsystem.

### 2.5 One bus, and what it costs

The board puts **all four I²C devices on D4/D5 (GPIO22/23)**: both INA219s, the OLED and
the temperature sensor. An earlier revision of this document specified two buses — the
sensors alone on GPIO6/7 so `LP_I2C` could reach them in deep sleep, the UI devices on a
separate gated bus. **The board does not do that, and the design follows the board.**

The consequence is not cosmetic and is stated up front:

> `LP_I2C` on the ESP32-C6 is hard-wired to **GPIO6 (SDA) / GPIO7 (SCL)** and is not
> remappable through the GPIO matrix. With the sensors on GPIO22/23 the LP core cannot
> reach them, so **there is no deep-sleep tier**: the counter cannot keep counting with the
> HP domain off. Tier **T2 (light sleep) is the floor**, at roughly 220–400 µA rather than
> the 40–90 µA a working LP handoff would have given (§9.3, §9.8). That is a deliberate,
> accepted trade — simpler hardware, no rework — not an oversight. §9.3 says what would
> have to change to get it back.

What one bus buys: a single HP I2C0 master, one set of pull-ups, no bus-ownership handoff
between two peripherals at every sleep boundary, and no second set of pads to verify. The
gauge sensors now share the bus with devices that are written rarely, which costs nothing
at 400 kHz — an OLED frame is ~1 KB and lands only during a UI wake.

What one bus costs, beyond the power floor: **bus contention is now a correctness concern
for the gauge.** The sampler and the UI both drive the same master, so every transaction
goes through one mutex, and an OLED write must never be able to delay a sample past its
deadline. The rule is that the sampler wins: display updates are chunked (§9.10 pushes only
dirty regions) and yield between chunks, so the longest the sampler can be blocked is one
short transfer.

**Power gating the display, on a shared bus.** An SSD1306 breakout is a poor sleeper: the
controller's own display-off state is ~10 µA, but the typical module adds an always-on LDO
and pull-ups that can total 50–100 µA — a quarter of the *entire* idle budget (§9.8). So
the display gets a high-side load switch (a small P-MOSFET plus a pull-up, or a part such
as the SiP32431) driven from GPIO21:

```
 3V3 ──┬────────────────────────────── BME280 + both INA219 VDD
       │                               Bus pull-ups 10k (always on)
       └──[ load switch ]──── VOLED ── SSD1306 VDD       (off except during a UI wake)
              ▲
              │ GPIO21
```

Two details that matter, both classic power-gating traps:

1. **Back-powering — now a gauge problem, not just a display problem.** The SSD1306's
   SDA/SCL pins have ESD diodes to its own VDD. With the bus pull-ups on the always-on 3V3
   rail, an unpowered display is fed through those diodes and neither turns off nor behaves
   predictably. On the *shared* bus of this board that misbehaviour lands on the sensor
   transactions too: a gated-off OLED clamping SDA takes the gauge down with it, which is a
   far worse failure than a blank screen. So the mitigation is **mandatory here, not
   advisory**: **1 kΩ series resistors** in the OLED's SDA and SCL lines to limit
   injection, and drive both pins low from firmware before cutting VOLED. If bus errors
   appear only when the display is off, this is the first thing to suspect.
2. **Ordering.** Power the rail, wait ≥100 ms for the SSD1306's internal charge pump and
   reset, *then* run the init sequence. Cutting the rail without first issuing display-off
   (`0xAE`) is electrically harmless but leaves visible ghosting on the next power-up.

If the load switch is omitted (a valid simplification for a bench build), the firmware
falls back to software display-off and the idle budget rises by whatever the module's
regulator costs — measure it before deciding it is acceptable. Omitting the switch also
removes the back-powering hazard above, which on a shared bus is worth something: an
always-powered OLED cannot corrupt a sensor read.

### 2.6 Button

A single momentary switch to ground on **GPIO2**, with a **1 MΩ external pull-up** and the
internal pull-up disabled. The internal pull-up (~45 kΩ) would draw ~73 µA for as long as
the button is held; 1 MΩ draws 3.3 µA and is still comfortably strong for a wake input
with a 100 nF debounce cap.

**The pin assignment is now unconstrained.** The LP-GPIO requirement in earlier revisions
existed only for *deep-sleep* GPIO wake, and with T2 as the floor (§9.3) the device never
deep-sleeps: light-sleep GPIO wake works on any GPIO. GPIO2 stays because it is already
wired and is a perfectly good choice — but it is no longer load-bearing, and one of the
design's open risks disappears with it.

Wake is configured as **low level** via `gpio_wakeup_enable()` plus
`esp_sleep_enable_gpio_wakeup()`. Debounce is 20 ms in firmware; the RC gives the analogue
edge, firmware gives the logical one.

Gestures (§9.10):

| Gesture | Action |
|---|---|
| Short press (<1 s) | Wake display / advance to the next screen |
| Long press (2 s) | Force a BLE advertising burst for 60 s (useful when `adv_timeout_min` has stopped advertising) |
| Very long (10 s) | Show the diagnostics screen (fault flags, uncertainty, tier and bus health) |

**No destructive action is reachable from the button.** Zeroing counters or forcing SoC
requires the BLE command path with its magic constants (§8.4). A button that can erase a
year of accumulation is a button that eventually will.

### 2.7 Temperature sensor placement

The BME280 measures *its own die*, which is a proxy for board temperature and only then a
proxy for pack temperature. Three consequences worth designing around:

- **Self-heating bias.** The ESP32-C6 during a BLE burst and the shunt under load both heat
  the board. Mount the BME280 as far from the shunt, the module and any regulator as the
  layout allows, ideally on a thermal island with a slot or a cutout. Firmware reads it
  **at the start of a wake, before the radio is enabled** (§4.4), which is when the board is
  closest to ambient.
- **It is not cell temperature.** For a pack in a different enclosure the correlation may be
  poor. The design accepts this — the corrections in §5.6 are first-order and a few degrees
  of error costs far less than having no temperature at all. If pack temperature is genuinely
  needed, run the BME280 on a short cable against the cells, or fit a DS18B20 there instead;
  the firmware's temperature source is a single abstraction and either can back it.
- **A configurable offset** (`cfg.t_offset_dC`, §8.3) lets the user null out a systematic
  installation bias against a reference thermometer.

### 2.8 Topology

```
        +-------------------------------------------------------+
   PACK+ o------+-----------------------------------o LOAD+/CHG+ |
                |                                                |
          [ Vbus divider, >=1M, GPIO-gated ] ---> INA219 Vbus    |
                                                                 |
   PACK- o---[ SHUNT 10 mR ]--------------------o LOAD-/CHG-     |
              |            |                                     |
              +------------+-------------------> INA219 VIN+/-   |
        +-------------------------------------------------------+
                          |
                          |  I2C: GPIO22/23 (D4/D5), one shared bus
                          v
                 +--------------------+
                 |   XIAO ESP32-C6    | --( BLE )--> Android
                 +--------------------+
                   |        |       |
        GPIO2 button|  GPIO21|       +--------> BME280   (3V3 always, 0.1 uA)
                   |   load |       +--------> SSD1306  (VOLED gated, 1k series R)
                   |  switch+----------------> VOLED rail
```

Sign convention throughout the firmware: **current > 0 = charging into the pack**,
**current < 0 = discharging**. This matches the JBD wire protocol (§7.5), so no sign
translation is needed at the protocol boundary.

---

## 2.9 Low-side sensing — the shunt in the negative lead

Low-side is the right place for the shunt in this design (§2.3): it is the only position
where *every* ampere, including the monitor's own, is forced through the sense element.
But the INA219 is fundamentally a high-side part, and using it low-side imposes real
constraints. They are listed here rather than discovered during bring-up.

### 2.9.1 The core problem: inputs go below ground

The INA219's IN+/IN− **common-mode range is 0 V to +26 V**, with an absolute maximum of
**GND − 0.3 V**. It has no headroom below its own ground.

Now put the shunt between the pack negative (call it **P**) and the system/load return
(call it **S**), and follow the current:

```
   discharge:  load ──> S ──[shunt]──> P ──> pack-      so  V(S) > V(P)
   charge:     chg  <── S <─[shunt]<── P <── pack-      so  V(S) < V(P)
```

Whichever node you choose as the ground reference, **the other one goes negative in one
of the two current directions.** That is arithmetic, not a wiring mistake, and no
rearrangement of the connections avoids it. A bidirectional low-side shunt always drives
a sense input below the local ground half the time.

So the question is not *how to avoid it* but *how far below, and which half*.

### 2.9.2 Constraint 1 — cap the shunt drop at ~100 mV

The excursion below ground equals the shunt drop. Against a −0.3 V absolute maximum,
design for **≤100 mV full scale** so a 3× transient overload still cannot reach the
damage threshold.

| Shunt | Max current at 100 mV | Current LSB (10 µV) | Offset error (±100 µV) |
|---|---|---|---|
| 10 mΩ | 10 A | 1 mA | ±10 mA |
| 5 mΩ | 20 A | 2 mA | ±20 mA |
| 2 mΩ | 50 A | 5 mA | ±50 mA |
| 1 mΩ | 100 A | 10 mA | ±100 mA |

Two consequences worth stating plainly:

1. **Low-side costs ~3× of current range** for a given shunt, versus the §2.3 high-side
   table. A 10 mΩ shunt covers ±32 A high-side but only ±10 A low-side.
2. **The PGA never needs its top range.** With a 100 mV design ceiling, PGA/4 (±160 mV)
   is the widest setting ever required and PGA/2 (±80 mV) covers most of it. The
   auto-ranging in §4.2 still works unchanged; it simply never selects /8. Set
   `cfg.pga_max` accordingly so a transient cannot range out into a setting whose
   full scale the hardware can no longer physically survive.

> **Honesty about the datasheet:** between 0 V and −0.3 V the part is inside its absolute
> maximum but *outside* its specified common-mode range, so offset and gain are
> unspecified there. This design accepts that and **verifies it by measurement** in M2 —
> comparing against a reference in *both* current directions, not just one. If accuracy
> degrades measurably on the negative side, fall back to §2.9.3 option L2 or change the
> part (§2.9.7). Do not assume it is fine because it appears to work at one polarity.

### 2.9.3 Constraint 2 — which node is ground?

This is the real architectural fork, and it trades two things the design cares about
against each other.

| | **L1 — reference to S** (system/load return) | **L2 — reference to P** (battery negative) |
|---|---|---|
| Monitor's own supply current | **Measured** — its return flows through the shunt | **Not measured** — it bypasses the shunt |
| §9.9 "correct by construction" | Holds | **Breaks**; self-draw must be modelled instead |
| Discharge (the common case) | IN− goes **negative** by up to the shunt drop | Both inputs ≥ 0 ✔ |
| Charge | Both inputs ≥ 0 ✔ | IN goes **negative** |
| VBUS to pack+ | Reads pack voltage **minus** the shunt drop — correctable in software (§2.9.5) | Reads true pack voltage directly |
| Grounding of MCU, OLED, BME280 | All on S | All on P |

**Recommendation: L1.** Self-consumption measurement is worth more than it looks. In
tier T2 the monitor draws ~220–400 µA continuously for months (§9.8); under L2 that is
invisible to the gauge and has to be re-introduced as a modelled constant, which is
exactly the kind of open-loop correction the whole design tries to avoid. Under L1 it is
simply measured, like any other load.

L1's cost — a bounded negative excursion during discharge — is the one that §2.9.2 caps
by design.

**Choose L2 instead if** the pack negative is already bonded to chassis and cannot be
separated (§2.9.4), or if M2 measurement shows the negative-side accuracy is
unacceptable. Under L2, set `cfg.self_discharge_comp_uA` (§8.3) to the measured dormant
draw so the gauge accounts for what it can no longer see.

### 2.9.4 Constraint 3 — parallel ground paths will ruin the measurement

This is the failure that actually bites people, and it is silent.

A low-side shunt sits *in* the ground path. **Any second electrical connection between P
and S bypasses it**, and the current that takes the bypass is simply never counted. There
is no error flag for this; the readings look entirely plausible and are just wrong.

Sources of an accidental bypass:

- A chassis bond on both sides (vehicles and boats, where pack− *is* chassis).
- A second charger, a PV array frame, a mains-powered charger with a bonded output.
- A shared negative between two loads wired to different sides of the shunt.
- **The USB cable during bring-up.** This one is guaranteed to catch someone: connect the
  XIAO's USB-C to a laptop, and the laptop's ground ties the monitor's ground to whatever
  else that laptop touches — including, often, the pack negative through a bench supply
  or a charger. The bring-up console and the shunt are then in parallel.

Mitigations, in order of preference:

1. **Single-point grounding.** Exactly one connection between P and S: the shunt. Every
   load, charger and chassis bond lands on S.
2. **Bring up on battery, not USB.** Power the XIAO from the pack and use BLE (M4+) or
   the OLED (M5b) instead of the USB console.
3. **A USB isolator** (ADuM3160-class) for the console during M1/M2, when USB is the only
   interface that exists yet.
4. At minimum, **check for the bypass**: with a known load running, compare the gauge's
   current against a clamp meter on the pack lead. A large under-read means current is
   escaping around the shunt.

### 2.9.5 Bus-voltage compensation (L1)

Under L1 the VBUS pin measures pack+ relative to **S**, but the true terminal voltage is
relative to **P**. They differ by exactly the shunt drop:

```
V_pack = V_bus_measured + V_shunt        (IN+ on S, IN− on P)
```

This is not an estimate. Both quantities come from the *same conversion pair* on the same
device, so the correction is exact and free — it is one addition in `convert()` (§4.3).
Without it a 100 mV drop is a 0.8 % error on a 12 V pack, which would land squarely on the
full-charge and empty voltage thresholds (§8.6) and bias every OCV re-sync (§5.4 C).

Implemented as `cfg.vbus_comp` ∈ {`NONE`, `ADD_SHUNT`, `SUB_SHUNT`}, because the sign
depends on which sense lead went to which node.

### 2.9.6 Constraint 4 — practical wiring notes

- **Series input resistors.** 10–100 Ω in each of IN+ and IN− limit current into the
  input ESD structures during transients and hot-plug. Keep the two **equal**: the
  INA219's input bias current across mismatched resistors becomes a differential offset,
  and at a 10 µV LSB even a fraction of a millivolt is tens of counts. Matched resistors
  turn it into a common-mode term instead, and the residual is absorbed by the zero
  calibration (§5.5).
- **Kelvin the shunt.** Sense at the shunt's own sense terminals, never at the power
  lugs. At 10 mΩ, 10 mm of copper in the wrong place is a several-percent error.
- **Check the breakout board.** Many INA219 modules hard-wire VBUS to IN−, which is
  correct for high-side and wrong for low-side — it would measure a node near ground
  instead of pack+. Verify the schematic and be prepared to cut a trace, or use a bare IC
  on your own board.
- **Route the monitor's own ground deliberately.** Under L1 it must land on S, on the
  load side of the shunt, or the self-consumption argument in §9.9 quietly stops being
  true.

### 2.9.7 If these constraints do not fit — alternative parts

| Part | Common-mode range | Verdict for bidirectional low-side |
|---|---|---|
| **INA219** (this design) | 0 … +26 V, abs max −0.3 V | Works within a ~100 mV drop budget. What we are doing. |
| **INA226** | 0 … +36 V, abs max −0.3 V | **Does not solve this.** Better offset and 16-bit resolution (§12), but the same 0 V-referenced limit. |
| **INA228 / INA229** | 0 … +85 V, abs max −0.3 V | **Also does not solve this.** Its hardware charge accumulator is still valuable (§12), but not for the low-side problem. |
| **INA282 / INA283** | **−14 V … +80 V** | Genuinely solves it — common mode well below ground. Analog output, so it needs an ADC; the C6's internal ADC is too noisy for coulomb counting, so this means an external delta-sigma ADC and a redesign of §4. |
| **INA240** | **−4 V … +80 V** | Same trade: solves the topology, costs the I²C simplicity. |
| **MAX17205 / BQ34Z100** | designed for low-side | Native low-side fuel gauges. They also bring their own gauging algorithm, largely replacing §5 — a different project, not a component swap. |
| Hall / fluxgate (ACS758 etc.) | galvanically isolated | No common-mode issue at all, but offset drift is orders of magnitude worse than a shunt. **Unsuitable for coulomb counting** — the §5.5 error budget would collapse. |

The honest summary: **staying with the INA219 low-side is fine, provided the shunt is
sized for ~100 mV and the grounding is single-point.** If the application needs both high
current *and* low-side, the INA282/INA240 route with an external ADC is the real answer,
and none of the I²C INA2xx parts will get you there.

> **This section describes the single-sensor design.** The board now carries **two**
> INA219s, one at each pole, so that the shunt can be installed in whichever lead the
> installation allows (§2.10). That changes the grounding recommendation from L1 to L2 and
> makes the software bus-voltage correction of §2.9.5 a *fallback* rather than the primary
> path. §2.9 remains the reference for **why** the constraints exist; §2.10 is the design.

---

## 2.10 Dual-sensor architecture — one INA219 per pole

### 2.10.1 Why two

The shunt cannot always go where the designer would like. On an existing installation you
break whichever lead is accessible: sometimes the negative at the battery post, sometimes
the positive at a fuse block. Fitting a sensor at **both** poles makes that an installation
choice rather than a board respin, and the firmware works out which one is looking at the
shunt.

The secondary payoff is that whichever sensor is *not* measuring current is not wasted: it
sits at the other pole with a correct voltage reference and a spare differential channel,
which is worth more than it sounds (§2.10.5).

**What two sensors do not buy, stated plainly:** a single INA219 already measures both
current and voltage correctly, using the exact software correction of §2.9.5. The second
device buys *installation flexibility, redundancy and diagnostics* — not accuracy that was
otherwise unreachable. If a build is committed to one pole forever, one sensor is enough
and §2.9 stands unchanged.

### 2.10.2 Position is encoded in the address

| Sensor | A1/A0 straps | Address | Physical position |
|---|---|---|---|
| **INA-A** | 0 / 0 | **0x40** | Positive pole |
| **INA-B** | 0 / 1 | **0x41** | Negative pole |

The address is a *physical fact about where the part is soldered*, never a configuration
item. That removes an entire class of "which sensor am I talking to" bug: role assignment
is then a pure function of the address and the installation mode, and a mis-strapped board
is caught at probe time rather than showing up as a sign error months later.

### 2.10.3 The two installation modes

Nodes: **PACK+**, **PACK−**; **LOAD+**, **LOAD−** are the outside world (load and charger
share them). The shunt sits in exactly one lead.

**Mode N — shunt in the negative lead** (the case that motivated this; §2.9)

```
   PACK+ ●─────────────────────────────● LOAD+ / CHG+
         │                             
   PACK− ●────[ SHUNT ]────────────────● LOAD- / CHG-
         P                S            
         │
      ═══╧═══ system ground = P   (grounding option L2, see §2.10.4)
```

| | INA-A @ 0x40 (positive pole) | INA-B @ 0x41 (negative pole) |
|---|---|---|
| GND | P | P |
| IN+ / IN− | both tied to **PACK+** (shorted) | across the shunt: IN+ = **S**, IN− = **P** |
| VBUS | **PACK+** | **LOAD+** |
| Provides | **true pack voltage**, plus a live offset self-check | **current**, plus load-side voltage |

**Mode P — shunt in the positive lead** (classic high-side)

```
   PACK+ ●────[ SHUNT ]────────────────● LOAD+ / CHG+
              Pp        Sp
   PACK− ●─────────────────────────────● LOAD- / CHG-
         │
      ═══╧═══ system ground = PACK−     (no offset problem at all)
```

| | INA-A @ 0x40 (positive pole) | INA-B @ 0x41 (negative pole) |
|---|---|---|
| GND | PACK− | PACK− |
| IN+ / IN− | across the shunt: IN+ = **Pp**, IN− = **Sp** | both tied to **PACK−** (shorted) |
| VBUS | **PACK+** | **LOAD+** |
| Provides | **current and pack voltage** | load-side voltage, redundancy |

In Mode P the high-side sensor does everything on its own and INA-B is purely diagnostic.
That asymmetry is expected: high-side is the topology the INA219 was designed for, and the
second sensor exists for Mode N.

**Both modes tie the idle sensor's IN+/IN− together rather than leaving them floating.**
Floating current-sense inputs drift to arbitrary common-mode and produce meaningless
differential readings; shorted, they read the device's own offset, which is a *useful*
number (§2.10.5).

### 2.10.4 Grounding: with two sensors, L2 wins

§2.9.3 recommended L1 (ground at the system return) because it kept the monitor's own
supply current inside the measured loop. With a second sensor at the positive pole that
calculus changes, and the reasoning is worth spelling out because it reverses an earlier
decision.

Under **L2** (ground = PACK− = P), for Mode N:

- **Pack voltage is exact with no correction.** INA-A's GND is at P, so its VBUS reads
  V(PACK+) − V(P) directly. No dependence on the shunt channel at all — which matters,
  because under L1 a fault in the shunt reading corrupted the *voltage* reading too.
- **The discharge direction is inside the specified common-mode range.** With GND = P and
  IN+ = S, discharge gives V(S) > V(P) = 0, so both inputs are positive (§2.9.1). Only
  charging goes below ground. Since charge currents are usually the smaller of the two,
  the excursion that lands outside the datasheet's guaranteed range is now the smaller one.
- **One ground domain.** Both sensors, the MCU, the display and the I²C pull-ups share P. The
  alternative — keeping ground at S and referencing only INA-A to P — puts two I²C slaves
  on grounds that differ by the shunt drop. That works at ≤100 mV, but it is a "within
  noise margin" argument rather than a design, and INA-A's own supply current would then
  bypass the shunt anyway, so it does not even deliver the self-measurement L1 was for.

The cost is L2's known one: **the monitor's own supply current returns to P and never
crosses the shunt, so self-consumption is not measured.** §9.9's "correct by construction"
does not apply in Mode N. The mitigation is unchanged and adequate: measure the dormant
draw once (Appendix C) and set `self_discharge_comp_uA` (§8.3). It is a bounded
~220–400 µA constant, and Appendix C measures it far more accurately than a 10 µV-LSB
shunt channel ever could. Note that the single-bus floor (§9.3) makes this constant
several times larger than the two-bus design assumed, which raises the cost of getting it
wrong — measure it, do not estimate it.

For **Mode P**, ground is PACK− and every one of these concerns evaporates — the monitor's
current flows through the high-side shunt on its way in, and there is no negative
common-mode excursion at all.

### 2.10.5 What the idle sensor is good for

The sensor not measuring current contributes four things, in descending order of value:

1. **A voltage reference that does not depend on the current channel.** In Mode N this is
   the primary pack-voltage source, and its independence is the point.
2. **Load-side voltage** via its VBUS pin on LOAD+. Pack voltage minus load-side voltage is
   the total drop across the shunt *and the cabling and connectors*. Comparing that against
   the shunt channel's own measurement of the shunt drop isolates **cable and connector
   resistance** — the difference is `I × R_wiring`. A connection that degrades over a
   winter shows up here long before it fails, and no single-sensor design can see it.
3. **A live offset monitor.** Its IN+/IN− are shorted, so its differential channel reads
   nothing but its own zero-current offset, continuously, with no need to disconnect the
   load. That is a direct measurement of the quantity §5.5 identifies as the dominant error
   source — on a *different die*, so it is not a substitute for calibrating the current
   sensor, but it tracks the same ambient and gives an independent handle on
   offset-versus-temperature (§5.6.4) without ever needing the user to disconnect anything.
4. **Redundancy.** If the current sensor stops answering, the gauge keeps a voltage source
   and can fall back to voltage-only SoC (§8.6) instead of going blind.

### 2.10.6 Role assignment and detection

Roles are assigned from `cfg.install_mode` ∈ {`AUTO`, `MODE_P`, `MODE_N`, `SINGLE`}.

Explicit configuration is the normal path. `AUTO` exists because getting it wrong inverts
the sign of every measurement, and a device that can check its own wiring is worth the code:

```
detect procedure (console `detect`, or AUTO at first boot):
  1. require |I| > 10 x the larger sensor offset          -- ask the user for a load
  2. read both differential channels
  3. the sensor across the shunt reads a differential proportional to that load;
     the shorted one reads its offset and does not move
  4. ratio > 10:1  -> assign roles, record the mode
     otherwise     -> refuse, report both readings, ask for a bigger load
```

The refusal matters. At zero current both sensors read approximately zero and the two modes
are indistinguishable — so detection without a load is not merely unreliable, it is
impossible, and the firmware says so rather than guessing. `AUTO` therefore stays in a
`ROLE_UNKNOWN` state, integrating nothing, until either a load appears or the mode is set
explicitly. Refusing to integrate is the right failure: a coulomb counter that runs with an
inverted sign produces confidently wrong numbers forever.

Once resolved, the mode is persisted (§6.1) and re-verified, not re-detected, on later
boots — with a warning if the evidence ever contradicts it.

### 2.10.7 Degraded operation

| Situation | Behaviour |
|---|---|
| Both sensors present, mode known | Normal. |
| Only 0x40 answers | Assume Mode P if it sees the shunt; otherwise fall back to **single-sensor Mode N with the §2.9.5 software bus correction**. Full function, one less cross-check. |
| Only 0x41 answers | Single-sensor Mode N with the §2.9.5 correction. Pack voltage becomes shunt-dependent again; raise `DEGRADED_VOLTAGE`. |
| Current sensor fails mid-run | Stop integrating, hold the count, add the gap to `drift_uncert_uAs` (§5.2), keep reporting voltage and voltage-derived SoC. Flag it. |
| Voltage sensor fails mid-run | Switch the voltage source to the current sensor's VBUS channel and enable the §2.9.5 correction at runtime. Coulomb counting is unaffected. |
| Neither answers | `SENSOR_FAULT`; serve the last persisted state over BLE and say so. Never fabricate. |

The single-sensor fallback is why §2.9.5 stays implemented even though §2.10 supersedes it:
it is what the firmware degrades *to*, and it means one dead sensor costs diagnostics rather
than function.

### 2.10.8 Cross-checks the pair makes possible

Each of these is cheap, and each catches a fault class that a single sensor cannot see:

| Check | Condition | Catches |
|---|---|---|
| Voltage agreement | `V_pack − V_load ≈ V_shunt + I × R_wiring` | Bad Kelvin connection, shorted or open sense lead, wrong shunt value |
| Wiring resistance trend | `(V_pack − V_load − V_shunt) / I` drifting up over weeks | Corroding lug, loose terminal, degrading connector |
| Idle-offset tracking | shorted sensor's differential vs. temperature | Sensor ageing; feeds §5.6.4 |
| Sign sanity | current positive while pack voltage falls steadily | Inverted sense leads, wrong install mode |
| Range sanity | one sensor saturated, the other nominal | Shunt in the wrong pole for the configured mode |

**What the pair still cannot catch is the parallel-ground-path bypass of §2.9.4.** The
bypass changes how much current flows *through* the shunt, and every sensor here measures
that same current honestly — so all the cross-checks pass while the answer is wrong.
Detecting it needs a shunt in *both* leads and a comparison of the two currents, which the
hardware could support in a future variant (both sensors across their own shunts, the
difference being leakage) but this design does not. **Single-point grounding remains a
build requirement, not something the firmware can verify.**

---

## 3. Firmware Architecture

### 3.1 Stack

- **ESP-IDF 5.3.x** in C. Not Arduino — the design needs NimBLE control, NVS tuning,
  power-management locks and the LP memory domain.
- **NimBLE** host (`CONFIG_BT_NIMBLE_ENABLED`), peripheral role only.
- FreeRTOS: 3 application tasks plus esp_timer callbacks.

### 3.2 Module layout

```
bat-monitor/
├── CMakeLists.txt
├── sdkconfig.defaults
├── partitions.csv
├── main/
│   ├── main.c                 # init sequence, task creation
│   ├── app_events.c/.h        # esp_event loop, event IDs
│   └── CMakeLists.txt
└── components/
    ├── ina219/
    │   └── ina219.c/.h        # register-level driver, PGA auto-range, raw→SI
    ├── bme280/
    │   └── bme280.c/.h        # chip-ID probe (BME280/BMP280), forced-mode read, compensation
    ├── ui/
    │   ├── ui_task.c/.h       # button gestures, screen state machine, wake/timeout
    │   ├── oled_ssd1306.c/.h  # rail gating, init sequence, 128×32 framebuffer
    │   └── screens.c/.h       # one render function per screen (Appendix D)
    ├── fuelgauge/
    │   ├── fg_core.c/.h       # integrator + anchor state machine
    │   ├── fg_ocv.c/.h        # chemistry OCV→SoC lookup
    │   └── fg_types.h         # fg_state_t (the persisted struct)
    ├── pm/
    │   └── pm_tiers.c/.h      # tier state machine, light-sleep entry/exit (§9.3)
    ├── nvstore/
    │   └── nv_state.c/.h      # A/B slots, CRC, retained-RAM mirror, commit policy
    ├── ble_svc/
    │   ├── ble_main.c         # NimBLE init, advertising, GAP callbacks
    │   ├── svc_jbd.c          # JBD/Xiaoxiang emulation (0xFF00 + NUS)
    │   ├── svc_bas.c          # standard Battery Service 0x180F
    │   ├── svc_vendor.c       # native TLV telemetry / config / command
    │   └── jbd_frame.c/.h     # frame encode/decode + checksum
    └── config/
        └── cfg_store.c/.h     # user configuration, NVS-backed, versioned
```

### 3.3 Tasks and timing

| Task | Prio | Cadence | Responsibility |
|---|---|---|---|
| `sampler` | 6 | 100 ms esp_timer | Read INA219, timestamp, push to queue. Never blocks on BLE. |
| `gauge` | 5 | queue-driven | Integrate charge/energy, run the anchor state machine, emit events |
| `persist` | 3 | 60 s + event-driven | Decide whether to commit state to NVS (§6.3) |
| `ble` | 4 | NimBLE host | GATT server, notifications, advertisement refresh |
| `ui` | 3 | event + 100 ms while awake | Button gestures, screen rendering, display rail and timeout (§9.10). Exists only while the display is on — or continuously in debug mode (§9.11). |
| `pm_tier` | 2 | 1 s | Evaluate tier promotion/demotion and configure the light-sleep floor (§9.2) |

`pm_tier` runs at the lowest priority on purpose: entering a sleep state is never urgent,
and it must never delay a sample or a BLE event.

`ui` reads the same seqlock-protected snapshot the BLE task uses. It never computes
anything — a screen that derives its own numbers is a screen that eventually disagrees
with the app.

Inter-task: a 16-deep `QueueHandle_t` of `sample_t` from sampler → gauge; an `esp_event`
loop broadcasting gauge → {ble, persist}. The shared `fg_state_t` snapshot is protected
by a seqlock (single writer = gauge, readers = ble), so BLE reads never block the
integrator and never tear.

**Timestamping:** `esp_timer_get_time()` (µs, monotonic, survives light sleep). The
integration interval is *measured*, never assumed — a delayed sample integrates its real dt.

---

## 4. Sampling and the INA219 Driver

### 4.1 Sensor configuration

The INA219 draws ~1 mA while converting and ~6 µA powered down, so the conversion mode is
**not fixed** — it is selected by the active power tier (§9.4). Two profiles:

```
PROFILE_CONTINUOUS   (tier T0/T1, high fidelity)
  CONFIG = BRNG=1 (32 V) | PG=auto | BADC=12b_128avg | SADC=12b_128avg
         | MODE=111 (shunt+bus continuous)
  → ~7 samples/s, ~1 mA continuous sensor draw

PROFILE_TRIGGERED    (tier T2, low power)
  CONFIG = BRNG=1 (32 V) | PG=auto | BADC=12b_1avg  | SADC=12b_4avg
         | MODE=011 (shunt+bus triggered, auto power-down after conversion)
  → one conversion on demand (~2.6 ms), then ~6 µA until the next trigger
```

In `PROFILE_CONTINUOUS` the 100 ms sampler tick is intentionally faster than the
conversion rate: the driver polls the `CNVR` (conversion-ready) flag and emits a sample
only when a fresh conversion exists, giving ~7 samples/s with the ADC's 128× hardware
averaging acting as the anti-alias filter. Hardware averaging is free and beats software
oversampling of a noisy 12-bit ADC.

In `PROFILE_TRIGGERED` the averaging is cut to keep the 1 mA window short. The resulting
per-sample noise is higher, but it is **zero-mean and uncorrelated**, so it averages out
in the integral rather than accumulating — noise costs √t growth in uncertainty, while a
longer conversion costs linear growth in charge consumed. Trading averaging for duty
cycle is the correct direction for a coulomb counter.

The `OVF` (math overflow) flag is checked on every read; if set, the sample is discarded
and `FG_EVT_SENSOR_FAULT` is raised.

### 4.2 PGA auto-ranging

A fixed PGA/8 wastes three bits while the pack idles at 50 mA. The driver auto-ranges:

```
if |Vshunt| > 0.90 × range_fs        -> step PGA down (coarser); discard this sample
if |Vshunt| < 0.40 × next_lower_fs   -> step PGA up (finer) after 8 consecutive samples
```

The 0.90/0.40 hysteresis plus the 8-sample dwell prevents oscillation. Every range change
discards one conversion. Range is never increased while |I| exceeds 25 % of full scale,
so a rising load transient cannot be clipped.

At PGA/1 (±40 mV) with a 10 mΩ shunt the LSB is 1 mA over a ±4 A span — the resolution
that matters most, because that is where the device spends 95 % of its life.

### 4.3 Raw → SI conversion

```c
/* shunt LSB = 10 µV regardless of PGA; sign-extend per the current PGA setting */
i_uA  = (vshunt_uV * 1000) / r_shunt_mOhm;          /* integer math throughout */
i_uA -= cfg.i_offset_uA;                            /* zero calibration, §5.5   */
i_uA  = (int32_t)((int64_t)i_uA * cfg.i_gain_ppm / 1000000);
if (cfg.invert_sign) i_uA = -i_uA;                  /* lead orientation, §2.9   */

v_uV  = (uint32_t)(((uint64_t)vbus_raw * 4000 * cfg.vbus_divider_q16) >> 16);
switch (cfg.vbus_comp) {                            /* low-side, §2.9.5         */
case VBUS_COMP_ADD_SHUNT: v_uV += vshunt_uV; break;
case VBUS_COMP_SUB_SHUNT: v_uV -= vshunt_uV; break;
default:                                   break;
}

p_uW  = (int32_t)((int64_t)i_uA * v_uV / 1000000);
```

Two of those lines exist only because of the low-side topology (§2.9). Both are applied
to the **raw** shunt reading, before the offset — the shunt drop the bus measurement needs
correcting by is the physical one, not the calibrated one.

No `float` anywhere in the hot path or in persisted state. Float accumulators silently
stop accumulating small increments once the exponent diverges — precisely the failure
mode a coulomb counter must not have.

### 4.4 Temperature acquisition

The BME280 is read in **forced mode**: wake, single measurement, automatic return to sleep
(~0.1 µA). Oversampling ×1 on temperature, humidity and pressure skipped where the part
allows — the measurement takes ~7 ms and costs ~2 µA·s, which at any sane cadence rounds
to nothing.

| Tier | Read cadence | Rationale |
|---|---|---|
| T0 / T1 | 10 s | Board is self-heating; track it |
| T2 (the floor) | 60 s | Thermal mass makes anything faster pointless; see below |
| Any UI wake | Immediately, **before enabling the radio** | Least self-heated moment (§2.7) |

**T2 and temperature.** In T2 the core light-sleeps between samples and the BME280 is read
once a minute, so the integrator is never working from a badly stale temperature — one of
the few places where losing the deep-sleep tier (§9.3) makes the design *simpler* rather
than worse. The two-bus design had to reason about a temperature captured up to 15 minutes
earlier; this one does not. What temperature affects during long idles — self-discharge
(§5.6) — is still computed from the interval's endpoint readings rather than a point
sample.

Bosch's compensation formulae use the part's factory calibration coefficients and are
implemented in fixed point (the `BME280_S32_t` variants from the datasheet), for the same
reason the rest of the firmware avoids floats. Output is `int16_t temp_dC` in units of
0.1 °C. The calibration coefficients are read once at init and cached, not re-read per
measurement.

### 4.5 Sampling two sensors

The two channels have genuinely different requirements, and treating them the same would
waste power for no accuracy:

| Quantity | From | Cadence | Why |
|---|---|---|---|
| Current | current-role sensor, differential channel | **every sample** (7 Hz / 1 Hz) | It is the integrand. A missed sample is uncounted charge (§5.2). |
| Pack voltage | voltage-role sensor, VBUS | **every 8th sample** | Voltage moves slowly and is used for thresholds and OCV, never integrated. |
| Load-side voltage | current-role sensor, VBUS | every 64th sample | Diagnostics only (§2.10.5). |
| Idle offset | voltage-role sensor, differential | every 64th sample | Slow-moving; feeds §5.6.4. |

Current is therefore read at full rate from one device while the second is polled at an
eighth of it. In `PROFILE_TRIGGERED` the voltage sensor is only triggered on the samples
where it is actually read, so its ~2 µA (§9.4) is divided by eight again.

**Ordering matters.** The current sensor is read *first* in each pass, and its timestamp is
the one that dates the sample. Voltage arriving a few tens of milliseconds later is
irrelevant to a quantity that moves in seconds; a delayed *current* reading directly
distorts `dt` and therefore the integral.

The two devices are never read in one transaction — I²C has no such thing — so their
readings are strictly not simultaneous. This is worth being explicit about because the
§2.10.8 voltage-agreement check compares them: the check is therefore only valid when the
current is steady, and it is skipped when `|dI/dt|` exceeds a threshold. Comparing a pack
voltage from one instant against a shunt drop from another during a load step produces
alarming nonsense.

### 4.6 Temperature sensor absence

Sensor absence is not a fault: if no BME280/BMP280 answers on the bus, the firmware logs it,
sets `TEMP_UNAVAILABLE`, and falls back to the ESP32 internal die sensor with a fixed
`-8 °C` nominal offset — poor, but better than disabling every correction in §5.6. All
temperature-dependent corrections are additionally gated on `TEMP_VALID`, so a failed
sensor degrades the gauge to its uncompensated behaviour rather than corrupting it.

---

## 5. Fuel Gauge Algorithm

### 5.1 Persisted state

```c
typedef struct __attribute__((packed)) {
    uint16_t magic;                 /* 0xB47C                                    */
    uint8_t  version;               /* struct version = 1                        */
    uint8_t  flags;                 /* FULL_SEEN, EMPTY_SEEN, CAL_VALID, DIRTY   */
    uint32_t seq;                   /* monotonic write sequence (A/B arbitration)*/
    int64_t  charge_uAs;            /* remaining charge, µA·s   <-- the core value*/
    int64_t  cum_charge_in_uAs;     /* lifetime charge in                        */
    int64_t  cum_charge_out_uAs;    /* lifetime charge out                       */
    int64_t  cum_energy_in_uWs;     /* lifetime energy in                        */
    int64_t  cum_energy_out_uWs;    /* lifetime energy out                       */
    int64_t  rem_uAs_frac;          /* carried truncation remainder              */
    uint32_t full_capacity_uAh;     /* learned capacity, NORMALISED TO 25 °C     */
    int32_t  offset_tempco_nA_per_K;/* learned INA219 offset slope (§5.6.4)      */
    int16_t  last_temp_dC;          /* last valid temperature, 0.1 °C            */
    uint16_t offset_fit_points;     /* least-squares sample count for the above  */
    uint32_t cycle_charge_uAs;      /* partial-cycle accumulator                 */
    uint32_t drift_uncert_uAs;      /* accumulated uncertainty since last anchor */
    uint16_t cycle_count;
    uint16_t soh_permille;
    uint32_t uptime_total_s;
    uint32_t last_full_s;           /* seconds-since-first-boot; 0 = never       */
    uint32_t crc32;                 /* over all preceding bytes                  */
} fg_state_t;                       /* ~104 bytes; pad to a 8-byte multiple */
```

**Units rationale.** Charge is `int64` **microampere-seconds**. 1 mAh = 1000 µA × 3600 s
= 3.6 × 10⁶ µA·s. An `int64` spans ±9.2 × 10¹⁸ µA·s ≈ ±2.5 × 10¹² mAh — 100 A for a
century overflows nothing — while every increment, however small, remains exactly
representable. A 1 µA leak over 1 s is exactly 1 count.

### 5.2 Integration

Per accepted sample, with `dt_us` measured since the previous accepted sample:

```c
int64_t dq_num = (int64_t)i_uA * dt_us + st.rem_uAs_frac;
int64_t dq_uAs = dq_num / 1000000;
st.rem_uAs_frac = dq_num % 1000000;          /* exact, no cumulative truncation */

st.charge_uAs += dq_uAs;
if (dq_uAs > 0) { st.cum_charge_in_uAs  += dq_uAs; st.cum_energy_in_uWs  += de_uWs; }
else            { st.cum_charge_out_uAs -= dq_uAs; st.cum_energy_out_uWs -= de_uWs; }
```

Energy accumulates the same way from `p_uW × dt_us`.

**Gap guards.** A `dt_us` above `MAX_GAP_US` (5 s — sensor stall, watchdog, long flash
write) integrates at the last known good current but sets `FG_FLAG_GAP` and adds the
worst-case error to `drift_uncert_uAs`. A gap beyond 60 s (crash restart, unexpected
sleep) does not integrate at all; the whole interval becomes uncertainty.

**Deadband.** Currents below `cfg.i_deadband_uA` in magnitude (default 3 mA ≈ 3× the
residual post-calibration offset) integrate as zero. Without this, a constant +400 µA of
uncorrected offset "charges" the pack by 3.5 mAh/day forever. This is the single most
important line in the gauge — see the error budget in §5.5.

**Peukert compensation** (`cfg.peukert_q8`, optional): for lead-acid, effective discharge
scales as `dq_eff = dq × (I / I_rated)^(k−1)`. Disabled (k = 1.0) by default for Li-ion,
where the effect is small and better captured by OCV re-anchoring.

**Temperature compensation:** applied from the BME280/BMP280 reading — see §5.6 for the
full model. Note what is *not* done there: the raw `dq_uAs` integration above is never
scaled by temperature. Charge that flowed, flowed. Temperature changes how much charge the
pack can *hold* and what a given voltage *means*, not how many coulombs passed through the
shunt.

### 5.3 Derived values

```
soc_permille    = clamp(charge_uAs × 1000 / (full_capacity_uAh × 3600), 0, 1000)
time_to_empty_s = (i_uA < −deadband) ?  charge_uAs / (−i_uA) : INFINITE
time_to_full_s  = (i_uA >  deadband) ? (full_cap_uAs − charge_uAs) / i_uA : INFINITE
```

Displayed current and power are additionally EMA-filtered (α = 1/8, ~1 s time constant).
**The filtered value is never integrated** — integration always uses the raw sample, or
the filter's lag would bias the count during load transients.

### 5.4 Drift correction — the anchor state machine

Coulomb counting is an open integrator; it needs anchors. There are four:

```
                 ┌──────────────┐
                 │   UNKNOWN    │  first boot / CRC failure
                 │  SoC := OCV  │
                 └──────┬───────┘
                        │ V and I stable
                        v
   ┌────────►  ┌──────────────────┐  ─────────►  ┌───────────────┐
   │           │     COUNTING     │              │  FULL_DETECT  │
   │           │  (normal state)  │  ◄─────────  │ charge := cap │
   │           └──────┬───────────┘    reset     └───────────────┘
   │                  │ V < v_empty under load
   │                  v
   │           ┌───────────────┐
   └────────── │  EMPTY_DETECT │  charge := 0 ; learn capacity
     resume    └───────────────┘
                        ▲
                        │ |I| < deadband for t_rest
                 ┌──────┴───────┐
                 │    RESTING   │  OCV re-sync, blended
                 └──────────────┘
```

**A. Full-charge detection** — the highest-confidence anchor. All true simultaneously for
`t_full_hold` (default 60 s):

- `V ≥ cfg.v_100pct_uV` (§8.6 — the user-configured 100 % voltage)
- `0 < I < cfg.i_taper_uA` (e.g. C/30)
- SoC was rising

Action: `charge_uAs := full_capacity_uAh × 3600`; set `FULL_SEEN`; zero
`drift_uncert_uAs`; if `EMPTY_SEEN` is set and the excursion depth was ≥ 60 %, run
capacity learning (D).

**B. Empty detection** — `V ≤ cfg.v_0pct_uV` (§8.6) while `I < −cfg.i_min_load_uA`, held for
`t_empty_hold` (10 s — short, to catch a genuinely sagging pack). A reading below 73 % of
`v_0pct` (8.4 V on 11.5 V) never qualifies: no 12 V chemistry rests there, so it is an
absent battery or a disconnected VBUS lead, and a single such sample once latched a
healthy pack at 0 % and saved it. Action:
`charge_uAs := 0`; set `EMPTY_SEEN`. Rejected if the discharge current exceeds
`cfg.i_sag_ignore_uA`, since a heavy transient sag is not an empty pack.

**C. Rest OCV re-sync** — after `t_rest` (default 10 min) with |I| below the **rest
current**, terminal voltage approximates OCV. The rest current is C/400 (110 mA on
44 A·h), deliberately *not* the integration deadband: a monitor powered from its own
pack draws a steady few to few tens of milliamps forever, and with the deadband as the
threshold such a pack never rested, so a wrong count was never corrected. At C/400 the
compensated I·R term is under a millivolt and polarisation a few, a fraction of a percent
of SoC. Look up `soc_ocv` and blend:

```
charge += ocv_blend_gain × (charge_from_ocv − charge)
```

where `charge_from_ocv` comes from the voltage→SoC mapping of §8.6, i.e. the OCV table
scaled to the user's `v_0pct_uV` / `v_100pct_uV` endpoints (or a straight line between
them in `v_curve_mode = 0`). The first rest after a boot **replaces** the count instead
of blending: a count restored from flash has not been checked against the pack in this
power-up, and blending a stale 0 % toward the truth at 25 % per rest period takes hours.

*As implemented:* one 11-point per-cell table per chemistry (`fg_chem_profile()`:
flooded, AGM, gel, LiFePO₄, Li-ion, LiPo, LTO, NiMH), multiplied by the cell count and
stretched to `v_0pct`/`v_100pct`. Flooded is the common 25 °C 12 V chart (0 % 11.50 V,
25 % 11.95 V, 50 % 12.25 V, 75 % 12.45 V, 100 % 12.70 V). The `ocv_valid_band` above is
the profile's trust band: LiFePO₄ re-syncs only below 15 % and above 95 %, NiMH below
15 % and above 90 %. A resting voltage in between leaves the count alone. Applied only when the disagreement exceeds 5 %; below that
the OCV table is less accurate than the counter. **Gated by chemistry:** for LiFePO₄ the
OCV curve is famously flat between roughly 20 % and 80 %, so `cfg.ocv_valid_band`
suppresses re-sync in that region.

**D. Capacity learning (SoH)** — on a DISCHARGE span between two anchors of depth ≥
`cfg.learn_min_depth`:

```
measured_cap      = |charge counted between anchors| / (soc_hi − soc_lo)
full_capacity_uAh = IIR(full_capacity_uAh, measured_cap, α = blend × depth/500)
soh_permille      = full_capacity_uAh × 1000 / cfg.design_capacity_uAh
```

The first measurement replaces the nameplate outright rather than blending toward it;
after that α is weighted by the span's depth, since a deeper span is better evidence.
Charging during a span disqualifies it: lead-acid puts back less than it takes.

Clamped to 5–150 % of design capacity. A result outside that range is a measurement
fault, not a battery fault, and is rejected with a log entry rather than accepted.

The implemented mechanism in full — what anchors a span, the gates a span must pass,
and why a board may never learn — is [CAPACITY.md](CAPACITY.md).

**Cycle counting:** `cycle_charge_uAs` accumulates discharge magnitude only; each time it
passes one design capacity, `cycle_count++` and it wraps. This is the standard
"equivalent full cycles" definition and matches what the JBD field means.

### 5.5 Zero-current calibration

At first boot, and on demand via the command characteristic, with load and charger
disconnected (the user must confirm this — firmware cannot verify it):

1. Force PGA/1; take 256 samples over ~20 s.
2. `i_offset_uA := mean`. Reject if `stddev > 5 × expected_noise` (something was drawing
   current) → return `CAL_ERR_UNSTABLE`.
3. Store in config NVS; set `CAL_VALID`.

Gain calibration is separate and user-driven: pass a known current, or discharge a known
charge into a measured load, then write `i_gain_ppm`. Documented in the user guide rather
than automated.

**Residual error budget after calibration** (10 mΩ shunt, PGA/1):

| Source | Magnitude | Daily drift |
|---|---|---|
| Offset drift over temperature, **uncompensated** | ±20 µV | ±2 mA → ±48 mAh |
| Offset drift over temperature, **compensated (§5.6.4)** | ±5 µV | ±0.5 mA → ±12 mAh |
| ADC noise (random walk at 7 Hz) | ±1 mA rms | ±0.5 mAh (grows as √t) |
| Gain error (post-calibration) | ±0.2 % | 0.2 % of throughput |
| Timebase (`esp_timer`, ±0.5 %) | — | 0.5 % of throughput |
| **Deadband suppression** | −3 mA … +3 mA | **0** |

With the deadband active, idle drift is zero and error accrues only while current
actually flows — bounded by the gain error, i.e. a *percentage of real throughput*
rather than an unbounded integral of noise. That is why the deadband matters more than
the offset calibration itself.

Temperature compensation of the offset (§5.6.4) is what makes it possible to *shrink* the
deadband, since the deadband must exceed the worst-case residual offset. That is the
second-order benefit of adding the BME280 and, over a year, probably the larger one.

### 5.6 Temperature correction

Four distinct corrections, in descending order of value. Each is independently switchable,
each is gated on `TEMP_VALID` (§4.4), and none of them touches `charge_uAs` retroactively —
they change *interpretation*, never the accumulated count. This is the same principle as
the voltage endpoints in §8.6, and for the same reason: the count is measurement, and
everything else is a model that may later be found wrong.

Reference temperature is 25 °C throughout. `T` is `temp_dC / 10`.

#### 5.6.1 Usable capacity vs. temperature

A cold pack delivers less. This is the correction users actually notice — a 100 %
indication at −10 °C that runs out in half the expected time reads as a broken gauge.

```
cap_eff_uAh = full_capacity_uAh × (1 + cap_tempco_ppm_per_K × (T − 25) / 1e6)
soc_permille = charge_uAs × 1000 / (cap_eff_uAh × 3600)
```

Defaults (`cfg.cap_tempco_ppm_per_K`): Li-ion +6000 ppm/K, LiFePO₄ +8000, lead-acid
+6000 — i.e. roughly 0.6–0.8 % of capacity lost per °C below 25 °C. Clamped so
`cap_eff` stays within 50–110 % of the nominal, because the linear model is only credible
over roughly −20…+45 °C and extrapolating it further produces nonsense rather than error.

`full_capacity_uAh` itself — the *learned* value — is always normalised back to 25 °C
before storage, so learning at 5 °C does not permanently shrink the recorded capacity.
Without that normalisation, a winter of learning would ratchet the pack's apparent health
downwards and never recover. This is the subtlest interaction in the temperature model and
the one most worth a unit test.

#### 5.6.2 Voltage endpoints and the OCV curve

Both `v_0pct_uV` and `v_100pct_uV` (§8.6), and every point of the OCV table, shift with
temperature:

```
v_corrected = v_nominal + v_tempco_uV_per_K_per_cell × cell_count × (T − 25)
```

Defaults (`cfg.v_tempco_uV_per_K_per_cell`): lead-acid **−4000 µV/K/cell** (the classic
−3…−5 mV/°C/cell charge-voltage coefficient — significant, and the main reason lead-acid
chargers need temperature compensation at all), Li-ion and LiFePO₄ **0** (small enough to
ignore relative to the OCV table's own uncertainty).

This feeds directly into full detection (§5.4 A), empty detection (§5.4 B) and rest OCV
re-sync (§5.4 C). A lead-acid pack charging at 0 °C legitimately reaches a higher terminal
voltage; without this correction the gauge would call it full early, every cold morning.

#### 5.6.3 Self-discharge

Self-discharge roughly doubles per 10 °C. When `cfg.self_discharge_comp_uA` (§9.9) is
non-zero it is scaled with an Arrhenius-style factor before being applied:

```
i_self_uA = self_discharge_comp_uA × 2^((T − 25) / 10)
```

Computed over each dormant interval from its endpoint temperatures (§4.4), not from a
stale point sample. This matters mainly for packs stored warm — a pack at 40 °C
self-discharges about three times faster than the 25 °C figure the user configured, and
across a summer the difference is real.

#### 5.6.4 INA219 offset drift — the highest-leverage correction

The INA219's offset drifts with temperature, and §5.5 identifies that drift as the
dominant residual error. With a thermometer on the board this becomes correctable rather
than merely tolerable:

```
i_offset_uA(T) = i_offset_uA_25 + offset_tempco_nA_per_K × (T − 25) / 1000
```

`offset_tempco_nA_per_K` is populated in one of two ways:

- **Manual:** the user runs the zero-current calibration (§5.5) at two well-separated
  temperatures; the firmware fits the slope through the two points. Documented as an
  optional advanced procedure.
- **Automatic (default):** whenever the gauge is in `RESTING` state with the load
  confirmed disconnected — which cannot be assumed, so this path requires the user to have
  set `cfg.allow_auto_offset_learn` — each rest period contributes an
  (offset, temperature) pair to a small least-squares accumulator. The slope emerges over
  weeks of ordinary use, with no procedure at all.

**Guard rails, because this correction can make things worse if it goes wrong:** the fitted
slope is clamped to ±500 nA/K, is only applied once at least 5 well-separated points
(≥10 °C span) have been collected, and is discarded entirely on `RESET_LEARN`. An
uncorrected offset is a known, bounded error; a wrongly-fitted tempco is an unbounded one,
so the default posture is to do nothing until the evidence is good.

#### 5.6.5 Reporting and alarms

- `temp_dC` appears in the vendor telemetry struct (§7.8), in the JBD NTC field (§7.5) and
  on the display (Appendix D).
- Over/under-temperature warning bits are set from the real sensor rather than the die
  sensor (§7.6), with `cfg.t_warn_hi_dC` / `t_warn_lo_dC` defaults of 60 °C / −20 °C.
- **Charging below 0 °C** sets a distinct warning bit for Li-ion and LiFePO₄ chemistries.
  The device cannot prevent it — there is no FET — but it is the single most damaging
  thing that can routinely happen to a lithium pack, and a monitor that can see it
  happening and says nothing is not doing its job. The warning propagates to the app, the
  advertisement flags byte and the display.
- With a BME280 fitted, relative humidity above 85 % combined with a board temperature
  within 2 °C of the dew point raises an optional condensation warning. Off by default;
  genuinely useful for marine and vehicle installations.

---

## 6. Non-Volatile Storage

### 6.1 What is stored where

| Data | Store | Cadence |
|---|---|---|
| `fg_state_t` accumulators | NVS namespace `fg`, blobs `state_a` / `state_b` | §6.3 |
| `cfg_t` user configuration | NVS namespace `cfg`, typed keys | on commit only |
| Hot mirror of `fg_state_t` | **LP SRAM** (`RTC_NOINIT_ATTR`) | every sample |
| Resolved `install_mode` + sensor roles | NVS namespace `cfg` | once, when detection succeeds (§2.10.6) |
| Event log (last 32 records) | NVS blob, ring | on event |

The LP-SRAM mirror is what makes the flash cadence relaxed: it survives reset, brownout
reset and every sleep state — everything except true power loss. On boot the firmware
compares the LP mirror and the NVS copy and takes whichever has a valid CRC and the higher
`seq`. Note that the mirror needs only *retained RAM*, not the LP core: it works exactly as
specified even though §9.3 gives up on LP-core execution.

There is no LP-core handoff buffer. In the two-bus design an `lp_accum_t` carried charge
accumulated while the HP domain was off, and every HP wake merged it; with T2 as the floor
the HP core never stops running, so the integrator owns the count continuously and there is
nothing to merge. One fewer shared structure, one fewer CRC-and-retry protocol, one fewer
class of bug — the honest compensation for a 5× worse idle current.

Because the mirror absorbs every sample, an idle week still costs the same number of flash
writes as an idle hour: the commit policy (§6.3) is driven by charge delta and elapsed
time, not by sample count.

### 6.2 Partition table

```csv
# Name,   Type, SubType, Offset,   Size
nvs,      data, nvs,     0x9000,   0x10000    # 64 KB (enlarged from the 24 KB default)
otadata,  data, ota,     0x19000,  0x2000
phy_init, data, phy,     0x1b000,  0x1000
ota_0,    app,  ota_0,   0x20000,  0x1C0000
storage,  data, nvs,     0x1E0000, 0x10000    # reserved: event log / future use
ota_1,    app,  ota_1,   0x1F0000, 0x1C0000
```

Two app slots for firmware updates over BLE (M7, brought forward; CLI.md §6). This
needs the XIAO's full 4 MB, not IDF's 2 MB default. Everything that existed in the
single-slot layout kept its offset: `ota_0` is where `factory` was, and `ota_1` goes
*after* `storage` rather than displacing it, so moving to this table over USB loses no
calibration, gauge state or bonds. The bootloader is built with
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE`: a new image boots on probation and is abandoned
at the next reset unless the app confirms it — and the firmware forces that reset after
ten minutes unconfirmed, so an image that boots but has lost its radio cannot strand the
board.

### 6.2a SoC history

288 points of two bytes plus a state letter each, in NVS namespace `hist`, rewritten
at every point. The interval between points is a setting (`hist every <s>`, cfg key
`hper`, 60..3600 s, default 300) because the ring is fixed at 288 points and the interval
is therefore the only thing that trades span against resolution: 5 minutes covers 24 h,
10 minutes covers 48 h, and a pack watched through a single charge cycle is better served
by the first. That is one ~900-byte blob write per point -- 288 a day at the default, a
small addition to the gauge commits below; the 60 s floor is ten times that and still
within budget. It is here rather than in the clients because the monitor is
the device that is always on. Clients graph the same history whenever they connect,
instead of each recording its own from the moment it happened to be connected. There is
no clock, so a restored history gets a gap mark at boot rather than a guessed duration
(CLI.md §6 `hist`).

### 6.3 Write policy and flash endurance

Commit `fg_state_t` when **any** of:

- 60 s elapsed **and** `|charge − last_committed| ≥ 0.5 %` of full capacity
- an anchor fired (FULL, EMPTY, OCV re-sync, capacity learned)
- configuration changed
- 15 minutes elapsed with any change at all (heartbeat)
- the brownout detector fired (§6.4)

**Endurance math.** Worst case ≈ 1 write/min = 1440/day = 525 600/year. The 64 KB NVS
partition is 16 × 4 KB pages. A 96-byte blob plus NVS overhead ≈ 128 bytes → ~30 entries
per page → a page fills every ~30 writes, and NVS round-robins across 16 pages, so each
physical page is erased roughly once per 480 writes. That is **~1100 erase cycles per
page per year** against a 100 000-cycle flash endurance spec → **> 90 years**. Flash wear
is a non-issue at this cadence; even a 10 s cadence would clear 15 years. The policy above
is deliberately generous.

### 6.4 Power-loss safety

Three layers:

1. **CRC + sequence, A/B slots.** Two keys written alternately; on boot, take the valid
   blob with the higher `seq`. A torn write can never destroy both. NVS is already
   power-safe at the entry level; A/B protects against a corrupted *blob*, not just a
   torn entry.
2. **Brownout hook.** Configure the ESP32 brownout detector one threshold *above* the
   reset level; the ISR-safe handler refreshes the LP-SRAM mirror and, if time permits,
   kicks one NVS commit. This needs ~10 ms of rail hold-up → **a ≥100 µF bulk capacitor
   on 3V3 is a hardware requirement; note it on the schematic.**
3. **Dirty-shutdown flag.** `DIRTY` is set at every commit and cleared only on a clean
   shutdown path. If it is set at boot, the gauge adds `MAX_GAP` worth of uncertainty
   and, if the pack is at rest, prefers an immediate OCV re-sync.

### 6.5 Migration

`fg_state_t.version` and `cfg_t.version` are checked on load. An older version is migrated
field-by-field through an explicit `migrate_vN_to_vN1()` chain; an unknown newer version
is refused and state is reinitialised from OCV. Never `memcpy` a blob of a different
version into the struct.

---

## 7. BLE Protocol

### 7.1 Requirement

> *Binary BLE serial protocol, preferably standard open source, for Android
> monitoring/configuring, preferably supported/emulated by other Play Store apps.*

### 7.2 Options considered

| Option | Play Store support | Open spec | Fits a coulomb counter? | Verdict |
|---|---|---|---|---|
| **GATT Battery Service 0x180F** | Universal, OS-level | Yes (Bluetooth SIG) | Only 0–100 % | **Include, insufficient alone** |
| **Nordic UART Service (NUS)** | Wide (nRF Toolbox, Serial Bluetooth Terminal, BLE Terminal) | Yes (Nordic, public) | Transport only, no semantics | **Include as transport** |
| **JBD / Xiaoxiang BMS protocol** | Strong: *Xiaoxiang BMS*, *Smart BMS*, *JBD Tools*, *BMS Tools*; plus ESPHome `jbd_bms`, Home Assistant, Python `jbdtool` on the desktop side | De-facto open — reverse-engineered, but thoroughly documented and independently reimplemented several times | **Excellent**: the frame literally carries residual capacity, nominal capacity, cycles, RSOC, current, voltage | **Primary choice** |
| Daly BMS (D2) | *Smart BMS*, *DalyBMS* | Reverse-engineered, documented | Good, similar fields | Viable fallback |
| JK BMS | *JK BMS* | Reverse-engineered | Cell-centric, heavier frames | Overkill |
| Victron VE.Direct / SmartShunt | Victron Connect | Advertisement **encrypted**, protocol partly NDA | Perfect semantically, closed in practice | Rejected |
| Bluetooth SIG ESS / Mesh | Sparse app support | Yes | Awkward for cumulative counters | Rejected |
| Fully custom + custom app | None | — | — | Rejected as primary |

**Decision.** Emulate the **JBD/Xiaoxiang BMS protocol** as the primary interface, served
over both its native `0xFF00` service and Nordic UART; add the standard `0x180F` Battery
Service for OS and generic-app integration; add a **vendor service** for everything JBD
cannot express (full-precision current, energy counters, calibration commands).

**Why JBD wins:** it is the only widely-app-supported protocol whose data model *is
already* a coulomb counter — `residual capacity` and `nominal capacity` in 10 mAh units
are exactly what this firmware computes. There is no semantic impedance mismatch, and the
user gets graphing, logging and CSV export from an app nobody had to write.

### 7.3 GATT layout

```
Advertisement
  Flags; Complete Local Name "BatMon-XXXX"  (XXXX = last 2 bytes of MAC)
  16-bit Service UUIDs: 0xFF00, 0x180F
  Service Data (0xFF00): 12-byte compact state record (§7.7)
  TX power

Service 0xFF00                                   ← JBD native; what the apps scan for
  ├─ 0xFF01  NOTIFY              device → phone, response frames (fragmented)
  └─ 0xFF02  WRITE, WRITE_NR     phone → device, request frames

Service 6E400001-B5A3-F393-E0A9-E50E24DCCA9E     ← Nordic UART, identical JBD framing
  ├─ 6E400003-…  NOTIFY  (TX)
  └─ 6E400002-…  WRITE   (RX)

Service 0x180F  Battery Service                  ← standard, for the OS / generic apps
  └─ 0x2A19  Battery Level, uint8 %, READ + NOTIFY

Service B47C0001-4D6F-6E69-746F-720000000000     ← vendor extension
  ├─ B47C0002  NOTIFY            Telemetry (packed struct, 1 Hz, §7.8)
  ├─ B47C0003  READ/WRITE        Config    (TLV, §8.3)
  └─ B47C0004  WRITE + INDICATE  Command   (TLV request/response, §8.4)
```

> **Bring-up caveat, stated plainly:** different JBD hardware generations expose either
> the `0xFF00 / FF01 / FF02` triplet or Nordic UART, and app versions differ in what they
> probe. Advertising and serving *both*, with identical framing, is cheap insurance. The
> exact UUIDs and the app's discovery filter **must be verified against the target app
> version during bring-up** (§11, M4). Treat the table above as a starting point, not as
> verified fact.

### 7.4 JBD frame format

Request (phone → device):

```
 0     1       2     3     4 .. 4+N-1    4+N..4+N+1   4+N+2
+-----+-------+-----+-----+-----------+-------------+------+
| DD  | A5/5A | REG | LEN |  DATA...  | CHECKSUM BE |  77  |
+-----+-------+-----+-----+-----------+-------------+------+
        0xA5 = read, 0x5A = write
```

Response (device → phone):

```
+-----+-----+--------+-----+-----------+-------------+------+
| DD  | REG | STATUS | LEN |  DATA...  | CHECKSUM BE |  77  |
+-----+-----+--------+-----+-----------+-------------+------+
        STATUS: 0x00 = OK, 0x80 = error
```

**Checksum:** `0x10000 − Σ(REG + LEN + DATA bytes)`, transmitted big-endian, 16-bit.

All multi-byte payload fields are **big-endian** — unusual for a little-endian MCU, so
the encoder must byte-swap explicitly. Do not `memcpy` structs into frames.

**Fragmentation:** with the default 23-byte ATT MTU, longer frames span several
notifications. The receiver must reassemble on `0xDD` … `0x77` boundaries with a length
check, never on notification boundaries. The firmware requests an MTU of 247 at connect
(making every frame single-packet in practice) but must still handle the fallback.

### 7.5 Register 0x03 — Basic Info (the main telemetry frame)

| Offset | Size | Field | Unit | Source in this firmware |
|---|---|---|---|---|
| 0 | u16 | Total voltage | 10 mV | `v_uV / 10000` |
| 2 | **i16** | Current | 10 mA | `i_uA / 10000`, negative = discharge |
| 4 | u16 | Residual capacity | 10 mAh | `charge_uAs / 36000` |
| 6 | u16 | Nominal capacity | 10 mAh | `full_capacity_uAh / 10000` |
| 8 | u16 | Cycle count | — | `cycle_count` |
| 10 | u16 | Production date | packed Y/M/D | build-date constant |
| 12 | u16 | Balance status, cells 1–16 | bitmap | 0 |
| 14 | u16 | Balance status, cells 17–32 | bitmap | 0 |
| 16 | u16 | Protection status | bitmap | mapped from warning flags (§7.6) |
| 18 | u8 | Software version | BCD | firmware version |
| 19 | u8 | RSOC | % | `soc_permille / 10` |
| 20 | u8 | FET status | b0 charge, b1 discharge | 0x03 (no FETs; report both "on") |
| 21 | u8 | Cell count | — | `cfg.cell_count` (display only) |
| 22 | u8 | NTC count | — | 1 |
| 23 | u16 | NTC 1 | 0.1 K | `temp_dC + 2731`, from the BME280/BMP280 (§4.4); falls back to the ESP32 die sensor if absent |

Also served:

- **0x04 — Cell voltages:** `cell_count` × u16 mV. Cells are not measured; the firmware
  reports `pack_mV / cell_count` for each so the app's bar chart renders sensibly. Config
  flag `report_synthetic_cells` (default **on**) can disable this and return `LEN = 0`,
  trading an uglier app screen for honesty.
- **0x05 — Hardware version:** ASCII, `"XIAO-C6-INA219 v1.0"`.

Cadence: the apps poll by writing read requests; the firmware additionally notifies
register 0x03 once per second when notifications are enabled, which the apps tolerate.

### 7.6 Protection-status bitmap mapping

The device has no protection function, but this field is the natural place to surface
warnings so they show up in the app UI:

| Bit | JBD meaning | Set when |
|---|---|---|
| 0 | Cell overvoltage | `V > cfg.v_max_uV` |
| 1 | Cell undervoltage | `V < cfg.v_0pct_uV` |
| 2 | Pack overvoltage | as bit 0 |
| 3 | Pack undervoltage | as bit 1 |
| 4 | Charge over-temperature | Charging and `T > cfg.t_warn_hi_dC` (60 °C) |
| 5 | Charge under-temperature | **Charging below 0 °C on a lithium chemistry** (§5.6.5) |
| 6 | Charge overcurrent | `I > cfg.i_max_chg_uA` |
| 7 | Discharge overcurrent | `I < −cfg.i_max_dsg_uA` |
| 8 | Discharge over-temperature | Discharging and `T > cfg.t_warn_hi_dC` |
| 9 | Discharge under-temperature | Discharging and `T < cfg.t_warn_lo_dC` (−20 °C) |
| 11 | Front-end IC error | INA219 I²C fault or OVF |
| 12 | *(repurposed)* | Gauge uncertainty high — documented deviation |

### 7.7 Advertisement service data (connectionless)

12 bytes, little-endian — lets a phone or passive scanner read state without connecting,
which matters when several monitors are in range:

```
 0     1       2..3        4..5        6..7        8..9      10..11
+-----+-------+-----------+-----------+-----------+---------+--------+
| ver | flags | soc ‰ u16 |  mV  u16  |  mA  i16  | mAh u16 | crc16  |
+-----+-------+-----------+-----------+-----------+---------+--------+
```

Refreshed every 2 s; advertising interval 500 ms. Legacy advertising is used rather than
extended advertising, to maximise Android compatibility.

### 7.7a Clients as built

The JBD service above is not implemented yet. Everything today speaks the NUS console
(CLI.md), and two clients exist:

- **Android app** (`android/`), a BLE central on the phone. Commands and firmware chunks
  share one serialised write path, because Android refuses a second GATT write while one
  is in flight. It dials the chosen board directly and redials after any drop, rather
  than scanning first: a board picked from a list is already known.
- **Remote display** (`remote/`), a second firmware on an ESP32-2432S028: a classic
  ESP32 with a 320×240 SPI panel and resistive touch. It is a NimBLE central. Its IO
  capability is keyboard-only, and against the monitor's display-only one that makes LE
  Secure Connections use passkey entry: the monitor shows six digits and the remote's
  keypad takes them.
  - It has no framebuffer (150 KB of the ESP32's RAM would starve the BLE stack), so
    fields redraw only on change and the SoC graph is rendered in strips.
  - The monitor keeps no history, so the remote records its own SoC history, one point a
    minute for 24 hours.

The monitor accepts three connections, so a phone and a remote can watch the same pack.
Telemetry is broadcast to both, and command replies are unicast to whichever sent the
command.

### 7.8 Vendor telemetry characteristic

Everything JBD truncates, at full precision, notified at 1 Hz:

```c
typedef struct __attribute__((packed)) {   /* little-endian, 48 bytes */
    uint8_t  ver;              /* = 1 */
    uint8_t  gauge_state;      /* UNKNOWN / COUNTING / FULL / EMPTY / RESTING */
    uint16_t flags;
    uint32_t v_uV;
    int32_t  i_uA;             /* full µA precision vs JBD's 10 mA */
    int32_t  p_uW;
    int64_t  charge_uAs;
    uint32_t full_capacity_uAh;
    uint32_t soh_permille;
    uint32_t drift_uncert_uAs;
    uint32_t time_to_x_s;      /* TTE if discharging, TTF if charging, 0xFFFFFFFF idle */
    uint32_t uptime_s;
    uint16_t cycle_count;
    int16_t  temp_dC;
} vnd_telemetry_t;
```

The four cumulative int64 counters are fetched on demand via `GET_ENERGY` (§8.4) rather
than pushed at 1 Hz, keeping each notification inside one MTU.

---

## 8. Configuration

### 8.1 Two paths, deliberately

1. **Through the Xiaoxiang/JBD app** — JBD "factory mode" register writes. Gives capacity
   and threshold configuration with zero custom software.
2. **Through the vendor Config characteristic** — everything JBD has no register for:
   shunt resistance, offset/gain calibration, deadband, chemistry, OCV table, rest time,
   learning parameters.

### 8.2 JBD factory-mode configuration

```
Enter factory mode:  write REG 0x00, data 0x5678
Write parameters:    write REG 0x10..0x2F, data = u16
Exit and save:       write REG 0x01, data 0x2828     (0x0000 = exit without saving)
```

| Reg | JBD name | Maps to |
|---|---|---|
| 0x10 | Design capacity (10 mAh) | `cfg.design_capacity_uAh` |
| 0x11 | Cycle capacity (10 mAh) | cycle-count denominator |
| 0x12 | Full-charge voltage (mV, per cell) | `cfg.v_100pct_uV` = value × `cell_count` (§8.6) |
| 0x13 | End-of-discharge voltage (mV, per cell) | `cfg.v_0pct_uV` = value × `cell_count` (§8.6) |
| 0x14 | Discharge rate (%) | ignored, echoed |
| 0x2A | Cell count | `cfg.cell_count` |

> The full JBD EEPROM map (roughly 0x10–0x5F) is documented in the `jbdtool` and ESPHome
> `jbd_bms` projects; the table above is the subset with a meaningful mapping here.
> Unmapped registers are kept verbatim in a shadow array and echoed on read, so the app's
> "read all settings" round-trip does not error. **Confirm these register numbers against
> a reference implementation during M4** — the map is community-documented and has varied
> between JBD firmware generations.

### 8.3 Vendor config TLV

Read the whole set or write individual items. Format: `[u16 tag][u16 len][payload]`
concatenated, little-endian, terminated by `TAG_END`.

| Tag | Type | Name | Default |
|---|---|---|---|
| 0x0001 | u32 | `r_shunt_uOhm` | 10000 (10 mΩ) |
| 0x0002 | i32 | `i_offset_uA` | 0 (set by calibration) |
| 0x0003 | u32 | `i_gain_ppm` | 1 000 000 |
| 0x0004 | u32 | `vbus_divider_q16` | 65536 (= 1.0) |
| 0x0005 | u32 | `i_deadband_uA` | 3000 |
| 0x0006 | u8 | `sense_topology` — 0 = high-side, 1 = low-side/L1, 2 = low-side/L2 (§2.9.3) | 1 |
| 0x0007 | u8 | `invert_sign` — swaps the current sign for reversed sense leads | 0 |
| 0x0008 | u8 | `vbus_comp` — 0 none, 1 add shunt, 2 subtract shunt (§2.9.5) | 1 |
| 0x0009 | u8 | `pga_max` — widest range auto-ranging may select (§2.9.2) | 2 (PGA/4) |
| 0x000A | u8 | `install_mode` — 0 auto, 1 Mode P (shunt in +), 2 Mode N (shunt in −), 3 single-sensor (§2.10.6) | 0 |
| 0x000B | u8 | `addr_pos_pole` | 0x40 |
| 0x000C | u8 | `addr_neg_pole` | 0x41 |
| 0x000D | u32 | `r_wiring_uohm` — learned cable resistance (§2.10.5), diagnostic only | 0 |
| 0x0010 | u32 | `design_capacity_uAh` | 10 000 000 (10 Ah) |
| 0x0011 | u8 | `chemistry` | 1 (0 = lead-acid, 1 = Li-ion, 2 = LiFePO₄, 3 = custom) |
| 0x0012 | u8 | `cell_count` | 4 |
| **0x0013** | **u32** | **`v_0pct_uV`** — pack voltage defined as 0 % SoC | per chemistry × `cell_count` (Appendix B) |
| **0x0014** | **u32** | **`v_100pct_uV`** — pack voltage defined as 100 % SoC | per chemistry × `cell_count` (Appendix B) |
| 0x0015 | u32 | `v_max_uV` — over-voltage warning only, not an SoC anchor | `v_100pct` × 1.02 |
| 0x0016 | u8 | `v_curve_mode` — 0 = linear between the two, 1 = OCV table scaled to them | 1 |
| 0x0020 | u32 | `i_taper_uA` | design/30 |
| 0x0021 | u16 | `t_full_hold_s` | 60 |
| 0x0022 | u16 | `t_rest_s` | 1800 |
| 0x0023 | u16 | `ocv_blend_gain_q8` | 64 (= 0.25) |
| 0x0024 | u16 | `learn_min_depth_permille` | 600 |
| 0x0030 | blob | `ocv_table` | 21 × u16 mV/cell, 5 % steps |
| 0x0040 | u16 | `peukert_q8` | 256 (k = 1.0) |
| 0x0041 | u32 | `self_discharge_comp_uA` | 0 (off) — §9.9, §5.6.3 |
| 0x0060 | i16 | `t_offset_dC` — installation bias null | 0 |
| 0x0061 | i32 | `cap_tempco_ppm_per_K` | per chemistry (§5.6.1) |
| 0x0062 | i32 | `v_tempco_uV_per_K_per_cell` | per chemistry (§5.6.2) |
| 0x0063 | i32 | `offset_tempco_nA_per_K` | 0 (learned, §5.6.4) |
| 0x0064 | u8 | `allow_auto_offset_learn` | 0 (off — requires the user to confirm no parasitic load) |
| 0x0065 | 2×i16 | `t_warn_hi_dC`, `t_warn_lo_dC` | 600, −200 |
| 0x0066 | u8 | `temp_corrections_en` | bitmask, all enabled |
| 0x0067 | u8 | `condensation_warn_en` | 0 (BME280 only) |
| 0x0070 | u8 | `display_present` | auto-probed; forces off if 0 |
| 0x0071 | u16 | `display_timeout_s` | 8 |
| 0x0072 | u8 | `display_contrast` | 0x40 (dim — see §9.10) |
| 0x0073 | u16 | `screen_enable_mask` | all screens (Appendix D) |
| 0x0074 | u8 | `display_flip` | 0 (180° rotation for enclosure fit) |
| 0x0075 | u8 | `debug_mode` — display always on, tier floor T1, screens D1–D3 (§9.11) | 0 — RTC-retained, **never** written to NVS |
| 0x0076 | u16 | `debug_timeout_min` — auto-exit from debug mode; 0 = never | 60 |
| 0x0050 | 3×u16 | `adv_interval_ms` per tier T0..T2 | 500 / 500 / 5000 |
| 0x0051 | u8 | `security_mode` | 0 = open (JBD-app compatible), 1 = bonded |
| 0x0052 | i8 | `tx_power_dbm` | 0 |
| 0x0053 | u16 | `adv_timeout_min` | 0 (never stop advertising) — §9.5 |
| 0x0054 | u8 | `power_profile` | 1 (0 = performance, tiers T0/T1 only; 1 = balanced; 2 = max endurance — demote to T2 fast and sample at 0.2 Hz) |

Every write is range-validated; an out-of-range write returns `ERR_RANGE` and changes
nothing. Configuration commits to NVS on `TAG_COMMIT`, not per item.

### 8.4 Command characteristic

| Cmd | Payload | Action |
|---|---|---|
| 0x01 `CAL_ZERO` | — | Run zero-current calibration (§5.5); indicate the result |
| 0x02 `SET_SOC` | u16 ‰ | Force `charge_uAs` to a given SoC (the user knows better) |
| 0x03 `SET_FULL` | — | Declare "the pack is full now" |
| 0x04 `RESET_COUNTERS` | u32 magic | Zero the lifetime cumulative counters |
| 0x05 `RESET_LEARN` | — | `full_capacity := design_capacity`; clear SoH |
| 0x06 `FACTORY_RESET` | u32 magic | Erase both NVS namespaces; reboot |
| 0x07 `GET_ENERGY` | — | Indicate the four int64 cumulative counters |
| 0x08 `GET_EVENTLOG` | u8 index | Indicate one event-log record |
| 0x09 `REBOOT` | u32 magic | Restart |
| 0x0A `SET_DEBUG` | u8 on, u16 timeout_min | Enter or leave debug mode (§9.11). Not persisted; returns `ERR_RANGE` for `on = 1` when `display_present = 0` |

Destructive commands (0x04, 0x06, 0x09) require a magic constant so a stray write cannot
erase a year of accumulation.

### 8.5 Security

Default `security_mode = 0` (Just Works, no bonding), because Xiaoxiang-class apps
generally do not expect pairing and bonding tends to break them. The consequence, stated
plainly: **anyone in radio range can read telemetry and change configuration.** For a
personal pack on private property that is an acceptable trade; it should be a conscious
one.

`security_mode = 1` enables LE Secure Connections with a static 6-digit passkey (config
tag), requiring encryption for the vendor Config and Command characteristics while
leaving JBD telemetry readable — the app still works, but nobody else can reconfigure the
device. Recommended once bring-up is complete. The destructive-command magic constants
apply in both modes.

### 8.6 Battery 0 % and 100 % voltages

These two values are first-class configuration, not derived constants, because they are
the only battery parameters the user genuinely knows and the firmware genuinely cannot
infer. Chemistry defaults (Appendix B) exist purely as sensible starting points; any
explicit write overrides them and clears the "derived from chemistry" flag so a later
chemistry change does not silently stomp a deliberate user setting.

```c
uint32_t v_0pct_uV;    /* pack voltage that means "empty",  SoC = 0 %   */
uint32_t v_100pct_uV;  /* pack voltage that means "full",   SoC = 100 % */
```

Both are **pack-level** voltages, not per-cell — this is what the user measures with a
multimeter and what the INA219 reports, so no mental arithmetic and no dependence on a
correct `cell_count`. Writing `cell_count` or `chemistry` recomputes them *only* if they
have never been explicitly set.

**Where they are used — four distinct roles:**

| Role | Use | Section |
|---|---|---|
| Full anchor | `V ≥ v_100pct_uV` is one of the three simultaneous conditions for full detection (with taper current and a hold time) | §5.4 A |
| Empty anchor | `V ≤ v_0pct_uV` under load, held, resets `charge_uAs` to 0 | §5.4 B |
| SoC bootstrap | On first boot or CRC loss, initial SoC comes from voltage mapped between these two endpoints | below |
| Wake trigger | In T2 the sampler promotes to T1 when the pack crosses either boundary | §9.3 |

**Mapping voltage → SoC** (`v_curve_mode`):

- **Mode 0, linear.** `soc‰ = 1000 × (v − v_0pct) / (v_100pct − v_0pct)`, clamped. Crude
  but predictable, and adequate for lead-acid and for the bootstrap case. Choose this if
  the OCV table is not trusted.
- **Mode 1, scaled OCV table (default).** The 21-point OCV table (tag 0x0030) is
  normalised so that its 0 % point maps to `v_0pct_uV` and its 100 % point to
  `v_100pct_uV`, with intermediate points scaled proportionally. This keeps the
  chemistry's curve *shape* — which is what makes the mid-range estimate meaningful —
  while honouring the user's endpoints. A user who sets a conservative `v_0pct` of 3.2
  V/cell to protect the pack gets a curve that genuinely reads 0 % there, rather than a
  table insisting the pack is at 15 %.

**Validation on write** — rejected with `ERR_RANGE`, leaving config unchanged:

```
v_100pct_uV > v_0pct_uV + MIN_SPAN            (MIN_SPAN = 200 mV; a degenerate span
                                               would make SoC a step function)
v_0pct_uV   ≥ 1.0 V                            (sanity floor)
v_100pct_uV ≤ 26.0 V                           (INA219 bus ceiling, §2.3 — or the
                                               divider-scaled equivalent)
v_max_uV    ≥ v_100pct_uV                      (warning threshold above full)
```

A write that changes either endpoint **does not** rewrite `charge_uAs`. The coulomb count
is the measured quantity and stays authoritative; only the *interpretation* of voltage
changes. If the user wants the count corrected too, that is the explicit `SET_SOC` or
`SET_FULL` command (§8.4) — two separate operations, deliberately, because conflating
them would let a routine configuration tweak destroy the accumulated count.

**Interaction with capacity learning:** moving the endpoints inward (a more conservative
usable window) shrinks the charge counted between anchors and will, over the next full
excursion, reduce the *learned* `full_capacity_uAh` accordingly. That is correct
behaviour — capacity is being reported for the user's defined window, not for the cell's
theoretical one — but it should be surfaced in the app, because "I changed a voltage and
my capacity dropped" is otherwise an alarming surprise.

**Via the JBD app:** these map directly onto factory-mode registers `0x12` (full-charge
voltage) and `0x13` (end-of-discharge voltage), so they are configurable from the stock
Xiaoxiang app without the vendor service. Note the JBD registers are per-cell in mV and
are multiplied by `cell_count` on the way in; the vendor TLV path is pack-level and exact,
and is preferred where available.

---

## 9. Power Management

### 9.1 The conflict, stated honestly

A coulomb counter wants to watch the shunt continuously. A low-power device wants
everything off. These pull in opposite directions, and the naive resolutions are both bad:

- **Never sleep** → ~15 mA average. On a 10 Ah pack that is 3.6 Ah/month, i.e. the monitor
  discharges the pack by ~36 % per month doing nothing. Unacceptable.
- **Deep-sleep the whole system and wake every N seconds** → the HP core plus BLE stack
  takes ~250–350 ms to boot and re-init per wakeup, which dominates the duty cycle, *and*
  the shunt is unobserved between wakeups, so any load transient in the gap is missed
  entirely. Missed charge is silent, unbounded error — the one failure mode this design
  exists to avoid.

The resolution is that **the thing that must stay awake is not the thing that costs
power.** Sampling the INA219 needs an I²C master and a 64-bit add. It does not need the
HP RISC-V core, the BLE controller, the Wi-Fi PHY, or 512 KB of SRAM.

### 9.2 Tiered power model

| Tier | Entered when | HP core | Radio | Sensor | Sample rate | Est. avg current |
|---|---|---|---|---|---|---|
| **T0 CONNECTED** | BLE link up | Active, DFS | Connected | Continuous | 7 Hz | 20–30 mA |
| **T1 ACTIVE** | Current above deadband, or <60 s since an event | Auto light sleep | Adv 500 ms | Continuous | 7 Hz | 6–10 mA |
| **T2 IDLE** — *the floor* | Idle below deadband >2 min; no connection >5 min | **Light sleep between samples** | Adv 5000 ms | Triggered | 0.5 Hz | **~220–400 µA** |
| **UI wake** | Button pressed (§9.10) | Active, 80 MHz | **Off** | Triggered | on demand | ~21 mA for 8 s, then back to the previous tier |
| **DEBUG** | `debug_mode = 1` (§9.11) | Active, 160 MHz | As T1 | Continuous | 7 Hz | ~21 mA **continuous** — bench only |

Tier transitions are hysteretic and one-way-fast: any of {current exceeds deadband, BLE
connection request, anchor condition detected, config write} promotes immediately to T1
or T0; demotion always requires the dwell time in the table. **Promotion is cheap and
demotion is slow** — the asymmetry is deliberate, because the cost of being awake for an
extra minute is microscopic next to the cost of missing a discharge event.

There is nothing below T2, and §9.3 explains why — the hardware, not the firmware,
decides that. The one exception in the other direction is **debug mode**, which pins the
floor at T1 and holds the display on indefinitely. It is not a tier the machine can enter by itself — only an operator sets it,
and §9.11 says what that costs.

### 9.3 T2 IDLE — the floor, and why there is nothing below it

The ESP32-C6 carries an LP (low-power) RISC-V core with its own `LP_I2C`, LP timer and
16 KB of LP SRAM, all of which stay powered while the HP domain is off. On paper it is
exactly the part needed to keep counting through deep sleep. In this build it cannot be
used for that, for one hardware reason:

> `LP_I2C` is hard-wired to **GPIO6 (SDA) / GPIO7 (SCL)** and is not remappable through the
> GPIO matrix. The board wires every I²C device to **GPIO22/23** (§2.5). The LP core
> therefore has no path to the INA219s.

**That closes off deep sleep entirely, and the reasoning is worth stating rather than
assuming.** Deep sleep is only admissible for a coulomb counter if *something* keeps
counting while the HP domain is dark. Nothing does here — so a deep-sleep tier would not be
a cheaper way to keep gauging, it would be a hole in the measurement as long as the tier
itself. A gauge that stops counting for 30 minutes at a time and calls the gap uncertainty
is worse than a gauge that costs 300 µA. **T2 is the floor. There is no T3.**

What T2 actually does:

```
        T2 IDLE  (HP domain on, automatic light sleep, ~220-400 uA)
        ┌───────────────────────────────────────────────────┐
        │  esp_timer @ 0.5 Hz ──→ wake (~2 ms exit)                 │
        │     → trigger both INA219 conversions (2.6 ms)          │
        │     → read, integrate, update the LP-SRAM mirror         │
        │     → evaluate deadband / voltage bounds → promote?      │
        │     → back to light sleep         (~20 ms awake, total)   │
        │                                                          │
        │  NimBLE wakes independently for advertising @ 5 s        │
        │  RAM retained throughout — no boot, no state reload      │
        └───────────────────────────────────────────────────┘
```

**The dominant cost is the wake, not the sleep.** This inverts the two-bus design's
economics and it is the single most useful thing to know when tuning:

| Sample rate | Awake duty | Wake term | Everything else | Total |
|---|---|---|---|---|
| 1 Hz | ~2 % | ~400 µA | ~70–170 µA | ~470–570 µA |
| **0.5 Hz** (default) | ~1 % | ~200 µA | ~70–170 µA | **~270–370 µA** |
| 0.2 Hz | ~0.4 % | ~80 µA | ~70–170 µA | ~150–250 µA |

Light-sleep base current is a fixed ~35–60 µA; the sensors, triggered, are ~3 µA. So
**sample rate is the knob**, and `power_profile = 2` (§8.3) simply moves it to 0.2 Hz.
Going below that buys progressively less against the fixed terms while widening the
worst-case unobserved window, which is the thing the gauge actually cares about.

**No blind spot, which is the point of paying for T2.** The worst-case interval with no
measurement is one sample period — 2 s at the default — not the length of a dormant
excursion. Every guarantee in §5.2 about gaps being accounted rather than ignored holds
unchanged at the floor, because the integrator never stops.

**What it would take to get the deep-sleep tier back.** Two wires: move the INA219 SDA/SCL
to GPIO6/7 and leave the OLED and temperature sensor on GPIO22/23. That restores the
two-bus arrangement, and with it the LP-core gauge (LP timer → `LP_I2C` trigger → read →
64-bit multiply-accumulate → wake the HP core on threshold), an `lp_accum_t` handoff buffer
in LP SRAM, and a ~40–90 µA dormant tier — roughly 5× better than this floor. It also
brings back everything that made that design harder: bus-ownership handoff at every sleep
boundary, a second CRC-and-retry protocol, a merge step on every wake, and a stale
temperature to reason about (§4.4). **This is recorded as an available hardware option, not
as planned work.** If a sub-100 µA figure ever becomes a requirement, this is the change
that delivers it, and §9.7 is the reason it would still not be sufficient on its own.

### 9.4 Sensor duty cycling

The INA219 at ~1 mA while converting would, if left continuous, dwarf every other idle
term — it is ~3× the entire rest of the T2 budget. It must be triggered:

| Scheme | Sensor charge per sample | At 1 Hz | At 0.2 Hz |
|---|---|---|---|
| Continuous, 12-bit ×128 avg | (always on) | 1000 µA | 1000 µA |
| Triggered, 12-bit ×128 avg (68 ms) | 68 µC | 68 µA | 14 µA |
| Triggered, 12-bit ×4 avg (2.1 ms) | 2.1 µC | **2.1 µA** | 0.4 µA |
| Triggered, 12-bit ×1 (532 µs, both ch.) | 1.1 µC | 1.1 µA | 0.2 µA |

`PROFILE_TRIGGERED` uses ×4 averaging as the knee of this curve: 2 µA is already an order
of magnitude below the light-sleep base current (§9.3), so buying less averaging saves
nothing measurable while costing resolution. **Do not optimise past the point where the
term stops mattering** — on this board the term that matters is the wake duty cycle, not
the sensor.

### 9.5 Radio power

- **Advertising interval scales with tier** (500 / 500 / 5000 ms). Advertisement payload
  is refreshed from LP SRAM, so a phone can read SoC, voltage and current (§7.7) *without
  ever connecting* — which is the lowest-energy way to service a casual "how full is it?"
  check, and a strong reason to keep the service-data advertisement in the design.
- **Connection parameters:** on connect, request a 200–400 ms interval with slave latency
  4 and a 6 s supervision timeout. Telemetry is 1 Hz, so a fast connection interval buys
  nothing. If the app renegotiates to something aggressive, accept it — a connected session
  is short and user-initiated.
- **TX power** default 0 dBm rather than the maximum; configurable (`cfg.tx_power_dbm`) for
  installations where the pack is in a metal enclosure.
- **Wi-Fi and 802.15.4 are never initialised.** `esp_wifi` is not linked in the production
  build. Firmware updates arrive over the BLE link that is already up (CLI.md §6), so
  OTA needs no second radio.
- Advertising **stops entirely** after `cfg.adv_timeout_min` (default 0 = never) of no
  connection, if the user opts in. With advertising off, T2 falls by its 15–40 µA radio
  term — worth having, but no longer the transformative saving it was when the floor was
  40–90 µA in total. The
  device is then only reachable after a power cycle or a magnet/button wake, so this is
  off by default and clearly labelled in the app.

### 9.6 CPU and peripheral configuration

```
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_PM_DFS_INIT_AUTO=y          # 160 MHz max, 40 MHz min, XTAL in light sleep
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_80=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_PM_SLP_DEFAULT_PARAMS_OPT=y
CONFIG_ESP_SLEEP_POWER_DOWN_FLASH=y
CONFIG_RTC_CLK_SRC_INT_RC=n        # external 32 kHz if fitted; else use RC with calibration
```

Automatic light sleep is compatible with BLE — NimBLE takes a power-management lock only
around radio events. `esp_timer` and the INA219's conversions run through light sleep.

Other measures: unused peripheral clocks gated; all unused GPIOs configured as inputs with
pulls disabled (a floating input on a CMOS pad is a real leakage path); logging over UART
compiled out in the production build (`CONFIG_LOG_DEFAULT_LEVEL_NONE` — UART TX at 115200
during a sample burst costs more than the sample); the console UART's power-management lock
explicitly released.

### 9.7 Board-level parasitics — likely the real limit

Below ~100 µA, the firmware stops being the deciding factor and the board does:

| Source | Typical | Mitigation |
|---|---|---|
| XIAO power LED | 1–3 mA | **Desolder.** No firmware fix exists. Single largest win. |
| Charge-status LED | 0–2 mA (when charging) | Desolder if the onboard charger is unused |
| Onboard LDO quiescent | 10–50 µA | Accept, or bypass the module regulator and feed 3V3 directly |
| Onboard battery-charger IC quiescent | 5–20 µA | Leave BAT pads unconnected if powering from the pack |
| INA219 breakout pull-ups (10 kΩ) | ~0 (idle high) | Fine; raise to 4.7–10 kΩ only if bus errors appear |
| Vbus divider (§2.3) | V²/R — a 100 kΩ divider on 12 V wastes 60 µA | **Size for ≥1 MΩ**, or gate its low side with a GPIO and enable only during a bus reading |

The divider point deserves emphasis: it is easy to spend more current measuring the
voltage than the core spends light-sleeping between samples. A GPIO-gated divider costs
one pin and removes the term completely.

### 9.8 Budget summary (T2 floor, target configuration)

| Item | Estimate |
|---|---|
| HP core in automatic light sleep, RAM retained | 35–60 µA |
| **Waking to sample at 0.5 Hz** (~20 ms at ~20 mA) | **150–220 µA** |
| BLE advertising, 5 s interval | 15–40 µA |
| INA219 ×2, triggered at 0.5 Hz, ×4 avg | 2–3 µA |
| BME280, forced mode, once a minute | <0.5 µA |
| SSD1306 + bus pull-ups, rail gated off | **0 µA** |
| Button, 1 MΩ pull-up, not pressed | ~0 µA |
| Vbus divider (1 MΩ, gated) | <1 µA |
| Board parasitics after LED removal | 15–70 µA |
| **Total, idle floor** | **~220–400 µA** |
| *Display use, amortised (§9.10)* | *+1–3 µA per press per day* |

Against a 10 A·h pack that is **1.9–3.5 A·h/year, or roughly 20–35 % of the pack per
year.** State it that way rather than in microamps, because it is the number that decides
whether this design is acceptable for a given installation:

- On a pack that is cycled and recharged regularly, 300 µA is irrelevant — it is 0.1 % of
  a 300 mA load and disappears into the next charge.
- On a pack left standing for a season, it is not irrelevant, and the monitor becomes a
  measurable fraction of the self-discharge it is trying to report.

For comparison: the two-bus LP-core design targeted 40–90 µA (0.5–1.3 A·h/year, under 1 %),
and the naive always-on design costs ~36 % per *month*. So this floor keeps most of the win
and gives up the best part of it — which is the trade §9.3 records, and §9.3 also records
the two wires that would buy it back.

These are datasheet-derived estimates, not measurements; M6 replaces them with real
numbers, and the wake term is the one most likely to be wrong — it depends on how quickly
the sampler actually gets back to sleep.

### 9.9 Interaction with the gauge

Two invariants the power design must not violate, both already enforced in §5:

1. **`dt` is always measured, never assumed** (§3.3). A tier transition mid-interval is
   therefore harmless — the sample after the transition integrates its true elapsed time,
   whether that was 140 ms or 5 s.
2. **Gaps are accounted, not ignored** (§5.2). Any interval where the sampler produced no
   reading adds to `drift_uncert_uAs` and is reported to the client. A low-power tier can
   degrade *precision*; it is never permitted to degrade it *silently*. With T2 as the floor
   (§9.3) the integrator runs continuously, so in normal operation this path is exercised
   only by faults — not, as in the two-bus design, by every dormant excursion.

**Self-consumption:** because the monitor sits inside the shunt loop (§2.8), its own draw
is measured and counted like any other load — including at the idle floor. Correct by
construction — **but only under grounding option L1** (§2.9.3). Under L2 the monitor's
return current bypasses the shunt entirely and this whole paragraph stops applying; the
dormant draw then has to be measured once (Appendix C) and entered as
`self_discharge_comp_uA`. A parallel ground path (§2.9.4) breaks it just as thoroughly and
far less visibly. Note the further consequence: at T2 the monitor's own ~300 µA is below
the 3 mA deadband (§5.2) and so is *deliberately not integrated*. This is still the right
trade — far better to under-count a known 300 µA than to let a 3 mA offset error accumulate
— but the single-bus floor makes it a bigger error than it was: **long idle periods
over-report SoC by roughly 2.6 A·h/year**, against 0.9 A·h/year for the two-bus design.
That is 26 % of a 10 A·h pack, so on this board configuring the correction is close to
mandatory rather than optional. `cfg.self_discharge_comp_uA` (§8.3) lets the user apply a fixed corrective drain,
defaulting to 0 (off), covering both this and the pack's own self-discharge.

### 9.10 The display wake — a tier of its own

A button press takes the device out of the T2 floor into a **UI wake**, which is
deliberately *not* tier T1: it is a short, radio-free excursion whose only jobs are to read
the temperature, render a screen, and go back to sleep.

```
button (GPIO2 low)
   → HP exits light sleep              (~2 ms — no boot, RAM already live)
   → (no accumulator merge needed: the integrator never stopped, §9.3)
   → read BME280                       (before any radio activity, §2.7)
   → VOLED on, 100 ms settle, SSD1306 init, render
   → 100 ms input loop: short press advances screen, timeout resets on each press
   → display_timeout_s elapsed → display off (0xAE), VOLED off, pins driven low
   → back to the T2 floor
```

Losing deep sleep makes this path strictly better: **the display shows numbers ~250 ms
sooner** because there is no boot, no NVS reload and no merge — the snapshot is already
live. It is the one user-visible place where the single-bus floor wins.

**The radio stays off** unless the user long-presses. This is the single most important
decision in the UI power design: bringing up NimBLE and advertising for the duration of a
display wake would roughly triple its cost and serves nobody — a person standing at the
device reading the OLED is not simultaneously looking at their phone.

**Energy per press** (8 s default timeout):

| Component | Current | Duration | Charge |
|---|---|---|---|
| HP core @80 MHz, radio off | ~15 mA | 8.0 s | 120 mA·s |
| SSD1306 128×32, contrast 0x40 | ~6 mA | 8 s | 48 mA·s |
| Wake + sensor read (no boot) | ~25 mA | 0.05 s | 1.3 mA·s |
| **Total per press** | | | **~180 mA·s = 0.05 mAh** |

At 10 presses/day that is 0.5 mAh/day ≈ 0.18 Ah/year. Against the T2 floor's
1.9–3.5 A·h/year that is now a ~5–10 % addition rather than the comparable-to-everything
term it was at 40–90 µA — but the knobs stay, because they cost nothing and the floor may
yet come down (§9.3):

- `display_timeout_s` (default **8 s**) is the dominant term and scales the cost linearly.
- `display_contrast` defaults to **0x40, not 0xFF**. A 128×32 OLED at full contrast draws
  2–3× what it draws at 0x40 and is *harder* to read indoors. Dim is both cheaper and
  better.
- Screens render **only on change**, and only the dirty region is pushed over I²C. A static
  screen costs nothing beyond the panel itself.
- The CPU drops to 80 MHz and light-sleeps between the 100 ms input polls; it is idle for
  most of the 8 seconds regardless of what the screen shows.

**A press is also a free anchor opportunity.** Because a UI wake already reads
temperature and has a live snapshot, it is the natural moment to evaluate the §5.4 anchor
conditions and, if the pack has been resting, run an OCV re-sync. The user pressing a
button to check the gauge is, without knowing it, improving the gauge.

---

### 9.11 Debug mode — the display stays on

Every decision above assumes an unattended device. On the bench the opposite holds: the
unit sits on a lab supply or USB, somebody is watching it, and a display that blanks after
8 s while the core light-sleeps between samples is actively obstructive. **Debug mode** is
the explicit escape hatch — one flag that trades the whole power design for observability.

`debug_mode` (§8.3 tag 0x0075) does exactly four things:

| | Normal | `debug_mode = 1` |
|---|---|---|
| Display | On for `display_timeout_s`, then off, rail cut | **Permanently on**, VOLED held up |
| `ui` task | Exists only while the display is on | Runs continuously, re-renders at 2 Hz |
| Tier floor | T2 IDLE (§9.3) | **T1 ACTIVE** — no light sleep between samples |
| Screens | Appendix D screens 1–5 | plus debug screens **D1–D3**; screen 5 loses its 10 s hold |

Nothing else changes. Debug mode is deliberately *not* a separate firmware build and *not*
a separate path through the sampler or the gauge: a debug screen reads the same §3.3
snapshot the BLE task reads, because a debug view that recomputes its own numbers is a
debug view that lies about the bug being chased.

**The cost is the entire point, so state it plainly:** ~21 mA continuous ≈ 500 mA·h/day —
about **70× the T2 floor** (§9.8). On a 10 A·h pack, debug mode flattens in three weeks what
the tiering exists to stretch over a year. Three guardrails follow from that:

- **It auto-expires.** `debug_timeout_min` (tag 0x0076, default **60**) drops the device
  back to normal operation — display blanks, tiering resumes. 0 means never, which is only
  sane on external power.
- **It announces itself.** `DEBUG_MODE` is set in `vnd_telemetry_t.flags` (§7.8) for as
  long as it is active, and the debug screens carry an inverted top line. A unit found with
  a suspiciously flat pack should be diagnosable from the telemetry log alone.
- **It does not survive a cold boot.** The flag lives in RTC-retained memory, never in NVS.
  It survives light sleep and a soft reboot — so a crash-and-restart loop stays observable —
  but a power cycle clears it, and no unit ships accidentally stuck in it.

**Burn-in is real.** A 128×32 OLED holding static text for hours retains it. In debug mode
the renderer inverts the full framebuffer for 1 s every 5 min and shifts content one row
every 15 min. Both are plainly visible, which is intended: they double as the cheapest
possible "the render loop is still running" indicator.

**Before M5b there is no panel**, so the same three views render over the console —
`debug screens` for a one-shot dump and `debug watch` for a 2 Hz stream until a key is
pressed — from the identical field set. That path stays after M5b; it is how the screens
get tested with no display fitted, and how they get read over USB with the enclosure shut.

---

## 10. Fault Handling

| Fault | Detection | Response |
|---|---|---|
| I²C NACK / bus hang | transaction error | Retry ×3 → bus recovery (9 clock pulses) → re-init INA219; raise `SENSOR_FAULT`, stop integrating, keep serving BLE |
| INA219 OVF | config-register flag | Discard sample, force PGA down |
| Reading stuck | 100 identical raw samples | Re-init the sensor |
| Both NVS slots fail CRC | boot check | Reinit from OCV, state `UNKNOWN`, log |
| Watchdog | TWDT on the gauge task | Reset; the retained-RAM mirror preserves the count (§6.1) |
| Impossible SoC (>130 %, <−10 %) | gauge sanity check | Clamp, raise uncertainty, request OCV re-sync |
| BLE stack failure | NimBLE reset callback | Restart the host; keep gauging |
| Light-sleep entry failed | `esp_light_sleep_start` return code | Stay in T1; retry demotion after 5 min. The gauge is unaffected — only the power bill |
| Sample deadline missed while the display was updating | Sampler tick late by >1 period | Log, count it; if it recurs, chunk display writes harder (§2.5). A UI redraw must never cost a sample |
| Bus wedged with the OLED rail off | NACK on a sensor address, recovers when VOLED is on | The §2.5 back-powering path is live: series resistors missing or pins not driven low. Flag `BUS_FAULT`; do not gate the rail again this power cycle |
| BME280/BMP280 absent or NACKs | Chip-ID probe at init | `TEMP_UNAVAILABLE`; fall back to the die sensor; **all §5.6 corrections disabled, not guessed** |
| Temperature implausible (<−40 or >+90 °C) | Range check | Reject the sample, hold the last valid value, re-probe after 60 s |
| OLED absent or NACKs | Probe after rail-up | `display_present = 0`; button still wakes and still merges the accumulator, it just renders nothing |
| Button stuck low | Held >60 s | Ignore further wakes from it, log, and **disarm the GPIO wake source** — a stuck button holds the core out of light sleep indefinitely, which costs ~21 mA and would flatten the pack in weeks |
| Debug mode left enabled | `debug_timeout_min` elapsed | Leave debug mode, blank the display, resume tiering, log the exit. The flag is RTC-only, so a cold boot clears it too (§9.11) |
| The bus hangs with the OLED rail on | Transaction timeout | Cut VOLED, wait 200 ms, re-power once. **On this board that bus is also the gauge's** (§2.5), so if it recurs, leave VOLED off, mark the display failed for this power cycle and keep sampling — never sacrifice the sensors to retry a screen |

**Principle: the gauge outlives everything else.** A BLE fault, a misbehaving app or a bad
configuration must never stop or corrupt the integration. The gauge task has no dependency
on the BLE task in either direction beyond a lock-free snapshot.

---

## 11. Implementation Plan

| Milestone | Deliverable | Exit criterion |
|---|---|---|
| **M1** Bring-up | IDF project, I²C, INA219 driver, console dump | V and I match a bench meter within 1 % over ±2 A |
| **M2** Calibration | PGA auto-range, offset/gain calibration, deadband | Idle drift < 1 mAh over 24 h with no load |
| **M3** Gauge | Integrator, SoC, anchor state machine, NVS persistence | A full cycle on a known pack counts within 3 % of reference; state survives 100 random power cuts |
| **M4** BLE / JBD | 0xFF00 + NUS + JBD framing | **Xiaoxiang BMS app connects and shows live values** — this milestone is what validates the §7.3 / §8.2 assumptions |
| **M5** Standard + vendor | 0x180F, vendor telemetry/config/command | Android shows a battery level; TLV round-trips |
| **M5b** UI + temperature | Rail gating, SSD1306 screens on the shared bus, button gestures, debug mode (§9.11), BME280 driver, §5.6 corrections | Press shows fresh SoC within 200 ms (no boot — §9.10); rail measures 0 µA when off **and sensor reads are unaffected with it off** (§2.5); capacity and endpoint corrections verified in a thermal chamber or a domestic freezer at ≥3 temperatures |
| **M6** Low power | Tier state machine, triggered sensor profile, automatic light sleep down to the T2 floor (§9.3), brownout hook, fault injection. No LP-core work — the single bus rules it out | Measured T2 < 400 µA (Appendix C), with the wake term itemised; 7-day soak against a reference coulomb counter with drift within budget *and* tier transitions exercised throughout |
| **M7** Polish | ~~OTA over Wi-Fi~~ OTA over BLE, user guide, calibration procedure | **Done**: two app slots with bootloader rollback, updates from versioned GitHub releases through the app (CLI.md §6) |

### Test strategy

- **Unit (Unity / host):** frame encode/decode against known-good JBD captures, checksum
  edge cases, fragment reassembly, integration arithmetic over synthetic current profiles,
  state-machine transitions, NVS migration.
- **Injection harness:** a build flag swaps the INA219 driver for a CSV-replay scripted
  current/voltage profile, so a simulated 30-day cycle runs in seconds and drift is
  measured deterministically. This is the highest-value test asset in the project.
- **Hardware-in-loop:** electronic load + bench supply, compared against a reference
  coulomb counter, over full and partial cycles.
- **Power-cut rig:** a relay cycling the supply at random intervals under load, asserting
  the persisted count never jumps by more than one write-cadence's worth of charge.
- **Tier-transition test:** the injection harness drives a profile that crosses the
  deadband repeatedly at varying rates, forcing T1↔T2 churn. Asserts that the total
  integrated charge matches the profile's known integral regardless of how many
  transitions occurred — a tier change must be invisible in the result, only in the power.
- **Power measurement:** per Appendix C, on real hardware, per tier. Not optional and not
  inferable from code review; every low-power design that was never measured is a
  low-power design that does not work.
- **Thermal test:** the injection harness replays a current profile while the temperature
  input is swept independently, asserting that (a) `charge_uAs` is bit-identical regardless
  of the temperature profile — proving §5.6 never touches the count — and (b) reported SoC
  and the learned 25 °C capacity move as the model predicts. Real-hardware confirmation at
  three temperatures in M5b.
- **UI test:** press/hold/timeout state machine on hardware, plus a stuck-button soak
  asserting the wake source is disarmed (§10) rather than looping.
- **Debug-mode test:** assert `charge_uAs` is bit-identical across one profile replayed with
  `debug_mode` 0 and 1 — debug mode must be observation only, never a second code path —
  that `debug_timeout_min` expiry restores the prior tier and blanks the panel, and that a
  cold boot never comes up in debug mode.

---

## 12. Known Limitations and Alternatives

1. **INA219 offset is the binding constraint** at shunts below ~10 mΩ (§2.3). If the
   application needs high current *and* long unattended accuracy:
   - **INA226** — 16-bit, ±10 µV offset, ±81.9 mV range, same I²C idiom. Roughly 10× better
     offset for the same effort. **Recommended upgrade if the design is not already
     committed to the INA219.**
   - **INA228 / INA229** — 20-bit, and critically it carries **hardware charge and energy
     accumulators** (40-bit `CHARGE` and `ENERGY` registers). The MCU would read
     accumulated coulombs directly instead of integrating in software, eliminating the
     sampling-gap and timebase error classes entirely and making duty-cycled sleep viable.
     If the sensor choice is still open, INA228 simplifies §5.2, §9 and half of §10.
2. **26 V bus ceiling** forces a divider on 24 V systems, adding its own gain error — and,
   if sized carelessly, more quiescent current than the rest of the device (§9.7).
3. **Low-side sensing runs the INA219 slightly outside its specified common-mode range**
   (§2.9.2). It is inside the absolute maximum, but accuracy below 0 V is unspecified and
   must be validated in both current directions during M2. This also caps the usable
   current range at roughly a third of the high-side figure.
4. **A parallel ground path silently destroys the measurement** (§2.9.4) and raises no
   fault — **and the dual-sensor cross-checks do not catch it** (§2.10.8), because every
   sensor honestly measures the reduced current that actually crosses the shunt. It is the
   most likely way for this device to be confidently wrong, and the USB console makes it
   easy to create by accident during bring-up. Detecting it would need a shunt in both
   leads; single-point grounding stays a build requirement.
5. **In Mode N the monitor's own draw is not measured at all** (§2.10.4), because grounding
   option L2 returns it to the battery negative without crossing the shunt. This is a
   deliberate reversal of the §2.9.3 recommendation, taken because a second sensor makes
   the pack voltage independent of the current channel. The ~220–400 µA has to be measured
   once and entered as `self_discharge_comp_uA`.
6. **`install_mode = AUTO` cannot resolve itself without a load** (§2.10.6). At zero
   current the two modes are indistinguishable, so the device stays in `ROLE_UNKNOWN` and
   integrates nothing until a load appears or the mode is set explicitly.
7. **There is no deep-sleep tier, and the idle floor is ~5× the original target**
   (§9.3). The board wires all I²C to GPIO22/23; `LP_I2C` is fixed to GPIO6/7 and cannot
   reach the sensors, so nothing can keep counting with the HP domain off. This was the
   design's largest open risk and it has now resolved *against* the low-power path —
   accepted deliberately, at a cost of roughly 20–35 % of a 10 A·h pack per year instead of
   under 1 %. Correctness is untouched; only endurance is. Two wires to GPIO6/7 reverse it.
8. **The monitor's own idle draw is below the deadband and therefore uncounted**
   (§9.9), causing an over-report of SoC of ~2.6 A·h/year unless
   `self_discharge_comp_uA` is configured. The single-bus floor makes this roughly 3×
   worse than the two-bus design, which promotes it from a footnote to a setup step. An INA228 (item 1) with its hardware accumulator
   and much lower offset would let the deadband shrink far enough for this to disappear.
9. **Board parasitics matter, but no longer dominate** (§9.7). At a ~300 µA floor the
   XIAO's 1–3 mA power LED is still worth desoldering — it is 3–10× everything else — but
   the 15–70 µA of regulator and charger quiescent is now a minor term rather than the
   binding constraint it would have been at 40–90 µA. If a sub-100 µA figure ever becomes a
   hard requirement, both the §9.3 rewire *and* module rework are needed; neither alone
   suffices.
10. **The BME280 measures the board, not the cells** (§2.7). Every correction in §5.6
   inherits that error. For a pack in a separate enclosure the temperature correction may
   be worse than none — which is why every correction is individually switchable and why
   `temp_corrections_en` exists.
11. **Automatic offset-tempco learning (§5.6.4) trusts that `RESTING` means no parasitic
   load.** It cannot verify this, so it is off by default. Enabled on an installation with
   a permanent standby load, it would learn a wrong slope and make the gauge worse.
12. **A UI wake costs ~0.05 mAh** (§9.10). Frequent display use is a real load; a user who
   checks it hourly spends more on the display than on everything else combined.
13. ~~**The button must occupy an LP GPIO (0–5).**~~ **Resolved by the single-bus
   decision** (§2.6): the constraint existed only for deep-sleep GPIO wake, and with T2 as
   the floor there is no deep sleep. Light-sleep wake works on any pin, so GPIO2 is a free
   choice and this risk is closed.
14. **Synthetic cell voltages** in JBD register 0x04 are a cosmetic fiction for app
   compatibility; the flag to disable them is documented (§7.5).
15. **JBD is reverse-engineered**, not a standard. It is well-documented across several
   independent open-source implementations, but there is no specification body and app
   behaviour can change. The vendor service exists precisely so the device stays fully
   usable if the JBD path breaks.
16. **Open BLE by default** (§8.5) — a deliberate compatibility trade, not an oversight.

---

## Appendix A — Reference implementations to consult

| Project | Use |
|---|---|
| ESPHome `jbd_bms` component | JBD frame and register semantics, C++ reference |
| `jbdtool` (Python) | Full EEPROM register map, factory-mode sequence |
| ESP32 "smart BMS simulation" projects | JBD *emulation* over BLE — closest prior art |
| Nordic UART Service documentation | UUIDs, flow control |
| Bluetooth SIG Battery Service 1.0 | 0x180F / 0x2A19 |
| TI INA219 datasheet (SBOS448) | Register map, offset and gain specifications |
| TI INA228 datasheet (SBOSA20) | If upgrading — hardware charge accumulator |
| Bosch BME280 datasheet (BST-BME280-DS002) | Fixed-point compensation formulae, forced-mode timing |
| SSD1306 datasheet (Solomon Systech) | Init sequence, charge-pump timing, contrast register |
| ESP-IDF `esp_pm` + automatic light-sleep docs | Tickless idle, PM locks, and what NimBLE holds (§9.3) |

## Appendix B — Default chemistry parameters

> **As implemented** (`fg_chem_profile()`, CLI.md §6 "Battery chemistry"): eight
> chemistries, each an 11-point resting-OCV curve per cell plus full voltage, taper and
> rated current as fractions of capacity, Peukert k, rest time, and a re-sync trust band.
> They are flooded, AGM and gel lead-acid, LiFePO₄ (re-sync below 15 % and above 95 %
> only), Li-ion NMC/NCA, LiPo, LTO, and NiMH (below 15 % and above 90 % only).
> `battery <chemistry> [cells]` loads one and restarts the count. The table below is the
> original design's three-chemistry starting point.

Starting points only. `v_0pct` and `v_100pct` are user configuration (§8.6); these values
seed them at first boot and whenever `chemistry` changes *before* either has been set
explicitly.

| Chemistry | `v_100pct`/cell | `v_0pct`/cell | `v_max`/cell | OCV re-sync valid | Peukert k |
|---|---|---|---|---|---|
| Li-ion (NMC) | 4.15 V | 3.00 V | 4.25 V | full range | 1.00 |
| LiFePO₄ | 3.45 V | 2.60 V | 3.65 V | < 20 % and > 85 % only | 1.00 |
| Lead-acid | 2.40 V | 1.80 V | 2.45 V | full range | 1.15 |

Per-cell values are multiplied by `cell_count` to obtain the pack-level thresholds
actually stored and used. Users running a conservative window (e.g. 3.20–4.05 V/cell on
Li-ion to extend cycle life) should set the endpoints directly rather than adjusting
chemistry — see the capacity-learning interaction noted in §8.6.

## Appendix C — Power measurement procedure (M6)

Estimates in §9.8 are datasheet arithmetic and must be replaced with measurements.

1. Cut the module's 3V3 rail and insert a precision current probe (Joulescope, Otii, or a
   µCurrent + scope); the on-board INA219 cannot measure its own supply and is far too
   coarse below 1 mA in any case.
2. Measure each tier separately by pinning the tier through a debug command, not by waiting
   for natural transitions.
3. Capture a full 24 h profile in the target installation, integrate it, and compare
   against the §9.8 budget line by line — a single aggregate number will not tell you
   *which* estimate was wrong.
4. Re-measure after any change to advertising parameters, sensor profile, or board
   population. LED removal (§9.7) should be visible as a step change of 1–3 mA and is the
   easiest sanity check that the rig is working.
5. Measure a UI wake as an **energy per press** (integrate one full press-to-sleep cycle),
   not as a current. An average is meaningless for an event that happens ten times a day.
6. Verify the gated rail reads **0 µA**, not "small". A non-zero figure means the
   back-powering path of §2.5 is live and the series resistors or the pin-low-on-sleep step
   is missing — and on this board's shared bus that path can corrupt sensor reads, so treat
   a non-zero reading as a correctness bug, not a power one.
7. **Itemise the wake term separately** (§9.8): trigger a tier-T2 pin, capture a single
   sample cycle at high time resolution, and integrate it. It is the largest line in the
   budget and the one most sensitive to firmware sloppiness — an extra 10 ms of
   awake time per sample is another ~100 µA, which no aggregate 24 h average will
   attribute correctly.

## Appendix D — Display screens (SSD1306 128×32)

128×32 is two lines of readable text, or one large number plus a small line. Screens
advance on short press and wrap; `screen_enable_mask` (§8.3) hides any the user does not
want. Screen 1 is always shown first — the whole point of the device is the number on it.

```
 Screen 1 — SoC (default)          Screen 2 — Live electrical
+------------------------------+  +------------------------------+
|  87%  [||||||||||||....]     |  | 13.42V      -2.150A          |
|  10.4Ah    2h 15m left       |  | -28.9W      25.3C            |
+------------------------------+  +------------------------------+

 Screen 3 — Counters               Screen 4 — Health
+------------------------------+  +------------------------------+
| In  1284.6Ah   Out 1201.3Ah  |  | SoH 94%   Cyc 138            |
| Full 12.0Ah    Since full 3d |  | Cap 11.3Ah  +/-0.4Ah         |
+------------------------------+  +------------------------------+

 Screen 5 — Diagnostics (10 s hold)
+------------------------------+
| BatMon-A3F2   T2  bus:ok     |
| flags 0x0040  unc 0.4Ah      |
+------------------------------+
```

Debug mode (§9.11) appends three more, reachable only while it is active:

```
 Screen D1 — Sensor raw             Screen D2 — Tier and power
+------------------------------+  +------------------------------+
| A40 sh -21400uV  13.420V     |  | T1  awake 15%  up 04:12:33   |
| B41 sh    +120uV PGA/4 ovf0  |  | i2c 0err  slp 99%  wakes 12  |
+------------------------------+  +------------------------------+

 Screen D3 — Gauge internals
+------------------------------+
| q +134786.2As  dt 100 n 128  |
| anch REST 00:12  off -14uA   |
+------------------------------+
```

- **D1** is the screen that finds wiring faults: both sensors, raw shunt µV *before* any
  scaling, the selected PGA and the OVF flag. A swapped pair, a mis-strapped address
  (§2.10.2) or a saturating range is obvious here and nowhere else.
- **D2** serves the §9 power work — current tier, measured awake fraction (the §9.8 wake
  term, live), uptime, bus error count, light-sleep residency and wake count since boot.
- **D3** exposes the integrator: `charge_uAs` at full precision rather than the 0.1 A·h the
  user sees, the last *measured* `dt`, samples integrated, the §5.4 anchor state and its
  dwell, and the applied offset.

Rendering rules:

- **Large digits for SoC only.** Everything else is the 6×8 font. If the user can read one
  thing at arm's length in a dim garage, it should be the percentage.
- **Show uncertainty where it exists.** Screen 4's `+/-0.4Ah` is `drift_uncert_uAs`
  rendered honestly. A gauge that displays a confident number it does not have is worse
  than one that admits the range — and this device knows its own uncertainty, so it should
  say it.
- **`Since full 3d`** is the most diagnostic single field on the device: a coulomb counter
  that has not seen a full-charge anchor in weeks is a coulomb counter to distrust, and
  this is where the user finds that out.
- Warning flags (§7.6) invert the top line rather than opening a modal — a 32-pixel display
  has no room for dialogs, and an inverted bar is visible from further away than text.
- All values come from the shared snapshot (§3.3). The display never recomputes.
