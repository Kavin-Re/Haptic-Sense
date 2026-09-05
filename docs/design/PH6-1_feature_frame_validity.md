# PH6-1 — P3→P2 Feature-Frame Handoff Spec

Status: **CLOSED 2026-09-05** (implementation landed same day; see commit cited at
the bottom). Closes audit findings **F-6d** (`redzone_audit_crosscutting_2026-07-11.md`)
and **M-4** (`redzone_audit_addendum_mpu6050_2026-07-11.md`), tracked as ledger item
**PH6-1** in `docs/PROJECT_DEFENSE.md` §"Phase 6 gates & documentation actions"
(previously the single item blocking Phase 6 implementation start).

## 1. What this closes

`docs/design/mpu6050_port_design_v1.md` §7 recorded the constraint but explicitly
deferred the design: *"the staleness indicator MUST travel inside the
semaphore-protected P3→P2 feature-frame buffer (e.g. a validity field in the frame
struct), never as a bare cross-task flag."* `docs/design/M3_EDIT_SPEC_MPU6050_EXTI_SIGSEM.md`
recorded the same constraint against the (unimplemented, never-shipped) EXTI/sigsem
IMU wake design. This doc is the actual spec, written against the polling design
("Option A") the project ships, and the code implementing it landed in the same
commit as this doc.

## 2. Buffer owner

`feature_frame` (`firmware/Appli/Core/Src/app_tasks.c`) is written exclusively by
`sensor_task` (TK_PRI 3) and read exclusively by `inference_task` (TK_PRI 2).

No new lock was added. The existing protection argument — informally documented as
a comment on the old `feature_buf` declaration since Phase 4 — is formalized here:
because the producer's priority (3) is numerically *lower* (µT-Kernel: lower number
= higher priority) than the consumer's (2), `sensor_task` can never preempt
`inference_task` mid-read. `inference_task` only reads the frame after
`data_ready_sem` is signalled once per cycle, so there is no window in which a
partially-written frame is visible to the consumer. This is the same "Red Zone #4"
paired-semaphore discipline used elsewhere in the task set, applied here via
priority ordering instead of an explicit lock because the ordering already
guarantees exclusivity.

**If this priority relation ever changes** (e.g. `inference_task` is ever moved to
a lower priority than `sensor_task`, or a third writer is added), this buffer must
be re-protected like `result_slot` — paired semaphores, not priority ordering.
That trigger condition is now written down explicitly (it previously only existed
as an unstated assumption).

## 3. No-DMA statement

`feature_frame` is written by the CPU only, on every path that touches it:

- `sensor_fill_frame()` (synthetic distance ramp + velocity/acceleration estimate)
- the MPU6050 accel merge (`mpu6050_get_stats()` → `feature_buf[FEAT_IDX_A{X,Y,Z}]`)
- the VL53L1X real-distance merge (landed 2026-09-05, same day as this doc), once `vl53l1x_service()`'s
  result replaces the synthetic ramp

Nothing DMAs into or out of `feature_frame`, on this board or in any planned phase.
This is unlike the project's three actual DMA buffers (`drv_buf`, `gate_buf`,
`mpu_buf`, `port_buf`), each of which carries its own cache-maintenance call at its
own call site.

Before this fix, `app_tasks.c`'s `sensor_task` loop called
`SCB_InvalidateDCache_by_Addr()` unconditionally on `feature_buf` every cycle. Per
`PROJECT_AUDIT_20260903.md` §3.3 this was wrong regardless of D-cache being off
today (CLAUDE.md Decision 2): an invalidate-without-writeback on a CPU-only-written
buffer discards just-written, not-yet-flushed data the instant D-cache is turned
on — a latent defect, not yet a live one. **That call has been removed.** No
replacement cache-maintenance call is needed because no DMA engine ever touches
this buffer.

## 4. Alignment

