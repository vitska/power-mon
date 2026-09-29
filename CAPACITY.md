# Capacity: what the gauge measures, and when

A battery's capacity is not a number you can read off it. The figure printed on the
case is a claim about a new cell at a rated discharge rate and a rated temperature;
the pack in front of you has a history. So the gauge **measures** capacity instead,
from coulombs it has counted itself, and the nameplate is only a starting point and a
sanity bound.

This document is the whole of that mechanism: what is measured, from what, when, and
why it sometimes never happens. The code is `components/fuelgauge/fuelgauge.c`; the
design rationale it belongs to is DESIGN.md §5.4.

---

## 1. Three numbers get called "capacity"

Most confusion about this feature is really a confusion between these:

| Number | Where | What it is |
|---|---|---|
| **Nameplate / design** | `soc.cap_uah`, set by `soc cap` | What the pack was sold as. A claim, never measured here. |
| **Learned / measured** | `soc.learned_uah` | What the gauge has measured this pack to hold. **Starts equal to the nameplate** and stays there until the first measurement. |
| **Remaining charge** | `charge` in `soc`, the `c,` telemetry record | What is in the pack *right now*. This is the one that moves minute to minute. |

State of health is `learned / design`, and the percentage everything else shows is
`charge / learned` — so the learned figure is the denominator of every SoC on every
screen. Getting it right matters more than it looks.

`soc.learn_count` says how many times capacity has actually been measured. **While it
is 0, `soc.learned_uah` is the nameplate copied, not a result** — an equal pair is
otherwise indistinguishable from a pack that measured exactly its rating. The clients
say so rather than quoting the copy: the remote shows `CAP 45.0AH (learning)` and the
app appends "(not measured yet)".

---

## 2. The measurement

Capacity is the charge that comes out between two moments when SoC is known *without
the count* — a **reference point**:

```
capacity = charge drawn / (SoC at the earlier reference − SoC at the later one)
```

That is all. Any discharge deep enough measures the pack, whether or not it ever
reaches 0 %: from 90 % to 30 % is 60 % of the pack, so the charge drawn over that span
is 60 % of its capacity.

**Discharge spans only.** Lead-acid coulombic efficiency is well under 100 %, so more
charge goes in than comes back out; a span with real charging in it would measure the
charger's losses and call them capacity.

`ref_point()`, `fuelgauge.c`.

---

## 3. What counts as a reference point

Three things, each a moment SoC is known from physics rather than arithmetic:

| Reference | Condition | Holds for |
|---|---|---|
| **FULL** | terminal voltage at or above `soc vfull` **and** mean current below the taper threshold | `t_full_hold_s`, 60 s |
| **EMPTY** | I·R-compensated voltage at or below `soc v0`, under load | `EMPTY_HOLD_S`, 10 s |
| **REST** | current idle, then resting OCV read off the chemistry's curve | `soc rest`, 600 s for lead-acid |

"Idle" is `|I|` below **design capacity / 110** (0.4 A on a 44 Ah pack), floored at the
integration deadband, and smoothed over ~10 s. Entering a direction needs twice that,
leaving it only once — hysteresis, so a flickering load does not flip the state. This
is why a display can read REST at −0.6 A: 0.6 is above the 0.4 A rest current but below
the 0.8 A needed to *enter* discharge.

The taper threshold is `soc taper` scaled by `learned / design`, so a pack that has
lost capacity is judged full at a proportionally lower current rather than never
reaching "full" at all.

For every lead-acid chemistry the resting curve is trusted at **all** states of charge
(`trust_lo = trust_hi = 1000`), so on a lead-acid pack every settled rest is a
reference point. On the flat chemistries — LiFePO₄, NiMH — a rest in the middle of the
curve says too little about SoC to anchor anything, and is ignored.

> **A rest re-anchors and resets the open span**, and it does so again every `soc rest`
> seconds while the rest continues. A 10-minute lull in the middle of a discharge ends
> the span there and starts a new one. Depth has to be reached *between two consecutive
> reference points*, not cumulatively.

---

## 4. When a span is actually used

At a new reference point, the span back to the previous one is measured only if all of
these hold:

1. **A previous reference exists.** Reset, a capacity change, or a first boot opens
   with none.
2. **SoC went down.** Discharge-only, per §2.
3. **Depth ≥ `soc depth`.** Default **200 ‰ (20 %)** for lead-acid; the setting is per
   board and may be higher.
4. **Charge in during the span ≤ 2 % of capacity.** Float ripple is tolerated, an
   actual recharge is not.
5. **The result is plausible:** between `design / 20` and `design × 1.5`. Outside that
   it is a mis-set shunt or a missed anchor, not a battery, and is discarded with a
   warning on the log.

---

