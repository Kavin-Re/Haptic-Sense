# BLOCK 6 → SUBMISSION — TECHNICAL EXECUTION PLAN
**2026-09-15, evening. Written after Stage 4/5 closed on the mechanical build.**
Companion to `BLOCK6_REPLAN_20260914.md` (schedule) and `RISK_ANALYSIS_20260830.md` (the
original risk register this plan re-audits against current state). Read this before starting
Block 6 — it opens with a blocker that document did not know was still open.

---

## DECISIONS LOCKED (15 Sep 2026, late — walked through one-by-one in session)

Section 1's open decisions below are no longer open. Recorded here and in the project decision
log; the numbered items further down are kept as-written for the reasoning, but treat this box
as the answer key.

| Item | Decision |
|---|---|
| G-1 label-shift k | **Not fixed yet, on purpose.** Build a short raw-logging test session (distance/velocity only, no label) first; pick k empirically from where the signal is actually distinguishable, then build the full shifted CSV logger |
| G-2 stale/HELD frames | Log `imu_valid`/`tof_valid` as CSV columns; exclude non-fresh rows **offline**, not in firmware |
| G-3 normalization | **[-1,1] uniformly** across all 15 features — one formula, one constants table |
| G-4 sample count | **No fixed per-class target** — collect as much as the Block 6 window allows |
| G-4 speed pinning | **Metronome app** to pace slow/normal/fast approaches |
| G-4 surfaces | **4 total**: bare hand, dark sleeve/fabric, light wall/clipboard, **+ laptop/phone (hard plastic/glass)** |
| G-4 angles | **Straight-on + 4 off-axis** (5 angles total) — more coverage than the original straight-on+2 baseline |
| G-4 distance sweep | **Full sensor range, 5cm–1.3m** |
| §1.5 tri-state validity | **Build it now** — overrides this doc's original "defer" call; extend `imu_valid`/`tof_valid` to FRESH/HELD/INVALID while already touching this code for G-2 |
| Open-source license | **GPL-3.0** |
| Courier ship window | **~20-23 Sep** pickup (see companion checklist) |

Still to write down formally: the 4th surface (laptop/phone) and the final angle count need to
land in an actual `claude/DATA_COLLECTION_PROTOCOL_20260916.md` per §1.4 below — the decision is
made, the document isn't written yet.

---

## 0. WHERE THINGS ACTUALLY STAND, RIGHT NOW

**Hardware: done, gate-passed, ready.** Blocks 0–5 are closed. G1–G5 all passed. Tonight's
findings specifically: the G5 soak fault-rate climb across long runs is thermal, not a degrading
joint (cooldown re-soak confirmed); the IMU-absent cold-boot requirement is closed via a
firmware-substitution test (code-equivalent to physical disconnect, since the build cannot be
safely reopened); the real, submission-candidate binary is confirmed back on the board and
running clean. Remaining Stage 5 items are non-technical: photos, the geometry write-up, one
more host-disconnected cold-boot run, and archiving the binary. None of this blocks Block 6.

**Software/ML: NOT ready, and this was not caught until this plan.** `RISK_ANALYSIS_20260830.md`
flagged four Tier-1 risks — "could invalidate the submission" — and said to decide them **before
9 September**. Verified against the actual repo tonight (2026-09-15): **none of the four are
resolved.** This is the real content of this document. Section 1 covers it. Do not start
Block 6 sample collection until Section 1's checklist is closed.

---

## 1. STOP — CLOSE THESE BEFORE COLLECTING ANY TRAINING SAMPLE

Verified by reading `app_tasks.c` directly tonight: no `ML_LABEL_DECISION.md` or
`DATA_COLLECTION_PROTOCOL.md` exists anywhere in the repo, and there is no UART CSV logging mode
in the sensor task. The stub classifier still evaluates the hazard rule on the **current** frame
only (`app_tasks.c:330`, present-frame `d < 800mm && v > 20cm/s`) — that stub is fine, it gets
replaced by NeuralART — but nothing downstream of it defines what a **training label** should be,
and nothing exists to get a feature vector off the board into Edge Impulse. Four gaps, all from
`RISK_ANALYSIS_20260830.md`, none closed:

### 1.1 G-1 — What does "predictive" mean, concretely? (DECIDED: determine k empirically, not fixed)

The project's title claims prediction. An if-statement fed the same two inputs it's compared
against cannot predict anything — a judge who asks "why is a 15→32→16→1 network better than the
two comparisons it was trained on" has no honest answer under the current framing.

**Mechanism, unchanged:** shift the label in time. Label frame *t* with the hazard rule evaluated
at frame *t+k*, k ≈ 5–15 frames (100–300 ms at ~47–50 Hz — use this project's actual measured
frame rate, not a nominal 50 Hz). The network then infers from ~200 ms of distance/velocity
history that a hazard is *about to* exist — the one thing the present-frame if-statement provably
cannot do. Costs one column shift in the training CSV, nothing in firmware changes for it.

**Decided in session (15 Sep, late): don't pick k from a recommendation — determine it from
data.** Before building the full k-shifted CSV logger (1.2), build a small raw-logging mode that
just prints distance + velocity per frame, no label. Run a short (~5 min) test session covering
a few approach speeds. Look at how far out (how many frames before contact) the closing-velocity
signal becomes clearly distinguishable from the "not approaching" case. Pick k from that —
starting hypothesis is still k≈10 (~200ms), but confirm or adjust against this project's actual
sensor noise and frame rate rather than assuming it. Write the confirmed k and the reasoning into
the collection protocol doc (1.4) once chosen, so it can be pasted into the write-up verbatim.

### 1.2 G-2 — Firmware must be the single source of truth for the feature space (build, ~1-2 hrs)

Edge Impulse's own DSP block will happily recompute features its own way from raw signals. If
that differs at all from what the firmware computes at inference time (and it will — Edge
Impulse's default pipeline is float, the firmware's `vlsq_velocity_cm_s`/`vlsq_accel_cm_s2` are
integer/fixed-point), the model trains on one distribution and sees another at inference. This
fails silently — plausible-looking wrong outputs, not an error.

**Mitigation, unchanged from the original analysis, still correct:** compute all 15 features
on-device exactly as the runtime pipeline will, log each vector over UART as one CSV row per
frame, and upload those exact rows to Edge Impulse as **raw features, DSP block disabled/passthrough.**
The firmware becomes the only definition of the feature space that exists.

**What to build:** a UART logging mode in `sensor_task` (or a `tm_printf` line alongside the
existing `[HB]`/`[ACC]` telemetry) that prints, once per accepted frame:
```
feat0..feat9 (d_hist mm), feat10 (v_cm_s), feat11 (a_cm_s2), feat12-14 (ax,ay,az mg), label
```
`label` is the *shifted* hazard rule from 1.1, computed against a k-frame-ahead buffer — this
needs a small ring buffer holding the last k raw `(d,v)` pairs so the label for row *t* can be
written once row *t+k*'s distance/velocity are known. Straightforward, but it is new code, not
config — budget real time for it, and test it against a short synthetic run before trusting a
live collection session to it.

