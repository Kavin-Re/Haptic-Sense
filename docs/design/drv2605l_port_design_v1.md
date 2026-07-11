# DRV2605L Haptic Driver Port — Design Document

Project: Haptic-Sense (TRON Forum Contest 2026)
Target: STM32N6570-DK, µT-Kernel 3.0, I2C1 (PH9=SCL / PC1=SDA, onboard 1.5 kΩ pull-ups)
Status: DESIGN — pre-implementation, pre-hardware
Primary source: TI SLOS854D (DRV2605L datasheet, May 2014 – rev. March 2018), in-repo copy.
Secondary source: `app_i2c.c` (Phase 5 L1 primitives, commit fc6ff88 lineage).

Citation convention: `(§x.y.z)`, `(Table N)`, `(Eq. N)`, `(Fig. N)` = SLOS854D unless
prefixed otherwise. Anything not traceable to a source is tagged `[UNVERIFIED]` with a
named verification step, collected in §9.

---

## 0. Anti-contamination header (read first)

This project has a documented history of cross-part spec contamination (VL53L5CX "88 KB
firmware" leaking into VL53L1X docs). The DRV2605L differs from the VL53L1X on exactly
the two axes most likely to be copy-pasted wrong:

| Property            | VL53L1X (ULD)                     | DRV2605L (this doc)                      |
|---------------------|-----------------------------------|------------------------------------------|
| Device address      | ULD API uses **8-bit** 0x52; shim right-shifts to 7-bit 0x29 | **7-bit 0x5A passed directly** to `dev7` — **no shift, no shim** (§8.5.3.1 NOTE: "slave address is 0x5A (7-bit)"; 0xB4/0xB5 are the 8-bit W/R forms, §8.5.3.1) |
| Register index size | 16-bit big-endian → `I2C_REG16`   | **8-bit → `I2C_REG8`** (single subaddress byte, §8.5.3.3, Fig. 21) |
| Multi-byte access   | index auto-increment              | sequential addressing supported for both R and W (§8.5.3.2) — usable but this design uses single-byte writes throughout for simplicity |

Bus compatibility: SCL max 400 kHz (§6.6); recommended pull-ups 660 Ω–4.7 kΩ (§8.5.3.1)
— onboard 1.5 kΩ is in range, **add nothing** (CLAUDE.md §2). VIH min 1.3 V (§6.3) —
3.3 V STM32 push-pull GPIO on EN/IN/TRIG is compliant; interface is 1.8-V compatible and
VDD-tolerant (§1).

---

## 1. Task split — the governing constraint

**Priority 1 (Hazard Alert) touches GPIO only. Priority 3 (Sensor Acquisition) owns every
I2C byte.** (CLAUDE.md §3 — any I2C call outside Priority 3 is a red-zone violation.)

| Actor | Owns | Mechanism |
|---|---|---|
| Priority 3, init phase | All DRV2605L register configuration, library select, sequencer pre-load, mode select, (optional) calibration | `i2c_wr(0x5A, reg, I2C_REG8, &val, 1)` |
| Priority 3, runtime (optional) | Re-tuning the pre-loaded effect ID at sensor cadence (urgency *intensity* band) | same primitive, outside the hazard path |
| Priority 1, runtime | **EN (PE7)**: emergency kill / enable. **IN/TRIG (pin TBD)**: fire pre-loaded effect via GPIO edge; pulse *rate* ∝ closing velocity | `HAL_GPIO_WritePin` only — zero I2C |

The DRV2605L was designed for exactly this split: "Using the external trigger pin has the
advantage that no I2C transaction is required to fire the pre-loaded effect" (§9.2.2.3);
external trigger suits "repeated firing of the same effect" (§8.3.5.6 NOTE).

---

## 2. EN pin semantics (Q1)

Facts, all from SLOS854D:

- EN gates the **shutdown** state — logic high = active, logic low = shutdown, the lowest
  power state (§8.4.1.3). Shutdown ≠ standby: standby is a *register bit* (0x01 bit 6,
  §8.4.1.4); the power-state diagram treats them as distinct states (Fig. 17).
- **Registers are NOT reset by EN low** (§8.4.1.3).
- **EN must be high to write I2C registers.** With EN low the device still ACKs, but "no
  read or write is possible" (§8.4.1.3) — a silent-failure mode: transactions look
  successful on the bus.
- In shutdown (and standby), output drivers present 15 kΩ to ground (§8.6.4, Table 7,
  HI_Z field description) — actuator is passively terminated, not floating.
- Shutdown current I(SD) = 4–7 µA (§6.5). Internal EN pull-down R(EN-GND) = 2 MΩ (§6.5)
  → before PE7 is configured (STM32 pins float at reset), EN reads low and the device
  sits in shutdown. Safe boot default.
- Startup latency: EN-high → output 1.5 ms (PWM/analog modes); GO/trigger → output
  0.7 ms (§6.7, t(start)).

### Design rules derived

- **R-EN-1:** EN (PE7) is asserted high once, during system init, *before* Priority 3
  issues any DRV2605L I2C write, and stays high in normal operation. (Consequence of the
  "no write with EN low" silent failure.)
- **R-EN-2:** Priority 1 may drive EN low at any time as an **emergency kill** — GPIO
  only, unconditional, effective regardless of any register state (§8.4.1.3 + 15 kΩ
  output termination).
- **R-EN-3:** Any EN-low event **invalidates the configured state** until Priority 3
  re-verifies and, if needed, re-initializes. Rationale: the datasheet is internally
  ambiguous about the state entered when EN rises with a retained STANDBY=0 — Fig. 17
  shows the EN=1 transition landing in Standby, while §8.4.1.4 says the STANDBY bit
  forces the state. `[UNVERIFIED]` — H-D2 below resolves it on hardware; R-EN-3 makes the
  design correct under either reading.

  **Mechanism (F-1, resolved 2026-07-12): readback-verify, never a flag.** An earlier
  draft of this rule had Priority 1 set a "config invalid" flag on kill and Priority 3
  clear it after re-init — a bare shared variable written by one task and cleared by
  another with no atomicity guarantee. The audit (F-1) correctly flagged this as a
  lost-update race: a kill landing in the exact window Priority 3 is mid-clear can be
  overwritten and silently dropped, leaving the DRV2605L unconfigured while the rest of
  the system still believes it's armed — for a hazard-alert wearable, a haptic that is
  silently dead is the worst-case failure. Fix: Priority 3 never learns about a kill
  through inter-task signaling at all. Register 0x01 (MODE/STANDBY) is ground truth;
  Priority 3 reads it back every sensor-loop iteration (throttle frequency TBD, §6.4) and
  compares against the expected armed encoding (MODE[2:0]==1, STANDBY==0). Any
  mismatch — EN-low kill, EN glitch, ESD, or anything else — triggers unconditional
  re-init via the §4 write sequence. This is **Option B** from the audit (the §6.4
  "optional hardening" idea, promoted here from optional to baseline): it sidesteps the
  P1→P3 signaling problem by not having one, so there is no cross-task shared state to
  race on and nothing to lose. Chosen over the counter-based alternative (Option A)
  because (a) it matches this codebase's existing ownership principle — `app_i2c.c:371`
  already documents "sensor_task is the sole caller, so no race exists to lose" for the
  I2C-completion handshake, and readback-verify extends the same sole-owner shape to
  config validity; (b) it adds zero new responsibility to Priority 1 beyond the GPIO
  write it already does under R-EN-2, keeping the GPIO-only / never-blocking / < 1 ms
  invariant untouched — a counter increment, even a single-instruction one, is still one
  more thing to reason about on the hazard path for no benefit here.
- **R-EN-4:** Priority 3's readback of register 0x01 (the R-EN-3 mechanism above) is the
  *only* authority on configured state. No boolean flag, counter, or other variable is
  shared between Priority 1 and Priority 3 for this purpose. Priority 1's responsibility
  for the kill path begins and ends at the EN GPIO write (R-EN-2) — it signals nothing
  and clears nothing.
- Answer to Q1: **yes, EN toggling by Priority 1 with zero I2C is safe**, under R-EN-1..4.

---

## 3. Trigger architecture (Q3)

### Why EN alone cannot fire haptics

Waveform playback is fired by the internal **GO signal**, sourced from either the GO bit
(register 0x0C — I2C, forbidden in Priority 1) or the **IN/TRIG pin** in external-trigger
modes (§8.4.3, Fig. 19). EN only gates power state. Therefore Priority 1 needs a GPIO
path to the GO signal → IN/TRIG.

### Option A — external edge trigger (RECOMMENDED)

- Priority 3 at init: load sequencer 0x04 = effect ID, 0x05 = 0x00 (zero terminator —
  playback stops at identifier zero, §8.6.5); set MODE[2:0] = 1, external edge trigger
  (§8.6.2, Table 5).
- Priority 1 at hazard: rising edge on IN/TRIG, **pulse width ≥ 1 µs** (§8.3.5.6.2).
  Falling edge does nothing, so a short pulse works without knowing waveform length
  (§8.3.5.6.2).
- **Urgency grading = pulse repetition rate**, timed entirely by Priority 1 GPIO logic:
  pulse rate ∝ closing velocity per the locked haptic semantics (CLAUDE.md §6). No I2C
  in the grading path.
- **Constraint T-1 (cancel semantics):** "An additional low-to-high transition while the
  GO bit is high also cancels the transaction" (§8.3.5.6.2). A re-trigger during playback
  cancels instead of restarting → at pulse periods shorter than the effect duration,
  every other edge cancels and the effective rate halves. **Minimum inter-pulse period
  must exceed the pre-loaded effect's duration.** Per-effect durations are not tabulated
  in SLOS854D → `[UNVERIFIED]`, H-D3.
- Baseline effect: **Strong Click – 100% = effect ID 1** (§12.1.2 Waveform Library
  Effects List). Short, unambiguous, library-portable (IDs 1–3 are Strong Click
  100/60/30%).
- Optional intensity dimension: Priority 3 may rewrite 0x04 with a different effect ID
  (e.g., ID 1 vs ID 2) at sensor-loop cadence based on urgency band. This is I2C, but it
  lives in Priority 3 and is *not* in the hazard-alert latency path — the alert path
  remains pure GPIO. Deferred; rate grading alone satisfies the locked semantics.

### Option B — EN-gated RTP buzz (fallback only)

MODE=5 (RTP) drives the actuator continuously at the amplitude in RTP_INPUT 0x02
(§8.5.8.2.1); EN would gate it on/off. Rejected as baseline because (a) it depends on the
EN-rise state ambiguity (R-EN-3 / H-D2), and (b) it makes continuous drive the *normal*
register state, weakening the §6 safety posture. Documented for completeness.

### Wiring requirement (NEW, blocking)

Option A requires IN/TRIG wired to a free STM32 GPIO (push-pull output, driven low at
init — never floating: in trigger modes IN/TRIG is a live trigger input, and the pin
table requires GND if unused (§5, Pin Functions)). Not in the locked pin map.
`[UNVERIFIED]` ×2 → H-D1: (a) confirm the Adafruit and SmartElex breakouts expose
IN/TRIG on a header pin; (b) allocate a free Arduino-header GPIO and add it to
CLAUDE.md §2.

Latency note for contest claims: the hardware-verified < 1 ms figure is CPU preemption
(D0→D1). End-to-end haptic onset adds t(start) 0.7 ms trigger→output (§6.7) plus ERM
mechanical rise time, 40–60 ms class for Library B (Table 1). Keep the two claims
separate in all contest material.

---

## 4. Init / configuration sequence (Q2)

Context: Priority 3 sensor task, after `app_i2c_init()`, after EN(PE7) driven high
(R-EN-1). All calls are the existing DMA primitive — no new I2C code paths.
`// ONLY CALL FROM PRIORITY 3 SENSOR TASK`

Precondition: **wait ≥ 250 µs after DRV2605L power-up before first I2C command**
(§9.3.1 step 1). Satisfied trivially by boot ordering; do not rely on it implicitly —
assert with a `tk_dly_tsk(1)` before the first write.

Baseline = **ERM open-loop**, per the datasheet's own recommendation for ROM libraries
(§9.3.1 step 7) and §5 below. Defaults are NOT relied on for mode-relevant registers
(see D-1 discrepancy, §6).

| # | Reg | Value | Purpose | Source |
|---|-----|-------|---------|--------|
| 0 (opt) | 0x01 | 0x80 | DEV_RESET — known-clean state; self-clears; poll 0x01 until bit7==0 (`i2c_rd`, `tk_dly_tsk(1)` between polls, ≤10 ms timeout `[UNVERIFIED]` — reset duration unspecified, H-D7) | §8.4.1.5, Table 5 |
| 1 | 0x01 | 0x00 | STANDBY=0, MODE=0 — device ready | §9.3.1 step 3, Table 5 |
| 2 | 0x17 | 0x8B | OD_CLAMP = 3.001 V — full-scale reference in open loop AND mode-independent peak clamp | Eq. 6, §8.5.2.2; calc §4.1 |
| 3 | 0x1A | 0x36 | N_ERM_LRA=0 (ERM); rest = datasheet defaults (FB_BRAKE_FACTOR=3, LOOP_GAIN=1, BEMF_GAIN=2) | Table 23 |
| 4 | 0x1D | 0xA0 | ERM_OPEN_LOOP=1; NG_THRESH=2 (4%); all else 0 — written explicitly, not assumed (D-1) | Table 26 |
| 5 | 0x03 | 0x02 | Library B — TS2200, 3 V rated / 3 V overdrive class | Table 7, Table 1; choice `[UNVERIFIED]`, H-D4 |
| 6 | 0x04 | 0x01 | Sequencer slot 1 = Strong Click 100% | §8.6.5, §12.1.2 |
| 7 | 0x05 | 0x00 | Zero terminator — playback stops after slot 1 | §8.6.5 |
| 8 | 0x01 | 0x01 | STANDBY=0, MODE[2:0]=1 — external edge trigger armed | Table 5 |

Call shape for every row: `i2c_wr(0x5Au, reg, I2C_REG8, &val, 1)` (app_i2c.c:399).
Post-init sanity read (recommended): `i2c_rd(0x5A, 0x00, I2C_REG8, ...)` — DEVICE_ID[2:0]
must read **7 = DRV2605L** (Table 4). This is the DRV2605L analogue of the VL53L1X
`GetSensorId == 0xEEAC` first-light check. Note DIAG_RESULT in the same register clears
on read (Table 4) — read 0x00 *before* any diagnostics interpretation, or account for it.

### 4.1 OD_CLAMP calculation (open loop, formula → substitution → result)

Eq. 6 (§8.5.2.1): V(ERM-OL_AV) = 21.59 × 10⁻³ × OD_CLAMP[7:0]

Target 3.0 V (ERM is 3 V class, CLAUDE.md §2):
OD_CLAMP = 3.0 / 0.02159 = 138.96 → **139 = 0x8B** → check: 139 × 0.02159 = **3.001 V**.

(Datasheet default 0x8C = 140 → 3.023 V; 0x8B chosen to stay ≤ 3.000 V.)

**Headroom flag:** output cannot exceed VDD; if VDD < clamp the clamp is unreachable
(§8.5.2.2 NOTE). On a 3.3 V rail the margin above 3.0 V is ~0.3 V. Breakout VDD rail is
`[UNVERIFIED]` — H-D5.

### 4.2 Closed-loop variant (only if chosen after H-D4 characterization)

Adds before calibration: 0x16 RATED_VOLTAGE = 0x8E — Eq. 4: V = 21.18×10⁻³ × N; 3.0 V →
N = 141.6 → 142 = 0x8E → 3.008 V (§8.5.2.1; "must be written before calibration",
§8.5.2.1). Then the §5 calibration procedure, then 0x1D with ERM_OPEN_LOOP=0.
Closed-loop is incompatible with Library A only; B–F are fine (§8.5.8.1.3 NOTE).

---

## 5. Auto-calibration (Q4)

**Required?** Only for closed-loop: "For closed-loop operation, the device must be
calibrated" (§8.5.5). Open loop ignores RATED_VOLTAGE and derives full scale from
OD_CLAMP (§8.5.2.1/§8.5.2.2); the calibration outputs — A_CAL_COMP 0x18, A_CAL_BEMF
0x19, BEMF_GAIN[1:0] in 0x1A (Fig. 25) — feed the back-EMF loop only. **Baseline
(open-loop) design: no calibration.**

**Decision for Kavin — loop mode:**
- **Option A (baseline): open loop.** Datasheet-recommended for ERM + ROM libraries
  (§9.3.1 step 7). No calibration, no per-unit state, fastest init. Cost: no automatic
  overdrive/braking → click sharpness depends on library time-offsets; rise/brake per
  Table 1 class.
- **Option B: closed loop.** Automatic overdrive and braking (§8.3.2.4) → crisper
  urgency pulses, unit-to-unit consistency across the 3 breakouts. Cost: one-time
  calibration per physical actuator + 3 stored constants + init rewrite.

**When it runs (if Option B):** once per physical actuator at bring-up — "TI recommends
that the calibration routine be run at least once for each actuator" (§8.5.6) — then
persist via §8.5.6 step 6(b): store results in host firmware and rewrite 0x18/0x19/
BEMF_GAIN at every init. The device also retains them through STANDBY and EN-low
(§8.5.6 step 6b), but rewriting removes any dependence on the H-D2 ambiguity.
**OTP (step 6c) is ruled out:** write-once, and programming requires VDD 4–4.4 V
(§8.5.7 step 2) — unreachable on a 3.3 V rail, and irreversible on shared breakouts.

**Procedure (Priority 3, bring-up only)** — §8.5.6 numbered list:
0x01←0x07 (exit standby + MODE=7); populate inputs: ERM_LRA=0, FB_BRAKE_FACTOR=2,
LOOP_GAIN=2, RATED_VOLTAGE=0x8E, OD_CLAMP=0x8B, AUTO_CAL_TIME=2, DRIVE_TIME per
§8.5.1.1, SAMPLE/BLANKING/IDISS/ZC_DET = LRA-only, ignored in ERM mode (§8.5.6, Fig. 25);
0x0C←0x01 (GO); poll 0x0C until bit0==0 via `i2c_rd` + `tk_dly_tsk(10)` — AUTO_CAL_TIME=2
bounds it at 500–700 ms (Table 27), timeout at 1000 ms; read 0x00, DIAG_RESULT bit3 must
be 0 = converged (Table 4; flag clears on read); read back 0x18, 0x19, 0x1A and record
the three values in CLAUDE.md.

---

## 6. Failure / safety analysis (Q5)

**Headline: the §8 ERM red zone (GPIO overcurrent damage) cannot be reopened by any
register write.** That failure mode was electrical — motor current through a GPIO pin.
The motor is connected only to OUT+/OUT− through the DRV2605L's protected output stage;
no register configuration routes actuator current anywhere else. The residual *software*
risk is unintended continuous vibration, analysed exhaustively below.

### 6.1 Every register path to sustained drive

| Path | Mechanism | Bound |
|---|---|---|
| MODE=5 (RTP) | drives continuously at RTP_INPUT amplitude (§8.5.8.2.1) | unbounded in time; bounded in amplitude by OD_CLAMP |
| MODE=3 (PWM/analog) | drives continuously from IN/TRIG signal (§8.3.5.1); floating IN/TRIG = noise-driven, mitigated by noise gate NG_THRESH (§8.3.5.7) but not eliminated | unbounded in time; amplitude ≤ OD_CLAMP |
| MODE=2 (level trigger), IN/TRIG stuck high | GO follows pin (§8.3.5.6.3) | bounded by waveform length — sequencer stops at zero terminator or 8 slots (§8.6.5) |
| MODE=1 (our mode), IN/TRIG stuck high | edge-triggered: one playback per rising edge (§8.3.5.6.2) | bounded — single effect duration |
| Sequencer misload (8 long effects, WAIT bits) | WAIT delay max = 127×10 ms per slot (§8.6.5) | bounded: 8 × (longest effect + 1.27 s) |

Only MODE=5 and MODE=3 permit *unbounded* drive, and this design never writes either
value. A corrupted write reaching 0x01 is the residual scenario.

### 6.2 Independent bounds that hold regardless of misconfiguration

1. **OD_CLAMP is mode-independent:** "always represents the maximum peak voltage that
   is allowed, regardless of the mode" (§8.5.2.2). At 0x8B the worst-case drive is
   3.0 V into a 3 V-class ERM — within actuator rating. Continuous worst-case = battery
   drain and wear, not damage.
2. **EN low = unconditional GPIO kill (R-EN-2):** shutdown, outputs 15 kΩ to GND
   (§8.4.1.3, Table 7). No register state overrides the pin. **Priority 1 always holds a
   working off-switch — this is the safety invariant answering Q5.**
3. **Safe power-on defaults:** 0x01 default 0x40 → STANDBY=1 asserted at reset (Table 5);
   EN internal 2 MΩ pull-down (§6.5) → shutdown until PE7 is deliberately driven. The
   device cannot self-start.
4. **Device-internal protections:** overcurrent latch OC_DETECT + shutdown, load
   threshold 4 Ω (§8.3.12.2, §6.5 ZL(th)); thermal shutdown + OVER_TEMP flag
   (§8.3.12.1); brownout reset (§8.3.12.4).
5. **I2C watchdog:** a hung transaction resets the I2C protocol after 4.33 ms — in all
   states *except standby*, where only a power cycle recovers I2C (§8.3.11). Interacts
   safely with the existing L1 timeout/recovery in `app_i2c.c` (i2c_xfer retry + bus
   recovery, app_i2c.c:357-392).

### 6.3 Documented datasheet discrepancy (D-1)

§9.3.1 step 7 prose: "The default setup is closed-loop bidirectional mode." Register map
Table 26: 0x1D default 0xA0 → ERM_OPEN_LOOP=1 (open loop). These conflict for ERM.
Resolution: **never rely on defaults for mode-relevant registers** — rows 3–4 of the §4
table write 0x1A and 0x1D explicitly. (BIDIR_INPUT default 1, Table 25, is consistent
with the prose's "bidirectional"; the open/closed-loop half is the conflicting part.)

### 6.4 Readback-verify — R-EN-3/R-EN-4 mechanism, BASELINE (promoted 2026-07-12, F-1)

Priority 3 periodic cross-check: `i2c_rd(0x5A, 0x01, ...)` and assert MODE[2:0]==1 &&
STANDBY==0; on mismatch, re-run the §4 write sequence unconditionally. This is no longer
optional hardening — it is the sole mechanism implementing R-EN-3/R-EN-4 (§2), replacing
the earlier flag-based kill-signaling design that the audit found race-prone (F-1). Cheap
(one 1-byte read) but adds steady-state bus traffic to the 50 Hz pipeline; still open is
only the *frequency* — every sensor-loop iteration vs a throttled subset (e.g. once per
second) — decide after ToF/IMU timing budget is measured (§10 item 3).

---

## 7. Runtime interaction summary

```
BOOT        PE7(EN) low (2 MΩ pulldown) → DRV2605L in shutdown, outputs 15 kΩ–GND
INIT (P3)   drive EN high → tk_dly_tsk(1) [≥250 µs, §9.3.1] → §4 write sequence
            → read 0x00, expect DEVICE_ID=7 → haptics ARMED
HAZARD (P1) rising edge ≥1 µs on IN/TRIG → effect plays (t(start) 0.7 ms, §6.7)
            pulse rate ∝ closing velocity; inter-pulse period > effect duration (T-1)
KILL (P1)   EN low (GPIO only, R-EN-2) → guaranteed off (any register state).
            No flag set — P1's responsibility ends at the GPIO write (R-EN-4).
VERIFY (P3) every sensor-loop iteration (frequency TBD, §6.4): i2c_rd(0x5A, 0x01) →
            compare MODE[2:0]==1 && STANDBY==0. Match → no action. Mismatch (kill, EN
            glitch, or any other cause) → EN high → re-run §4 rows 1–8 → next readback
            confirms ARMED. (R-EN-3 mechanism; F-1 resolved 2026-07-12 — readback-verify
            replaces flag-based signaling, eliminating the lost-update race.)
```

---

## 8. Licensing / file placement

New files `app_drv2605l.c/.h` — own code, Apache-2.0 SPDX header (CLAUDE.md §7). All
register constants derived from SLOS854D with section citations in comments; no TI code
copied. Every public function commented `// ONLY CALL FROM PRIORITY 3 SENSOR TASK`
except the two Priority 1 GPIO helpers (EN kill, IN/TRIG pulse), which must contain no
I2C and carry the inverse comment.

---

## 9. Hardware-gated verification items (H-D series)

| # | Item | Verification step |
|---|------|-------------------|
| H-D1 | **IN/TRIG wiring** `[UNVERIFIED]`: (a) do the Adafruit and SmartElex breakouts expose IN/TRIG on a header pin? (b) which free Arduino-header GPIO hosts it? | Physical inspection of both breakout silkscreens + Adafruit/SmartElex schematics; then pin-map update in CLAUDE.md §2. **Blocking for Option A.** |
| H-D2 | EN-rise state `[UNVERIFIED]`: with registers retained (MODE=1, STANDBY=0), does EN low→high return the device to armed external-trigger operation without I2C? (Fig. 17 vs §8.4.1.4 ambiguity) | Bench test: configure, pulse IN/TRIG (confirm click), EN low 100 ms, EN high, pulse IN/TRIG. Click without re-init ⇒ resumes; silence ⇒ R-EN-3 re-init is mandatory, not just defensive. |
| H-D3 | Effect duration `[UNVERIFIED]` (not tabulated in SLOS854D): duration of effect ID 1 in Library B on this ERM → sets minimum inter-pulse period (T-1) | Logic analyzer on OUT+ (or scope with §7.1 RC filter, 100 kΩ/470 pF, Fig. 11) — measure trigger-to-quiescent time; set min pulse period = duration + margin. |
| H-D4 | Library choice `[UNVERIFIED]` (B assumed from 3 V rating): measured rise/brake vs Table 1 classes B/C/D | Same capture as H-D3; if rise > 80 ms pick C/D per Table 1. Also drives the open-vs-closed-loop decision (§5 options). |
| H-D5 | Breakout VDD rail `[UNVERIFIED]`: 3.3 V assumed; OD_CLAMP headroom check (§4.1) and OTP infeasibility claim (§5) both reference it | Read breakout schematic; measure VDD pin. If 5 V rail available, headroom concern vanishes (OD_CLAMP math unchanged). |
| H-D6 | ERM coil impedance vs 4 Ω OC threshold `[UNVERIFIED]` (ZL(th), §6.5): must be > 4 Ω or the driver latches OC_DETECT | Multimeter across motor terminals before wiring. |
| H-D7 | DEV_RESET self-clear time `[UNVERIFIED]` (unspecified in SLOS854D) | Poll-loop instrumentation on first hardware run; record for CLAUDE.md. |
| H-D8 | I2C1 SCL frequency ≤ 400 kHz (§6.6) with DRV2605L on the shared bus | Confirm timing constant in `i2c_timing.h` / logic-analyzer capture of SCL. Presumed already met for VL53L1X (also 400 kHz class `[UNVERIFIED]` for that part — check its own datasheet, not this one). |

---

## 10. Decisions requiring Kavin's sign-off

1. **Loop mode:** Option A open-loop (baseline, no calibration) vs Option B closed-loop
   (calibration + stored constants). Recommendation: A for first light; revisit after
   H-D3/H-D4 if click sharpness is inadequate.
2. **IN/TRIG GPIO allocation** (H-D1b) — new pin-map entry.
3. **Readback-verify polling frequency** (§6.4, mechanism itself now baseline per F-1) —
   every sensor-loop iteration vs a throttled subset; decide once 50 Hz pipeline timing
   is measured.