## 5. What the result does

`learn_capacity()`, `fuelgauge.c`.

- **The first measurement replaces the nameplate outright.** A nameplate is a claim
  about a new battery; blending a real 4 A·h against a claimed 45 at a quarter per
  cycle would take a dozen cycles to admit what the pack already is.
- **Every measurement after that moves capacity part of the way**, by
  `learn_blend_q8 × depth / 500`, clamped to 1/256…256/256. With the default
  `learn_blend_q8` of 64, a 20 % span moves about 10 % of the distance and a 60 % span
  about 30 %: deeper spans are better evidence and count for more.

The result is written to flash with the rest of the gauge state, and survives reboots
and OTA updates.

---

## 6. The charge counted is Peukert-adjusted

A discharge increment is scaled by Peukert's law before it is accumulated, so a pack
worked hard counts more charge out than the shunt literally measured — which is what
the pack actually loses. Charging is **never** scaled: the effect is asymmetric in the
physics, and applying it to charge going in would quietly inflate the count on every
recharge.

The rate ratio is clamped to 1/32…32 of `soc irated` before the exponent: Peukert's law
is an empirical fit good within a couple of decades of the rated rate, and outside that
it produces confident nonsense.

`k = 1.0` (`soc peukert 256`) disables it. Defaults are per chemistry — 1.15 for
flooded lead-acid.

---

## 7. Where to see it

`soc` on the console answers every question this document raises:

```
charge       34.330 Ah of 45.0 Ah
SoH          100 %   (45.0 Ah learned / 45.0 Ah nameplate, 0 learns)
learn span   from 76.5 % reference, 2.140 Ah since; learns at the next
             FULL/EMPTY/rest at least 60.0 % lower
Peukert      k 1.148, x1.0021 at the present rate
```

- `0 learns` — nothing measured yet; the learned figure is the nameplate.
- `learn span` — where the open span started, how much has come out of it, and how far
  down the next reference has to be. `none` means no span is open.
- `last raw measurement` appears once there has been one, before blending.

Programmatically: `soc.cap_uah`, `soc.learned_uah`, `soc.learn_count` in `config`
(CLI.md §6). In the app, Configure → Fuel gauge. On the remote, `CAP 45.0AH (43.97)`
under the chemistry.

---

## 8. The settings that decide how often it learns

| Setting | Default | Effect on learning |
|---|---|---|
| `soc depth <permille>` | 200 (20 %) | The depth a span must reach. Higher is stronger evidence and far rarer. |
| `soc rest <s>` | 600 | How long idle before a rest anchors — and therefore how easily a lull splits a span. |
| `soc cap <uAh>` | — | Changing it **rescales the learned capacity and resets `learn_count`**: a deliberate capacity is a new claim, and the next measurement replaces it outright. |
| `soc taper <uA>` | design/30 | How easily FULL is reached, and so how often a span closes at the top. |
| `soc irated <uA>` | C/20 | The rate Peukert is measured against. |
| `soc peukert <q8>` | per chemistry | 256 disables the adjustment. |

`soc reset` clears the count and abandons the open span. `soc reset all` also clears
the SoC history.

---

## 9. If it never learns

In order of likelihood:

1. **`soc depth` is set deeper than the pack is ever cycled.** At 600 ‰ you must draw
   60 % of the pack between two consecutive reference points — roughly 27 A·h on a
   45 A·h battery. Check it in `soc` ("at least 60.0 % lower"); 200–300 learns from
   ordinary partial cycles.
2. **The load keeps pausing.** Any 10-minute lull under the rest current anchors a new
   reference and restarts the span. A span needs the depth in one go.
3. **Something charges during the span.** More than 2 % of capacity in, and the span is
   discarded — a solar controller on a sunny morning is enough.
4. **The measurement was implausible and thrown out.** `capacity measurement …
   implausible against … nameplate -- ignored` on the log means the shunt or the
   calibration, not the battery.
5. **The pack never reaches a reference at all.** No rest long enough, never full,
   never empty — nothing closes the span.

---

## 10. History

Firmware **before 0.11.7 could not keep a learned capacity at all** on any pack whose
design capacity was not exactly the compiled-in 44 A·h default. `fg_set_config()`
compared the incoming capacity against the value in RAM, which at boot still held that
default because the gauge is initialised before stored settings are pushed into it — so
restoring the setting looked like changing it, and the reset fired: learned capacity
back to the nameplate, `learn_count` back to 0, on every single boot. The gauge could
learn, and did, and lost it at the next power cycle.

The comparison is now against `dcap`, persisted alongside the learned figure: the design
capacity that figure was actually measured against. Boards written by older firmware
have no `dcap`, which reads as "nothing to react to" and keeps what they learned.
