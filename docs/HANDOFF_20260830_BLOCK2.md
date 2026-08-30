# HAPTIC-SENSE — HANDOFF INTO BLOCK 2
**Written 2026-08-30 ~07:20 UTC at the end of the Block 1 session.**
Read `CLAUDE.md` first. Plan: `docs/PLAN_TO_SUBMISSION_20260830.md` (v2).
Risks: `docs/RISK_ANALYSIS_20260830.md`. This session: `docs/PHASE5_T0_T1_REVIEW_20260830.md`,
`docs/evidence/phase5/PHASE5_T1_CYCCNT_CLOCK_20260830.md`, `docs/BUS2_SCL_FREQUENCY_20260830.md`.

---

## 1. STATE

**BLOCK 1 IS COMPLETE.** The board detects a synthetic hazard and drives a real ERM through
the DRV2605L, end to end. Keep that capability working from here on — it is the fallback demo.

**Hardware on the bench:** one SmartElex DRV2605L on I2C1. VCC/GND CN8 pins 4/7, SDA/SCL
CN12 pins 9/10, **EN CN12 pin 1 (D8/PE7)**, **TRIG CN11 pin 7 (D6/PE13) → IN**, ERM on O−/O+,
100 µF across the 3V3 rail. IN→GND jumper removed. VL53L1X and MPU6050 not soldered.
Camera FFC unplugged from CN14. CN4 empty.

**Last serial (build `ab725a0`, verified over ~93 s):**
```
[I2C] init=0 gate=0 whoami=0x201e0 wr=0 wrseen=0x27 addr=0x5a ok=3005 err=0 tmo=0 recov=0
[CLK] cpu=800000000 sysb=400000000 pclk1=200000000
[DRV] init=0 id=7 mode=0x1 lib=0x2 seq=0x1 odc=0x8b sts=0xe0 armed=1
[TRG] pulses=28 suppressed=532 cyc=1600 rst=1 rstmode=0x40
[HLT] polls=88 faults=0x0 cfglost=0
[EFF] n=5 last=58605 min=58605 max=58708 late=0 stuck=0
[DWT] ok=1 cyc=1565876939 cyc_per_ms=800000
```

**Closed this session:** G-8 (CYCCNT clock), H-D2 (retention mechanism), H-D3 (effect
duration), H-D4 (library choice), H-D6 / V-W-6 (coil resistance), H-D7 (DEV_RESET self-clear).
**Reopened:** H-D8 (delivered SCL frequency). **Corrected:** G-10 (the A-to-C cable claim).

**Branch:** `master` and `phase5/i2c-first-light` both at `8691f9e`. Stray git lock files are
parked in `.git/_stale_locks/` — safe to `rm -rf`.

**BUILT AND FLASHED: `ab725a0`. NOT YET FLASHED: `f990135` and `8691f9e`** (`8691f9e` is
docs-only). `f990135` carries the R-3 floor 125 → 75 ms and the retuned urgency curve.
**First job is to build and flash it.**

---

## 2. WHAT CHANGED IN THE CODE

- **DEV_RESET before configuration.** The DRV2605L retains its registers across an MCU reset,
  so readback verification could pass on state a *previous* boot wrote while every write this
  boot silently failed. `drv2605l_init()` now resets and confirms MODE reads 0x40 first.
- **OD_CLAMP replaces WAVSEQ1 in the pass condition.** WAVSEQ1's expected 0x01 *is* its reset
  value, and `(seq_rb & 0x7F) == 0` was false for the 0xA5 sentinel too — it passed in every
  failure mode it existed to catch.
- **Runtime health poll**, ~1 Hz from TK_PRI 3. `faults_seen` latches OVER_TEMP / OC_DETECT,
  which clear on read and were previously unobservable. `ok` is now `24 + 2 × polls`.
- **DWT enabled unconditionally**, with a NOCYCCNT + two-read liveness check.
- **Edge trigger replaces the level.** `drv2605l_trig_fire()` with R-3 enforced inside the
  driver. `drv2605l_trig_set()` is gone — a level in edge mode fires once and then goes silent.
- **HAP-T9 in firmware**: times the GO bit against DWT, 5 samples, rides on hazard pulses that
  fire anyway. **Set `DRV_EFF_SAMPLES_MAX` to 0 before the Block 9 evidence run.**

---

## 3. TOMORROW, IN ORDER

### B0 — Flash `f990135` (first)
Expect **3 pulses per burst** instead of 2, `suppressed` dropping to match, and `[EFF]`
re-measuring to ~58.6 ms. A different `[EFF]` means the config path changed.

### B1 — BUS-2: capture SCL, before the VL53L1X is soldered
**H-D8 is reopened and this is the gate in front of Block 2.** Probe PH9 = CN12 pin 10,
ground CN8 pin 7, 24 MHz (~54 samples/period). Best target is the HAP-T9 poll: ~600
back-to-back transactions in 58 ms within 20 s of boot.
- **period ≥ 2500 ns (≤ 400 kHz)** ⇒ H-D8 closes, record the number, no change.
- **period < 2500 ns** ⇒ lower `I2C_Charac[I2C_SPEED_FREQ_FAST].freq` (NOT `I2C_BUS_HZ`, which
  is discarded — see `docs/BUS2_SCL_FREQUENCY_20260830.md` §5) and re-capture.
