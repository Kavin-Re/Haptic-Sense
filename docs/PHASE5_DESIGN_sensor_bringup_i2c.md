# Phase 5 Design — Sensor Bring-Up: VL53L1X + MPU6050 on I2C1 (DMA, Priority 3)

**Project:** Haptic-Sense · TRON Forum Contest 2026
**Prerequisite:** Phase 4 exit criteria passed (task architecture frozen, preemption < 1 ms evidenced, synthetic pipeline end-to-end).
**Status:** DESIGN — not yet implemented.
**Contract with Phase 4:** this phase replaces ONLY the body of `sensor_task` (TK_PRI 3). The 13-feature buffer layout, semaphore interfaces, and cache-invalidate call site are frozen — real data flows through the identical path the synthetic generator used.

---

## 1. Objective

Real ToF + IMU data at 50 Hz into the locked feature buffer, using **DMA/IT-mode I2C only, exclusively from the Priority 3 task** — no blocking HAL call anywhere, ever.

The central design problem: **ST's VL53L1X ULD API is written around a blocking platform layer** (`VL53L1_WrByte`/`RdByte`/`ReadMulti` that don't return until the bus transaction completes). Our red-zone rules forbid `HAL_I2C_Master_Transmit/Receive`. This doc resolves that tension without rewriting the ULD.

## 2. First actions (in order, before any driver code)

1. **Flip `DEVCNF_USE_HAL_IIC` → 1** in `config_bsp/stm32_cube/config_bsp.h:40` (the deferred action carried since Phase 2.5). Rebuild; confirm 0 errors and the `msdrvif.c` static-pool fix still holds.
2. **Inspect the BSP's IIC driver source** (`mtk3_bsp2/` sysdepend HAL IIC device driver) and answer one question: *does it use blocking, IT, or DMA HAL calls internally?* This decides Option A vs B in §4.1. Do not assume — read the code.
3. **Physically detach the MB1854B camera module (FFC CN14)** for all of Phase 5. Its VL53L5CX also answers at **0x29 on I2C1** (CLAUDE.md §2) — a hard address collision with the VL53L1X breakout. Side benefit: without the module, Type-A→C USB power is sufficient (CLAUDE.md §1).
4. **Bus scan I2C1** (probe addresses 0x08–0x77 via IT-mode `HAL_I2C_IsDeviceReady`-equivalent). Expected: `0x29` (VL53L1X), `0x68` or `0x69` (MPU6050 — resolves the AD0 question, CLAUDE.md §2). Record the scan output as evidence. Anything else on the bus must be identified before proceeding.

## 3. Hardware (all from CLAUDE.md §2 / MB1939 schematic-verified map)

| Item | Value |
|---|---|
| Bus | I2C1: PH9 = SCL, PC1 = SDA, onboard 1.5 kΩ pull-ups — **add nothing external** |
| VL53L1X | 0x29 · XSHUT hard-wired to 3.3 V rail · GPIO1 (INT) → PD0 (ARD_D2) |
| MPU6050 | 0x68 (0x69 if AD0 high — bus scan decides) · INT → PE9 (ARD_D3) |
| Bus speed | 400 kHz Fast-mode target. 1.5 kΩ pull-ups comfortably support Fm — *(rise-time margin unverified; if scope shows marginal edges, drop to 100 kHz — bandwidth budget in §7 shows 100 kHz still fits)* |

**Consequence of XSHUT-to-rail:** no software power-cycle of the ToF is possible. Recovery is limited to the device soft-reset register and I2C bus recovery (§8). Acceptable because only one 0x29 device exists on the bus (camera detached) so no address reassignment is ever needed. Document this in the wiring notes.

## 4. Driver architecture — four layers

```
L3  sensor_task loop (TK_PRI 3)          — pacing, feature computation, handoff
L2  vl53l1x.c / mpu6050.c                — device logic (ULD port + register driver)
L1  i2c_xfer(): sem-wrapped async I2C    — "synchronous to caller, non-blocking to CPU"
L0  HAL I2C1 + DMA/IT + IRQ plumbing     — configured once at init
```

### 4.1 L1 — the primitive that resolves the blocking-API conflict

```c
/* task: sensor_task, TK_PRI 3 — ONLY CALL FROM PRIORITY 3 SENSOR TASK */
ER i2c_xfer(...) {
    /* start HAL_I2C_..._DMA() or _IT() transfer            */
    /* tk_wai_sem(i2c_done_sem, 1, I2C_TMO_MS);  <-- task SLEEPS, CPU is free */
    /* I2C/DMA completion IRQ -> tk_sig_sem(i2c_done_sem)   */
    /* on timeout or error flag -> bus recovery path (§8)   */
}
```

