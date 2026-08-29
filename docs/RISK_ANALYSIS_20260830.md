# HAPTIC-SENSE — GAP AND RISK ANALYSIS
**2026-08-30 · adversarial pass over the whole project, not just the schedule.**
Companion to `docs/PLAN_TO_SUBMISSION_20260830.md` (v2). Read that for the plan; this is
what the plan does not cover, ranked by how much damage it does.

Labelled per CLAUDE.md §9: **evidence** (measured or read in a source), **inference**
(follows from evidence), **speculation** (plausible, unverified).

---

## TIER 1 — could invalidate the submission

### G-1. The ML label is a deterministic function of the features. *(inference, high confidence)*

CLAUDE.md §6 locks: `Hazard = distance < 80 cm AND closing velocity > 20 cm/s`, evaluated on
the current frame. The feature vector contains `d(t)` and `v`. So the network is being trained
to reproduce, at inference time, an if-statement it is being fed the inputs of.

`app_tasks.c` makes this explicit — the current stub classifier *is* the rule in C, and Phase 6
"replaces ONLY the classify step with NeuralART". A judge will ask why a 15→32→16→1 network is
better than the two comparisons it was trained on, and "it isn't" is the honest answer to the
question as currently framed.

The project title says **predictive**. A predictive model labels the future, not the present.

**Options, in order of how much they cost:**
- **(a) Shift the label in time.** Label frame *t* with the rule evaluated at *t+k* (k ≈ 5–15
  frames, 100–300 ms). The network then does something an if-statement provably cannot: infer
  from 200 ms of distance history that a hazard is *about to* exist. This is what the project
  claims to do and it costs nothing but a column shift in the training CSV. **Recommended.**
- (b) Reframe as a learned noise-robust detector — defensible if the ToF is genuinely noisy,
  but weaker, and it needs noise characterisation you have not done.
- (c) State plainly that the classifier is a demonstration vehicle for the NPU path. Honest,
  and much weaker as a contest result.

**Cost of deciding late:** the label definition is frozen the moment data collection starts
(plan Block 6). Deciding after that means recollecting. **Decide before 9 Sep.**

### G-2. On-device feature computation must match training bit-for-bit. *(inference, high confidence)*

Edge Impulse will happily apply its own DSP block and its own float pipeline. The firmware
computes features in integer arithmetic — the existing synthetic code does
`v_cm_s = (d_prev - d_mm) * 5` and `a = (v - v_prev) * 50`. If EI computes anything differently,
the model sees one distribution in training and another at inference, and the failure is silent:
plausible-looking outputs that are simply wrong.

**Mitigation:** compute all 15 features **on-device**, log the vectors over UART, and upload
those exact rows to Edge Impulse as **raw features with no DSP block**. The firmware becomes the
single definition of the feature space. This is not recorded in any design document and it
changes what Block 4 has to build — the sensor task needs a CSV-shaped UART logging mode.

### G-3. INT8 quantisation across wildly different feature scales. *(inference)*

The 15 features span: distance 0–4000 mm, velocity roughly ±100 cm/s, acceleration ±several
thousand cm/s², accelerometer ±4000 mg. A single per-tensor INT8 scale across that range
crushes the resolution of the small-magnitude features — and velocity, the *discriminative*
one for a closing-hazard task, is among the smallest.

**Mitigation:** normalise each feature to a comparable range in firmware before it enters the
model, using fixed constants that are part of the locked contract alongside `FEAT_COUNT`.
Decide with G-2, not after.

### G-4. No data-collection protocol exists. *(evidence — no document defines one)*

The VL53L1X's returned range depends on target reflectance and ambient IR. Training on a white
wall and demonstrating against dark clothing are different problems. Nothing specifies how many
samples, at what approach speeds, at what angles, against what surfaces, under what lighting.

Collection is irreversible (plan Block 6) and it is the input to everything downstream.
**Write the protocol before the first sample, not during.**

---

## TIER 2 — could cost days, or a wrong claim in the write-up

### G-5. End-to-end latency is ~50 ms, not 3.375 µs. *(evidence)*

The measured 3.375 µs is **PH5 → PD6**: semaphore signal to hazard-task wake. The user-perceived
path adds DRV2605L `t(start)` ≈ 0.7 ms and — dominating everything — ERM mechanical rise of
**40–60 ms** (SLOS854D Table 1, Library B).

`drv2605l_port_design_v1.md` already says to keep these separate, which is right. The risk is
narrative, not technical: a contest write-up that leads with "3.375 µs" next to a photo of a
vibrating motor reads as overclaiming, and a sharp judge will catch it.

**Frame it as two numbers with two meanings:** RTOS scheduling determinism (3.375 µs, the
µT-Kernel result, which is what a TRON contest actually judges) and end-to-end perceptual
latency (~50 ms, bounded by physics, not software). Both are good numbers. Conflating them
loses the credibility of the first.

### G-6. One board, no spare. *(evidence)*

There is exactly one STM32N6570-DK. Block 5 involves soldering a permanent assembly, and Block
10 puts the only unit in a courier. Between 18 Sep and 30 Sep there is no hardware at all.

