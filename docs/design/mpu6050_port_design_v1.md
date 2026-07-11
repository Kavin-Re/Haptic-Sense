# PHASE5_L2 — MPU6050 IMU Port Design (v1)

Haptic-Sense · TRON Forum Contest 2026 · Review pass 2026-07-09 (chat/Opus)

**Review inputs (this pass):** `app_i2c.c` (uploaded, verified byte-identical role to prior pass), **RM-MPU-6000A-00 rev 4.0** (Register Map, 2012-03-09) — cited `{RM §x.y}`, **PS-MPU-6000A-00 rev 3.4** (Product Specification, 2013-08-19) — cited `{PS §x.y}`. All hardware claims below trace to these two uploads unless tagged `[UNVERIFIED → V-n]` (ledger in §8).
**NOT available:** `app_i2c.h` / `i2c_timing.h` (same gaps as VL53L1X pass), GY-521 breakout schematic (no official one exists — board-level facts are hardware-gated by design).

**Convention:** identical to the VL53L1X doc — `[V-n]` verification steps, sources in braces, options as A/B for the developer's decision.

---

## 0. CROSS-SENSOR CONTAMINATION GUARDS (read first — this is the stated failure mode)

The MPU6050 and VL53L1X share the bus and the primitive but differ on exactly the axes where habit transplants a bug:

| Property | VL53L1X (prior doc) | MPU6050 (this doc) | Contamination failure |
|---|---|---|---|
| Register index width | 16-bit, `I2C_REG16` | **8-bit, `I2C_REG8`** — the protocol sends a single register-address byte `{PS §9.3: master "puts the register address (RA) on the bus" as one byte; RM §3: all addresses 0x0D–0x75}` | Passing `I2C_REG16` emits a phantom high byte; the device treats it as the index and the real index as data — silent garbage |
| `dev` parameter convention | ULD hands the shim an **8-bit** address (0x52); shim shifts ≫1 | Docs speak **7-bit** natively: 1101000/1101001 `{PS §6.4}` — **pass 0x68/0x69 straight into `i2c_rd/i2c_wr`, NO shift** | Applying the VL53L1X ≫1 habit gives 0x34 — NACK on every transaction |
| Multi-byte data order | Big-endian (MSB at lower address) | **Same** — `_H` before `_L` `{RM §3 note: "Register Names ending in _H and _L contain the high and low bytes, respectively"}` | None — same reassembly pattern, keep it |
| Identity check | RdWord 0x010F = 0xEACC | RdByte **0x75 = 0x68** `{RM §4.34: "The default value of the register is 0x68"}` | The value 0x68 coincidentally equals the 7-bit address — it is the register content, not an echo of the address |

**Closure of a standing `[UNVERIFIED]`:** the gate-test comment in `app_i2c.c:421-424` ("WHO_AM_I=0x75 / expected 0x68: recalled from RM-MPU-6000A, UNVERIFIED") is now **VERIFIED** against the uploaded RM rev 4.0: register 117 (0x75) is WHO_AM_I, default 0x68, `WHO_AM_I[6:1]` = upper 6 bits of the 7-bit address, bits 0 and 7 hard-coded 0, **AD0 not reflected in this register** `{RM §4.34}`. Update the comment; the gate's expected byte stands.

---

## 1. ADDRESS RESOLUTION (item 1)

- The 7-bit address is `b110100X`; the LSB is the **logic level on pin AD0**: AD0 low → **0x68** (`1101000`), AD0 high → **0x69** (`1101001`) `{PS §6.4 I2C ADDRESS table; §9.2}`. Two devices may share a bus this way `{PS §9.2}`.
- **The GY-521's AD0 strap state is board-level and undocumented** — commonly pulled low by an onboard resistor, but that is memory, not source `[UNVERIFIED → V-1]`. The bus scan IS the verification step, and it is **already implemented**: `app_i2c_gate_test()` (app_i2c.c:427-443) tries 0x68 then 0x69, reads register 0x75, and records both the ACKing address (`stats.gate_addr`) and the WHO_AM_I byte (`stats.gate_whoami`).
- **Pass criterion:** exactly one of {0x68, 0x69} ACKs AND `gate_whoami == 0x68` `{RM §4.34}`. Both ACKing would mean an address conflict (or a second device); neither means wiring/power.
- **Driver handling:** probe once at init (reuse the gate-test loop), latch the winner into a file-static `mpu_addr7`, and use it for every subsequent call. Never hard-code either value; fail init loudly (E_NOENT-style) if the probe finds nothing — do not fall back to a guess. **If AD0 is floating rather than strapped** (possible on clone boards), the address can be nondeterministic — the fix is to wire AD0 to GND explicitly `[UNVERIFIED whether your unit straps it → V-1 covers this: if the scan result is unstable across power cycles, strap AD0]`.
- WHO_AM_I cannot distinguish 0x68 from 0x69 (AD0 not reflected `{RM §4.34}`) — the distinguishing evidence is *which address ACKed*, the whoami byte only confirms the part family.

