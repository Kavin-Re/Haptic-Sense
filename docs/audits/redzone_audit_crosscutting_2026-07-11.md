# CROSS-CUTTING RED-ZONE AUDIT — VL53L1X / MPU6050 / DRV2605L + Task Architecture

Project: Haptic-Sense (TRON Forum Contest 2026) · Audit pass 2026-07-11 (chat/Fable)
Type: adversarial review. Verdicts: **PASS** / **VIOLATION** / **VIOLATION RISK** / **UNVERIFIED**.
Locked decisions honored, not re-litigated: MPU6050 DLPF_CFG=4, ±4 g; DRV2605L open-loop baseline.

**Evidence base (files read in full this pass):**
- `app_i2c.c` — 449 lines, uploaded (commit fc6ff88 lineage)
- `drv2605l_port_design_v1.md` — 338 lines ("DRV doc")
- `vl53l1x_port_design.md` — 521 lines — VL53L1X design, 2026-07-07 pass ("L1X-v1")
- File uploaded as `mpu6050_port_design_v1.md` — 158 lines — **content is "PHASE5_L2 — VL53L1X ULD Platform Port Design (v2)", 2026-07-09 pass** ("L1X-v2")
- `CLAUDE.md` — project memory, uploaded copy
- One live lookup: ST community record of I2C-v2 HAL Mem-path internals (F-5 below)

**NOT in evidence:** the MPU6050 design document (see A-1); `app_i2c.h`; `i2c_timing.h`; Phase 4 task-body source (`app.c` task creation / handoff code); any device datasheet PDF. Claims gated on these are marked UNVERIFIED, never assumed.

---

## A. AUDIT-INPUT INTEGRITY FINDINGS

### A-1 — MPU6050 design doc missing from evidence — **AUDIT BLOCKER**

`mpu6050_port_design_v1.md:1` reads `# PHASE5_L2 — VL53L1X ULD Platform Port Design (v2)`. The file is the VL53L1X 2026-07-09 review pass, not the MPU6050 design. **Every MPU6050-specific audit item (task boundary, blocking, cache, semaphores, contamination) is therefore UNVERIFIED in this report.** No finding below about the MPU6050 design is derived from model memory of prior sessions.
**Verification step:** upload the actual MPU6050 port design doc; re-run the MPU6050 slice of all six items. Until then, treat the MPU6050 driver as **un-audited**, and do not start its implementation on the strength of this report.

### A-2 — Two VL53L1X docs in circulation, in active conflict

L1X-v2 supersedes L1X-v1 in the areas it restates (L1X-v2:4), but L1X-v1 as uploaded still carries pre-correction text that contradicts v2 on two implementation-critical points (F-3, F-4 below). A superseded doc that still circulates with stale silent-failure-class content is itself a hazard, per the project's own contamination history.
**Verification step:** edit L1X-v1 in place — strike/annotate §6 step 3, §8-H1, and the §3 mapping table as superseded — or merge into a single v3.

---

## 1. I2C TASK-BOUNDARY VIOLATIONS

| Scope | Verdict | Evidence |
|---|---|---|
| `app_i2c.c` primitives | **PASS** | Task context declared: everything runs in sensor_task TK_PRI 3 except thin IRQ handlers (`app_i2c.c:11-14`); mandatory comment present at `:14`, `:363`, `:425`. HAL callbacks perform `tk_sig_sem` only — no I2C from ISR (`:75-89`); ISR→kernel legality grounded in-file (NVIC level 1, BASEPRI analysis, `:36-39`). |
| VL53L1X shim | **PASS** | All nine shim functions P3-only with mandatory comment (L1X-v1:152-155). EXTI production path: PD0 ISR does **no I2C**, signals a semaphore only; `GetResult`/`ClearInterrupt` run in P3 (L1X-v1:347-350). |
| DRV2605L | **PASS on the letter; see F-1** | P3 owns every register byte (init, effect-ID retune, optional watchdog — DRV:41-42, 286-288); P1 is `HAL_GPIO_WritePin` only, zero I2C (DRV:43). No design line implies I2C from P1. However the P1/P3 **coordination protocol** around EN has a race — F-1, filed under items 4/6. |
| MPU6050 | **UNVERIFIED** | A-1. Named step: re-upload doc, re-audit. |

