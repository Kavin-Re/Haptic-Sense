# HAPTIC-SENSE — HANDOFF INTO BLOCK 2
**Written 2026-08-30 ~20:30 IST at the end of a long session (03:00 → 20:15).**
**Supersedes `docs/HANDOFF_20260830_BLOCK2.md`, which is stale — twelve commits landed after it.**

Read `CLAUDE.md` first. Plan: `docs/PLAN_TO_SUBMISSION_20260830.md` (v2, with Block 2 step 10
struck — see §2.7). Risks: `docs/RISK_ANALYSIS_20260830.md` (G-10 revised).

---

## 1. STATE

**Branch:** `master` and `phase5/i2c-first-light` both at **`64afd7b`**. Working tree carries
only CubeIDE noise (`.cproject`, `language.settings.xml`, a stray `.launch`). Stray git lock
files are parked in `.git/_stale_locks/` — safe to `rm -rf`.

**Flashed and verified:** the `64afd7b` build, `-Trusted.bin` at 14:42 UTC, newer than the
commit (CLAUDE.md §9 rule satisfied). **Nothing is owed a reflash.**

**Bench, working and soaked:**
one SmartElex DRV2605L on I2C1 — VCC/GND CN8 pins 4/7, SDA/SCL CN12 pins 9/10, **EN CN12 pin 1
(D8/PE7)**, **TRIG CN11 pin 7 (D6/PE13) → IN**, ERM coin motor on O−/O+.
**VL53L1X and MPU6050 still unsoldered.** Camera FFC out of CN14. CN4 empty.
**The 100 µF bulk capacitor is NOT fitted** — T3 step 5, still owed (§4).

**Last verdict — 10-minute soak, `docs/evidence/phase5/soak_10min_20260830.log`:**
```
570 heartbeats over 9.9 min · 265 motor fires · all 14 checks PASS
faults=0x0  cfglost=0  err/tmo/recov=0  canary_err=0  qovr=0  frames==inf on all 570 lines
effect 58539-58760 us · cyc_per_ms median 799999 · 47.6 Hz
```
Reproduce with `python3 docs/evidence/phase5/check_soak.py <log>`.

**BLOCK 1 IS COMPLETE.** Synthetic hazard → 2 µs edge on PE13 → DRV2605L → ERM buzz, every
constant traced to a measurement. Keep it working — it is the fallback demo.

---

## 2. WHAT HAPPENED TODAY (12 commits)

### 2.1 H-D8 — the bus was 11% out of spec. CLOSED.
ST's timing algorithm computes `tSCL = tSCL_L + tSCL_H + trise + tfall` and hit 2500 ns
exactly — but only 2150 ns of that is hardware; the rest was an **assumed** worst-case edge
time. Measured on the LA: **444.4 kHz against a 400 kHz limit on both the DRV2605L (SLOS854D
§6.6) and the VL53L1X (Table 7)**. Fixed by `I2C_Charac[I2C_SPEED_FREQ_FAST].freq`
400000 → 350000 in `i2c_timing.c`. Re-measured **381.0 kHz, and not one of 288 periods below
2500 ns**. Before/after captures archived.

**Lowering `I2C_BUS_HZ` does nothing** — `I2C_GetTiming()` uses it only to pick a speed bucket,
then targets the hardcoded `.freq`. Any value in [320000, 480000] gives a byte-identical
TIMINGR. Do not "fix" the bus clock there.

### 2.2 The pull-up budget was incomplete, and its mitigation insufficient.
CLAUDE.md §2 counted only SmartElex boards. Measured, boards disconnected:

| board | pull-up | tied to |
|---|---|---|
| 7SEMI VL53L1X | 2 × 9.9 kΩ | **VIN directly** |
| GY-521 MPU6050 | 2 × 2.15 kΩ | **its onboard LDO output, not VCC** |

