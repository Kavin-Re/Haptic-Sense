# HAPTIC-SENSE — FULL PROJECT AUDIT
**2026-09-03. Repo at `4abe79a`, sensor ranging, 15 days to hardware freeze.**

Five parallel adversarial passes: CLAUDE.md against the datasheets and the
firmware; the 26-document corpus against itself; all 9.5 KLOC of own source;
the evidence base re-derived from the raw logs; and the design docs against
what was actually built. Where two passes disagreed I adjudicated against the
primary source and recorded it in §6.

Labelled per CLAUDE.md §9: **evidence**, **inference**, **speculation**.

---

## 0. VERDICT

**The engineering is sound and the record of it is not.** Every headline
number that a contest submission would rest on reproduces exactly from the
archived logs — 3.375 µs over 2,229 events, 90,929 frames in `frames == inf`
lockstep with `canary_err = 0`, 47.6 Hz, 265 motor fires, zero faults. Those
were re-derived independently, not read from an analysis file, and they hold.

What does not hold is the paperwork around them. The corpus has a **structural
defect**: `PROJECT_DEFENSE.md §5` discipline 10 states the rule — *"Correct
superseded text in place. A correction that lives only in a newer doc leaves
the old doc as a contamination source"* — and the project has violated it for
**every correction since 26 August**. Seventeen of the thirty-nine
contradictions found have that identical shape: the correction exists, it is
well-reasoned and well-cited, and it lives only in a document the superseded
one does not reference.

Three things are worse than paperwork. **A silent-failure defect of the
established class is live in the firmware right now** (§3, D-A/D-H). **The
tool that decides whether a soak counts as evidence passes on an empty set**
(§4). And **the contest logistics hour has now been named as the project's
highest-value action in nine consecutive documents over 34 days and has never
been done** (§1.1).

---

## 1. TIER 0 — CAN STILL COST THE SUBMISSION

### 1.1 Every contest fact in the project is unsourced *(evidence)*

No `docs/CONTEST_LOGISTICS.md` exists. **Not one contest assertion anywhere
carries a citation, a URL-plus-date, or a retrieval record.** The two tron.org
URLs appear in five documents and have evidently never been opened.

| Claim | Asserted as fact in | Source |
|---|---|---|
| Deadline **30 Sep** | `CLAUDE.md:4` | none |
| Deadline **25 Sep** | `PROJECT_DEFENSE.md:4` | none |
| Ships 18 Sep, ~12 days transit margin | `PLAN §0:13-14` | none; no courier contacted |
| Benchtop prototype is the deliverable | `CLAUDE.md:4` | none |
| Judges power it on with nothing attached | `PLAN §0.3:59` | explicitly an assumption, which then drives Block 10 |
| "It judges µT-Kernel work" | `HANDOFF_20260903:197`, `RISK:184` | none — and `RISK G-13:148` concedes it is *unknown* |
| Shipping address, submission format, language, video | Blocks 10/11 sized against them | unknown |