Archive the `.sr` either way.

### B2 — Bench checks before soldering anything, RE-RANKED 2026-08-30
- **Read the pull-up markings on the 7SEMI VL53L1X and the GY-521, and check for a jumper or
  solder bridge to disconnect them.** Highest value of anything in this group and it takes two
  minutes. CLAUDE.md §2's budget counts only SmartElex boards; adding these two puts the bus at
  559–697 Ω with the SmartElex jumper closed, which is over the 3 mA I2C budget and in three of
  four cases below the DRV2605L's own 660 Ω minimum. Opening one jumper may not be enough.
- **V-W-1** CN8 pin 4 → pin 7, expect 3.25–3.35 V. 60 seconds, worth having as a reference
  number, but it will not find anything — the rail demonstrably works. The failure mode that
  matters is sag under the ERM transient, which a DMM cannot see.
- **V-W-7 CANNOT BE DONE WITH A MULTIMETER.** VOL is the level during the ~1.3 µs low phase of
  a bus that is idle-high more than 99.9% of the time (two transactions per second). A DMM
  averages and will read close to VDD. It is a scope measurement — do it in lab, or rely on the
  pull-up arithmetic above, which is the thing the measurement was standing in for.
- **H-D5** breakout VDD — **defer past Block 5.** It measures breadboard contact quality on a
  breadboard that gets replaced by protoboard on 7–9 Sep. Re-open it after the rebuild, where
  the answer will still be true.

### B3 — Block 2, VL53L1X (plan v2, 1–4 Sep) ← highest technical risk
Fix the `firmware/Lib/` vs `firmware/Appli/Lib/` duplicate-tree split first. Then the ULD
platform shim — nine bodies currently `return 255`, already compiled in. Two lines carry the
risk: `dev7 = dev >> 1`, and `I2C_REG16` **as the symbol, never the literal 2**.

### B4 — Non-bench work, and it is overdue
- **D1** — the two contest pages. Deadline, submission format, **whether entry registration
  closes before the work does**, shipping address and recipient. Also resolves `CLAUDE.md:4`
  (30 Sep) vs `PROJECT_DEFENSE.md` (25 Sep).
- **Customs paperwork.** Proforma invoice, declared value, HS code.
- **G-1** — the ML label, before collection starts 9 Sep. Then G-2, G-3, G-4, G-7 with it.

---

## 4. WHAT NOT TO DO

- Do not solder the VL53L1X before BUS-2 (B1) — it has the same 400 kHz limit.
- Do not lower `I2C_BUS_HZ` and believe you have changed the bus clock. You have not.
- Do not enable D-cache (D2).
- Do not touch `DLPF_CFG` or `AFS_SEL` once collection begins.
- Do not re-run the Phase 4 correctness soak — RZ4 is closed.
- Do not plug anything into CN4 or CN10, or reconnect the camera FFC to CN14.
- Do not put a claim on the printed card that an A-to-C cable will fail to boot the board. It
  does boot, measured 2026-08-30.

---

## 5. OPENING PROMPT FOR THE NEXT SESSION

Paste from here down, with this file attached.

---

I'm continuing the Haptic-Sense build (TRON Forum Contest 2026, STM32N6570-DK, µT-Kernel 3.0).
Repo is at `~/haptic-sense`, branch `phase5/i2c-first-light` (= `master`, both at `8691f9e`).
Read `CLAUDE.md` first — locked architecture, hardware map with connector numbers, and the red
zones including RZ9 (GPDMA channel security attributes).

Attached is the handoff. Also in the repo: `docs/PLAN_TO_SUBMISSION_20260830.md` (hardware
ships 18 Sep, documentation until 30 Sep), `docs/RISK_ANALYSIS_20260830.md`, and
`docs/BUS2_SCL_FREQUENCY_20260830.md`.

State: Block 1 is complete — synthetic hazard → 2 µs edge on PE13 → DRV2605L → ERM buzz, with
the R-3 floor set from a measured 58.7 ms effect duration. CPUCLK is confirmed 800 MHz.
`f990135` is committed but **not yet built or flashed**.

Work through §3 of the handoff in order, starting with B0. B1 (the BUS-2 SCL capture) gates
Block 2 and must happen before the VL53L1X is soldered.

Be adversarial. Look for defects rather than summarising what looks fine. Three silent-failure
defects were found in the last week — a GPDMA channel with no security attributes that
transferred nothing while reporting success, a DMA destination buffer pre-filled with a value
indistinguishable from a real reading, and a readback verification that could pass on
configuration written by a previous boot. Assume there are more of that class.

Apply citation discipline. Every hardware specific — pin number, register address, connector
label, register value — must trace to a file in the repo, the local datasheet copies under
`docs/datasheets/`, or a live documentation lookup, and you name the source. Anything from
memory gets tagged `[UNVERIFIED]` with the verification step named. This project has already
had three cross-sensor spec contaminations; don't add a fourth.