Note on EN pin ownership: PE7 is written from **both** P3 (init: "INIT (P3) drive EN high", DRV:296) and P1 (kill, R-EN-2, DRV:75-77). Pin-level writes via `HAL_GPIO_WritePin` use BSRR and are atomic per-pin, so there is no read-modify-write hazard `[UNVERIFIED for the N6 HAL GPIO implementation specifically — verification: confirm `HAL_GPIO_WritePin` writes BSRR in `stm32n6xx_hal_gpio.c`]`. The hazard is at protocol level, not pin level → F-1.

---

## 2. BLOCKING CALLS

| Scope | Verdict | Evidence |
|---|---|---|
| `app_i2c.c` transfer path | **PASS** | Only `HAL_I2C_Mem_Read_DMA` / `HAL_I2C_Mem_Write_DMA` (`app_i2c.c:327-333`). Wait is `tk_wai_sem` — task sleeps, P1/P2 preempt freely (`:337-339`). Timeout path aborts with `_IT` variant, non-blocking (`:342`). Bus recovery paced by `tk_dly_tsk(1)`, no busy wait, bounded ~45 ms (`:277-289`); full-failure envelope bounded ~145 ms, frame dropped, task never stalls (`:357-362`). |
| N6 HAL internal busy-wait inside `Mem_*_DMA` entry | **UNVERIFIED — F-5** | The memory-address phase is non-blocking on N6: the DMA path prefetches the index MSB into TXDR and stages the LSB for the TXIS interrupt — grounded by L1X-v1's in-repo read of `stm32n6xx_hal_i2c.c:3117-3120` (write) and `:3297-3300` (read) (L1X-v1:118-128). **What that read did not establish:** whether the `HAL_I2C_Mem_Write_DMA`/`Read_DMA` *entry* contains any `I2C_WaitOnFlagUntilTimeout(BUSY)`-class spin before starting. This is not paranoia: the same I2C-v2 HAL family's `Mem_*_IT` variants historically busy-waited inside `I2C_RequestMemoryRead` (`I2C_WaitOnTXISFlagUntilTimeout` in the function body), with documented multi-second blocking complaints on the ST forum. A bounded spin at P3 while P1/P2 preempt is not a red-zone breach as written in CLAUDE.md §3, but it must be *known*, not assumed. **Verification step (one grep):** `grep -n "WaitOnFlag\|WaitOnTXIS\|WaitOnSTOP" stm32n6xx_hal_i2c.c` within the `HAL_I2C_Mem_Write_DMA` / `HAL_I2C_Mem_Read_DMA` bodies; record max spin bound in CLAUDE.md if nonzero. |
| VL53L1X | **PASS** | `WaitMs → tk_dly_tsk`, "NOT a busy loop" (L1X-v1:170). Complete `WaitMs` inventory: four call sites, none on the frame path (L1X-v1:390-403). Per-frame path (`CheckForDataReady`/`GetResult`/`ClearInterrupt`) contains zero `WaitMs`, verified by grep + body read (L1X-v1:400-403). Init-class blocking (SensorInit first measurement, ~100 ms default budget) sleeps under `tk_dly_tsk`, preemptible (L1X-v1:405-413). |
| DRV2605L | **PASS, one budget error — F-8** | Every init row is a single-byte `i2c_wr(0x5A, reg, I2C_REG8, &val, 1)` (DRV:159-171); DEV_RESET and calibration polls are `i2c_rd` + `tk_dly_tsk` (DRV:161, 228). **F-8 (minor):** row 0's "≤10 ms timeout" for the DEV_RESET poll (DRV:161) is stated in wall-clock terms, but a device that NACKs mid-reset makes each failed `i2c_rd` cost up to ~145 ms through the primitive's retry+recovery envelope (`app_i2c.c:357-362`). A wall-clock 10 ms budget is unachievable through this primitive on the failure path. **Fix:** specify the poll timeout in *attempts* (e.g., ≤3 polls), not milliseconds, and note the per-attempt worst case. |
| MPU6050 | **UNVERIFIED** | A-1. |