`feature_frame` is now declared `__attribute__((aligned(32)))`, matching every
DMA-touched buffer in the tree (`drv_buf[32]`, `gate_buf[32]`, `mpu_buf[32]`,
`port_buf[PORT_BUF_SZ]`). This is defensive, not required by DMA (§3 established
there is none): the audit's original concern (§3.3) was that an *unaligned*
buffer sharing a cache line with an unrelated static could have a cache op on one
spill onto the other. Since the incorrect invalidate call is gone, that specific
risk is moot for this buffer today — but alignment costs nothing here and keeps
`feature_frame` consistent with the rest of the tree's static-buffer convention if
a future maintainer ever adds a cache op nearby without re-reading this doc.

## 5. Validity field inside the protected buffer

```c
typedef struct {
	W	feat[FEAT_COUNT];
	UW	imu_valid;	/* mirrors mpu6050_stats_t.last_valid, PH6-1 */
	UW	tof_valid;	/* 1 only on a fresh accepted ToF frame      */
} feature_frame_t;

static feature_frame_t feature_frame __attribute__((aligned(32)));
#define feature_buf	(feature_frame.feat)
```

`imu_valid` and `tof_valid` live **inside** `feature_frame_t`, alongside `feat[]`,
not as separate file-static bare flags. They are covered by the exact same
priority-ordering protection argument as `feat[]` (§2) because they are the same
struct — there is no separate handoff path for validity that could race against
the feature data it describes. This is the concrete fix for the F-1-class risk
`mpu6050_port_design_v1.md` §7 named by analogy (`drv2605l_port_design_v1.md` §2,
R-EN-3/R-EN-4's lost-update shape): a bare cross-task validity flag can be read by
the consumer at a different instant than the feature data it was meant to qualify,
even under a protection scheme that is otherwise correct for the data itself.

The `feature_buf` macro (`#define feature_buf (feature_frame.feat)`) keeps every
existing `feature_buf[i]` call site — in `sensor_fill_frame()` and
`inference_task_fct()` — compiling unchanged; nothing outside the struct
definition and the two producer sites (§6) needed to change.

### 5.1 imu_valid — deliberately NOT gated on INT/DATA_RDY

The original Option A design (`mpu6050_port_design_v1.md` §4) proposed staleness
gated on the MPU6050's INT pin going low. This spec does **not** use that signal,
because this session's own hardware testing (2026-09-04/05, `app_mpu6050.c`
service-loop comment) found DATA_RDY unreliable on this specific board: it
asserted correctly for the first ~120 samples after arming, then stopped
asserting entirely while the sensor continued producing valid accel data for the
full remainder of a 10-minute capture. Gating validity on that pin would have
mislabeled every one of those later, genuinely-good samples as invalid.

Instead, `mpu6050_stats_t.last_valid` (`app_mpu6050.h`/`.c`) is computed each
`mpu6050_service()` call from three things this session actually validated as
reliable:

1. the device is armed (`mstats.armed`);
2. the 14-byte accel burst read succeeded this cycle (no `rderr`);
3. a live `PWR_MGMT_1` re-check, performed every cycle, confirms the part is
   awake right now (`pwrmgmt_live_rb == MPU_PWR_WAKE_PLL_XG`) — this check
   already existed (added 2026-09-04 for the brownout/sleep-reversion finding)
   and is reused here rather than adding a fourth signal.

`sensor_task`'s MPU6050 merge copies this verdict into the protected frame the
same cycle it copies `ax_mg`/`ay_mg`/`az_mg`:

```c
mpu6050_service();
{
	const mpu6050_stats_t *m = mpu6050_get_stats();
	feature_buf[FEAT_IDX_AX] = m->ax_mg;
	feature_buf[FEAT_IDX_AY] = m->ay_mg;
	feature_buf[FEAT_IDX_AZ] = m->az_mg;
	feature_frame.imu_valid = m->last_valid;
}
```

INT/DATA_RDY is still polled and counted (`mstats.stale`) as a diagnostic signal
only — it does not gate the read and does not feed `imu_valid`. If a future board
revision or a re-soldered INT line turns out to be reliable, that is a reason to
revisit this criterion, not a reason it was designed around in the first place.

### 5.2 tof_valid — real ToF distance, landed same day as this doc

