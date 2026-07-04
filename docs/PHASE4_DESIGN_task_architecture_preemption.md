# Phase 4 Design — Task Architecture & Preemption Verification

**Project:** Haptic-Sense · TRON Forum Contest 2026
**Prerequisite:** Phase 3 exit criteria passed (own app boots from Flash Boot; iteration loop known; priorities 1–3 + 10 confirmed free in `sysdef.h`).
**Status:** DESIGN — not yet implemented.

---

## 1. Objective

Stand up the full four-task µT-Kernel architecture **with synthetic data — zero I2C, zero NPU** — and produce hardware-verified evidence that the hazard path preempts everything else with **deterministic sub-millisecond latency (< 1 ms, hardware-verified)**. (Never "zero-latency" — CLAUDE.md §3.)

This phase de-risks Red Zones #3 (preemption measurement), #4 (inference-buffer race), and #7 (priorities) *before* real sensors and the real model add their own failure modes. When Phase 5 swaps synthetic data for I2C DMA and Phase 6 swaps the stub classifier for NeuralART, the task skeleton, semaphores, and instrumentation are already proven.

## 2. Scope

**IN:** 4 tasks, paired-semaphore pattern, GPIO + DWT instrumentation, logic-analyzer measurement campaign, cache-maintenance call sites (placed, inert).
**OUT:** All I2C (`DEVCNF_USE_HAL_IIC` **remains 0** — the flip to `1` in `config_bsp/stm32_cube/config_bsp.h:40` is the first action of Phase 5). NPU inference. DRV2605L. Any real sensor.

## 3. Task set (per CLAUDE.md §3, with Phase-4 stub bodies)

| TK_PRI | Task | Phase-4 stub body | Permanent hard constraints |
|---|---|---|---|
| 1 | `hazard_task` | wait `result_ready_sem` → read result → if hazard: set PD6 (TIMING_D1) + drive DRV_EN pin PE7 pattern (no driver attached yet — scope/LA observes it) → sig `result_free_sem` | GPIO only. NO I2C, NO printf, NO blocking I/O — ever |
| 2 | `inference_task` | wait `data_ready_sem` → stub classifier: `hazard = (d < 800mm && v_close > 20 cm/s)` (the §6 label rule as code) → paired-sem handshake to hazard → set PH5 (TIMING_D0) at hazard-signal instant | NO I2C, NO printf. Body later replaced by NeuralART call — interfaces unchanged |
| 3 | `sensor_task` | synthetic generator @ 50 Hz via `tk_slp_tsk(20)`: distance ramps (approach scenarios), fills the same 13-feature buffer layout locked in §6 → sig `data_ready_sem` | ALL future I2C lives here, DMA only. Synthetic path must write through the **identical buffer** the DMA path will use |
| 10 | `heartbeat_task` (from Phase 3) | demoted to slow status printer: 1 Hz `tm_printf` of counters (hazard events, inference count, worst-case Δt from DWT) | Only task allowed printf |
| 15 | idle | `tk_slp_tsk`/WFI loop | **WFI only, NEVER Stop mode** (wake latency breaks < 1 ms guarantee) |

Producer/consumer naming (mandatory per CLAUDE.md): `sensor_task` (P) → `data_ready_sem` → `inference_task` (C); `inference_task` (P) → `result_ready_sem`/`result_free_sem` → `hazard_task` (C).

## 4. Synchronization design

### 4.1 Paired semaphores (Red Zone #4 — implement BEFORE any task body)

```c
/* runs in: system init, before tk_sta_tsk of tasks 1-3 */
ID data_ready_sem;    /* init 0: sensor(P) -> inference(C) */
ID result_free_sem;   /* init 1: hazard returns the slot   */
ID result_ready_sem;  /* init 0: inference -> hazard        */
```

Inference: `tk_wai_sem(result_free_sem,1,TMO_FEVR)` → write result struct → `tk_sig_sem(result_ready_sem,1)`.
Hazard: `tk_wai_sem(result_ready_sem,1,TMO_FEVR)` → read → `tk_sig_sem(result_free_sem,1)`.

**Design decision — data_ready as counting-sem-as-event:** sensor signals `data_ready_sem` each 20 ms frame; inference drains it with `TMO_FEVR`. If inference ever falls behind (later, with real NPU latency), the count grows — heartbeat task reports `tk_ref_sem` count as an overload canary.

### 4.2 Cache maintenance (placed now, exercised in Phase 5)

The call site goes in `sensor_task` at the point where, in Phase 5, the I2C DMA-complete path hands off the buffer:

```c
/* task: sensor_task, TK_PRI 3 — ONLY CALL FROM PRIORITY 3 SENSOR TASK */
SCB_InvalidateDCache_by_Addr((uint32_t*)sensor_buf, sizeof(sensor_buf));
tk_sig_sem(data_ready_sem, 1);
```