---

## 3. CACHE MAINTENANCE

**Structural verdict: PASS today, with three latent items and one specification gap.**

- **PASS — read-side invalidate, correctly sequenced.** `SCB_InvalidateDCache_by_Addr` runs inside `i2c_xfer_once` after every successful DMA read, *before returning to the caller* (`app_i2c.c:349-353`). Since any P3→P2 semaphore signal necessarily happens after the primitive returns, the CLAUDE.md §3 ordering (invalidate before signalling inference) is guaranteed by construction for **every** driver that routes through `i2c_rd` — all three do by design (L1X-v1:157-171; DRV:171; MPU6050 per locked project rule, doc unverified). Currently inert: D-cache OFF (`app_i2c.c:65-66`, citing `app_config.h:21`).
- **F-6a (latent, acknowledged in evidence):** write-side `SCB_CleanDCache_by_Addr` is **absent** before `HAL_I2C_Mem_Write_DMA` (`app_i2c.c:326-335` — read path only). Inert with D-cache off; with D-cache on, DMA reads stale RAM. Already carried at L1X-v1:210-214 with owner = the primitive. Verdict: PASS-latent; becomes a VIOLATION the day D-cache is enabled without this fix.
- **F-6b (latent, acknowledged):** ULD passes an unaligned stack buffer — `GetResult`'s `Temp[17]` (L1X-v1:215-219, citing `VL53L1X_api.c:590`). Invalidating an unaligned/unpadded buffer with D-cache on can clobber adjacent stack. Named fix already on record: bounce buffer in the shim's `ReadMulti`.
- **F-6c (NEW — same class, DRV2605L, unacknowledged):** the DRV doc's read paths (0x00 DEVICE_ID sanity read, 0x0C GO poll, 0x01 readbacks — DRV:172-175, 228) and every `&val` write hand **1-byte stack locals** to the DMA primitive with **no alignment/padding rule stated anywhere in the DRV doc**. Identical latent hazard to F-6b, currently masked by D-cache-off. The 32-byte aligned+padded rule exists in the codebase precedent (`gate_buf`, `app_i2c.c:64-67`) but the DRV design never imports it. **Verdict: VIOLATION RISK (design gap).** Fix: add a buffer rule to the DRV design (static aligned scratch buffer for all DRV2605L transfers), or hoist a single bounce-buffer rule into the primitive so no driver can get it wrong.
- **F-6d (specification gap):** the audit item asks to confirm maintenance "for all three sensor buffers ... before any semaphore signal to Priority 2." No uploaded document specifies the **P3→P2 handoff buffer** (the 13-feature vector assembly). If the handoff is a CPU copy from DMA-landed buffers into a shared static buffer, no additional maintenance is needed (CPU→CPU through the same cache is coherent) — but that "if" is stated nowhere in evidence. **Verdict: UNVERIFIED.** Named step: the Phase 6 design must specify the handoff buffer, its owner, alignment, and that no DMA engine touches it (or the required maintenance if one does).
- **MPU6050 burst-read buffer: UNVERIFIED (A-1)** — and it is the highest-stakes instance of this whole item, since the IMU burst read is the largest recurring DMA read feeding the feature vector.

---

## 4. SEMAPHORE PATTERN CONSISTENCY