`sensor_fill_frame()` now calls `vl53l1x_service()` itself (moved out of the
main `sensor_task` loop, same call-site convention as `mpu6050_service()`) and
merges its result the same cycle: when `vl53l1x_stats_t.init_result == E_OK`
(the sensor is ranging) and `frames` has advanced since the last cycle checked,
`d_mm` is set from `tstats.last_mm` and `tof_valid = 1`.

**Correction versus this doc's first draft:** the original plan here was to gate
freshness on `tstats.last_status == 0`. Reading `vl53l1x_service()`'s actual body
(`app_vl53l1x.c`) before implementing this showed that assumption was wrong —
`last_status`'s own field comment says "most recent NON-ZERO result.Status": it
is only ever written on the drop-status path and is never reset to 0 on a good
frame, so it cannot distinguish a fresh good frame from an old one. `frames` is
the only field the accepted path updates, so comparing it against a per-cycle
last-seen value is the correct (and only) freshness signal. Implemented that way
instead — see the landing commit's message for the full note.

When `init_result == E_OK` but `frames` has NOT advanced this cycle (not ready
yet, or a dropped frame), `d_mm` is held over at its last accepted value and
`tof_valid = 0` — same discipline as the MPU6050 rderr hold-over path. When
`init_result != E_OK` (pre-solder / pre-init), the original synthetic
approach/retreat ramp still drives `d_mm` unchanged, and `tof_valid = 0`, so
nothing downstream can mistake synthetic data for a real, valid ToF reading.

One noted, self-clearing transient: if the sensor comes online mid-run, the
first accepted frame can jump `d_mm` in one step from wherever the synthetic
ramp had wandered to. This is expected — it clears within `VLSQ_WINDOW` frames
like any other history refill, the same way MPU6050 arming mid-run does.

## 6. What PH6-1 explicitly does not do

The stub classifier in `inference_task_fct()` does not read `imu_valid` or
`tof_valid` yet — today it only reads `feat[0]` and `feat[FEAT_IDX_VCLOSE]`, and
accel data isn't part of the hazard decision at all yet. Deciding how the
classifier should react to an invalid frame (skip it, hold the last result, fall
back to a reduced feature set) is Phase 6 classifier work, not this handoff spec.
PH6-1's job was only to guarantee the transport exists, is race-free, and is
populated honestly by the producer — consuming it is scoped separately.

## 7. Verification

- Manual review: struct layout, macro expansion at every `feature_buf[i]` call
  site, single (not duplicated) `vl53l1x_service()` call site, and brace/paren
  balance across all four touched files (`app_tasks.c`, `app_mpu6050.c`,
  `app_mpu6050.h`, this doc's own two landing commits) — checked clean.
- No cross-compiler is available in this working environment to produce a binary
  build check; `arm-none-eabi-gcc` build via STM32CubeIDE (or the project's own
  toolchain) on the actual dev machine is the outstanding verification step
  before this is soaked on hardware.
- Functional/soak verification deferred to that same build-confirmed pass:
  `imu_valid` should read 0 during a forced-sleep or read-failure window and 1
  otherwise; `tof_valid` should read 1 only on cycles where `d(t)` actually
  moved by a real accepted frame, and `frames`/`d(t)`/`tof_valid` should all
  advance together once the ToF sensor is soldered and initialized.

## 8. Sources

- `docs/PROJECT_DEFENSE.md` — F-6d, M-4, PH6-1 ledger rows (updated in place by
  this doc's landing commit)
- `docs/design/mpu6050_port_design_v1.md` §7 (updated in place)
- `docs/design/M3_EDIT_SPEC_MPU6050_EXTI_SIGSEM.md` (unimplemented Option B design;
  its M-4 constraint statement is the one this doc closes)
- `docs/audits/redzone_audit_crosscutting_2026-07-11.md` (F-6d origin)
- `docs/audits/redzone_audit_addendum_mpu6050_2026-07-11.md` (M-4 origin)
- `claude/PROJECT_AUDIT_20260903.md` §3.3 (the cache-invalidate defect this doc
  fixes as a side effect of formalizing the buffer)
- This session's hardware findings on MPU6050 INT/DATA_RDY unreliability
  (`app_mpu6050.c` service-loop comment, 2026-09-04/05)