---

## 2. REGISTER-READ DESIGN — byte order and the one-transaction burst (item 2)

### 2.1 Byte order — confirmed big-endian
`ACCEL_XOUT_H` (0x3B) holds `ACCEL_XOUT[15:8]`, `ACCEL_XOUT_L` (0x3C) holds `[7:0]` `{RM §4.18 register table}`; the general rule is stated at `{RM §3 note}`. Values are **16-bit 2's complement** `{RM §4.18, §4.20 Parameters}`. On the little-endian M55, reassemble explicitly — same rule as VL53L1X doc §3, no memcpy:

```c
int16_t v = (int16_t)(((uint16_t)buf[H] << 8) | buf[L]);
```

### 2.2 The burst is not just an optimization — it is a correctness requirement
The sensor registers are double-banked: an internal set updates at the Sample Rate, and the user-facing set duplicates it only while the serial interface is idle, which **"guarantees that a burst read of sensor registers will read measurements from the same sampling instant"**; with single-byte reads *the user* must guarantee coherence via the Data Ready interrupt `{RM §4.18, §4.19, §4.20 — stated identically for accel, temp, gyro}`. So six single reads could tear a sample across two instants; one burst cannot.

### 2.3 The transaction
The output registers are contiguous: accel 0x3B–0x40, temp 0x41–0x42, gyro 0x43–0x48 `{RM §3}` — **14 bytes from 0x3B in one repeated-start read** (burst reads supported, `{PS §9.3 Burst Read Sequence}`):

```c
/* sensor_task, TK_PRI 3 — ONLY CALL FROM PRIORITY 3 SENSOR TASK */
static UB imu_buf[32] __attribute__((aligned(32)));   /* 14 used; 32-B aligned+padded per design §4.1 rule */

err = i2c_rd(mpu_addr7, 0x3Bu, I2C_REG8, imu_buf, 14);
/* buf[0..5]=ax,ay,az  buf[6..7]=temp  buf[8..13]=gx,gy,gz — each MSB-first */
```

Reading 14 (including temp) rather than two 6-byte reads costs 2 wasted bytes and buys one transaction, one semaphore round-trip, and the same-instant guarantee across accel AND gyro. Die temperature comes free if ever wanted: `°C = raw/340 + 36.53` `{RM §4.19}`.

**HAL Mem path with `I2C_MEMADD_SIZE_8BIT` emits exactly the protocol the MPU6050 expects** (single RA byte, repeated start, auto-incrementing burst) — the 8-bit branch needs no byte-order care at all; H1-class analysis from the VL53L1X doc applies only to the 16-bit branch. What remains symbol-hygiene: **pass `I2C_REG8`, never a literal** (same rule class as VL53L1X doc §1.3; `app_i2c.h` still unuploaded `[→ V-2]`).

---

## 3. INIT SEQUENCE (item 3) — minimum WrByte chain

**The device powers up asleep** — `{RM §4 note: "The device will come up in sleep mode upon power-up"}`, PWR_MGMT_1 reset value 0x40 = SLEEP set `{RM §3 reset-values note}`. Nothing streams until SLEEP is cleared. Every step below is `i2c_wr(mpu_addr7, reg, I2C_REG8, &v, 1)` from sensor_task.