The binding constraint is the weakest **driver**, and a chip counts whether or not its pull-ups
are fitted: **MPU6050 3 mA** vs DRV2605L and VL53L1X 4 mA. Result: **if the IMU ships, BOTH
2.2 kΩ pairs must come off** — the SmartElex jumper *and* two desoldered resistors on the
GY-521, which has no jumper. Either alone leaves ~3.55 mA against a 3 mA spec.
**If the IMU is dropped, nothing needs modifying at all.** Full table in CLAUDE.md §2.

Also corrected: the DRV2605L's `VIL` is an absolute **0.5 V** (SLOS854D §6.3), not the 0.99 V
generic 0.3×VDD this file used to quote. Tightest receiver threshold on the bus.

**Good news for Block 2:** the 7SEMI's pull-ups go to VIN, and its VIH range is 1.12–3.5 V, so
**3.3 V logic needs no level shifter** and the ToF can go on the bus with nothing modified.

### 2.3 OC_DETECT — a real hardware fault, caught by instrumentation added the same morning.
`faults=0x1` with every other indicator clean. Cause: two bare twisted joints where the motor's
enamelled leads met the jumper wires, ~1 cm apart, unsupported. **The short only happened while
the motor ran — its own vibration closed it.** `[EFF] late=3 stuck=1` identified it as the
shutdown-and-restart cycle rather than a hard short. Fixed by separating and offsetting them;
265 fires clean afterwards, coil undamaged. `docs/evidence/phase5/PHASE5_OC_DETECT_20260830.md`.

**Block 5 rule earned the hard way: every motor connection soldered, sleeved, offset so no two
conductors can meet, strain-relieved.** A courier will do far more than the ERM did.

### 2.4 Ledger closed today
**G-8** (CYCCNT clock = CPUCLK 800 MHz), **H-D2** (config retained across MCU reset, lost on a
true power cycle — which is why `drv2605l_init()` does DEV_RESET first), **H-D3** (effect 58.7 ms),
**H-D4** (Library B confirmed by bounding), **H-D6/V-W-6** (coil 32.5 Ω), **H-D7** (DEV_RESET
self-clears in <1 tick), **H-D8**, **H-D10**.

### 2.5 Firmware changes, all flashed and verified
DEV_RESET before configuration; OD_CLAMP replaces WAVSEQ1 in the pass condition; runtime health
poll at ~1 Hz (`faults_seen` latches OC_DETECT/OVER_TEMP, which clear on read); DWT enabled
unconditionally with a liveness check; edge trigger with R-3 enforced **inside** the driver;
HAP-T9 GO-bit timing; R-3 floor 125 → **75 ms**; urgency curve retuned (400 ms at the 20 cm/s
threshold, 75 ms floor at 100+).

**`ok` is now `24 + 2×polls` plus ~485 per HAP-T9 sample.** A stalled `ok` means a stalled
sensor task.

### 2.6 Tools now in the repo
`docs/evidence/phase5/analyze_scl.py` — BUS-2 SCL period from a sigrok CSV, auto-detects the
channel (the probe silkscreens CH1–CH8; the driver names them D0–D7, so **CH3 = D2**).
`docs/evidence/phase5/check_soak.py` — turns a soak log into a verdict over 14 checks. **Reuse
it for the Block 9 long soak**, which is submission evidence.

### 2.7 The ULD platform shim is WRITTEN, COMPILED AND LINKED
`firmware/Appli/Core/Src/app_vl53l1_port.c` + `Inc/app_vl53l1_port.h`, Apache-2.0. ST's stub is
renamed `vl53l1_platform.c.ST_STUB_UNUSED` with a README beside it; verified exactly one
compiled definition of each of the nine symbols, and the map file resolves them from
`app_vl53l1_port.o`.

A guard per trap: **parity check on `dev`** (8-bit 0x52 is even, the 7-bit 0x29 someone would
wrongly pass is odd — rejected outright); **`_Static_assert(I2C_REG16 != I2C_REG8)`** with no
literal `2` anywhere in the file; **one aligned bounce buffer** sized 32 from the ULD's real
worst case (`VL53L1X_api.c:593` reads 17), oversized counts refused not truncated, reads
pre-filled with the 0xA5 sentinel. Stats block gives `last_er`/`last_index`, because the ULD
collapses every failure into one `int8_t` ORed across dozens of calls.

