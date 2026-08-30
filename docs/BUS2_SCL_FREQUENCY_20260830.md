# BUS-2 — SCL IS PROBABLY RUNNING ABOVE 400 kHz
**2026-08-30. H-D8 REOPENED. No code changed — the fix is one constant, but it must not be
applied before the measurement.**

Labelled per CLAUDE.md §9: **evidence** (measured or read in a source), **inference**
(follows from evidence), **speculation** (plausible, unverified).

---

## 1. Claim

**The I2C1 SCL clock is probably running at ~445 kHz, about 11% above the 400 kHz maximum
of every device on the bus.** *(inference, medium-high confidence)*

Both device limits are hard:

| Device | Limit | Source |
|---|---|---|
| DRV2605L | `f(SCL)` max **400 kHz**, no wait states | SLOS854D **§6.6 Timing Requirements** |
| VL53L1X | `FI2C` operating frequency 0 – **400 kHz** | VL53L1X datasheet **Table 7**, fast mode |

**Citation correction:** CLAUDE.md §8 H-D8 attributes `f(SCL)` to "§6.7 Switching
Characteristics". It is in **§6.6 Timing Requirements**; §6.7 is the following section and
contains `t(start)` and `fO(PWM)`. The value 400 kHz is right; the section number is not.

## 2. How the timing is computed *(evidence — `firmware/Appli/Core/Src/i2c_timing.c`)*

`app_i2c.c` calls `I2C_GetTiming(pclk1, I2C_BUS_HZ)`. That is ST's reference algorithm.
Ported faithfully to Python and run at `pclk1 = 200 MHz` (the measured value), it selects:

```
PRESC=6  SCLDEL=9  SDADEL=1  SCLL=35  SCLH=21     TIMINGR = 0x60911523
tSCL_L = 1320 ns
tSCL_H =  830 ns
```

The algorithm then declares success because of this line (`i2c_timing.c`, `I2C_Compute_SCLL_SCLH`):

```c
/* tSCL = tf + tLOW + tr + tHIGH */
uint32_t tscl = tscl_l + tscl_h + I2C_Charac[I2C_speed].trise + I2C_Charac[I2C_speed].tfall;
```

`1320 + 830 + 250 + 100 = 2500 ns` → exactly 400.0 kHz, error zero. The algorithm is working
as designed.

**But only 2150 ns of that period is hardware.** `tSCL_L` and `tSCL_H` are what the peripheral
actually drives. The remaining 350 ns is `trise` + `tfall` **assumed** from the
`I2C_Charac[I2C_SPEED_FREQ_FAST]` table — 250 ns and 100 ns, the fast-mode worst case.

## 3. Why that assumption is wrong on this board *(evidence → inference)*

CLAUDE.md §2 already computes this bus's rise time, for the pull-up budget:

> Rise time is never the problem here: 892 Ω × ~100 pF ⇒ **tr ≈ 76 ns**, well inside the
> 300 ns fast-mode limit.

Today the bus is even lighter than that figure — one SmartElex, so 1.5 kΩ ∥ 2.2 kΩ = 892 Ω,
and short breadboard runs. Fall time is a push-pull pull-down, faster still.

| assumed tr / tf | real period | real SCL |
|---|---|---|
| 250 / 100 ns (ST table) | 2500 ns | 400 kHz |
| **76 / 20 ns (CLAUDE.md computed)** | **2246 ns** | **445 kHz** |
| 76 / 10 ns | 2236 ns | 447 kHz |
| 50 / 10 ns | 2210 ns | 452 kHz |

The accept band in the table is `freq_min = 320000, freq_max = 480000`, so the algorithm
would have accepted anything up to 480 kHz as "fast mode" — it never checks the delivered
frequency against the 400 kHz device limit, because it believes the edges eat 350 ns.

## 4. Independent corroboration from the HAP-T9 log *(inference)*

The GO-bit poll issues back-to-back 1-byte register reads, and `ok=` counts them. Three
measurement windows in the 2026-08-30 90-second capture:

| sample | reads consumed | implied poll window |
|---|---|---|
| 1–3 (mean) | 493 | 38.9 ms |
| 4 | 731 | 57.7 ms |
| 5 | 595 | 47.0 ms |

Polling starts 0–20 ms after the trigger edge depending on sensor-loop phase, so the window
is 38.6–58.6 ms. **A single constant transaction time of ≈ 79 µs fits all three** with
implied pickup latencies of 19.7, 0.9 and 11.6 ms — a plausible spread across a 20 ms cycle.

A 1-byte register read is 4 bytes × 9 bits = 36 bit-times plus START/RESTART/STOP.
At 445 kHz, 36 bit-times = **80.9 µs**, against the ≈ 79 µs observed. At 400 kHz it would be
90 µs. The two independent lines agree.

**This is weaker evidence than it looks and must not be quoted as a bus measurement.** The
79 µs includes DMA setup, `tk_wai_sem` and a task switch, and the bit count is assumed. It
is corroboration, not measurement.

## 5. The fix that does NOT work — and would have looked like it did

The obvious move is to lower `I2C_BUS_HZ` from 400000 to 350000. **It would change nothing.**
`I2C_GetTiming()` uses the requested frequency **only to select a speed bucket**:

```c
if ((i2c_freq >= I2C_Charac[speed].freq_min) && (i2c_freq <= I2C_Charac[speed].freq_max))
```