| # | Reg | Value | Effect | Source |
|---|---|---|---|---|
| 0 | 0x75 read | expect 0x68 | identity gate before any write | {RM §4.34} |
| 1 *(opt)* | 0x6B | 0x80 | DEVICE_RESET — all registers to defaults; **bit self-clears when done** → poll 0x6B until bit7==0, **bounded** (closes audit M-2, 2026-07-11): ≤5 attempts, `tk_dly_tsk(1)` between each `i2c_rd`; exhaustion = fail init loudly (E_NOENT-style), never proceed silently — same "fail loudly, never guess" discipline as the §1 address probe. Matches the bounded-poll pattern already used for DRV2605L row 0 and the VL53L1X boot gate. Rationale: an unbounded/unpaced poll can permanently livelock the pipeline if the device never clears (unpowered/wedged), and each failed `i2c_rd` costs ~145 ms through the primitive's retry+recovery envelope | {RM §4.30 Parameters} |
| 2 | 0x6B | **0x01** | SLEEP=0, CYCLE=0, TEMP_DIS=0, CLKSEL=1 = PLL w/ X-gyro reference — "highly recommended… for improved stability" over the internal oscillator | {RM §4.30 + CLKSEL table} |
| 3 | — | wait | PLL settling 1–10 ms {PS §6.6}; gyro ZRO settling **30 ms** {PS §6.1}; accel path wake-up ≥4 ms {RM §4.28}. One `tk_dly_tsk(50)` covers all three with margin |  |
| 4 | 0x1A | DLPF_CFG (opt. A/B below) | DLPF on ⇒ gyro output rate = **1 kHz** (prerequisite for step 5's arithmetic) | {RM §4.2, §4.3} |
| 5 | 0x19 | **19** | Sample Rate = Gyro Output Rate / (1 + SMPLRT_DIV) = 1000/(1+19) = **50 Hz** — exact match to the locked pipeline | {RM §4.2} |
| 6 | 0x1B | 0x00 | FS_SEL=0, ±250 dps (bits [4:3]) | {RM §4.4} |
| 7 | 0x1C | AFS_SEL (opt. C/D below, bits [4:3]) | accel full-scale | {RM §4.5} |
| 8 | 0x37 | 0x30 | INT_LEVEL=0 (active-high), INT_OPEN=0 (push-pull), **LATCH_INT_EN=1** (bit5: held until cleared), **INT_RD_CLEAR=1** (bit4: any read clears) | {RM §4.15} |
| 9 | 0x38 | 0x01 | DATA_RDY_EN — interrupt "each time a write operation to all of the sensor registers has been completed" ⇒ fires at the 50 Hz sample rate | {RM §4.16} |
| 10 | 0x19–0x1C, 0x37, 0x38 read-back | == written | one 1-byte read per config register; catches any silently-failed write before first-light data is trusted |  |

Notes: write order is wake-first (step 2 before 4–9) — the RM does not state whether config writes land during sleep, so don't rely on it. Gyro stays powered even though the feature vector uses no gyro features (CLAUDE.md §6): **CLKSEL=1 uses the X gyro as the clock reference** `{RM §4.30}`; putting gyro axes in standby would silently fall back to the ±5%-tolerance internal oscillator (`{PS §6.6}` CLK_SEL=0 initial tolerance) and drag the 50 Hz timebase with it. The `{RM §4.31}` note confirms standby-ing the clocking axis auto-switches to the 8 MHz oscillator.

**Option A vs B — DLPF (step 4), the anti-aliasing decision at 50 Hz sampling (Nyquist 25 Hz):**
- **A (recommended): DLPF_CFG=4** → accel BW 21 Hz / 8.5 ms delay, gyro 20 Hz / 8.3 ms `{RM §4.3 table}`. Fully below Nyquist — no aliasing into the feature vector; the 8.5 ms group delay is well inside the 20 ms frame and constant (the ML model sees a consistent shift).
- **B: DLPF_CFG=3** → 44/42 Hz BW, 4.9 ms delay `{RM §4.3}`. Crisper transients for impact-like signatures, but content between 25–44 Hz aliases. Choose only if training data will be collected with the identical setting.

**Option C vs D — accel full-scale (step 7):**
- **C (recommended): AFS_SEL=1, ±4 g, 8192 LSB/g** `{RM §4.18 / PS §6.2}` → walking/impact transients at torso height can exceed ±2 g; clipping corrupts the feature vector worse than the halved resolution (0.122 mg/LSB is far below sensor noise anyway, PSD 400 µg/√Hz `{PS §6.2}`).
- **D: AFS_SEL=0, ±2 g, 16384 LSB/g** — finest resolution; acceptable only if recorded wear data shows no saturation. The range is trainable-data-coupled: **lock it before Edge Impulse data collection and never change it after** (a range change rescales every raw count).

---

## 4. INT ON PE9 — poll vs interrupt at 50 Hz (item 4)

Electrical: with step 8's config, INT is push-pull, active-high, latched until cleared, and any register read clears it (INT_RD_CLEAR=1 ⇒ the 14-byte burst itself is the clear) `{RM §4.15}`. PE9 = plain input, `GPIO_NOPULL` (push-pull driver needs no pull). Logic level: INT high = 0.9×VLOGIC min `{PS §6.4}`; **the GY-521's VLOGIC net wiring is board-level and unverified** — it must sit at 3.3 V for PE9 compatibility `[UNVERIFIED → V-3: meter/LA on the INT pin at first light; expect clean 0/3.3 V swings]`. Also note `{RM §4.29 / PS §10}`: MPU-6050 requires `I2C_IF_DIS=0` (reset default — no action, listed for completeness).

**Option A (recommended for bring-up): latched-level poll, no EXTI.** Sensor task runs its existing 20 ms cadence (`tk_dly_tsk`/cyclic pattern); each frame: `HAL_GPIO_ReadPin(PE9)` — if high, burst-read (which clears the latch `{RM §4.15 INT_RD_CLEAR}`); if low, mark IMU frame stale and reuse the last sample (the 12-feature fallback path in CLAUDE.md §6 already anticipates IMU degradation). Zero new ISR surface, zero new kernel objects, and the latch means a data-ready that arrived *between* polls is never missed — level, not edge, is what makes polling safe here.
- Cost: up to one frame of added latency on the IMU path (bounded 20 ms) and the two clock domains (MPU's ±1% `{PS §6.6 CLK_SEL=1,2,3}` vs kernel tick) beat against each other — occasionally a poll finds no fresh sample or two samples' worth elapsed. At 50 Hz vs 50 Hz nominal this is a ~1%-of-frames effect; the stale-frame flag handles it.

**Option B: EXTI on PE9 → `tk_sig_sem(imu_data_ready_sem)`.** The MPU becomes the pipeline timebase; jitter collapses to interrupt latency. **Standardized 2026-07-11 (audit M-3): `tk_sig_sem`, not `tk_wup_tsk`** — count-carrying semantics mean a data-ready event that fires while sensor_task is mid-frame is never lost (a `tk_wup_tsk` wake-count can saturate/collapse under the same condition), and this matches the VL53L1X production EXTI path (`vl53l1x_port_design.md` §6.1: PD0 handler signals a semaphore that sensor_task waits on) and the project's paired-semaphore architecture (CLAUDE.md §3). Constraints if chosen: the handler is registered via `tk_def_int(TA_HLNG)` + `EnableInt` at level 1..15 exactly like the four I2C IRQs (app_i2c.c:36-39, 219-237), does **nothing but** `tk_sig_sem` — any I2C from the handler is a red-zone violation, kernel-legal level, otherwise identical to the prior design — and the ToF read then rides the IMU's clock, which slightly complicates the VL53L1X's own intermeasurement cadence bookkeeping.
- Recommendation: A for first light and the soak; migrate to B only if the logic-analyzer timing campaign (H3-class evidence) shows the poll beat-frequency actually degrading the feature vector. The decision is reversible in one function.

---

## 5. RAW → mg SCALING (item 5)

Sensitivity table `{RM §4.18 / PS §6.2, identical}`: AFS_SEL 0/1/2/3 → 16384 / 8192 / 4096 / 2048 LSB/g. Output is 16-bit 2's complement `{RM §4.18}`.

For the recommended **±4 g (AFS_SEL=1)**:

```
mg = raw × 1000 / 8192
```

Integer form for TK_PRI 3 (no float dependency): `mg = ((int32_t)raw * 1000) / 8192`. Exact-ish check: worst case |raw|=32768 → |raw×1000| = 32,768,000 — fits int32 with 65× headroom; truncation error < 1 mg (< 1 LSB-equivalent). For ±2 g substitute 16384. **Rest-state sanity criterion** (doubles as first-light pass): flat on the bench, X/Y ≈ 0 mg, **Z ≈ +1000 mg** `{PS §7.8: "When the device is placed on a flat surface, it will measure 0g on the X- and Y-axes and +1g on the Z-axis"}` — within initial calibration tolerance ±50 mg X/Y, ±80 mg Z `{PS §6.2 Zero-G Output}`. Gyro at rest: within ±20 dps initial ZRO tolerance `{PS §6.1}`.

---

## 6. FIRST-LIGHT SEQUENCE (MPU6050)

- **F0** — wiring: GY-521 VCC/GND/SDA/SCL/INT(PE9). Which rail feeds VCC is board-gated: GY-521 carries an onboard regulator on most variants `[UNVERIFIED → V-4: identify the regulator/jumper on your physical unit; if regulator present, 5 V or 3.3 V input both land at 3.3 V logic — confirm VLOGIC per V-3]`. AD0 per §1.
- **F1** — bus scan: existing `app_i2c_gate_test()`; pass = one address ACKs, whoami 0x68 (§1). This simultaneously closes the CLAUDE.md §2 "verify AD0 by bus scan" item.
- **F2** — init chain §3 with per-register read-back (step 10).
- **F3** — rest-state burst: 14-byte read, reassemble, check §5 criteria (Z≈+1000 mg, gyro ≈0).
- **F4** — INT: LA or scope on PE9 — expect 50 Hz rising edges, each cleared by the following burst read (latched-high intervals ≈ poll latency). Closes V-3 and validates SMPLRT_DIV arithmetic on hardware.
- **F5** — dynamic: hand-shake the board; ax/ay/az track motion, saturation check at the chosen range (Option C/D revisit point).

## 7. WHAT THIS DOC DOES *NOT* COVER
Sensor-fusion timing between ToF and IMU inside the 20 ms frame (belongs to the pipeline-integration doc once both sensors have first light); MPU6050 motion-interrupt/DMP features (DMP explicitly out of scope — NPU does the inference, and DMP would add an undocumented firmware dependency); temp compensation.

**Binding constraint recorded, not designed (audit M-4, 2026-07-11):** §4 Option A's
stale-IMU-frame indicator ("if INT low, mark frame stale, reuse last sample, fall to
12-feature vector") currently has no specified transport across P3→P2 — that handoff is
Phase 6 scope, not this doc's. The constraint Phase 6 inherits: **the staleness
indicator MUST travel inside the semaphore-protected P3→P2 feature-frame buffer (e.g. a
validity field in the frame struct), never as a bare cross-task flag** — a bare flag
would be an F-1-class unprotected shared-state race (see `drv2605l_port_design_v1.md`
§2, R-EN-3/R-EN-4: the same lost-update shape, config-invalid flag vs readback-verify).
Closes M-4 together with F-6d at Phase 6 handoff design.

## 8. VERIFICATION LEDGER

| ID | Claim gated | Verification step | Type |
|---|---|---|---|
| V-1 | GY-521 AD0 strap state (→ 0x68 vs 0x69) | F1 bus scan via existing gate test; if unstable across power cycles, strap AD0 to GND | hardware |
| V-2 | `I2C_REG8` symbol value (moot if symbol passed, §2.3) | read `app_i2c.h` | desk |
| V-3 | GY-521 VLOGIC = 3.3 V; INT swings 0/3.3 V into PE9 | meter/LA at F4 | hardware |
| V-4 | GY-521 regulator presence → correct VCC rail | physical inspection of the unit | hardware |
| V-5 | `I2C_BUS_HZ` ≤ 400 kHz — MPU6050 fast-mode max `{PS §6.4}`, same ceiling as VL53L1X | read `i2c_timing.h` (shared with VL53L1X ledger V-4) | desk |
| — | WHO_AM_I 0x75 = 0x68 | **CLOSED this pass** `{RM §4.34}` — update app_i2c.c:421-424 comment | done |

## 9. SOURCES
| Key | Source |
|---|---|
| {RM} | RM-MPU-6000A-00 rev 4.0, "MPU-6000 and MPU-6050 Register Map and Descriptions", 2012-03-09 (uploaded) |
| {PS} | PS-MPU-6000A-00 rev 3.4, "MPU-6000 and MPU-6050 Product Specification", 2013-08-19 (uploaded) |