- **PASS — `i2c_done_sem` is a correct, documented *completion-signal* pattern, deliberately not the paired free/ready pattern.** Producer/consumer named per the CLAUDE.md rule (ISR callback → sensor_task, `app_i2c.c:16-19`). The paired free/ready pattern in CLAUDE.md §3 is for *shared-buffer handoff between tasks*; a single-client I/O completion wait correctly uses one semaphore with `isemcnt=0, maxsem=1` plus a pre-transfer drain and a double-fire guard (`app_i2c.c:52-59, 79-84, 186-189, 319-323`). Auditing this as "pattern inconsistency" would be wrong; it is the right tool with a three-layer defence, and the abort/late-signal interleavings were traced this pass: the recovery path's `HAL_I2C_DeInit` (`:264`) kills any pending abort completion, and the drain (`:321`) eats one that fired earlier — bounded by `maxsem=1`.
- **PASS with standing condition — `i2c_busy` tripwire.** Explicitly documented as NOT a lock (`app_i2c.c:369-375`), valid only under the single-client (P3-only) contract, with the named upgrade path (`tk_loc_mtx`) if a second client ever appears. Any future design that calls `i2c_rd/i2c_wr` from another task converts this instantly into a real race — the DRV and L1X docs both keep all calls in P3, so the contract holds in evidence.
- **VIOLATION RISK — F-1: the DRV2605L "config invalid" flag is unprotected shared state with a lost-update race.** Protocol as designed: KILL (P1) drives EN low and "sets 'config invalid' flag"; RECOVER (P3) "sees flag → EN high → re-run §4 rows 1–8 → clear flag" (DRV:300-301; R-EN-3, DRV:78-83). Two defects:
  1. **No mechanism stated.** The flag is written by P1 and read-and-cleared by P3 with no semaphore, no atomic type, no `volatile` qualifier stated. A single aligned byte write is atomic on Cortex-M, but the *check-then-act* sequence in P3 is not, and the design's own precedent (`app_i2c.c:369-375`) shows the project knows the difference. The audit criterion "no shared-state access without a semaphore boundary" is not met as written.
  2. **The race, concretely.** P1 kills again *while P3 is mid-re-init*. With EN low, the DRV2605L **still ACKs but takes no writes** — "no read or write is possible" yet transactions look successful on the bus (DRV:59-61). Every remaining `i2c_wr` in rows k..8 returns E_OK. P3 then clears the flag. End state: **flag clear, device unconfigured, haptics silently dead** — on a hazard-alert wearable, a missed-alert failure, which for the user is the worst failure direction this product has.
  **Fix options (developer decides):** (A) replace the boolean with a **monotonic kill counter** incremented by P1; P3 samples it before re-init and compares after — if changed, loop; "clear" becomes compare-and-acknowledge, never a blind clear. (B) post-re-init **readback verify** of 0x01 (MODE bits must read 0x01) plus a final flag re-check before acknowledging. A is simpler and closes the window completely; B additionally catches EN-low-during-readback (a read with EN low is also dead per DRV:59-61 — note a readback under EN-low returns garbage or stale data, which is itself detectable). **Verification step:** amend the DRV design §7 protocol, re-review; H-D2's bench test is unchanged and still required.
- VL53L1X EXTI semaphore (production path): **PASS at design level** — ISR producer signals, P3 consumer waits, no I2C in ISR (L1X-v1:347-350). Implementation review gate applies when written.
- **Evidence boundary:** the Phase 4 task bodies and the P3→P2 / P2→P1 paired-semaphore implementations are **not in evidence** (not uploaded). This item's architecture-level verdicts are limited to CLAUDE.md §3's pattern text plus the three driver docs. The Phase 4 soak result is prior work, not re-audited here.
- MPU6050: **UNVERIFIED** (A-1).

---

## 5. ADDRESS / PROTOCOL CROSS-CONTAMINATION