The caller experiences a synchronous read/write; the CPU never busy-waits and priority 1–2 tasks preempt freely during the transfer. This satisfies the red-zone rule *literally and in spirit*: only `_DMA()`/`_IT()` HAL variants, only from Priority 3, and the < 1 ms hazard guarantee is untouched because the sensor task is asleep, not spinning.

**The ULD port becomes trivial:** implement `VL53L1_WrByte/RdByte/WrWord/RdWord/ReadMulti/WriteMulti` (the platform porting layer ST explicitly leaves to the integrator) on top of `i2c_xfer()`. The ULD core is untouched → its ST license stays clean, our platform file is Apache 2.0.

**Option A vs B for L0/L1 ownership (developer decides):**
- **A — bypass the BSP IIC device driver; own HAL directly.** Full control of DMA config, error paths, and cache behavior. Cost: we bypass µT-Kernel's device-driver abstraction (`tk_opn_dev`/`tk_rea_dev`).
- **B — use the BSP IIC driver** (the thing `DEVCNF_USE_HAL_IIC=1` enables) *if and only if* §2 step 2 shows it is IT/DMA-based internally. If it's blocking inside, B is disqualified regardless of its nicer API.
- **Recommendation: A.** One integrator, one bus, hard real-time rules — the abstraction buys little and hides the exact details (DMA channels, IRQ priorities, error flags) we must control. Flip `DEVCNF_USE_HAL_IIC` anyway (it gates HAL I2C compilation into the BSP build); simply don't route through the device-driver API. Re-verify this claim against the actual config gating when reading the source in §2 step 2.

### 4.2 L0 items requiring verification (do not code from memory)

- **DMA request routing for I2C1 RX/TX on STM32N6** (GPDMA channel/request numbers): take from CubeMX for STM32N657X0HXQ or RM0486 — *unverified from training; N6 DMA architecture differs from older families*.
- **IRQ integration with µT-Kernel:** I2C event/error and DMA IRQs must be registered via the BSP's interrupt definition mechanism (`tk_def_int`-style), not bare NVIC writes, and their NVIC priorities must sit in the range the kernel permits for ISRs that call `tk_sig_sem`. Verify the exact mechanism and priority constraints in `bsp2_stm32_cube_en.md` + `sysdef.h` before wiring any handler.
- **Cache-safe buffers:** every DMA-target buffer `__attribute__((aligned(32)))` and padded to a multiple of 32 bytes (M55 cache line), so `SCB_InvalidateDCache_by_Addr` (call site already placed in Phase 4) never clobbers adjacent data. Static allocation only.

## 5. Steady-state data flow (50 Hz)

```
VL53L1X GPIO1 (data-ready) ──EXTI PD0── ISR: tk_sig_sem(tof_drdy_sem)
                                              │
sensor_task: tk_wai_sem(tof_drdy_sem) ◄───────┘        (sensor paces the loop)
  → L2: read ToF result regs (distance, range status) via i2c_xfer
  → L2: clear ToF interrupt (register write)
  → L2: MPU6050 burst read — 6 accel bytes in ONE transaction
  → compute features → write locked 13-feature buffer
  → SCB_InvalidateDCache_by_Addr(sensor_buf, ...)      (Phase 4 call site, now live)
  → tk_sig_sem(data_ready_sem)                          (Phase 4 handshake, unchanged)
```

- **MPU6050 INT (PE9) stays unused in Phase 5.** Polling the IMU once per ToF frame is one fewer ISR path and guarantees d/accel sample alignment. PE9 EXTI is reserved for later if IMU-rate decoupling is ever needed. *(Option kept open, not exercised.)*
- **Range status must be checked every frame** — the VL53L1X reports validity per measurement; feed only valid ranges into the d(t) history, mark invalid frames (hold-last + stale counter, see §8).

## 6. Sensor configuration & the 50 Hz question (decision required)

Feature vector is locked at 50 Hz (CLAUDE.md §6) → 20 ms frame period. The VL53L1X constraint chain (values recalled from UM2510/datasheet — **verify all three against UM2510 before locking**): timing budget from a discrete set with **15 ms available only in Short distance mode**; inter-measurement period ≥ timing budget (plus small overhead); Short mode max range ≈ 1.3 m.

- **Option A — true 50 Hz:** Short mode, TB = 15 ms, IMP = 20 ms. Cost: ranging ceiling ≈ 1.3 m. Hazard rule fires < 80 cm, and the d(t)…d(t−9) window (200 ms) tracks the approach from ~1.3 m at walking speed (1.4 m/s covers 28 cm per window) — workable but early-warning headroom is thin.
- **Option B — 33 Hz sensor, 50 Hz spec broken:** Medium/Long mode (TB 33 ms), range to ~3–4 m, but the locked feature spec and Edge Impulse training rate must both change. Ripples into §6 of CLAUDE.md and the model pipeline.
- **Recommendation: A.** The spec is locked, the deadline is real, and the hazard semantics (< 80 cm, closing) live entirely inside Short-mode range. Bank Option B as a documented tradeoff for the contest write-up ("range ceiling chosen for update-rate determinism").

