# DATA COLLECTION PROTOCOL — BLOCK 6
**Written 2026-09-15/16, after the Block 6+ decision session.** This is the formal write-up of
G-4 (`RISK_ANALYSIS_20260830.md`), required before any training sample is collected per
`BLOCK6_TO_SUBMISSION_TECHNICAL_PLAN_20260915.md` §1. Every parameter below was decided in that
session — see the project decision log for the reasoning behind each choice; this doc states the
protocol itself, in the form a collection session actually needs to follow it.

**Do not begin real collection until the label-shift value k (§3) is filled in.** Everything else
here is ready now.

**Firmware for §1's k-determination test is drafted separately** — see
`RAWLOG_K_TEST` in `app_tasks.c` (added 2026-09-16, guarded by a compile-time `#define`, off by
default). Build with `RAWLOG_K_TEST` defined, run the ~5 min session over UART, then set it back
to 0 before building the real G-2 CSV logger.

**Feature vector grew from 15 to 17 (added 2026-09-16).** Two new features, `amb` (VL53L1X
Ambient) and `spad` (VL53L1X SigPerSPAD — return signal strength, correlates with target
reflectance), ride along on the exact same accepted ToF read that already produces distance —
zero extra I2C cost. This is what "object detection" means for this project: not camera-based
detection (out of scope, see `CLAUDE.md`'s locked MB1854/VL53L5CX decision), but surface/material
discrimination from the ToF's own signal quality — directly useful given the protocol already
spans 4 surfaces with different reflectance (§4). **Not part of the hazard label** — the label
stays `d < 800mm && v > 20cm/s`, unchanged; these are context features for the classifier only.
`RAWLOG_K_TEST`'s print line now includes `amb=`/`spad=` too, so the same k-determination bench
session doubles as the data needed to pick real normalization constants for these two features —
`NORM_AMBIENT_SCALE`/`NORM_SIGSPAD_SCALE` in the firmware are currently **unverified
placeholders** and must be replaced with real numbers from that session before `CSV_LOG_ENABLE`
is trusted for actual collection.

---

## 1. Prerequisite — the k-determination test (run this first, separately)

Before any labeled sample is collected, run a short (~5 min) raw-logging session: distance +
velocity only, no label, no CSV feature vector — just enough to see how many frames ahead of
contact the closing-velocity signal becomes clearly distinguishable from "not approaching."
Cover a few approach speeds during this session (slow/normal/fast, per §2's speed protocol) so
the chosen k holds across all three. Pick k from what that data actually shows; starting
hypothesis is k≈10 (~200ms at ~47-50Hz), but confirm or adjust it here, don't assume it.

Once k is picked, fill in §3 below and only then start real collection.

---

## 2. Approach speeds — metronome-paced

Three speeds, each paced against a metronome app rather than a marked-distance-and-count method,
for tighter reproducibility across sessions and across surfaces/angles:

| Speed | Metronome tempo (approach cycles/min) | Notes |
|---|---|---|
| Slow | pick and record a fixed BPM before the session starts | Record the exact BPM used per speed in the session log (§6) — it's the reproducibility anchor |
| Normal | " | " |
| Fast | " | " |

Pick specific BPM values immediately before the first real collection session (once hands are on
the bench and a natural slow/normal/fast feel is established), write them here or in the session
log, and hold them fixed for the rest of Block 6.

---

## 3. Label definition

Hazard label for frame *t* = the existing hazard rule (`d < 800mm && v > 20cm/s`) evaluated at
frame *t+k*, where **k = _____ frames (_____ ms), confirmed empirically per §1 above.**

This requires a k-frame-ahead ring buffer in the logger (holding the last k raw `(d, v)` pairs)
so the label for row *t* is written once row *t+k*'s distance/velocity are actually known — the
label necessarily lags k frames behind the feature row it's attached to.

---

## 4. Surfaces — 4 total

| Surface | Why it's included |
|---|---|
| Bare hand | What the actual demo uses — the primary target |
| Dark sleeve / fabric | Low IR reflectance, a real-world case (long sleeves, gloves) |
| Light wall / clipboard | High IR reflectance, tests the other end of the VL53L1X's response range |
| Laptop or phone (hard plastic/glass) | Plausible object near a judged demo table — distinct reflectance from all three above |

Collect across all four surfaces at each speed (§2) and each angle (§5) — don't let one surface
dominate the session just because it's the easiest to set up.

---

## 5. Approach angles — straight-on + 4 off-axis (5 total)

Straight-on, plus four off-axis approaches (left, right, and two others — e.g. slightly
above/below straight-on, or a second pair of diagonals; pick whatever combination is physically
easy to repeat on the bench and record exactly which four in the session log). The VL53L1X is
single-zone with no directional sensitivity, so this isn't about direction detection — it's about
varying the effective reflecting surface geometry the sensor sees as a hand/object approaches at
different angles.

---

## 6. Distance sweep — full sensor range, 5cm to 1.3m

Sweep the full usable range on both sides of the 80cm label threshold: from ~5cm (very close,
clearly-hazard territory) out to ~1.3m (the VL53L1X's short-ranging mode ceiling, clearly-safe
territory). Don't concentrate samples only near 80cm — the model needs clean examples at both
extremes as well as the ambiguous zone around the threshold.

---

## 7. Sample count / class balance — no fixed target

No fixed per-class count. Collect as much as the Block 6 window allows, biasing toward covering
every surface × speed × angle combination at least once over hitting a specific raw count —
coverage across the parameter grid matters more than volume given the tight schedule to 30 Sep.

---

## 8. Frame validity / exclusion policy

Every logged row carries `imu_valid`/`tof_valid` columns (extended to the FRESH/HELD/INVALID
tri-state per the Block 6 plan's §1.5 decision, if that firmware lands before collection starts;
otherwise the existing plain boolean). Non-fresh rows are **excluded offline** during Edge
Impulse data prep, not filtered in firmware — cost is negligible (a handful of rows per session)
and this keeps the firmware-side logger simple.

---

## 9. Lighting

Note the room and lighting condition for each session in the log (§10) — the actual demo room in
Tokyo is unknown, so this becomes an honestly-scoped line on the printed instruction card later
("works best under normal indoor lighting" or similar), not a hard requirement to control for
here. Vary it across sessions if convenient (different times of day, different rooms) rather than
controlling it tightly — more lighting diversity in training data is free robustness.

---

## 10. Freeze after this block

Once real collection starts: do not touch `DLPF_CFG`, `AFS_SEL`, the physical mounting, or the
feature/normalization constants. Any of those changes invalidates already-collected samples
silently. Watch the live heartbeat/I2C counters throughout every session exactly as during the G5
soaks — a fault mid-collection is a data-quality problem, not just a bus-health one.

**IMU-6 gate, immediately before sample one:** flat bench, confirm Z reads within the established
as-built baseline (~1114-1125mg total magnitude, already characterized and accepted as this
board's own fixed offset) before starting the first real session.

---

## 11. Session log — fill in as you go

| Date/time | Speed BPMs used | Surfaces covered | Angles covered | Lighting/room | Faults during session | Notes |
|---|---|---|---|---|---|---|
| | | | | | | |

Log this the same way every other gate result has been logged this project (project decision
log, `hardware-and-decisions.md`).