- **PASS — DRV2605L doc is affirmatively decontaminated.** Its §0 header explicitly inverts the two axes most likely to be copy-pasted wrong from the VL53L1X: 7-bit 0x5A passed directly, **no shift, no shim**, vs the ULD's 8-bit-0x52-with-shim; 8-bit register index `I2C_REG8` vs 16-bit `I2C_REG16` (DRV:21-25). Every init row and the call shape are consistent with it (DRV:159-171). No VL53L1X or MPU6050 detail found in the DRV doc.
- **PASS — MPU6050 material in `app_i2c.c`.** Gate test uses 0x68/0x69 7-bit and `I2C_REG8` (`app_i2c.c:429-435`), consistent with CLAUDE.md §2; the WHO_AM_I=0x75/expect-0x68 claim is explicitly labeled UNVERIFIED **in the code itself** with its verification step (`app_i2c.c:421-424`) — correct discipline. The value stays UNVERIFIED until first hardware run or an RM-MPU-6000A check.
- **VIOLATION — F-3: the two VL53L1X docs carry conflicting first-light sensor-ID pass criteria, both claiming grounding.** L1X-v1: "**id must equal 0xEEAC** (`api.h:197`)", "Anything but 0xEEAC = debug the shim", Confidence: certain, from a claimed in-repo source read (L1X-v1:273-279; §8-H1 at :501-502). L1X-v2: "**★ CORRECTION: 0xEACC, not 0xEEAC**" per UM2510, with log-don't-hard-fail semantics, and V-6 (read the in-repo `api.h` doc comment) still open (L1X-v2:98, 119, 133). These cannot both be the pass criterion. The plausible reconciliation — the in-repo v3.5.5 comment genuinely says 0xEEAC while UM2510 and the MODEL_ID byte composition (0x010F=0xEA, 0x0110=0xCC) say 0xEACC, i.e. ST's own doc drift, consistent with L1X-v2's {ID-VAR} evidence — is **inference, not established**. **Verification step: execute V-6** (one file read), then annotate L1X-v1 §6 step 3 and §8-H1 as superseded, and update CLAUDE.md "Key learnings" (already noted there as corrected to 0xEACC — confirm the log-don't-hard-fail caveat is attached). The v2 test-plan semantics (log, don't hard-fail; 0x0000/0xFFFF = bus failure, not a variant) are the correct posture either way.
- **VIOLATION RISK — F-4: superseded literal-`2` regsz in L1X-v1's mapping table.** Every line of the v1 §3 mapping passes the literal `2` (`i2c_wr(dev7, index, 2, b, 1)` etc., L1X-v1:161-171, restated at :159). L1X-v2's bolded rule is the exact opposite: "**the shim must pass the symbol `I2C_REG16`, never the literal `2`**" — because the primitive dispatches on `regsz == I2C_REG16` (`app_i2c.c:316-317`), and if `I2C_REG16` is not numerically 2 (`app_i2c.h` not in evidence — V-2 open), a literal silently falls to the 8-bit branch, the sensor ACKs anyway, and every read returns wrong data with no error (L1X-v2:28). If implementation is typed from the v1 table, the project re-imports the precise silent-failure class v2 was written to kill. **Verification steps:** (1) edit the v1 table or mark it superseded in-file; (2) execute V-2 (read `app_i2c.h`) so the doc conflict is also numerically closed.
- **VIOLATION (open correction) — F-2: CLAUDE.md §2 still carries the contaminating figure.** The uploaded CLAUDE.md §2 upgrade-path line reads "~88 KB firmware upload over I2C at init" for the VL53L5CX. L1X-v2's corrections register establishes ~84 KB (86,016 B = 0x8000+0x8000+0x5000) and directs exactly this CLAUDE.md edit (L1X-v2:118); L1X-v1's Gate-1 item 9 (paste the correction into the sibling bring-up doc) is also still marked OPEN (L1X-v1:496-497). Neither is a driver defect — but the project's documented contamination vector is still standing in the project's own memory file. **Verification step:** make both edits at the next CLAUDE.md commit; this audit report is the third pointer to it.
- MPU6050 doc-level contamination check: **UNVERIFIED** (A-1). Note this is the check the missing doc most needs, given the documented MPU6050↔VL53L1X register-addressing conflation in project history.

---

## 6. ERM SAFETY PATH

- **PASS — electrical path closed.** Motor connects only to OUT+/OUT− through the protected output stage; no register write can route actuator current to a GPIO (DRV:237-241). CLAUDE.md §8 zone-8 rule stands permanently.
- **PASS — no software path to unbounded drive.** The only unbounded-drive register states are MODE=5 and MODE=3, and the design never writes either value (DRV:245-254). The init sequence arms last: rows 0-7 leave the device idle (MODE=0, GO=0); the external-trigger arm (MODE=1) is row 8, the final write (DRV:159-169). A P3 fault at any row boundary leaves the device idle or in a state where playback is bounded by the zero-terminated single-slot sequencer (DRV:167-168, 249-251). Independent bounds hold regardless: OD_CLAMP mode-independent at 3.001 V (DRV:258-261, calc at 179-184), EN-low unconditional kill with 15 kΩ output termination (DRV:262-264), safe power-on defaults — STANDBY=1 at reset plus 2 MΩ EN pull-down (DRV:265-267), device-internal OC/thermal/brownout protections (DRV:268-270).
- **VIOLATION RISK — F-1 (the other failure direction).** The §6 analysis is exhaustive on *unintended drive* but silent on *alert-silently-dead*: the EN-low-silent-ACK property (DRV:59-61) combined with the F-1 flag race can leave the device unconfigured while flagged armed (full mechanism in item 4). For a hazard-alert wearable, a missed alert is the higher-consequence failure. The §6 safety argument should gain a short subsection covering the dead-alert direction, closed by the F-1 fix plus (optionally) the §6.4 MODE watchdog, which incidentally detects exactly this state.
- **VIOLATION RISK — F-7: IN/TRIG arming order under-specified.** The wiring requirement says push-pull, driven low at init, never floating — "in trigger modes IN/TRIG is a live trigger input, and the pin table requires GND if unused" (DRV:131-133). But no ordering rule ties *IN/TRIG GPIO configured and driven low* strictly **before** row 8 arms MODE=1 (and before EN rises). If init order ever becomes EN-high → rows 1-8 → GPIO config, or P3 faults between row 8 and GPIO config, a floating trigger input on an edge-triggered device can noise-fire spurious clicks — bounded single effects, not unsafe current, but a **false hazard alert to a visually impaired user is a functional safety event** for this product. **Fix (one sentence in the design):** "IN/TRIG GPIO is configured push-pull and driven low before EN(PE7) rises; row 8 must never execute with IN/TRIG unconfigured." Merge into R-EN-1's init ordering.
- **PASS — T-1 cancel semantics correctly derived, but flag the implementation obligation.** Re-trigger during playback cancels (DRV:108-113); the design derives the min-inter-pulse-period constraint. The P1 pulse-rate grading code **must clamp** its minimum period above the measured effect duration — H-D3 supplies the number; the clamp itself is a P1 implementation checklist item that should appear in the eventual `app_drv2605l.h` as a named constant, not folklore.
- **UNVERIFIED (hardware-gated, correctly ledgered):** coil ≥ 4 Ω vs OC_DETECT (H-D6, DRV:325), breakout VDD rail / OD_CLAMP headroom (H-D5, DRV:324), effect duration (H-D3), EN-rise state ambiguity (H-D2). All carry named steps; nothing to add.
- **UNVERIFIED (NEW, minor) — F-9: I2C-wedge-in-standby recovery.** DRV:271-274 records that the device's I2C watchdog recovers a hung transaction in all states *except standby*, where "only a power cycle recovers I2C" (§8.3.11 as cited there). Rows 0-1 execute **while the device is in its power-on standby state** (STANDBY=1 default, DRV:265) — if a wedge occurred in exactly that window, breakout VDD is hardwired and EN-toggle is shutdown, not power removal (registers survive EN-low, DRV:58). Whether EN-toggle clears a standby-wedged I2C engine is not derivable from the doc. Probability low (window = first two transactions after power-on), consequence = bench power-cycle. **Verification step:** one bench attempt if ever observed; note in the design that the L1 bus-recovery ladder does not cover this specific state.

---

## 7. CONSOLIDATED FINDING REGISTER

| ID | Verdict | Severity | One-line | Owner action |
|---|---|---|---|---|
| A-1 | BLOCKER | — | MPU6050 design doc not in evidence; wrong file uploaded | Re-upload; re-run MPU6050 slice before implementing that driver |
| A-2 | VIOLATION | med | Superseded L1X-v1 still circulates with pre-correction text | Annotate/merge docs |
| F-1 | VIOLATION RISK | **high** | DRV kill/re-init lost-update race → haptics silently dead while flagged armed; flag itself unprotected shared state | Kill-counter or readback-verify protocol; amend DRV §7 + R-EN-3 |
| F-2 | VIOLATION (open) | low | CLAUDE.md §2 still says "~88 KB"; correct figure ~84 KB (86,016 B); Gate-1 item 9 sibling paste also open | Two edits at next CLAUDE.md commit |
| F-3 | VIOLATION | med | 0xEEAC (v1, `api.h:197`) vs 0xEACC (v2, UM2510) — conflicting first-light pass criteria, both claiming grounding | Execute V-6 (read in-repo `api.h`); keep log-don't-hard-fail |
| F-4 | VIOLATION RISK | **high** | v1 mapping table passes literal `2`, violating v2's symbol-only rule; silent 8-bit-fallback failure class | Fix v1 table; execute V-2 (read `app_i2c.h`) |
| F-5 | UNVERIFIED | med | N6 `HAL_I2C_Mem_*_DMA` entry may contain internal busy-flag spin (IT-variant precedent on ST forum); address phase already proven non-blocking (`stm32n6xx_hal_i2c.c:3117-3120`) | One grep in the in-repo N6 HAL; record bound |
| F-6a | PASS-latent | med* | Write-side D-cache clean absent from primitive (acknowledged, L1X-v1:210-214) | Add at the D-cache-enable milestone |
| F-6b | PASS-latent | med* | ULD unaligned `Temp[17]` vs invalidate (acknowledged; bounce-buffer fix named) | Same milestone |
| F-6c | VIOLATION RISK | med* | DRV doc has **no** buffer alignment rule for its 1-byte DMA transfers — F-6b's class, unacknowledged | Add buffer rule to DRV design (or hoist bounce buffer into primitive) |
| F-6d | UNVERIFIED | med | P3→P2 feature-vector handoff buffer unspecified in any evidence | Specify in Phase 6 design (owner, alignment, no-DMA statement) |
| F-7 | VIOLATION RISK | med | IN/TRIG configure-low-before-arm ordering not an explicit rule; floating trigger in MODE=1 = spurious alert | One-sentence ordering rule merged into R-EN-1 |
| F-8 | minor | low | DEV_RESET poll "≤10 ms" wall-clock budget incompatible with primitive's ~145 ms failure envelope | Respecify timeout in attempts |
| F-9 | UNVERIFIED | low | I2C wedge during power-on standby window not recoverable by EN-toggle or bus recovery (per §8.3.11 as cited) | Note in design; bench only if observed |
| U-1 | UNVERIFIED | med | `I2C_BUS_HZ` ≤ 400 kHz gates **all three** devices (DRV H-D8; L1X V-4; MPU6050 max `[UNVERIFIED — check MPU-6000 product spec]`); `i2c_timing.h` not in evidence | One file read + one LA capture closes it for the bus |
| U-2 | UNVERIFIED | — | Everything MPU6050-design-specific | Blocked on A-1 |

\* F-6a/b/c severities are conditional: zero today (D-cache off, `app_config.h:21` per `app_i2c.c:65-66`), med-high at the D-cache-enable milestone. Recommend a single CLAUDE.md line: "D-cache enable is gated on closing F-6a/b/c."

## 8. WHAT PASSED CLEANLY (for the record, with citations — not reassurance, scope closure)

The primitive's task-context discipline, DMA-only transfer path, timeout/abort/recovery bounding, read-side cache sequencing, and completion-semaphore triple defence all held under adversarial trace, including the abort-late-signal interleaving (`app_i2c.c:14, 319-323, 337-353, 357-392`; recovery `:246-306`). The VL53L1X per-frame path is provably free of blocking waits (L1X-v1:400-403). The DRV2605L init sequence arms last and never writes an unbounded-drive mode (DRV:159-169, 253-254), and its anti-contamination header is a pattern the missing MPU6050 doc should be checked for (A-1). EN-low remains an unconditional P1 kill under every register state (DRV:262-264) — the safety invariant survives every finding above; F-1 attacks the *recovery* protocol, not the kill.

— End of audit. Re-issue required for the MPU6050 slice once the correct doc is in evidence.