**Also decide: what to do with `HELD`/stale frames in the log.** `ADVERSARIAL_REVIEW_I2C_DECISION_20260905.md`
already worked this out in detail (§4) and the recommendation there is sound: log `imu_valid`/
`tof_valid` (or the richer FRESH/HELD/INVALID state, if you build it — see 1.5) as columns
alongside the features, and **exclude non-fresh rows offline**, not in firmware. Cost is
negligible (~4 rows per 2-hour session per that doc's estimate). Do this — it is already designed,
just wire the existing `imu_valid`/`tof_valid` fields into the new CSV line.

### 1.3 G-3 — Per-feature normalization before quantization (DECIDED: [-1,1] uniformly)

The 15 features span wildly different scales: distance 0–4000mm, velocity roughly ±100cm/s,
accel several thousand cm/s², accelerometer ±4000mg. A single per-tensor INT8 scale (Edge
Impulse's default) crushes the small-magnitude features — and velocity, the most discriminative
feature for a closing-hazard task, is one of the smallest.

**Decided in session: normalize every feature to [-1,1] uniformly**, in firmware, before it's
logged/fed to the model, using fixed constants chosen once and locked alongside `FEAT_COUNT` as
part of the same contract. Considered a mixed scheme ([0,1] for the always-positive distance
features, [-1,1] for the signed velocity/accel features) — rejected in favor of one uniform
formula, since each feature already carries its own scale/offset constants regardless, so a
mixed scheme adds a second code path for no accuracy benefit. Simple per-feature scale/offset is
enough — this does not need to be learned. Write the constants into the same doc as 1.1's label
decision.

### 1.4 G-4 — Write the collection protocol (one page, ~30-45 min, do this first — it's a prerequisite for 1.2's test run)

No document anywhere defines how much data, at what speeds, against what surfaces, at what
angles. The VL53L1X's return depends on target reflectance — a white wall and a dark sleeve are
different problems, and the demo will use a hand. Collection is irreversible; get this down
before frame one.

**Write `claude/DATA_COLLECTION_PROTOCOL_20260916.md` covering, concretely — decisions below are
locked, the document itself still needs to be written:**
- **Sample count/class balance: DECIDED — no fixed target.** Collect as much as the Block 6
  window allows rather than stopping at a specific per-class count, given the tight schedule to
  30 Sep. Bias toward covering every surface/speed/angle combination at least once over hitting
  a raw count.
- **Approach speeds: DECIDED — metronome app.** Pace slow/normal/fast approaches against a
  metronome tempo rather than a marked-distance-and-count method, for tighter reproducibility.
- **Angles: DECIDED — straight-on + 4 off-axis (5 angles total).** More coverage than the
  original straight-on+2 baseline — single-zone ToF has no directional sensitivity, but approach
  angle still changes the effective reflecting surface geometry.
- **Surfaces: DECIDED — 4 total.** Bare hand (what the demo uses), dark sleeve/fabric, light
  wall/clipboard, **plus a laptop/phone (hard plastic/glass surface)** as the 4th — chosen for
  plausibility in an actual judged-demo room, distinct reflectance from all three others.
- Lighting: note the room, since the actual demo room in Japan is unknown (this becomes a line on
  the printed card later — "works best under normal indoor lighting" or similar, honestly scoped).
  Not a locked decision, just documentation.
- **Distance sweep: DECIDED — full sensor range, 5cm to 1.3m.** Covers both "clearly safe" (near
  1.3m) and "clearly hazard" (very close) plus the ambiguous zone around the 80cm label threshold.
- The label-shift value k from 1.1 (determine empirically, see above) and the frame-drop/exclusion
  policy from 1.2 (log validity flags, exclude offline — see below), stated plainly once k is
  confirmed.

### 1.5 DECIDED (overrides this doc's original recommendation): build the FRESH/HELD/INVALID tri-state now

`ADVERSARIAL_REVIEW_I2C_DECISION_20260905.md` §4 fully designed a three-state validity extension
(`imu_valid`/`tof_valid` → `FRESH`/`HELD(age)`/`INVALID`, sentinel-safe per the project's own
BSS-zero rule) that is **more correct** than the current plain boolean, but verified tonight: **not
implemented** — `app_tasks.c` still carries plain 0/1 `imu_valid`/`tof_valid`. The current
boolean scheme is already safe by construction (untouched reads as invalid, the dangerous
direction is already excluded) and the velocity/accel computation already holds over stale values
correctly on an invalid ToF cycle (`app_tasks.c:489-524`). This doc's original recommendation was
to defer the tri-state as unforced scope this close to the wire.

**Overridden in session (15 Sep, late): build it now, while the UART CSV logger (1.2) is already
new code touching this exact path.** Rationale for the override: since 1.2 requires logging
`imu_valid`/`tof_valid` as CSV columns anyway, extending them to the richer three-state form at
the same time is marginal extra surface area, not a second separate change later. Scope this as
part of the same 1.2 implementation work, test it against the same short synthetic run, and don't
let it grow beyond the already-designed three-state extension in the referenced doc.

**Do not start collecting real training samples (Block 6.2 below) until 1.1–1.4 are closed.**
This is not a formality — recollecting after the fact means re-running the entire mechanical
freeze's worth of irreversibility a second time, with less runway.

---

## 2. BLOCK 6 — DATA COLLECTION (target: 16–17 Sep, now gated on Section 1)

1. Close Section 1 (protocol doc, label-shift decision, normalization constants, UART CSV logging
   built and smoke-tested on a short synthetic run).
2. **IMU-6 gate, immediately before sample one:** flat bench, confirm Z within your established
   as-built baseline (this board's own fixed offset is ~1114–1125mg total magnitude, already
   characterized and accepted — confirm it reads there again, not the generic ±80mg-of-1000mg
   spec, since your board's own deviation was already decided as permanent).
3. Collect against the protocol from 1.4. Watch the live heartbeat/I2C counters throughout the
   session exactly as you did during the G5 soaks — a fault mid-collection is a data-quality
   problem, not just a bus-health one, so don't collect blind.
4. **Freeze after this block:** do not touch `DLPF_CFG`, `AFS_SEL`, the physical mounting, or the
   feature/normalization constants once collection starts. Any of those changes invalidates
   already-collected samples silently.
5. Log the actual session parameters (sample counts per class, any protocol deviations, fault
   events during collection) to the project's decision log the same way every other gate result
   has been logged this project.

---

## 3. BLOCK 7 — MODEL, CPU FIRST (target: 18–19 Sep)

1. Edge Impulse: upload the logged CSV as **raw features, no DSP block** (per Section 1.2 — the
   firmware is the feature-space definition, EI must not recompute anything).
2. Architecture: 15→32→16→1 sigmoid, INT8 quantized. **NPU-safe ops only** —
   FullyConnected/ReLU/Sigmoid (per `CLAUDE.md` §6; never LSTM/GRU/attention — unsupported ops
   silently fall back to CPU, 10–30× slower, no error raised).
3. `generate-n6-model.sh` → `network.c` + `network_data.hex`. `user_neuralart.json` options string
   copied verbatim from the reference repo — never hand-edited (`CLAUDE.md` §5).
4. **CPU inference first.** Validate against a held-out set of saved test vectors (from the
   logged CSV, not resynthesized) before the NPU is touched at all. This is the gate that catches
   a G-2/G-3 mismatch before it becomes an NPU debugging session.

---

## 4. BLOCK 8 — NPU (target: 20 Sep) — largest unknown, hard call point

1. Enable the NPU. **Compare NPU output to CPU output on identical vectors** — divergence, or an
   `inference_ms` that looks implausibly fast/slow, means silent CPU fallback (Red Zone #6,
   `CLAUDE.md` §6).
2. Measure `inference_ms` via `tk_get_otm()`.
3. **Hard call, same day:** if the NPU isn't working cleanly by end of this block, ship the
   CPU-only pipeline and describe it accurately in the write-up. A working CPU path beats a
   broken NPU one, and there is no version of this plan where NPU debugging happens after the box
   is packed. This was already the right call in the original plan and nothing has changed that.

---

## 5. BLOCK 9 — EVIDENCE CAPTURE (target: 21 Sep) — everything that needs the board, ever

This is the last day the board exists for testing purposes before packing. Nothing here defers.

1. **Re-run the preemption campaign under NPU load.** The 3.375µs figure was measured NPU-idle;
   the latency claim isn't valid for the shipped configuration until this is redone with the NPU
   active and inferring.
2. Long soak with the **full real pipeline** (real sensors, real model, real NPU or confirmed
   CPU-fallback path) — not the synthetic generator. Save the log; this is the soak that goes in
   the write-up, not tonight's mechanical-verification soaks.
3. **Demo video.** Hand approaching, motor firing, urgency rising with closing speed. Multiple
   takes, good light — no reshoots after this.
4. **Slide deck material captured here too** (see the companion submission-logistics doc) — the
   photos/measurements/soak numbers this block produces are exactly what the required slide deck
   needs; capture with that deck in mind, not just as a checkbox.
5. Archive everything: soak logs, serial transcripts, logic-analyzer captures, build photos, the
   exact `-Trusted.bin` now on the board, and the flash procedure that produced it.
6. **Cold-boot test, host disconnected**, on the final NPU-enabled binary specifically (the one
   from tonight was on the pre-NPU binary — this needs re-verifying once the real model is
   flashed). Then leave that exact binary on the board and do not touch it again.

---

## 6. RISK REGISTER — RE-AUDITED AGAINST TONIGHT'S ACTUAL STATE

| Risk | Original status (30 Aug) | Status tonight (15 Sep) |
|---|---|---|
| Prototype doesn't survive courier | Open — was a breadboard | **Mostly closed.** Soldered build, cable-tie retention, G1-G5 passed, tap-and-shake clean. One I2C bus-bypass segment still breadboard/hot-glued per the 15 Sep glue-down decision — acceptable per that decision's own reasoning, not a re-open |
| Evidence not captured before board is unavailable | Open | On track — Block 9 (§5 above) still the mandatory capture point, not yet reached |
| Mechanical rebuild after data collection | Open | **Closed** — mechanical freeze (Stage 3-5) is done, precedes Block 6 as required |
| NPU never works | Open, largest unknown | **Still fully open** — Block 8 untouched so far |
| `I2C_REG16` silently wrong | Open | **Closed** — L1 probe and LA capture done in Block 2, long since verified clean in every soak since |
| Write direction broken | Open | **Closed** — H-D9, proven 2026-08-30 |
| **G-1 predictive label** | "Decide before 9 Sep" | **Still fully open, and it's 15 Sep.** See §1.1. Newly re-flagged tonight — was not caught by any session between 30 Aug and now |
| **G-2 feature/training match** | "Not recorded in any design doc" | **Still fully open.** See §1.2. No UART CSV logging exists |
| **G-3 INT8 normalization** | Open | **Still fully open.** See §1.3 |
| **G-4 collection protocol** | "Write before first sample" | **Still fully open** — no document exists. See §1.4 |
| G-7 frame-drop policy | Open | **In progress by decision** — boolean valid/invalid + hold-last-on-stale already implemented and safe by construction; the tri-state design (§1.5) is now scheduled to be built alongside the G-2 CSV logger, overriding this doc's original defer recommendation |
| Customs/courier delay | Open, thought tight | **Resolved, much better than feared** — real requirement is *arrival* by 30 Sep 18:00 JST, not an earlier ship date; DHL India→Japan margin is comfortable at a ~20-23 Sep ship date (see companion logistics doc) |
| Entry registration / submission format unknown | Open | **Resolved** — `CONTEST_LOGISTICS.md` §10 has the full submission form field list from the primary source |
| V-W-6 motor resistance deviation | N/A, not yet found | **New, open, low severity** — 60.4Ω vs 32.5Ω baseline, accepted deviation, ruled out as active degradation via tonight's cooldown test, cause still uncharacterized. Watch, don't block on it |

**Bottom line: the hardware track that consumed this whole session is in excellent shape and
mostly de-risked. The ML/data track that Block 6 is about to depend on has had zero attention
since 30 August and carries every one of its original Tier-1 risks, unresolved, the night before
collection was scheduled to start.** Section 1 is not optional preamble — it is the actual
critical path for the next 24 hours, ahead of anything else in this document.
