# AUDIT ADDENDUM — MPU6050 SLICE (closes A-1)

Project: Haptic-Sense · Addendum to `redzone_audit_crosscutting_2026-07-11.md` · Pass 2026-07-11 (chat/Fable)
Scope per instruction: **items 1, 2, 4, 5 only**, MPU6050 driver design. Original findings F-1…F-9, U-1 stand unchanged; VL53L1X and DRV2605L are not re-audited here (note: they did not pass clean — F-1 and F-4 remain open high-severity items in the base report).

> **Snapshot of 2026-07-11.** Findings below (including M-1/M-2/M-3, and the base
> report's F-1/F-4) were fixed after this report was written; the finding register is
> historical. For live dispositions see `docs/PROJECT_DEFENSE.md` §3.1. (Note added
> 2026-07-12.)

**Evidence added this pass:**
- `mpu6050_port_design_v1.md` — 150 lines, the actual MPU6050 IMU Port Design v1, 2026-07-09 ("MPU doc"). **A-1 is CLOSED.**
- `vl53l1x_port_design_v2_reconciliation.md` — 158 lines. Content matches the v2 pass already in evidence (the previously mislabeled file); all base-report cross-references to "L1X-v2" carry over unchanged. Its V-1 note (N6 HAL grep still pending, L1X-v2:23) does not alter F-5.

**Still NOT in evidence:** `app_i2c.h`, `i2c_timing.h`, the RM-MPU-6000A / PS-MPU-6000A PDFs themselves (the MPU doc's author had them uploaded in that session; this audit takes the doc's `{RM}`/`{PS}` citations as the doc's grounding, and independently spot-checks arithmetic — it cannot re-verify the PDFs' text).

---

## ITEM 1 — I2C TASK-BOUNDARY VIOLATIONS (MPU6050)

**Verdict: PASS.**

- Every I2C access in the design is `i2c_wr`/`i2c_rd` from sensor_task: the burst-read snippet is explicitly tagged `sensor_task, TK_PRI 3 — ONLY CALL FROM PRIORITY 3 SENSOR TASK` (MPU:53), and the init chain states "Every step below is `i2c_wr(mpu_addr7, reg, I2C_REG8, &v, 1)` from sensor_task" (MPU:68).
- The address probe reuses the existing gate test, which already carries the P3-only comment (`app_i2c.c:425`; MPU:30).
- Option A data-ready polling is a **GPIO read** (`HAL_GPIO_ReadPin(PE9)`), not I2C, and runs in P3 anyway (MPU:100).
- Option B EXTI is designed clean: handler registered via `tk_def_int(TA_HLNG)` + `EnableInt` at kernel-legal level exactly per the `app_i2c.c` precedent (`:36-39, 219-237`), does "**nothing but** `tk_wup_tsk` — any I2C from the handler is a red-zone violation" (MPU:103). No design line implies I2C outside P3.

---

## ITEM 2 — BLOCKING CALLS (MPU6050)

**Verdict: PASS, one bounded-ness defect — M-2.**

- All register traffic is the DMA primitive: single-byte config writes (MPU:68), one 14-byte burst read per frame (MPU:56), per-register read-back as 1-byte `i2c_rd`s (MPU:82). No blocking HAL variant appears or is implied anywhere in the doc.
- The init settling wait is `tk_dly_tsk(50)` — a sleep, sized against three cited settling times (PLL 1–10 ms `{PS §6.6}`, gyro ZRO 30 ms `{PS §6.1}`, accel wake ≥4 ms `{RM §4.28}`) (MPU:75). PASS.
- **M-2 — VIOLATION RISK (low-med): the DEVICE_RESET poll is unbounded and unpaced.** Row 1 says "poll 0x6B until bit7==0 rather than a fixed delay" (MPU:73) — with **no attempt cap and no inter-poll delay stated**. Compare the project's own discipline elsewhere: DRV row 0 polls with `tk_dly_tsk(1)` and a timeout (DRV:161); the VL53L1X boot gate mandates "a bounded attempt count" (L1X-v2:104). Two failure amplifiers make the bound non-optional: (a) if the device NACKs mid-reset, each failed `i2c_rd` costs up to ~145 ms through the primitive's retry+recovery envelope (`app_i2c.c:357-362`) — the F-8 wall-clock-vs-attempts lesson applies verbatim; (b) a device that never clears bit7 (unpowered, wedged) livelocks the sensor pipeline permanently, violating the primitive's own review requirement that the caller "never stalls the task permanently" (`app_i2c.c:361-362`). No red-zone latency breach (P3 sleeps inside the primitive; P1/P2 preempt freely) — the loss is the entire sensor pipeline, silently. **Fix: bound the poll in attempts (e.g. ≤5), pace with `tk_dly_tsk(1)`, and fail init loudly on exhaustion — the same "fail loudly, never guess" rule the doc itself imposes on the address probe (MPU:32).**

---

## ITEM 4 — SEMAPHORE PATTERN CONSISTENCY (MPU6050)

**Verdict: PASS on what is specified; one consistency note (M-3) and one design gap (M-4).**

- **Option A (recommended, bring-up): PASS.** Latched-level poll adds zero new kernel objects and zero new shared state; the no-missed-event argument is grounded in the latch semantics (LATCH_INT_EN=1, INT_RD_CLEAR=1 — the 14-byte burst itself clears, `{RM §4.15}`, MPU:80, 98-100). Level-not-edge is precisely why polling is race-free here; the doc states it.
- `mpu_addr7` file-static: written once at P3 init, read only by P3 thereafter (MPU:32) — single-task state, no boundary required. PASS.
- **M-3 — consistency note (low, not a violation): two different ISR→P3 wake mechanisms now exist across the sensor designs.** MPU6050 Option B wakes sensor_task with `tk_wup_tsk` (MPU:103); the VL53L1X production EXTI design signals a semaphore via `tk_sig_sem` (L1X-v1:347-350). Both are kernel-legal from handler context and both appear in CLAUDE.md §3's approved API list; functionally either works. But if both EXTI paths go live in Phase 6, two mechanisms for the same event class is exactly the kind of asymmetry that breeds a subtle bug during integration (e.g., wakeup-count vs semaphore-count draining semantics differ when events pile up while P3 is mid-frame). **Action: before Phase 6, pick one mechanism for all sensor data-ready ISRs and record it in CLAUDE.md §3 — a one-line decision now, a debugging session later.**
- **M-4 — UNVERIFIED (design gap, med): the stale-IMU-frame flag's transport is unspecified.** Option A says "if low, mark IMU frame stale and reuse the last sample" feeding the 12-feature fallback (MPU:100). Who reads that mark, and how does it cross P3→P2? If it is implemented as a bare file-static flag read by the inference task, it is a second F-1-class unprotected cross-task flag. The doc legitimately punts pipeline integration to a later doc (MPU:132), so this is a gap to close, not a defect to fix in place. **Named step: the Phase 6 handoff spec (base-report F-6d) must carry the staleness indicator *inside* the semaphore-protected P3→P2 buffer (e.g., a validity field in the feature-frame struct), never as a side-channel flag.** F-6d and M-4 close together.

---

## ITEM 5 — ADDRESS / PROTOCOL CROSS-CONTAMINATION (MPU6050)

**Verdict: PASS on the doc itself — the strongest anti-contamination discipline of the three designs — with one HIGH-severity conflict against the project's locked-decision record (M-1).**

- **PASS — affirmative decontamination.** The §0 guard table inverts every dangerous axis explicitly, each with the failure mode named: 8-bit index `I2C_REG8` (phantom-high-byte failure if `I2C_REG16` transplanted), 7-bit native 0x68/0x69 **no shift** (the VL53L1X ≫1 habit yields 0x34 = universal NACK), big-endian data = same as VL53L1X (correctly marked "None" risk), and the WHO_AM_I=0x68 coincidence pre-empted ("it is the register content, not an echo of the address") (MPU:16-23). Symbol hygiene is restated (pass `I2C_REG8`, never a literal — MPU:62), inheriting the F-4 rule class correctly.
- **PASS — closure of a standing `[UNVERIFIED]`.** The gate-test WHO_AM_I claim (`app_i2c.c:421-424`) is closed against the uploaded RM rev 4.0: reg 0x75 default 0x68, AD0 **not** reflected (`{RM §4.34}`, MPU:23, 33). **Action item (doc-directed): update the `app_i2c.c:421-424` comment from UNVERIFIED to verified-with-cite at next commit.**
- **PASS — arithmetic spot-checks (independent of the PDFs):** 0x48−0x3B+1 = 14 registers = the 14-byte burst (MPU:50); 1000/(1+19) = 50 Hz (MPU:77); reg 0x37 = 0x30 = bit5|bit4 = LATCH_INT_EN|INT_RD_CLEAR as stated (MPU:80); int32 headroom 2³¹/32,768,000 ≈ 65× as claimed (MPU:118). The burst-coherence argument (double-banked registers, burst = same-instant guarantee, singles can tear) is RM-cited and is a genuine correctness catch, not an optimization claim (MPU:47).
- **PASS — locked-decision conformance:** DLPF_CFG=4 is the doc's Option A recommendation (MPU:87) = the lock; ±4 g is Option C (AFS_SEL=1) (MPU:91) = the lock. Neither is re-litigated.
- **M-1 — VIOLATION RISK (HIGH): sensitivity-constant conflict on the locked ±4 g decision — 8192 vs 4096 LSB/g.** The MPU doc states **±4 g (AFS_SEL=1) → 8192 LSB/g**, cited `{RM §4.18 / PS §6.2}`, and its §5 table runs 16384/8192/4096/2048 for AFS_SEL 0/1/2/3, with the mg conversion built on 8192 (MPU:91, 110-118). The project's locked-decision record from the pre-audit sessions pairs **"±4 g (4096 LSB/g)"**. These cannot both be true: in the doc's own RM-cited table, **4096 LSB/g is the ±8 g row**. If the 4096 figure is transplanted into the mg conversion while the device is configured AFS_SEL=1, **every accelerometer feature is scaled 2× high** — silently, plausibly, and (per the doc's own warning at MPU:92) baked irreversibly into the Edge Impulse training data. This is the highest-consequence contamination-class finding in either report because it corrupts data, not code. **Verification step (one table read): RM §4.5 / §4.18 sensitivity table in the uploaded RM rev 4.0 — then correct the locked-decision record (CLAUDE.md "locked driver decisions" line) to "±4 g (AFS_SEL=1, 8192 LSB/g)" if the doc is right, or the doc's §5 if not; additionally `grep -rn "4096"` across project docs to catch any other transplant. GATE: do not begin Edge Impulse data collection until M-1 is resolved** — the lock's purpose (fixed scaling before collection) is defeated if the recorded constant is the wrong one.
- One nuance for the record: the doc is **internally consistent**; M-1 is a conflict between the doc and the project's decision record, and the doc's value is the one consistent with its cited RM table. The audit cannot independently read the RM PDF (not in this pass's evidence), so the verification step above is mandatory, not a formality.

---

## ADDENDUM FINDING REGISTER

| ID | Verdict | Severity | One-line | Owner action |
|---|---|---|---|---|
| A-1 | **CLOSED** | — | Actual MPU6050 doc now in evidence; slice audited | — |
| M-1 | VIOLATION RISK | **high** | ±4 g paired with 4096 LSB/g in the locked-decision record vs 8192 in the RM-cited doc — a latent 2× feature-scale error | Read RM §4.18 table; fix the record; grep for "4096"; **gates Edge Impulse collection** |
| M-2 | VIOLATION RISK | low-med | DEVICE_RESET poll unbounded/unpaced; NACK path costs ~145 ms per attempt; wedge = permanent pipeline stall | Bound in attempts, pace with `tk_dly_tsk(1)`, fail loudly |
| M-3 | note | low | `tk_wup_tsk` (MPU EXTI) vs `tk_sig_sem` (ToF EXTI) — two ISR→P3 mechanisms for one event class | Pick one before Phase 6; record in CLAUDE.md §3 |
| M-4 | UNVERIFIED | med | Stale-IMU-frame flag transport unspecified — F-1-class risk if implemented as a bare cross-task flag | Fold into the F-6d Phase 6 handoff spec: validity field inside the protected buffer |
| — | closure note | — | `imu_buf[32] __attribute__((aligned(32)))` (MPU:54) pre-closes the F-6c-class buffer-alignment gap for the MPU6050 — the base report's "highest-stakes UNVERIFIED cache instance" (U-2 cache slice) is resolved by design, though full item-3 re-audit was outside this addendum's instructed scope | Record; no action |
| — | carry-over | — | V-2/V-5 (`app_i2c.h`, `i2c_timing.h` reads) now gate three drivers identically (MPU:139, 142; base U-1) | Same two file reads close all three |

**Bottom line:** the MPU6050 design is the cleanest of the three on items 1, 2, 4, 5 — explicit task-context tagging, DMA-only throughout, minimal shared state, and the best contamination guard in the project. The two things that must move before hardware: **M-1 before any training data exists**, and M-2 before the init chain is implemented. Base-report findings F-1 and F-4 remain the open high-severity items project-wide.

— End of addendum.
