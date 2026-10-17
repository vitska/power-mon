# Wiring, as built

What is physically connected to what, on all three boards, with the values the firmware
expects. The reasoning behind these choices is DESIGN.md §2; this is the reference you
want with a soldering iron in hand, and the bench procedure that follows it is
CALIBRATION.md.

Every pin and address here is a **Kconfig default** (`main/Kconfig.projbuild`) — change
it there and reflash rather than editing code. The shunt value and every calibration
constant are *settings*, kept in flash and set over the console; they are not compiled
in.

---

## 1. The three boards

| | Board | Does |
|---|---|---|
| **Monitor** | Seeed XIAO ESP32-C6 | measures, counts, keeps the history and the pack's log; serves the console over USB and BLE |
| **Remote display** | ESP32-2432S028 ("Cheap Yellow Display"), GUITION JC2432W328R or JC1732W319 | BLE client; its own firmware in `remote/` |
| **Phone** | any Android 8+ | BLE client; `android/` |

Only the monitor touches the battery. The other two are read-only observers over BLE and
need no connection to the pack at all.

---

## 2. Monitor: power and the measurement loop

**Power the monitor from the pack it measures.** Anything else gives the two INA219s a
different ground reference from the SoC and the measurement stops meaning anything
(§2.9.4). The XIAO's onboard regulator accepts the 5 V pin or the BAT pads; a 12 V pack
needs a buck in front of it.

```
        +---------------------------------------------------------------+
   PACK+ o------+-------------------------------------o LOAD+ / CHG+     |
                |                                                        |
          [ Vbus divider, >=1M, only above 26 V ] --> INA219 VBUS        |
                                                                         |
   PACK- o---[ SHUNT 10 mOhm ]------------------------o LOAD- / CHG-     |
              |              |                                           |
              +--------------+----------------------> INA219 VIN+ / VIN- |
        +---------------------------------------------------------------+
                          |
                          |  I2C, one shared bus: GPIO22 (SDA) / GPIO23 (SCL)
                          v
                 +--------------------+
                 |   XIAO ESP32-C6    | --( BLE )--> phone, remote display
                 +--------------------+
                   |          |      |
            GPIO2  |   GPIO21 |      +--------> BME280 / BMP280   (3V3 always)
           button  |  rail    |      +--------> SSD1306 OLED      (gated rail)
          (planned)|  gate    +---------------> VOLED rail
```

**Sign convention: current > 0 is charge going into the pack**, < 0 is discharge. The
firmware, the console, both clients and the JBD protocol all agree on this; nothing
anywhere inverts it. If your board reads backwards, fix it with `sense sign invert`
rather than by swapping the shunt leads — the leads also carry the VBUS reference.

---

## 3. Pin map

### Monitor — XIAO ESP32-C6

| Pin | Silk | Net | Notes |
|---|---|---|---|
| GPIO22 | D4 | I²C SDA | all four devices share it |
| GPIO23 | D5 | I²C SCL | 400 kHz; fit 4.7 kΩ pull-ups and turn the internal ones off |
| GPIO21 | D3 | VOLED gate | high-side load switch for the display rail |
| GPIO2 | D2 | Button | wake and screen cycling — **not implemented yet** (M5b) |
| GPIO3 | — | RF switch enable | driven LOW at boot |
| GPIO14 | — | Antenna select | LOW = onboard, HIGH = u.FL |
| 5V / BAT | — | Supply | from the monitored pack, via a buck for 12 V |

Internal pull-ups are ~45 kΩ and marginal at 400 kHz. They exist so a bare sensor works
on the bench; a permanent install gets external 4.7 kΩ and
`BATMON_I2CA_INTERNAL_PULLUPS=n`. **If the bus is unreliable, suspect this first.**

### I²C addresses

| Device | Address | Role |
|---|---|---|
| INA219 **A** | **0x40** | positive-pole sensor |
| INA219 **B** | **0x41** | negative-pole sensor |
| SSD1306 128×32 OLED | 0x3C | local readout |
| BME280 / BMP280 | 0x76, else 0x77 | ambient temperature; probed by chip ID |

`scan` lists what is actually on the bus, and is the first command to run on a new
build. The two INA219 addresses are set by their A0/A1 straps — **position is encoded in
the address**, which is how the firmware knows which sensor is on which pole without
being told (§2.10.2).

### Remote display — ESP32-2432S028, GUITION JC2432W328R or JC1732W319

Fixed by the board; nothing to wire. The board is a build-time choice
(`remote/main/board.h`, `tools/idf.ps1 -Board`).

| Function | ESP32-2432S028R (`cyd`, `cyd2usb`) | JC2432W328R (`jc2432w328r`) | JC1732W319 (`jc1732w319`) |
|---|---|---|---|
| Panel, SPI2 | ILI9341 (`cyd`) or ST7789 (`cyd2usb`): SCLK 14, MOSI 13, MISO 12, CS 15, DC 2 | ST7789, the same pins | ST7789 320x170, the same pins |
| Panel reset | tied to EN | tied to EN | tied to EN |
| Backlight | GPIO21, active high | GPIO27, active high | GPIO21, active high |
| Touch (XPT2046) | SPI3: CLK 25, MOSI 32, MISO 39, CS 33 | on the panel's SPI2, CS 33 | none |
| Touch axes (raw) | X 200 left → 3700 right, Y 240 top → 3800 bottom | both reversed: X 3700 → 550, Y 3400 → 650 | — |
| RGB LED, common anode | not used | red 4, green 16, blue 17 (PWM, 10 %) | none |