With synthetic (CPU-written) data the invalidate is harmless but **keep it in from day 1** so Phase 5 cannot forget it. All buffers static, no malloc (CLAUDE.md §7).

## 5. Preemption instrumentation (Red Zone #3)

All guarded by `#ifdef DEBUG_TIMING`.

### 5.1 GPIO method — contest evidence

| Signal | Pin | Arduino | Set by | Source |
|---|---|---|---|---|
| TIMING_D0 | PH5 | D4 | `inference_task` at the instant it signals `result_ready_sem` | CLAUDE.md §2/§3 |
| TIMING_D1 | PD6 | D7 | `hazard_task` first instruction after wake | CLAUDE.md §2/§3 |

Δt(D0↑ → D1↑) on the logic analyzer = scheduler preemption + context-switch latency. LA: 24 MHz sigrok clone → 41.7 ns resolution, ~4 orders of magnitude finer than the 1 ms bound — ample. **Pre-flight: `sigrok-cli --scan` must list the device** before the campaign (CLAUDE.md §3).

### 5.2 DWT method — firmware self-measurement

`DWT->CYCCNT` snapshot at D0-set and D1-set; `cycles / 600000 = ms` at 600 MHz (CLAUDE.md §3). Heartbeat task prints running min/max/mean. DWT and LA must agree within measurement error — disagreement is itself a bug to chase.

**Unverified item:** on Cortex-M55, DWT->CYCCNT typically requires `CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk` plus `DWT->CTRL` CYCCNTENA before counting; confirm the exact enable sequence against the bundled CMSIS headers in `STM32Cube_FW_N6/` (and whether TrustZone/secure state gates DWT access in our FSBL-configured world) before relying on it. The GPIO/LA method is the primary evidence; DWT is corroboration.

### 5.3 Load generation

Preemption latency measured on an idle system is meaningless. During the campaign, heartbeat task runs a busy-loop "chatter" mode (`#ifdef DEBUG_TIMING`) so priority-1 wakeups must genuinely preempt running lower-priority code. Also test the worst realistic case: hazard fires **while inference stub is mid-computation**.

## 6. Measurement campaign protocol (contest evidence package)

1. `sigrok-cli --scan` — record output.
2. Synthetic scenario: distance ramp crossing the hazard rule ≥ **100 times** over a run (sensor stub cycles approach/retreat).
3. Capture Δt(D0→D1) for all events in PulseView; export CSV.
4. Report **worst case**, not average: `max Δt`, plus min/mean/σ. Pass = `max Δt < 1 ms` across ≥ 100 events under chatter load.
5. Repeat with chatter off (baseline) — both datasets go in the contest write-up.
6. Screenshot one representative PulseView trace, annotated. Archive CSVs + DWT-reported stats in `docs/evidence/phase4/`.

**Why worst-case:** app executes from XSPI RAM (CLAUDE.md §1); a cache miss on the hazard path costs an XSPI fetch. Average latency will look great; the contest claim must survive the tail. If `max Δt` violates the bound, the ranked suspects are: (a) cache-miss on hazard path code — consider pinning hot path (investigate ITCM/fast-RAM options in the N6 memory map — *verification needed against RM0486 before claiming feasibility*), (b) a section with interrupts masked in BSP/HAL code, (c) idle entering a deeper sleep than WFI.

## 7. Pre-implementation checklist

- [ ] `sysdef.h` priority verification comment block written (carried from Phase 3)
- [ ] Semaphores created before any `tk_sta_tsk` of tasks 1–3
- [ ] Every I2C-future function stubbed with header comment `// ONLY CALL FROM PRIORITY 3 SENSOR TASK`
- [ ] Every code block labeled with task + TK_PRI (CLAUDE.md §7)
- [ ] All instrumentation inside `#ifdef DEBUG_TIMING`
- [ ] Result struct + 13-feature buffer layout match §6 of CLAUDE.md exactly (so Phase 5/6 swap bodies, not interfaces)

## 8. Exit criteria

- [ ] All 4 tasks + idle running; heartbeat counters advance; no watchdog/hard fault over a ≥ 30 min soak
- [ ] Paired-semaphore handshake verified: hazard never reads a half-written result (torture test: inference writes a canary pattern, hazard validates it every event)
- [ ] **max Δt(D0→D1) < 1 ms over ≥ 100 hazard events under load** — LA evidence archived
- [ ] DWT numbers corroborate LA within error
- [ ] Synthetic hazard scenario produces PE7 (DRV_EN) activity visible on LA — the full chain sensor→inference→hazard→actuator-pin proven before any real hardware on the far end
- [ ] CLAUDE.md updated: Red Zones #3, #4, #7 marked RESOLVED with evidence pointers

---
*After exit: Phase 5 = flip `DEVCNF_USE_HAL_IIC` to 1, VL53L1X + MPU6050 DMA drivers into `sensor_task` — body swap only, architecture frozen here.*