**Mitigations, cheap:** build the sensor assembly as a **separate shield that plugs into the
Arduino headers** rather than modifying the DK; never solder on the DK itself; anti-static
discipline through Block 5; and treat the Block 9 evidence archive as the real deliverable,
because after 18 Sep it is the only thing that exists.

### G-7. Frame drops silently distort velocity. *(inference, high confidence)*

`d(t)` is gated on `result.Status`. A rejected frame leaves a hole in a history buffer that the
model was trained to see as evenly spaced at 20 ms. `v = (d_prev - d) * 5` assumes exactly one
frame of separation; after a drop the real interval is 40 ms and the computed velocity is half
the true value — in the *same* direction as making a hazard look non-hazardous.

**Needs a defined, documented policy** — hold-last, interpolate, or mark-invalid-and-skip — and
**the identical policy must be active during collection and inference.** Nothing specifies one.

### G-8. `sysclk = 400 MHz` vs the DWT constant. *(evidence, unresolved)*

The board reports `sysclk=400000000` while CLAUDE.md §1 says 600 MHz and §3 converts DWT cycles
with `/600000`. On the N6 the CPU clock and the system bus clock are separate, so 400 MHz may be
correct for the bus — but until it is confirmed which clock feeds `DWT->CYCCNT`, **every
DWT-derived number in the write-up is suspect** (the logic-analyzer numbers are unaffected).

Ten minutes with the reference manual. Do it before anything quotes a DWT figure.

### G-9. Contest submission format still unknown. *(evidence)*

Unresolved since v1 of the plan. Sizes Block 11, and an entry-registration deadline earlier than
the work deadline is a live possibility. **This is the highest-value hour in the project right
now** and it involves no soldering.

---

## TIER 3 — worth knowing, unlikely to be fatal

- **G-10. The judge's cable.** Type-A-to-C limits the board to ~550 mA and it will not boot
  (UM3300 §6.1 note 1). A C-to-C goes in the box, and the printed card says so. *(evidence)*
  Also worth testing the demo on a deliberately marginal supply before packing. *(speculation)*
- **G-11. Ambient IR.** VL53L1X range degrades in strong ambient infrared. The demo room's
  lighting is unknown. One line on the card about expected working distance. *(inference)*
- **G-12. I2C IRQs share NVIC level 1 with SysTick.** They cannot preempt each other, so a long
  ISR delays the tick. Runtime transfers are microseconds; the 91-write VL53L1X init is one-time
  and happens before the pipeline runs. Low risk, but it is an untested interaction. *(inference)*
- **G-13. µT-Kernel API surface.** This is a TRON contest. The project uses tasks, semaphores,
  `tk_def_int`, `tk_dly_tsk` — a narrow slice. Whether breadth of µT-Kernel usage is judged is
  unknown and depends on G-9. Do not add API usage for its own sake; do mention the ones the
  design deliberately rejected and why (e.g. `tk_wup_tsk` rejected for `tk_sig_sem` because it
  does not carry count — audit M-3). Deliberate rejection reads as competence. *(speculation)*
- **G-14. Solo project, no reviewer.** Two silent-failure defects were found in 24 hours, both by
  adversarial checking rather than by testing. The base rate suggests more exist. Budget for
  finding at least one more, and prefer tests that fail loudly over tests that pass quietly.
  *(inference)*

---

## SCHEDULE VALVES — what to drop, in the order to drop it

If two blocks slip by more than a day, stop and open this list rather than compressing
everything uniformly.

1. **MPU6050 EXTI wake** — already descoped; polling ships.
2. **The IMU entirely.** CLAUDE.md §6 already defines a **12-feature fallback** (10 distance
   history + v + a, no accelerometer). If the IMU is not streaming by 6 Sep, drop it and ship the
   ToF-only vector. This removes a sensor, a solder job, a driver, three features and a whole
   class of mounting-orientation risk — and the hazard rule does not use accelerometer data
   anyway. **This is the single largest schedule valve in the project and it costs almost nothing
   in capability.**
3. **NPU** — ship CPU-only inference and describe it accurately (plan Block 8, hard call 16 Sep).
4. **Graded urgency** — fixed-rate pulses instead of velocity-proportional. Removes the
   dependency on the HAP-T9 duration measurement.
5. **Never drop:** the Phase 4 timing evidence, the cold-boot demo, or the Block 9 archive. Those
   are the submission.

---

## THE ASSET THAT IS ALREADY BANKED

Worth stating plainly, because it changes how much risk the rest deserves.

**This is a TRON Forum contest — it judges µT-Kernel work.** The µT-Kernel result is *already
finished and hardware-measured*: a four-task architecture with a proven paired-semaphore
handshake (~91,000 events, zero torn reads over a 32-minute soak) and deterministic preemption at
a worst case of 3.375 µs over 2,229 events under load, with the baseline statistically identical.
That is the contest's actual subject and it is done.

Phases 5 and 6 turn it into a product and a story. They are worth doing well. But if Phase 6
collapses on 16 September, there is still a defensible, evidenced, on-topic submission — provided
the Block 9 archive exists. **Protect the archive above the features.**