The touch calibration is in `remote/main/board.h` with the rest of each board's wiring,
measured on the board with the remote's console (`touch log on`, then the corners).

The panel runs at **40 MHz**, which is above the ILI9341's documented 10 MHz write
timing. Every driver for these modules does it and it works; if a particular board shows
torn or speckled pixels, that is the first thing to turn down (`lcd.c`, `pclk_hz`).

---

## 4. The shunt

**10 mΩ** by default. The constraint is the INA219's ±320 mV full scale and, low-side,
the need to keep the drop small enough that the inputs stay in range:

| Placement | Shunt | Full scale | Why |
|---|---|---|---|
| High side (mode P) | 10 mΩ | ±32 A | the drop floats on the pack rail, no ground problem |
| **Low side (mode N)** | 10 mΩ | **±10 A** | the drop must stay under ~100 mV (§2.9.2) |

Size it for the largest current you will actually see, not the largest the part allows:
the gauge's resolution is the shunt drop, and a shunt chosen for a starter motor will not
resolve a parasitic load.

### Mode N — shunt in the negative lead

The case this project was built for, and the harder one: the INA219 inputs sit below the
SoC's ground.

```
   PACK+ o---------------------------------o LOAD+ / CHG+
         |
   PACK- o----[ SHUNT ]---------------------o LOAD- / CHG-
         P              S
         |
      ===+=== system ground = P
```

| | INA-A @ 0x40 | INA-B @ 0x41 |
|---|---|---|
| GND | P | P |
| IN+ / IN− | both tied to **PACK+** (shorted) | **IN+ = S**, **IN− = P** |
| VBUS | **PACK+** | **LOAD+** |
| Gives | true pack voltage, and a live zero reference | current, and load-side voltage |

The shorted sensor is not idle: reading a deliberately zero current while the other
sensor works is what makes the offset self-check possible without disconnecting anything
(§2.10.5).

### Mode P — shunt in the positive lead

```
   PACK+ o----[ SHUNT ]---------------------o LOAD+ / CHG+
              Pp         Sp
   PACK- o---------------------------------o LOAD- / CHG-
         |
      ===+=== system ground = PACK-
```

Classic high-side sensing, no ground offset to manage. The roles of the two sensors
swap; the firmware works it out from the addresses and the install mode.

`shunt loc p|n|single|auto` tells the firmware which you built. **AUTO resolves it by
watching current flow** and refuses to integrate until it has seen some — at zero current
the two are indistinguishable, and guessing would invert the sign.

---

## 5. Grounding — the part that ruins measurements

One rule, and it is not optional: **the SoC's ground and the INA219 grounds must be the
same node, and current must not flow through that connection.** A second ground path —
a USB cable to a mains-powered laptop, a charger that bonds LOAD− to earth, a shared
chassis return — puts part of the load current through your reference and the shunt
reading becomes fiction.

Symptoms: the current reads low by a fraction that changes with load, or the zero drifts
when something else in the system switches on. §2.9.4 has the full treatment.

While calibrating on the bench, run the monitor from the pack and talk to it over BLE
rather than USB, for exactly this reason.

---

## 6. Voltage above 26 V

The INA219's VBUS input tops out at 26 V. A 24 V pack on charge, 7S Li-ion or 8S LiFePO₄
exceeds that, so fit a resistive divider into VBUS and tell the firmware its ratio with
`curve v divider`. Use ≥1 MΩ total so the divider is not a parasitic load of its own, and
calibrate the voltage channel after fitting it — the divider's tolerance is now part of
your measurement.

---

## 7. Bring-up order

1. `scan` — every expected address present? If not, stop here; it is wiring or pull-ups.
2. `shunt <uohm>` — the resistance you actually fitted.
3. `shunt loc n|p|single` — where you fitted it.
4. `detect` — with a load drawing current, confirms which sensor is on which pole.
5. `battery <chemistry> <cells>` — loads the OCV curve and the charge endpoints.
6. `soc cap <uAh>` — the nameplate capacity.
7. Calibrate: CALIBRATION.md, in that order, preconditions respected.
8. `options` — read back everything the board now believes.

---

## 8. Where these numbers live

| Thing | Set by | Stored |
|---|---|---|
| Pins, I²C addresses, bus speed | `idf.py menuconfig` | compiled in |
| Shunt resistance, install mode | `shunt`, `shunt loc` | NVS `cfg` |
| Offsets, gains, divider | `cal`, `curve` | NVS `cfg` |
| Battery chemistry, cells, capacity | `battery`, `soc cap` | NVS `cfg` |
| Learned capacity, cycles, lifetime | measured by the gauge | NVS `fg` |

Everything in the lower four rows survives a reboot, an OTA and a rollback. Only the top
row needs a rebuild.
