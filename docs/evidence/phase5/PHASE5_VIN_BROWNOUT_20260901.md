# ROOT CAUSE FOUND: THE VL53L1X HAS NEVER BEEN POWERED IN SPEC
**2026-09-01. VIN at the 7SEMI measures 2.04 V. Everything else follows from that.**

---

## 1. THE MEASUREMENT THAT ENDS THE INVESTIGATION *(evidence)*

| node | measured | expected |
|---|---|---|
| CN8 pin 4, system 3V3 rail | **3.32 V** | 3.25–3.35 V ✓ |
| DRV2605L breakout VCC | **3.31 V** | 10 mV drop ✓ |
| **7SEMI VL53L1X VIN** | **2.04 V** | should be ~3.31 V |

**That is 1.28 V of drop into one breakout on a rail that loses 10 mV into the
other.**

`docs/datasheets/vl53l1x_datasheet.pdf` (ST DS12385 Rev 8), supply table:

> **AVDD: min 2.6 V · typ 2.8 V · max 3.5 V**

**2.04 V is 560 mV BELOW the absolute minimum.** The VL53L1X has not been
operated inside its specification once in this project. Every symptom recorded
since the sensor was soldered is a brownout.

## 2. WHY 2.04 V AND NOT 0 V — the mechanism *(inference, high confidence)*

The VIN path is open or very nearly open, and **the die is being phantom-powered
through its own I2C pull-up resistors.**

The 7SEMI fits 2 × 9.9 kΩ from SDA and SCL to VIN (CLAUDE.md §2, DMM, board
disconnected). SDA and SCL are held near 3.3 V by the DK's own 1.5 kΩ pull-ups.
With VIN not otherwise connected, current flows **from the bus, through those
two 9.9 kΩ resistors, into the VIN node**, and powers the chip:

```
two 9.9 kOhm in parallel                 = 4.95 kOhm
I into VIN at the measured 2.04 V        = (3.3 - 2.04) / 4950 = 255 uA
```

255 µA runs a digital core sitting idle. It cannot run a VCSEL.

**And the two timing constants fall straight out of the same RC.**

```
recovery, measured on the analyzer   = 12.8 ms +/- 0.1 ms, three times
implied node capacitance C = t / R   = 12.8 ms / 4.95 kOhm = 2.6 uF
                                       (a 2.2 uF carrier decoupling cap)

collapse, measured                   = ~8.7 ms of sustained bus traffic
implied load I = C * dV/dt           = 2.6 uF * (1.2 V / 8.7 ms) = 359 uA
                                       (the digital block under continuous
                                        clocking, drawing more than 255 uA)
```

The node charges through 4.95 kΩ, the chip drains it faster than that when the
bus is toggling hard, and the recovery time is simply the RC recharge. **The
12.8 ms was never a device state machine and never a boot. It is a capacitor.**

## 3. EVERY OBSERVATION, EXPLAINED BY THIS ONE FAULT

| observation | explanation |
|---|---|
| L1 probe passes, `id=0xEACC`, `blk=0xEACC10` | four light transactions; the node holds charge |
| exactly 30–31 config writes land, then collapse | node drains after ~8.7 ms of sustained toggling |
| recovery **12.8 ms, identical across three boots** | RC recharge through 4.95 kΩ into ~2.6 µF |
| **address** NACKs, never data NACKs | the I2C slave block loses its supply entirely |
| 4,236 reads clean before the config block | idle-rate traffic, node keeps up |
| **845 of ~2000 reads NACK once all 91 registers land** | the part is finally *working*, so it finally draws real current |
| `wr_refused=0` with `wr_retry_max=14` | 14 × 1–2 ms of retry ≈ the recharge time; retries were waiting out a capacitor |
| XSHUT 2.62 V earlier, VIN 2.04 V now | the same sagging node, measured at two different loads |
| `[DRV]` flawless throughout, `faults=0x0`, `cfglost=0` | the DRV2605L has real power; the system rail was never the problem |
| the DK never reset, LD1 fine | the MCU was never browning out |

**Nothing is left unexplained.** *(inference, high confidence)*

## 4. THE FIX — five minutes, no parts

**This is a wiring fault, not a design fault.** With power OFF:

1. **Continuity, 3V3 rail → 7SEMI VIN pad. Must read < 1 Ω.**
   kΩ or open is the fault, confirmed.
2. **CHECK THE BREADBOARD POWER-RAIL SPLIT FIRST.** Most breadboards break the
   `+` and `−` rails in the middle of the strip. If the DRV2605L sits on one
   half and the 7SEMI on the other, the 7SEMI has no supply at all and is left
   exactly as measured — phantom-fed through its pull-ups. This is the single
   most likely cause and it matches every number above.
3. **Continuity, 7SEMI GND pad → CN8 pin 7. Must read < 1 Ω.** A missing GND
   return produces the identical symptom.
4. **Reflow the VIN and GND joints** on the 7SEMI. Reseat both jumper ends.
5. Then power up and **re-measure VIN at the 7SEMI pin: must be ≥ 3.2 V.**

**Do not flash anything, change any code, or take any capture until VIN reads
≥ 3.2 V.** Every measurement taken below 2.6 V is a measurement of a brownout
and tells you nothing about the design.

## 5. WHAT THE FIRMWARE WORK WAS ACTUALLY WORTH

None of it was wasted, and one piece becomes permanent.

- The L1 raw probe and its REG8 negative control **closed V-5/H1** — 16-bit
  addressing proven on the wire — while the sensor was browning out.
- The `[PRT]` / `[RTY]` counters localised a hardware fault to a specific
  register index and a specific recovery time, from firmware, without a scope.
- **`wr_retries` and `rd_retries` now become a permanent regression detector.**
  On correctly powered hardware they must read **0**. Any non-zero value in a
  future soak means the supply has degraded again — which is exactly what a
  courier does to a breadboard. **Keep them, keep them printed, and add them to
  `check_soak.py` as a check that must be zero.**
- The 0x2E / 0x2F pad fix (`bit 0 = 1`, pull-ups referenced to AVDD not 1.8 V)
  is **correct for this board regardless** and stays: ST's table declares a
  3.3 V bus to be a 1.8 V bus. It was not the cause, but leaving it wrong would
  have been a latent defect. *(inference)*
- Our own 91-write config loop replaces `VL53L1X_SensorInit` and closes F-2
  properly — ST's version discards the status of all 91 writes.

## 6. WHAT THIS COSTS AND WHAT IT BUYS

Nothing on the schedule. Block 2 was never blocked by software: the driver,
the shim, the probe and the gates are written, compiled and proven to catch
real faults. **The sensor has simply never been given 2.6 V.**

The lesson is one this project already wrote down once, after OC_DETECT:
**measure the supply at the load, not at the source.** V-W-1 metered CN8 and the
DRV2605L breakout on 31 Aug and both were fine. The 7SEMI's own VIN was not
metered until 1 Sep, and it was the only number that mattered.

**Add to the Block 5 checklist: after the mechanical rebuild, meter VIN AT EVERY
BREAKOUT, not at the rail.** A soldered assembly can carry exactly this fault
invisibly, and after 18 Sep there is no meter.