350000 still falls inside the FAST band [320000, 480000], and every subsequent computation
targets `I2C_Charac[speed].freq`, which is hardcoded **400000**. The requested value is
discarded. A change to `I2C_BUS_HZ` anywhere in [320000, 480000] produces a byte-identical
TIMINGR. Requesting 300000 matches no bucket at all and returns 0, which `app_i2c_init()`
correctly turns into `E_PAR`.

**This is the same silent-failure shape as RZ9:** the edit looks right, the build succeeds,
the bus keeps working, and nothing has changed.

The fix that does work is to lower `I2C_Charac[I2C_SPEED_FREQ_FAST].freq` (the target),
leaving `freq_min`/`freq_max` alone so bucket selection still matches. `.freq = 350000` gives
a 2857 ns target, so `tSCL_L + tSCL_H ≈ 2507 ns` and the delivered clock stays **under
400 kHz for any edge speed down to zero**. Cost is ~11% bus throughput, on a bus with large
headroom.

Do **not** instead lower `trise`/`tfall` in that table: they also feed `tscldel_min` and
`tsdadel_max`, where understating them tightens data setup/hold. The two uses pull in
opposite safety directions.

## 6. Also noticed: tw(L) has 20 ns of margin *(evidence)*

SLOS854D §6.6 gives `tw(L)`, SCL low pulse duration, **min 1.3 µs**. The computed
`tSCL_L = 1320 ns`. That is legal by **20 ns**, and it is legal only because the algorithm's
`lscl_min` check is `tscl_l > 1300`. Worth knowing before anyone changes `pclk1` or the
prescaler — a different PLL configuration could land the other side of it.

## 7. What has NOT been done, and what settles it

**No code has been changed.** The 445 kHz figure is inference built on a *computed* rise time
(CLAUDE.md's 76 ns) that has never been measured. If the real edges are slower than computed,
the delivered clock could already be at or under 400 kHz and no fix is needed.

**The single test that settles it: BUS-2 — capture SCL on the logic analyzer and measure the
period.** The hardware is on hand (24 MHz sigrok clone; at 445 kHz that is ~54 samples per
period, ample). The bus is currently at its *lightest* — one device — which is the worst case
for overshoot, so measuring now is measuring the maximum.

This is already a planned deliverable: plan v2 Block 2 step 13 folds BUS-2 into the L5
`WrByte` capture. **Recommend moving it earlier, before the VL53L1X is soldered.** Otherwise
Block 2's 91-write init and its per-frame 17-byte reads all run at an unverified clock
against a second device whose limit is also 400 kHz.

Decision rule once the capture exists:
- **period ≥ 2500 ns (≤ 400 kHz)** → H-D8 genuinely closes, no change, record the number.
- **period < 2500 ns** → apply §5's `.freq` change and re-capture to confirm.

---

## 8. RESULT — measured, fixed, re-measured. H-D8 CLOSED. *(evidence)*

Both captures: `sigrok-cli --driver fx2lafw --config samplerate=8m --time 5s`, SCL on **D2**
(the pin silkscreened **CH3** — these clones label CH1–CH8 while the driver names the same
lines D0–D7), SDA on D3, ground to CN8 pin 7. Analysed with
`docs/evidence/phase5/analyze_scl.py`.

| | before · `.freq = 400000` | after · `.freq = 350000` |
|---|---|---|
| capture | `bus2_scl_20260830.sr` | `bus2_scl_20260830_fixed.sr` |
| SCL rising edges | 380 | 304 |
| in-byte periods | 360 | 288 |
| **minimum period** | **2250 ns** | **2500 ns** |
| median period | 2250 ns | **2625 ns** |
| mean period | 2285 ns | 2644 ns |
| median frequency | **444.4 kHz** | **381.0 kHz** |
| verdict | **FAIL — 11% over** | **PASS** |

**The compliance statement is the minimum, not the median: after the fix, not one of 288
measured periods fell below 2500 ns.** Before the fix, not one rose above it.

**Read the minimum correctly.** `min = 2500.0 ns` is exactly 20 samples at 125 ns
quantisation; the true period is ~2605 ns and lands on 20 or 21 samples depending on where the
edges fall relative to the sample clock. It is **not** a real excursion to 400.0 kHz, and the
"SCL freq max 400.0 kHz" line the tool prints must not be quoted as though the bus touches the
limit. The honest single figure is the median, 2625 ns / 381 kHz, with the true value bounded
below at 2500 ns by direct observation.

**The prediction held.** §3 predicted 2246 ns from a *computed* rise time; the measurement
returned 2250 ns. That also confirms CLAUDE.md §2's `tr ≈ 76 ns`, which had never been
measured — real edge time comes out at 2250 − 2150 = **~100 ns** against the 350 ns the ST
table assumes.

**Decoder cross-check**, same capture file:
```
i2c-1: Address write: 5A / Data write: 00 → Address read: 5A / Data read: E0
i2c-1: Address write: 5A / Data write: 01 → Address read: 5A / Data read: 01
```
STATUS = 0xE0, MODE = 0x01 from 0x5A — that is `drv2605l_poll()` on the wire, which confirms
the probe was on the right bus and that the runtime health poll does what it claims.

**Optional refinement, not required.** A 24 MHz capture would give 41.7 ns resolution and pin
the period to ~2605 ± 42 ns instead of bounding it. The 8 MHz data already proves compliance,
and 8 MHz is the rate the Phase 4 runbook established as drop-free on this clone.