**Plan v2 Block 2 step 10 is struck.** `firmware/Lib/` and `Appli/Lib/` are not duplicate trees:
the build uses both correctly, `Appli/Lib/`'s other directories are empty shells. Nothing to fix.

---

## 3. NEXT SESSION, IN ORDER

### B1 — Block 2, VL53L1X (plan v2, 1–4 Sep) ← highest technical risk
The bus is in spec, the shim is compiled, and the 7SEMI needs **no pull-up modification**.

1. Solder the 7SEMI. **XSHUT unconnected.** GPIO1 → **PD0** (CN11 pin 3), input, **no internal
   pull**, EXTI falling.
2. **L1 raw probe, before any shim code runs:** `i2c_rd(0x29, 0x010F, I2C_REG16, buf, 1)`,
   expect **`0xEA`**. First 16-bit-addressed transfer in the project's history. One variable at
   a time.
3. `vl53l1_port_reset_stats()`, then through the shim: `BootState` → `GetSensorId`
   (**log, don't hard-fail** — 0xEACC per UM2510 vs 0xEEAC in the in-repo header; only
   0x0000/0xFFFF/no-ACK is real failure) → `SensorInit` → `SetDistanceMode(1)` **before**
   `SetTimingBudgetInMs(15)` (15 ms exists only in short mode) → `SetInterMeasurementInMs(20)`
   (**the API does not check IMP ≥ budget — enforce it**) → `StartRanging`.
4. Frame loop: `CheckForDataReady` → `GetResult` (one 17-byte `ReadMulti`) → `ClearInterrupt`
   **every frame, mandatory**. Gate `d(t)` on `result.Status`.
5. **L5 — LA capture of one `WrByte`.** Wire bytes must read `0x52 idxMSB idxLSB data`. The only
   hardware proof that 16-bit addressing reaches the wire. Capture workflow and probe mapping
   are in §2.6. **Archive the `.sr`.**

### B2 — Cheap and still owed
- **V-W-1**, 60 seconds: black probe on a `GND` pin of CN8, red on `3V3`. The double-GND is the
  landmark — back one is 5V, back two is 3V3. Expect **3.25–3.35 V**. Also measure across the
  DRV2605L breakout's own VCC/GND; the difference is the wiring drop and closes H-D5 for free.
- **Fit the 100 µF** across 3V3/GND next to the DRV2605L, stripe to GND. Not urgent for the
  DRV2605L alone, owed before the sensors join that rail.

### B3 — Off the bench (developer is doing this 31 Aug)
**D1** — the two contest pages: deadline, submission format, **whether entry registration closes
before the work does**, shipping address and recipient. Also resolves `CLAUDE.md:4` (30 Sep) vs
`PROJECT_DEFENSE.md` (25 Sep). **Then start the customs proforma** — declared value, HS code.

### B4 — Decide before 9 Sep, information now complete
- **G-1** the ML label (currently a deterministic function of two of its own input features —
  shift it in time), then **G-2** on-device feature parity, **G-3** INT8 normalisation,
  **G-4** the collection protocol, **G-7** the frame-drop policy.