**MPU6050 init sequence** (registers recalled from RM-MPU-6000A — **verify against register map rev 4.2 before coding**): wake from sleep via PWR_MGMT_1, WHO_AM_I sanity check (expect 0x68), accel FS ±4 g (walking-impact headroom), DLPF enabled to tame the 50 Hz finite-difference noise. Gyro unused — do not read it (bus time is cheap, but the feature vector doesn't include it and scope discipline applies).

### Feature computation — must be bit-consistent with training

v and a are derived, and **the firmware formula must exactly match the Edge Impulse preprocessing**, or the deployed model sees a distribution it never trained on (silent accuracy loss — cousin of Red Zone #6).

- Naive v = (d(t−1) − d(t))/20 ms amplifies mm-level ToF noise: ±3 mm frame noise → ±15 cm/s velocity noise, on the same order as the 20 cm/s hazard threshold. **Unacceptable.**
- **Design: least-squares slope over the last 5 samples (100 ms window)** for v; a = same estimator over the v history. Sign convention: **positive v = closing**. Implement once in a tiny shared C file compiled both into firmware and into the training-data generator script — one source of truth.

## 7. Bus bandwidth budget (formula → substitution → result)

Per 20 ms frame, Fast-mode 400 kHz, ~9 bits/byte on the wire, payload ≈ ToF result block + interrupt-clear + IMU burst ≈ 30 bytes + 3 transaction overheads (~5 bytes addressing/start/stop each):

t_bus = bits / f = (45 bytes × 9) / 400 000 Hz = 405 / 400 000 ≈ **1.0 ms of 20 ms → 5 % bus utilization.**
At 100 kHz fallback: ≈ 4.1 ms → 20 %. **Both fit comfortably** — bus speed is not a schedulability risk; choose based on signal integrity only.

## 8. Error handling & recovery (design now, not after the first lockup)

| Fault | Detection | Response |
|---|---|---|
| NACK / transfer error | HAL error callback → error flag + `tk_sig_sem` | retry once; on second failure escalate to bus recovery |
| Bus stuck (SDA held low) | timeout on `i2c_done_sem` | classic recovery: reconfigure SCL as GPIO, clock out 9 pulses, STOP, re-init I2C peripheral |
| ToF gives invalid range status | per-frame status check | hold last valid d, increment `stale_frames` |
| Stale data threshold | `stale_frames > 10` (200 ms) | set `data_valid = false` in the result path; inference/hazard treat as no-hazard **plus** fault counter surfaced by heartbeat task. (User-facing fault haptics = later phase; Phase 5 only guarantees we never act on dead data.) |

All recovery code lives in the Priority 3 task context. The hazard path never blocks on sensor failure — the paired-semaphore design already guarantees it only ever consumes completed results.

## 9. Exit criteria

- [ ] Bus scan evidence: 0x29 + 0x68/0x69 found, nothing unexpected, AD0 question closed in CLAUDE.md
- [ ] WHO_AM_I (MPU6050) and VL53L1X model-ID register read back correct via the L1 primitive — first proof the sem-wrapped path works
- [ ] ToF streaming: measured frame rate = 50 Hz ± jitter, logged via `tk_get_otm()` over ≥ 60 s; jitter stats recorded
- [ ] Ruler test: static targets at 20/50/80/120 cm, logged mean & σ per distance — accuracy evidence for the write-up
- [ ] IMU flat-table test: az ≈ +1 g, ax/ay ≈ 0, correct sign conventions documented
- [ ] Walking-approach test: hand/board approach triggers the stub hazard → PD6/PE7 activity on LA — full real-data chain sensor→inference→hazard
- [ ] Preemption re-measured under real I2C DMA load: **max Δt(D0→D1) still < 1 ms** (the Phase 4 evidence is void if DMA/IRQ traffic changed the tail — re-run the 100-event campaign)
- [ ] 30 min soak: zero unrecovered bus errors; recovered-error count logged
- [ ] No blocking HAL I2C call anywhere in the tree (`grep -rn "HAL_I2C_Master_Transmit(\|HAL_I2C_Master_Receive("` returns only `_DMA`/`_IT` variants) — mechanical red-zone audit
- [ ] CLAUDE.md updated: `DEVCNF_USE_HAL_IIC=1` recorded as done, AD0 resolved, sensor config (mode/TB/IMP) locked

---
*After exit: Phase 5.5 = DRV2605L init (I2C config from Priority 3 at init per CLAUDE.md §2, EN/pattern control from Priority 1) — a thin addition on the now-proven L1/L2 stack. Then Phase 6 = Edge Impulse data collection using this pipeline's logged output.*
