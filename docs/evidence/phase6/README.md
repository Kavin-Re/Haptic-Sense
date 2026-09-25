# Phase 6 evidence: full real pipeline, 2026-09-19

## `npu_live_soak30_20260919_0712.log.gz`: 30-minute soak

Real sensors (VL53L1X, MPU6050, DRV2605L), trained model on the NPU path
(`HAZARD_CLASSIFIER_USE_NPU`), `HAZARD_NPU_DIAG` on (it logs a CPU-vs-NPU
comparison every 50 frames and on every disagreement). Continuous capture;
`up_ms` rises monotonically with no reset.

Figures below were re-derived from the log on 2026-09-25:

| | |
|---|---|
| Duration | 29.8 min (`up_ms` 14,488 → 1,800,480) |
| Frames | 48,242; `frames == inf` on all 1,487 heartbeats (~27 Hz) |
| Handshake integrity | `canary_err = 0`, `q = 0`, `qovr = 0` on every heartbeat |
| Hazard events | 1,053 |
| Preemption latency, in-firmware DWT | 43,723 events in 1,494 windows; **worst 3,349 cycles = 4.19 µs** at 800 MHz, best 2,639 cycles = 3.30 µs |
| NPU output | varies with the scene (`pre`/`postq`/`poste`). This is the hardware confirmation of the 0x71000000 weight-address fix (`docs/NPU_SATURATION_DEBUG_FINDINGS.md`) |
| CPU vs NPU decisions | 104 disagreements out of 1,056 logged comparisons (mismatches are always logged, matches only every 50th frame, so this is not a disagreement rate) |

How the two latency figures relate: **3.375 µs** (`docs/evidence/phase4/`)
was measured with a logic analyzer, pin to pin, in the Phase 4 campaign
(synthetic sensor data, no classifier yet). **4.19 µs** was measured here by
the firmware's own DWT cycle counter, on the full real pipeline. The two use
different methods and different builds; both are well under 1 ms.

This soak ran before the 2026-09-19 safety-net change and before the
diagnostic defines were stripped, so it is the closest measured build to the
shipped one, not the shipped binary itself.

## `live_v10_validate_20260919_1113.log.gz`

The capture cited in `app_tasks.c` (search "SAFETY-NET FALLBACK"). Its feature
vectors, scored offline, showed the model going quiet at point-blank
fast approaches, which is what led to the rule-based fallback being ORed in.