Open since 30 Aug, restated verbatim in **seven** places, never actioned:
`PLAN Decision 1` · `RISK G-9` (*"the highest-value hour in the project right
now"*) · `HANDOFF_20260830_BLOCK2:150` · `HANDOFF_20260831 B3:151` (assigned to
"developer is doing this 31 Aug") · `BLOCK2_FOUR_DAY_PLAN §2.1` (*"Do this
Monday morning, before anything else"*) · `WHILE_YOU_WAIT §6` (*"If you get one
hour and one hour only — do §2.1. Not the sensor."*) · `HANDOFF_20260903:133`.

**Consequence.** The failure the project has itself named five times: *"working
firmware with a missed registration is the one failure no later effort can
undo."* And it is worse than a single risk — **if the true deadline is 25 Sep,
or if hardware must *arrive* rather than be *sent*, the 18 Sep ship date and
the whole block schedule are wrong.** Every schedule number in the corpus
descends from two unsourced dates that disagree with each other.

**This is one hour. It has been one hour for five weeks. Do it before the
soldering iron comes out again.**

### 1.2 A live silent-failure defect: `E_OK == 0`, and so does BSS *(evidence)*

`app_vl53l1x.c:95` — `static vl53l1x_stats_t tstats;` → BSS zero.
`app_vl53l1x.c:469` — `if (tstats.init_result != E_OK) return;`
The `1 = not run` sentinel is assigned at `:431`, **inside `vl53l1x_init()`** —
the one function that does not run in the failure case.

`app_tasks.c:416` gates the *init* on the raw probe. `app_tasks.c:445` calls
`vl53l1x_service()` **unconditionally**. So with the 7SEMI absent — the
configuration the comment at `:409-415` explicitly claims to support — the
guard passes on the BSS zero and the frame loop runs against a sensor that was
never initialised. Every frame then NACKs into the shim's 25-attempt retry
(`app_vl53l1_port.c:264-285`), each paced by `tk_dly_tsk(1)`: **25–50 ms of
delay inside a 20 ms frame budget, for ever.** *(evidence for the path;
inference for the rate collapse to ~15–22 Hz.)*

**The same defect shape sits on five more sentinels** (`app_i2c.c:667, 795`,
`app_drv2605l.c:311`, plus `stats.gate_result` and `stats.tof_ctl`). If
`app_i2c_init()` fails, the heartbeat prints:

```
[I2C] gate=0 wr=0 ...      <- reads as "L1 gate passed, write direction proven"
[TOF] res=0 step=0 ctl=0x0 <- three of the five terms of the documented PASS signature
[RNG] init=0 step=0        <- reads as "ranging"
```

**Fix, and it is one line each: initialise the sentinels at the definition, not
in the function.** `static vl53l1x_stats_t tstats = { .init_result = 1 };`
Then gate `vl53l1x_service()` on a separate flag set only on the success path.

### 1.3 The velocity estimator the design calls "Unacceptable" is the one that shipped *(evidence)*

`PHASE5_DESIGN_sensor_bringup_i2c.md:101-102`, verbatim:

> *"Naive v = (d(t−1) − d(t))/20 ms amplifies mm-level ToF noise: ±3 mm frame
> noise → ±15 cm/s velocity noise, on the same order as the 20 cm/s hazard
> threshold. **Unacceptable.** Design: least-squares slope over the last 5
> samples (100 ms window)… Implement once in a tiny shared C file compiled both
> into firmware and into the training-data generator script — one source of
> truth."*

Built: `app_tasks.c:354` — `v_cm_s = (d_prev - d_mm) * 5;`. The rejected form.

Harmless **today** because the input is a noiseless synthetic ramp. Block 4
replaces the ramp with a real ToF and the noise arrives with it — and `v` is
both the discriminative feature (G-3) and half the hazard rule. **No document
records the least-squares design as descoped.** `RISK G-2` independently
re-derives half the requirement without noticing that the estimator is already
specified and unbuilt, and the "one shared C file" single-source rule — which
is G-2's entire mitigation for train/inference mismatch — appears in no plan,
no handoff and no block.

**This is the single most likely thing to be discovered as a bug after data
collection has been made irreversible.**

There is a second, compounding error. `v_cm_s = Δd × 5` hardcodes exactly 50
frames/s. The loop measures **47.6 Hz** in all three soaks, so **`v` is already
5.0 % high**, and `a` carries that error squared. `tk_slp_tsk(20)` at
`app_tasks.c:473` is a delay, not a period, so the error is unbounded: one bus
recovery (~45 ms), one retry burst (up to 50 ms) or one `[EFF]` measurement
(~58 ms) drops the loop to 15–22 Hz while `v` is still scaled as if 50. The
hazard rule `v > 20 cm/s` then fires on approaches up to 3× slower than the
threshold, with no error and no counter. *(evidence for the code and the
measured rate; inference for the 3× figure.)*

### 1.4 `check_soak.py` certifies subsystems that do not exist *(evidence — reproduced)*

Run the Phase 5 checker against the Phase 4 log, which predates the DRV2605L,
the motor and I2C entirely:

```
$ python3 phase5/check_soak.py phase4/phase4_soak.log
  PASS  faults (OC_DETECT | OVER_TEMP), sticky    values seen:
  PASS  cfglost -- MODE still 0x01                max 0
  PASS  I2C bus faults (err - nacks)              max 0
  PASS  I2C tmo                                   max 0
  PASS  I2C recov                                 max 0
RESULT: PASS over 31.8 min. Bench is clean.       EXIT=0
```

Five checks pass on an **empty set** (`set() <= {0}` is `True`). The tool
printed `values seen:` — the evidence of its own vacuity — and passed anyway.
A two-line file containing only two `[HB]` lines scores *"10 PASS, 0 FAIL,
Bench is clean."*

Four more defects in the same tool:
- **Six checks vanish silently** behind bare `if rows[...]:` guards, so the
  check *count* is a property of the log, not the tool. **This is the direct
  cause of the 14/15/16 confusion across five documents.**
- **`pulses + suppressed == hazard` uses `abs()`.** Positive skew is benign
  print ordering; **negative skew is a genuinely dropped hazard** — the exact
  failure the check exists to catch — and `abs()` hides up to four of them.
- **The R-3 floor rule is printed, never enforced.** `*** BELOW 1.2 ***` is a
  bare `print()`, not a `check()`. A log with a 200 ms effect duration passes.
- **No continuity check.** A board that reset mid-run, a board that hung for
  9m58s, and the same log concatenated twice all report PASS.

**This is the instrument that decides whether a soak is evidence.** Every fix
is one or two lines.

### 1.5 The archived Phase 4 analysis carries the wrong clock *(evidence)*

`docs/evidence/phase4/analyze_timing.py:35` still reads `CPU_MHZ = 600`, and
both archived outputs still print *"max = 3.375 us (2,025 cycles @ 600 MHz)"*.
These are the artifacts `PLAN Block 9.37` says to archive and `CLAUDE.md:251`
cites as the RZ3 evidence pointer — **the submission evidence for the project's
headline number, carrying a figure the project corrected on 30 August.**

`PHASE4_RUNBOOK_timing_campaign.md:85` — the procedure for the **PH6-3 re-run
under NPU load**, the one campaign that cannot be repeated after 18 Sep — still
instructs the reader to divide by 600000.

`BLOCK2_PREFLIGHT F-1` caught exactly this trap on the firmware side and fixed
it. Nobody fixed the runbook that says what to do with the output, or the
archived analysis that a judge would read.

**Fix `CPU_MHZ`, regenerate both `.txt` files while the `.sr` captures still
exist, and correct `CLAUDE.md:251`'s "~4 µs" to 3 µs** (2700/800) — which
`HANDOFF_20260903:190` already flags as owed by hand.

---

## 2. TIER 1 — THE STRUCTURAL DEFECT

Seventeen contradictions share one shape. The most consequential:

| # | Two documents say | Correct | Why it bites |
|---|---|---|---|
| **F-01** | `HANDOFF_20260903` is self-designated *"paste §10 as the next session's first message"* and says VIN = 2.04 V, sensor does not range, four changes unbuilt | **All false as of today** | A session started that way refuses to flash until it "fixes" a fault already fixed, rebuilds what is on the board, and never reaches §5/§6 where the live work is |
| **F-02** | Three design docs instruct **hard-tying XSHUT to 3.3 V**, two citing CLAUDE.md §2 as their source for the opposite instruction | CLAUDE.md's *rule* (leave unconnected) — but see §6.1, its stated *reason* is wrong | Invisible to anyone who trusts the citation instead of following it |
| **F-05** | Six documents order `DEVCNF_USE_HAL_IIC → 1` as "first action, in order" | `CLAUDE.md:196` — stays 0 **permanently**; flag=1 gives duplicate-symbol link errors **and** a compile error from a one-char typo in `hal_i2c.c:60` | CLAUDE.md declares the design doc's premise wrong and **never annotated the design doc** |
| **F-06** | Two design docs call the feature vector **13** and one declares it *frozen* and *"matching §6 of CLAUDE.md exactly"* | **15** (`app_tasks.c:92`) | Red Zone #6 by the project's own definition — a checklist item instructing you to verify against a value that does not exist |
| **F-07** | `drv2605l_port_design_v1 §6.2` calls **R-EN-2 (P1 drives EN low)** *"the safety invariant answering Q5"*; `CLAUDE.md:109` still says P1 owns EN | **R-1** — EN init-only, decided, implemented, hardware-verified | See §3.1 — R-1 **deletes** that safety invariant and nothing re-derives it |
| **F-08** | `DRV2605L_P3_INIT_ARMING §3` runs a MODE=6 actuator diagnostic that **spins the motor at every boot** | Dropped (`PLAN Decision 4`) | Re-implementing §3 puts a motor spin into the judge's cold boot |
| **F-09** | Same doc writes `OD_CLAMP = 0x8C` | **0x8B** | 0x8C equals its own reset value, destroying the only register in the init readback that discriminates "my write landed" from "the part reset" from "the DMA never ran" |
| **F-12** | `CLAUDE.md:277` says the R-3 floor is still the interim **125 ms** and H-D3 is what blocks it | **75 ms** — and `CLAUDE.md:264`, thirteen lines earlier, says H-D3 is CLOSED and the floor is 75 | The same section asserts both. Re-deriving from 125 ms understates the graded-urgency claim by 40 % |
| **F-13** | `vl53l1x_port_design §6.1` specifies **rising**-edge EXTI on PD0; §2's LOCKED map says GPIO1 is pulled up | **Falling** (`0x0030 → 0x11` decoded on the wire) and **no pull-up** (2.46 MΩ, §8 refutes §2 260 lines later) | Implementing EXTI from §2 configures a falling edge on a floating pin — 7,202 spurious wakes in 1.4 ms, on the Priority-1 chain |
| **F-16** | `PROJECT_DEFENSE §3:402` — *"the single source of truth for open items through September"* | Last updated **12 July** | **~14 of ~40 rows wrong, all in the same direction** — work already done, presented as blocking. §7's standing order sends a session to re-close closed items |
| **F-18** | `PLAN §0.3` specifies printed-card wording predicting an A-to-C boot failure | Measured: **it boots** | `PLAN Block 10.39` says only "printed card (§0.3)", so the card gets written from the retracted section |
| **F-20** | Four places still carry *"Adafruit INT max 1.8 V, needs a divider"* | Retired 2026-08-29 | This claim blocked an item for a month once already |
| **F-25** | **Every `file:line` citation in the corpus is now wrong** — `app_i2c.h:15` is at 25, `:19` at 29, `:20` at 30, and several are the *entire* evidence for a CLOSED ledger row | — | A reader who follows a citation and lands in a comment block concludes the claim is unverifiable |

Full 39-finding map and the 44-row numbers-drift table are in the working
notes; the rows above are the ones with a consequence attached.

---

## 3. FIRMWARE — WHAT THE CODE AUDIT FOUND

Beyond D-A/D-H (§1.2), fifteen findings. The five that matter:

### 3.1 There is no kill path at all *(evidence)*

`app_drv2605l.c:195` states: *"the kill path is a STANDBY write from TK_PRI 3,
not a GPIO yank."* **`grep -rn "0x41"` over `Appli/Core` returns zero hits.**
There is no kill function in the driver or its header.

R-EN-2 (the P1 GPIO kill) was correctly superseded. **R-2, the mechanism that
replaced it, was never built.** So Priority 1 has no off-switch, the design
document's entire failure analysis (Q5) rests on a mechanism that no longer
exists, and nothing in the corpus re-derives the safety argument under R-1.
`PLAN Decision 4:118` gestures at it — *"playback is a finite one-shot ROM
effect… 'motor stuck on' is close to unreachable by construction"* — but that
reasoning was never written into the document an implementer reads.

**A comment describing a safety mechanism that isn't there is worse than no
comment.** Either build R-2 or delete the claim and record the descope.

### 3.2 Two faults are detected and neither gets a response *(evidence)*

`app_drv2605l.c:531-540` reads MODE and the fault register at ~1 Hz. On
mismatch it does `dstats.cfg_lost++`. On a latched fault it sets `faults_seen`.
**Neither clears `drv_armed`, and no re-arm is ever attempted** — both of which
`DRV2605L_P3_INIT_ARMING §6.4` requires unconditionally.

So a de-configured or over-current DRV2605L keeps receiving TRIG edges from
Priority 1, and the only symptom is a counter on a heartbeat line. This is the
direction the F-1 audit called *"the worst failure this product has."* Note the
poll itself is not decorative — it is what caught the real `OC_DETECT` on 30
Aug.

### 3.3 `SCB_InvalidateDCache_by_Addr` on a CPU-written buffer *(evidence, latent)*

`app_tasks.c:427` invalidates `feature_buf` immediately after
`sensor_fill_frame()` writes it with the CPU. `feature_buf` is **never** a DMA
destination. `DCIMVAC` invalidates without write-back, so with D-cache on the
just-written frame is **discarded** and `inference_task` reads stale AXISRAM.

Aggravating: `static W feature_buf[FEAT_COUNT]` (`app_tasks.c:131`) is 60 bytes
**with no alignment attribute**, so the head and tail cache lines are
invalidated whole — taking adjacent dirty statics with them. Every other DMA
buffer in the tree is correctly `[32] aligned(32)`; this one is the exception.

`CLAUDE.md §3` prescribes this call verbatim, so **the rule itself is wrong for
a CPU-written buffer.** Delete the call, or align and pad the buffer and keep
the invalidate only around an actual DMA.

Companion, also latent and also gating D-cache: **there is no
`SCB_CleanDCache_by_Addr` anywhere in the tree** (F-6a). On the write path
`drv_wr8()` stores into a dirty line and the GPDMA reads stale SRAM — the RZ9
failure mode with the security attributes now correct.

### 3.4 The `[RNG]` diagnostic is still suppressed by a failure it should catch *(evidence)*

I fixed one instance of this in the D-1..D-8 patch. **There is a second.**
`app_tasks.c:601` guards on `frames || drop_xfer || drop_status ||
drop_ready_err || drop_clear` — `notready` is printed but is **not in the
guard**. In the state CLAUDE.md §8 already records on this hardware
(`0x0030 → 0x11`, `0x0031 → 0x03`, so `isDataReady` can never be set),
`CheckForDataReady` *succeeds* and reports not-ready: all five guard terms stay
zero and **the line never prints**. The one counter that is moving — `notready`,
at ~48/s — is on the line that does not print.

Add `|| v->notready > 0u`.

### 3.5 `drv_meas_t0` is clobbered mid-measurement *(evidence + inference)*

`app_drv2605l.c:478` clears `drv_meas_pending` at the **top** of a ~58 ms poll
and increments the sample counters at the **bottom**, so for the whole poll both
halves of the Priority-1 re-arm condition are true. `drv_meas_t0` is written by
P1 and read by P3 at `:511`, and P1 preempts P3 by construction.

Window from the stamped edge to `:511` is up to **~78.7 ms** (one 20 ms frame +
58.7 ms effect). `DRV_R3_FLOOR_MS` is **75 ms**, and the urgency curve returns
exactly the floor at v ≥ 100 cm/s. **A second TRIG at the floor lands inside the
measurement window**, re-stamps `t0`, and the code records ~3–4 ms as a valid
effect duration — into the counter whose maximum feeds the R-3 floor derivation.

Latch `t0` into a local before clearing the flag.

**Also worth knowing:** `dwt_ok` is computed correctly and then **gates
nothing** — it is read by exactly one print. With a frozen counter, `[EFF]`
records five valid-looking samples of a 0 µs effect and `[HB] dt_us` prints
zeros. That second one is the Block 9 PH6-3 campaign.

---

## 4. EVIDENCE — WHAT SURVIVES TO 18 SEPTEMBER

### 4.1 Reproduces exactly, safe to cite

Re-derived with independent python from the raw logs:

| | Claimed | Measured | |
|---|---|---|---|
| Phase 4 soak | ~32 min, ~91,000 frames | **31.825 min, 90,929** | PASS |
| `frames == inf` | every line | **3034 / 3034 across all three logs, zero divergences** | PASS |
| `canary_err`, `q`, `qovr` | 0 | all `{0}` | PASS |
| Preemption | 3.375 µs / 2,229 / 1,776, missed 0 | **exact, all four** | PASS |
| `cyc_per_ms` wrap-safe | "reproduces exactly" | **1,135 / 1,135 intervals** | PASS |
| Motor fires | 265 | **265 delta** (counter reads 269) | PASS |

The Phase 4 log is continuous, monotonic, no gaps — **the strongest single
artifact in the project.**

### 4.2 Two claims that will not survive a sharp reviewer

**σ ≈ 41–45 ns is not a jitter measurement.** All 4,005 pooled events take one
of exactly two values: 3.250 µs (26 samples) or 3.375 µs (27 samples) at 125
ns/sample. σ = 0.125·√(p(1−p)) reproduces both figures exactly. **It is an
artifact of counting how often the answer lands on tick 26 versus tick 27.**
CLAUDE.md's gloss *"σ ≈ 41–45 ns (below LA resolution — deterministic)"* reads
as if sub-resolution σ proves determinism. It does not.

> Defensible instead: *"every measured event fell within one 125 ns sample
> period; the distribution is unresolved below that."* The 3.375 µs worst case
> is unaffected and is still three orders of magnitude inside the 1 ms line.

**`pulses + suppressed == hazard` "exactly" is false on 12.6 % of samples.**
Tested on every line: exact on 498/570 and 496/567, skew always `{0, +1, +2}`,
**never negative**, and cumulative totals reconcile exactly at end of run
(269 + 3291 = 3560).

> Defensible instead, and *stronger*: *"reconciles exactly at every run total,
> and instantaneously to within 2 events — the positive-only print-ordering skew
> of the heartbeat. A negative skew, which would indicate a lost hazard, never
> occurs in 1,137 logged samples."*

**And one number nobody has reconciled: 47.6 Hz measured against "50 Hz" in 13
documents.** Put it in writing before a reviewer does it for you.

### 4.3 What depends on a `.sr` — the 18 September list

**All ten `.sr` captures are now committed** (that landed tonight in `4abe79a`).
These claims have no other support:

| Capture | Sole support for |
|---|---|
| `timing_chatter*.sr` ×4, `timing_baseline*.sr` ×3 | **The RZ3 headline — 3.375 µs over 2,229 events.** The most quoted number in the project |
| `bus2_scl_20260830.sr` + `_fixed.sr` | H-D8 both halves — the 444.4 kHz overshoot **and** the 381 kHz compliance proof |
| `sensorinit_fail_20260901.sr` | **V-5/H1, the L5 deliverable**, plus eight further statistics (the 12.8 ms window, 4,609 bus gaps, 207,130 SCL pulses, "Block 1 intact") |

**Two captures cannot be located at all** *(evidence)*:
- The **GPIO1 8 s capture** — *"7,202 low episodes in a 1.4 ms window"*, the
  proof GPIO1 has no pull-up and the basis for the §2 spec correction.
  **No filename appears anywhere in the tree.**
- The **XSHUT 8 s capture** — *"XSHUT never once low"*, which eliminated the
  reset hypothesis. Recorded only as a shell template, `xshut_probe_$(date).sr`.

`xshut_probe_20260901.sr` and `xshut_init_20260901.sr` are in the repo and are
probably these — **but nothing ties either filename to either claim.** Add one
line to each evidence record naming the file, before 18 Sep.

**Also not files at all** — bench measurements existing only as prose, with no
instrument log and no photograph: ERM coil 32.5 Ω · CN8 3.32 V / breakout
3.31 V · XSHUT↔VIN 4.13 kΩ · GPIO1↔VIN 2.46 MΩ · bus 1.612 kΩ · the 2.04 V
brownout · **and tonight's 3.32 V VIN fix.** Each backs a real design decision.
**Photograph the meter for each one.** It costs ten minutes and after 18 Sep
there is no meter.

---

## 5. DESIGN vs BUILT

Traceability across ~90 labelled requirements. The pattern:

**Built and correct, ahead of its recorded gate:** F-6b (ULD bounce buffer),
F-6c (DRV buffer rule) — both still listed OPEN-LATENT in `PROJECT_DEFENSE`.
F-4 (`I2C_REG16` symbol) is the best-closed item in the project: fixed, then
hardened with a `_Static_assert` **and** a runtime negative control nobody asked
for.

**Built but written down nowhere:**
- The entire D-1..D-8 rationale exists only in source comments — and collides
  with **two other "D-n" namespaces** already in the docs.
- The `[EFF]` in-firmware effect-duration rig (~120 lines, cross-task DWT
  handshake, deliberate 20 ms budget overrun). The design specified a *scope*
  measurement. **The number the R-3 floor rests on came from the undocumented
  firmware rig.**
- `feature_buf`'s lock-free justification — a load-bearing safety argument —
  exists only in `app_tasks.c:124-130`.
- `hazard_urgency_interval_ms()`, the product's entire perceptual behaviour,
  retuned 2026-08-30 with the rationale only in a comment.

**Specced and silently dropped:** the velocity estimator (§1.3); the
stale-frame policy (`stale_frames > 10` → `data_valid = false`, specified in
July, and `RISK G-7` now says *"nothing specifies one"* — something does, it was
forgotten); R-2; the F-7 sentinel check; four L1 header-contract items.

**PH6-1 does not exist and is past its gate.** `PROJECT_DEFENSE:525` marks it
*"BLOCKS Phase 6 implementation start"*; `PLAN:218` budgets 20 minutes at Block
4. It is 3 Sep and it is unwritten.

### 5.1 The MPU6050 decision, costed *(evidence)*

**Code that exists for the IMU: none.** Exhaustive grep returns three things —
two negative-control rows in the probe table (which never execute on a healthy
bench) and three synthetic constants.

| Drop | Keep |
|---|---|
| **~7 edited lines** — `FEAT_COUNT` 15→12, delete three indices. The 12-feature fallback is already locked (`CLAUDE.md:228`) | **~300 lines** of new driver, plus stats and heartbeat plumbing |
| **No hardware modification at all** — 818 Ω / 3.54 mA against a 4 mA limit, OK | **Two irreversible desoldering operations** on 0402-class resistors on an unreplaceable GY-521 (no jumper), plus the SmartElex jumper. Removing either alone leaves the MPU6050 at ~3.55 mA against 3 mA |
| **3 days reclaimed** (Block 3), plus the Block 5 axis freeze and the IMU-6 pre-collection gate | 3 scheduled days, 4 open hardware verifications, and one gate whose failure mode is *"every sample unrecoverable"* |
| Reduces PH6-1 to a genuinely 20-minute document | Keeps PH6-1's hardest clause — the staleness-validity protocol exists **solely** to serve the IMU |
| **Capability lost against the locked hazard rule: zero.** `Hazard = d < 80 cm AND v > 20 cm/s` uses no accelerometer term | — |
| Write-up cost: one paragraph | Adds a third device to a bus whose clock was 11 % out of spec five days ago |

*(inference, high confidence: with the label a deterministic function of d and
v — G-1 — the three IMU features carry no label-relevant information at all. If
G-1 option (a) shifts the label to t+k, IMU features become plausibly
informative about gait — but that is a hypothesis with no data to test it.)*

**The evidence points one way. The decision is yours.** Note that
`PLAN Block 3 step 14` — which starts tomorrow — still prescribes the mitigation
that `CLAUDE.md:43` proved insufficient, so **the stale instruction is currently
hiding the second, independent argument for the largest schedule valve in the
project.**

---

## 6. ADJUDICATIONS — WHERE THE PASSES DISAGREED

### 6.1 The XSHUT abs-max reason is a fourth cross-contamination *(evidence)*

`CLAUDE.md:33` — *"Never hard-tie to 3V3: most VL53L1X carriers run the die from
a 2.8 V LDO and **XSHUT abs max is VDD+0.3 = 3.1 V**."*

**DS12385 Rev 8 Table 12** gives one row: *"AVDD / SCL, SDA, XSHUT, and GPIO1 —
Min −0.5 · Max **3.6 V**"* — an absolute, rail-independent limit. **Table 13**
adds: *"The I/Os are internally failsafe with no diode connecting them to
AVDD."* The `VDD + 0.3` form is **SLOS854D §6.1 — the DRV2605L.**

So the corpus contradiction (F-02, three docs instructing a hard tie) is real
but **not destructive**: 3.3 V on XSHUT is inside abs max. **The rule survives;
the reason is wrong and is a DRV2605L spec applied to an ST part.** The correct
citation is DS12385 Table 13 note 1 / §3.2: *"XSHUT has to be raised only when
the power supply is tied on."*

**Fix the reason, keep the rule, annotate the three design docs.** And log this
as cross-contamination #4 — the class the project is most sensitive to, found
inside the file that warns about it.

### 6.2 The "both pull-up pairs must come off" rule is over-derived *(evidence)*

`CLAUDE.md:54` cites *"MPU6050 3 mA (VOL 0–0.4 V at 3 mA sink, PS-MPU-6000A
§6.5)"*. That table's column is headed **"Typical"**, not Max, and it has **two**
IOL points — the second, `VOL = 0.6 V → 5 mA`, is omitted. Solving
I_pullup = I_sink with both points (slope 10 mA/V) against the file's own
corrected 0.5 V threshold:

```
818 Ω → VOL settles 0.449 V   inside 0.5 V
811 Ω → VOL settles 0.451 V   inside 0.5 V
593 Ω → VOL settles 0.562 V   OUTSIDE — genuine fail
```

**The all-fitted row is right. The two "one mitigation only → FAIL" rows are
not** — either single mitigation suffices. The file also computes the sink at
VOL = 0.4 V while arguing pass/fail at 0.5 V two paragraphs later.

This matters because the rule prescribes **irreversible destructive work on an
irreplaceable board.** If you want to keep the strict version, state it as a
deliberate conservative margin ("Typical column, no guaranteed max, design to
the 3 mA point") — not as a datasheet limit. Two independent vendor floors do
support the all-fitted FAIL and are cited nowhere: **TI SLOS854D §8.5.3.1
recommends 660 Ω–4.7 kΩ**, and **DS12385 Table 4's lowest suggested value is
0.8 kΩ.**

### 6.3 The soak verdict claim is misattributed *(evidence — reproduced)*

`HANDOFF_20260903:188` banks *"A NACK is no longer a bus fault — hardware-
verified, 9.9-minute soak, **16/16**."*

- `soak_10min_20260830.log` → **16/16 PASS**, exit 0 — but it contains **no
  `[NAK]` line at all**, so it cannot evidence that claim.
- `soak_10min_20260830_nackfix.log` → **15 PASS + 1 expected FAIL**, exit 3 —
  this is the log that demonstrates the separation, and it never reads 16/16.

The claim sits in the *"BANKED — do not re-litigate"* section. A future session
re-running the soak sees 15/16, compares against banked 16/16, and concludes
something broke.

---

## 7. SCHEDULE

**15 days to hardware freeze. 27 to submission.** Remaining plan blocks total
roughly 17 working days against 15 available, with PH6-1 unwritten and blocking
Phase 6, and G-1/G-2/G-3/G-4/G-7 all past their "decide before 9 Sep" gate.

**It does not fit as written.** Two changes make it fit:

1. **Drop the IMU** (§5.1). Reclaims 3 days, removes two irreversible hardware
   operations, deletes four open verifications and one unrecoverable-failure
   gate, and reduces PH6-1 to a 20-minute document. Costs ~7 lines and one
   paragraph of write-up.
2. **Do the contest hour first** (§1.1). Not because it takes time, but because
   every other date in the plan is derived from two unsourced numbers that
   disagree — and if the answer is 25 Sep or "must arrive", the whole ordering
   changes and you want to know that today, not on the 16th.

The asset is already banked and that should govern the risk appetite for
everything else: **a four-task µT-Kernel architecture with a proven paired-
semaphore handshake over ~91,000 events with zero torn reads, and deterministic
preemption at 3.375 µs worst case over 2,229 events under load, all
hardware-measured and all reproducing exactly from the archived logs.** That is
the contest's actual subject and it is finished. **Protect the archive above the
features.**

---

## 8. ORDERED ACTIONS

**This week, before the iron:**
1. **The contest hour.** Both tron.org pages. Registration deadline, submission
   format, ship-vs-arrive, address. Write `docs/CONTEST_LOGISTICS.md` with URLs
   and retrieval dates. Settle 25 vs 30 Sep.
2. **Decide the IMU.** The evidence in §5.1 is one-directional.
3. **Fix the six BSS sentinels** (§1.2) — one line each, and one of them is live.
4. **Fix `check_soak.py`** (§1.4) — five one-line fixes, then re-run against both
   archived logs and record the true count everywhere that says 14 or 15.
5. **`analyze_timing.py: CPU_MHZ = 800`**, regenerate both `.txt`, and correct
   `CLAUDE.md:251` "~4 µs" → 3 µs.

**Before Block 4:**
6. **Build the least-squares velocity estimator, or descope it in writing with
   the noise arithmetic rechecked** (§1.3). Derive Δt from `tk_get_otm()` while
   you are in there.
7. **Write PH6-1.** It has been blocking Phase 6 since July.
8. **Adopt or reject the July stale-frame policy** — do not re-derive it from
   scratch as G-7 currently assumes.
9. Build R-2 or delete the comment claiming it exists (§3.1); give `cfg_lost`
   and `faults_seen` a response (§3.2).

**Documentation hygiene, one sitting:**
10. Annotate every superseded doc **in place** — F-01, F-02, F-05, F-06, F-07,
    F-08, F-09, F-12, F-13, F-18, F-20. Add a one-line `SUPERSEDED BY` header to
    each. This is the project's own discipline 10.
11. Rewrite `HANDOFF_20260903` §1/§2.2/§3/§10 to the current state, or mark the
    file superseded and write a new one.
12. Reconcile `PROJECT_DEFENSE §3.2` or demote it from "single source of truth".
13. Correct the two overstated evidence claims (§4.2) and reconcile 47.6 vs
    50 Hz in writing.

**Before 18 Sep, non-deferrable:**
14. Name the GPIO1 and XSHUT capture files in their evidence records (§4.3).
15. Photograph the meter for every prose-only measurement, tonight's 3.32 V
    included.