- **The IMU (schedule valve #2), due 6 Sep.** It now has a second, independent argument: with the
  MPU6050 you must desolder pull-ups on a new board; without it, nothing needs modifying and the
  12-feature fallback is already defined.

---

## 4. OPEN / UNVERIFIED — carry these forward

- **100 µF not fitted.** T3 step 5.
- **V-W-1 never done.** 60 seconds.
- **H-D5 deferred past Block 5** deliberately — it measures a breadboard that gets replaced.
- **V-W-7 is not a meter measurement** and never was. VOL is the level during a ~1.3 µs low
  phase on a bus idle-high >99.9% of the time; a DMM averages. Scope only. The pull-up
  arithmetic in §2.2 is what it was standing in for.
- **HSI accuracy `[UNVERIFIED]`.** All four PLLs source `RCC_PLLSOURCE_HSI` (`main.c:207`) —
  there is no crystal. Board time measured 0.57–0.75% slow against wall clock over 10 minutes,
  consistent with an RC oscillator. **`cyc_per_ms = 800000` is a RATIO** (CYCCNT and up_ms share
  the same HSI), not an absolute frequency. The 3.375 µs contest claim is LA-referenced and
  unaffected. Look up the spec before any absolute timing claim.
- **`mtkernel_bsp.c:31`** hand-declares `extern void tk_dly_tsk(int32_t)` and its own `SYSTIM`
  instead of including `tk/tkernel.h`; the real signature is `ER tk_dly_tsk(RELTIM)`. Works by
  accident on AAPCS, formally UB. **Deliberately not fixed** — working boot code, not on the
  Block 2 path.
- **`DRV_EFF_SAMPLES_MAX` should go to 0** for the Block 9 evidence build, so HAP-T9 does not
  perturb the PH6-3 preemption re-run.

---

## 5. WHAT NOT TO DO

- Do not lower `I2C_BUS_HZ` and believe the bus clock changed. It does not.
- Do not restore `vl53l1_platform.c` into the build, and do not fill ST's template in place.
- Do not solder the GY-521 without deciding the IMU question and the pull-up removals.
- Do not enable D-cache (D2).
- Do not touch `DLPF_CFG` or `AFS_SEL` once collection begins.
- Do not re-run the Phase 4 correctness soak — RZ4 is closed.
- Do not plug anything into CN4 or CN10, or reconnect the camera FFC to CN14.
- Do not claim on the printed card that an A-to-C cable will fail to boot the board. **It boots.**
- Do not quote `analyze_scl.py`'s "SCL freq max 400.0 kHz" line — that is 125 ns quantisation,
  not the bus touching the limit. Quote the median, 381 kHz.

---

## 6. OPENING PROMPT FOR THE NEXT SESSION

Paste from here down.

---

I'm continuing the Haptic-Sense build (TRON Forum Contest 2026, STM32N6570-DK, µT-Kernel 3.0).
Repo is at `~/haptic-sense`, branch `phase5/i2c-first-light` (= `master`, both at `64afd7b`).
Read `CLAUDE.md` first — locked architecture, hardware map with connector numbers, and the red
zones including RZ9 (GPDMA channel security attributes).

Read `docs/HANDOFF_20260831_BLOCK2.md` for state. It supersedes the 30 Aug handoff. Also in the
repo: `docs/PLAN_TO_SUBMISSION_20260830.md` (hardware ships 18 Sep, documentation until 30 Sep),
`docs/RISK_ANALYSIS_20260830.md`, `docs/BUS2_SCL_FREQUENCY_20260830.md`, and
`docs/evidence/phase5/`.

State: **Block 1 is complete and soaked** — synthetic hazard → 2 µs edge on PE13 → DRV2605L →
ERM buzz, 10 minutes and 265 motor fires with zero faults. The I2C bus is measured in spec at
381 kHz after being found 11% over. **The VL53L1X ULD platform shim is written, compiled and
linked** (`Core/Src/app_vl53l1_port.c`); ST's stub is renamed out of the build. The VL53L1X and
MPU6050 are not yet soldered.

Work through §3 of the handoff in order, starting with B1 — Block 2, the VL53L1X. The L1 raw
probe (`0x010F` → `0xEA`) comes before any shim code runs.

Be adversarial. Look for defects rather than summarising what looks fine. This project has now
found five silent-failure defects: a GPDMA channel with no security attributes that transferred
nothing while reporting success; a DMA buffer pre-filled with a value indistinguishable from a
real reading; a readback verification that could pass on configuration written by a previous
boot; an I2C clock running 11% over every device's limit with no error anywhere; and a pull-up
budget whose prescribed mitigation was insufficient. Assume there are more of that class.

Apply citation discipline. Every hardware specific — pin number, register address, connector
label, register value — must trace to a file in the repo, the local datasheet copies under
`docs/datasheets/`, or a live documentation lookup, and you name the source. Anything from
memory gets tagged `[UNVERIFIED]` with the verification step named. This project has already had
three cross-sensor spec contaminations; don't add a fourth.
