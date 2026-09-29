# Capacity: how the gauge measures the pack

The number on the case is a claim about a new battery. The gauge measures the pack you
actually have, by counting coulombs between two moments when it knows the state of
charge independently. Code: `components/fuelgauge/fuelgauge.c`. Rationale: DESIGN.md
§5.4 D.

## Three numbers get called "capacity"

| Number | Key | What it is |
|---|---|---|
| Nameplate | `soc.cap_uah` | what the pack was sold as — you set it, nothing measures it |
| Measured | `soc.learned_uah` | what the gauge measured. **Starts as a copy of the nameplate** |
| Remaining | `charge` | what is in the pack right now |

`soc.learn_count` is how many measurements have happened. **While it is 0 the measured
figure is just the copy**, and the clients say so rather than quoting it: the remote
shows `CAP 45.0AH (learning)`, the app "(not measured yet)".

State of health is measured / nameplate. Every SoC percentage is charge / measured.

---

## How to get it to learn

1. **Set the nameplate** — `soc cap 45000000` for 45 A·h. It bounds what will be
   believed later (§ *the gates*, rule 5).
2. **Set a depth you will actually reach** — `soc depth 200` asks for a 20 % discharge,
   which ordinary use provides. The default is 200; higher is better evidence and far
   rarer.
3. **Open a span.** Charge to full, or leave the pack idle for `soc rest` (5 min).
   Either one anchors a reference point. `soc` then shows `learn span from …`.
4. **Discharge past the depth in one go.** No charging, and no idle gaps longer than
   `soc rest` — a rest is itself a reference point and starts the span over.
5. **Close the span.** Stop the load and leave it idle for `soc rest`, or discharge
   until the pack reads empty.
6. **Check it** — `soc` should now say `1 learn` and a state of health under 100 %.

The best measurement, if you want to deliberately characterise a pack: charge full,
rest, discharge at a steady moderate current until empty, rest. That is a full-depth
span with no gaps, which is the strongest evidence the algorithm can be given.

---

## How it works

At a reference point, if a span is open back to an earlier one:

```
capacity = charge drawn / (SoC then − SoC now)
```

From 90 % to 30 % is 60 % of the pack, so the charge drawn is 60 % of its capacity.
The pack never has to reach 0 %.

**A reference point** is a moment SoC is known without the count:

| | Condition | Held for |
|---|---|---|
| **FULL** | voltage ≥ `soc vfull` and current below the taper threshold | 60 s |
| **EMPTY** | I·R-compensated voltage ≤ `soc v0`, under load | 10 s |
| **REST** | current idle, then resting voltage read off the chemistry curve | `soc rest` |

A rest anchors after `soc rest` — 5 minutes on lead-acid, longer on the flat
chemistries, which settle more slowly. Idle means `|I|` below `soc irest` — 15 per mille of the design capacity by
default, 0.68 A on a 45 A·h pack — smoothed over ~10 s,
with hysteresis: entering a direction takes twice that, leaving it only once. On
lead-acid every settled rest anchors; on flat chemistries (LiFePO₄, NiMH) only rests
near the ends of the curve do.

**The gates.** A span is measured only if:

1. a previous reference exists;
2. SoC went **down** — charging puts back less than it takes, so a span with charging
   in it would measure the charger;
3. depth ≥ `soc depth`;
4. charge in during the span ≤ 2 % of capacity (float ripple, not a recharge);
5. the result is between nameplate/20 and nameplate × 1.5 — outside that it is a shunt
   or calibration fault, and is rejected with a log line.

**The result.** The first measurement replaces the nameplate outright — a nameplate is
a claim, not a measurement of this pack. Later ones move capacity part of the way,
weighted by the span's depth, so deeper spans count for more.

Counted charge is Peukert-adjusted on discharge only (`soc peukert`, `soc irated`), so
a pack worked hard counts out more than the shunt literally measured.

---

## Settings

| Setting | Default | Effect |
|---|---|---|
| `soc depth <permille>` | 200 (20 %) | depth a span must reach |
| `soc rest <s>` | 300 | idle time before a rest anchors — and so how easily a lull splits a span |
| `soc irest <permille>` | 15 (1.5 % of C) | the current below which the pack counts as idle at all |
| `soc cap <uAh>` | — | changing it **rescales the measured capacity and resets the count** |
| `soc taper <uA>` | design/30 | how easily FULL is reached |
| `soc irated`, `soc peukert` | per chemistry | the Peukert adjustment; `soc peukert 256` disables it |

`soc reset` clears the count and the open span; `soc reset all` also clears the SoC
history.

---

## If it never learns

In order of likelihood:

1. **`soc depth` is deeper than the pack is ever cycled.** At 600 ‰ you must draw 60 %
   between two references — 27 A·h on a 45 A·h battery. `soc` prints the requirement.
2. **The load keeps pausing.** Any gap longer than `soc rest` under the rest current
   anchors a new reference and restarts the span.
3. **Something charged during the span** — more than 2 % in and it is discarded. A
   solar controller on a sunny morning is enough.
4. **The measurement was rejected as implausible** — `capacity measurement … implausible
   against … nameplate` on the log means the shunt or the calibration, not the battery.
5. **No reference is ever reached** — never full, never empty, never idle long enough.

---

## What survives a restart

Everything the measurement needs is in flash: the open span (where it started and how
much has come out of it), the measured capacity, the learn count and the coulomb
counters. The gauge writes them every 300 s or half a percent of SoC, **and once more
on the way down** -- `reboot`, an OTA and a rollback all save before restarting, so a
firmware update costs nothing.

The one case where a span is deliberately dropped is a **power-on or brownout** reset.
The board may have been dark for a minute or a month with the load still draining the
pack, and a span with unmeasured coulombs in it would measure the wrong capacity
confidently. A software restart is the opposite -- a second or two, powered throughout,
state written on the way down -- and keeps the span. The log says which happened.

A gap of more than 60 s in the samples while running closes the span for the same
reason: charge flowed that nobody counted.

## History

Firmware before **0.11.7** could not keep a measured capacity on any pack whose
nameplate was not the compiled-in 44 A·h default: at boot the gauge compared the
restored setting against that default, read it as a deliberate change, and reset the
measurement and the count. It learned, saved, and lost it at every power cycle. The
comparison is now against `dcap`, persisted beside the measured figure.
