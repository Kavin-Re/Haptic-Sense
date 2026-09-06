# PROJECT DEFENSE — Haptic-Sense

Project: Haptic-Sense (TRON Forum Contest 2026) · STM32N6570-DK (Cortex-M55 @ 800 MHz + Neural-ART NPU) · µT-Kernel 3.0
Written: 2026-07-12. **Both facts on this line are stale, corrected 2026-09-06:**
~~Hard deadline: 2026-09-25~~ and ~~Cortex-M55 @ 600 MHz~~ were wrong even at the time (F-16,
`PROJECT_AUDIT_20260903.md` §2 -- unsourced and contradicted by `CLAUDE.md:4`'s 30 Sep; the
600 MHz was corrected on hardware 2026-08-30, see `CLAUDE.md` §1 G-8). **The real submission
mechanics are sourced and settled in `CONTEST_LOGISTICS.md`: code/docs may be submitted up to
2026-09-30 23:59 JST; hardware (if used, and it is here) must ARRIVE by 2026-09-30 18:00 JST --
two independent deadlines, neither of them 25 Sep.** Clock is 800 MHz, hardware-confirmed.

**What this document is.** Insurance, not design. Three drivers are designed and audited
(VL53L1X, MPU6050, DRV2605L), the four-task RTOS architecture is hardware-proven, and the
adversarial-review capacity that caught this project's worst latent bugs (Fable-class
review) will not be available through implementation and integration. This document is
the transfer artifact: it assumes the reader — Kavin in two months, or a less-capable
model with zero session memory — knows nothing about how these findings were made.

**How to use it.**
- §1–§3 are reference: look things up when something behaves strangely (§1), before
  starting Phase 6 work (§2), and whenever asking "what's still open?" (§3 is the single
  source of truth for September).
- §4–§7 are habits and gates: §4 before changing any configuration, §5 as standing
  discipline, §6 every single flash, §7 for how to work without Fable.
- Appendix A lists source-file discrepancies found while writing this. **None of them
  have been applied** — each needs Kavin's separate approval.

**Provenance.** Everything below is synthesized from repo files read in full on
2026-07-12: the two audit reports (`docs/audits/redzone_audit_crosscutting_2026-07-11.md`,
`docs/audits/redzone_audit_addendum_mpu6050_2026-07-11.md`), the five design docs
(`docs/design/vl53l1x_port_design.md` "L1X-v1",
`docs/design/vl53l1x_port_design_v2_reconciliation.md` "L1X-v2",
`docs/design/mpu6050_port_design_v1.md` "MPU doc",
`docs/design/drv2605l_port_design_v1.md` "DRV doc",
`docs/PHASE5_L1_i2c_xfer_design.md` "L1 doc"), `CLAUDE.md`, git history, and targeted
firmware-tree reads (each cited inline). Every technical specific is cited or tagged
`[UNVERIFIED]` with a named step. The audit reports are dated snapshots of 2026-07-11;
**§3.1 of this document, not the report text, is the live disposition of each finding**
(several were fixed by commits after the reports were written).

---

## 1. SILENT-FAILURE CATALOGUE

Every entry is a failure this project actually hit or provably nearly hit. Format:
**the trap** (concrete instance) → **the pattern** (what to generalize) → **why it's
invisible** (the mechanism that defeats normal debugging) → **the check** (the specific
act that catches it). When you meet a new situation, match it against the *pattern*,
not the instance.

### 1.1 Cross-part spec contamination

**The trap.** The VL53L5CX's firmware-upload-at-init requirement (~84 KB over I2C, every
power-on) leaked into the VL53L1X design as "~88 KB firmware upload" — a sensor that has
*no firmware upload at all* (its `SensorInit` is 91 single-byte register writes,
`VL53L1X_api.c:184-186`; L1X-v1 §0.1). The transplanted number was *also* wrong for the
part it came from: 84 KiB (86,016 B = 0x8000+0x8000+0x5000), not 88 KB (audit F-2,
fixed in commit `226651a`; corrections register L1X-v2 §7).

**The pattern.** Facts from a sibling part (same vendor, same family, same I2C address,
similar name) migrate into the wrong part's docs — and the migrated fact mutates in
transit.

**Why it's invisible.** A sibling-part fact is *plausible* — it survives review because
it doesn't look wrong, and nothing executes it until integration. Worst case it drives
real design work (here: a chunking/buffer design for a transfer that doesn't exist).

**The check.** Every new-part design doc opens with an **anti-contamination header**: a
table of exactly the axes where this part differs from the previously designed part
(address convention, register-index width, endianness, identity check, init cost).
Institutionalized in MPU doc §0 and DRV doc §0 — the audit rated the MPU version "the
strongest anti-contamination discipline of the three designs" (addendum item 5). Never
write a per-part numeric claim with only a family-level citation.

### 1.2 Locked-constant transcription error (adjacent-row datasheet read)

**The trap.** The project's locked-decision record paired "±4 g" with **4096 LSB/g**.
The correct ±4 g (AFS_SEL=1) sensitivity is **8192 LSB/g**; 4096 is the ±8 g row of the
same table (RM-MPU-6000A §4.18). Caught by audit M-1 (fixed in commit `58753d2`) —
critically, **before Edge Impulse data collection began**. The audit called it "the
highest-consequence contamination-class finding in either report because it corrupts
data, not code."

**The pattern.** A constant copied from a datasheet table lands one row off, then gets
"locked" and propagates with the authority of a locked decision.

**Why it's invisible.** Everything runs. The firmware compiles, the sensor streams,
values look like acceleration. Every training sample is silently scaled 2×; the model
trains on scaled data and *appears to work* — the corruption exists only relative to
physical ground truth, which nothing in the pipeline checks. Past data collection it
becomes irreversible without recollecting everything.

**The check.** Two parts: (a) cite every scale/sensitivity constant to the exact
datasheet row *with its selector value* ("AFS_SEL=1 → 8192", never "±4g → 8192") — the
selector makes an off-by-one-row visible; (b) a **physical sanity gate at first light**:
flat on the bench, Z must read ≈ +1000 mg (PS §7.8, tolerance ±80 mg; MPU doc §5). A 2×
scale error reads 500 or 2000 mg and fails instantly. Run the physical gate *before*
collecting any training data.

### 1.3 Symbol-vs-literal silent dispatch fallback

**The trap.** The L1X-v1 mapping table passed literal `2` as the `regsz` argument. The
primitive dispatches on `regsz == I2C_REG16` — a *symbolic* comparison (`app_i2c.c:316`).
Today `I2C_REG16` happens to equal `2u` (`app_i2c.h:20`, verified 2026-07-12), so the
literal works; if the symbol were ever redefined, every literal call site silently falls
to the 8-bit branch. Audit F-4 (rated high severity), fixed in commit `108fb60`.

**The pattern.** A call site encodes the *current value* of a contract instead of the
contract itself; the two drift apart later with no compile-time complaint.

**Why it's invisible — twice over.** First, the bug is dormant: literal and symbol are
numerically identical today, so no test can catch it. Second, when it fires, the failure
is still silent at runtime: only the index LSB is emitted on the wire, and
**register-mapped I2C slaves ACK any index** — the transaction "succeeds," reads return
data from the wrong register, and no error code appears anywhere (L1X-v2 §1.3).

**The check.** Pass symbols, never literals, for any argument that a dispatch compares
symbolically. Before merging any driver: `grep -n "i2c_rd\|i2c_wr" <driver>.c` and
confirm no bare numeric in the regsz position. Deeper habit: when writing a call site,
read the *dispatch code* it lands in, not just the prototype.

### 1.4 Device ACKs while functionally dead

**The trap.** DRV2605L with EN low: the device **still ACKs on the bus, but "no read or
write is possible"** (SLOS854D §8.4.1.3; DRV doc §2). An entire init sequence executed
with EN low looks perfectly successful — every transaction ACKed — and configures
nothing.

**The pattern.** Bus-level success (ACK, HAL_OK, E_OK) is mistaken for device-level
success. The transport can be healthy while the device is in a state where writes don't
land.

**Why it's invisible.** All the observable signals — ACK bits, HAL return codes, the
semaphore completing — report success, because they measure the *bus*, not the device.

**The check.** **Readback-verify**: after any config write sequence, read the registers
back and compare (MPU doc init step 10; DRV doc reads 0x01 and asserts
MODE[2:0]==1/STANDBY==0 every sensor loop, §6.4). Plus an identity read as the first
transaction to any device (WHO_AM_I=0x68, DEVICE_ID=7, ToF model-ID word) — proving
reads return *content*, not just ACKs.

### 1.5 Cross-task bare-flag lost-update race

**The trap.** The original R-EN-3 mechanism: Priority 1 sets a "config invalid" flag on
emergency kill; Priority 3 clears it after re-init. A second kill landing while P3 is
mid-re-init is overwritten and dropped — and because of §1.4 (EN low = silent ACKs),
every remaining init write "succeeds" on a dead device. End state per the audit: **"flag
clear, device unconfigured, haptics silently dead"** — for a hazard-alert wearable, the
worst-case failure direction. Audit F-1 (high severity); fixed in commit `226651a` by
eliminating the flag entirely (DRV doc §2 R-EN-3/R-EN-4: P3 re-derives config validity
by reading DRV register 0x01 back; P1 signals nothing).

**The pattern.** State shared between tasks as a bare variable, written by one and
cleared by another, with no atomicity — a lost update whose window is microseconds wide.

**Why it's invisible.** Races are timing-dependent: 32 minutes of soak and ~91k clean
frames (Phase 4) prove nothing about a window this narrow. When it fires it leaves no
trace — no error, no log, just a haptic that doesn't fire during an actual hazard.

**The check.** Exactly two acceptable shapes for cross-task state, nothing else:
(a) it travels **inside the semaphore-protected buffer** (paired-semaphore pattern,
CLAUDE.md §3); or (b) it is **not shared at all** — re-derived from device-register
ground truth by its sole owner. Any bare `volatile` flag crossing task boundaries in a
design is an automatic reject. **This exact race is queued to reappear in Phase 6** as
the IMU staleness indicator — see §2.3.

### 1.6 Mislabeled file trusted over content

**The trap.** `docs/design/mpu6050_port_design_v1.md` was committed (at `dc8e5cd`)
containing the *VL53L1X v2 review* content. The project was bitten **twice**: once when
the wrong content was committed under the trusted filename, and again when the
cross-cutting audit consumed the file by name — audit finding A-1, an **AUDIT BLOCKER**
that voided the entire MPU6050 slice of the report and forced a second audit pass (the
addendum). Recovery took two commits (`81e3548`, `77a9c9c`).

**The pattern.** Filename, title, or label is accepted as evidence of content. Every
automated and human process downstream inherits the mislabel.

**Why it's invisible.** Every surface-level check passes: the file exists, git tracks
it, `ls` shows the expected name, the commit message says the right thing. Only
*reading the body* reveals it.

**The check.** Before citing, auditing, or building on any doc: **read the title block
and first section and confirm they match the filename.** When attaching files to a
review session, verify each attachment's first lines, not its name (the addendum did
exactly this on receiving the replacement file — "content matches… all cross-references
carry over"). When committing a doc, re-read the staged diff header — the mismatch was
visible in the diff at `dc8e5cd` and not looked at.

### 1.7 The same vendor's docs disagree with each other — LIVE ITEM

**The trap.** Expected VL53L1X sensor ID: in-repo `VL53L1X_api.h:197` says **0xEEAC**
(verified by direct read 2026-07-12); UM2510 says **0xEACC**; a real L1X in an ST
community thread returned **0xEEAA**, with ST staff confirming the docs are inconsistent
across silicon iterations (L1X-v2 §6, `{ID-VAR}`; audit F-3). Related instances: the
reference repo README names `network_weights.hex`, a file that doesn't exist (actual:
`network_data.hex`; CLAUDE.md §4); SLOS854D §9.3.1 prose says the default is closed-loop
while its own Table 26 default is open-loop (DRV doc §6.3, D-1).

**The pattern.** Two authoritative sources conflict; whichever you read first becomes
"the fact," and a hard-coded check built on it fails good hardware.

**Why it's invisible.** Each source is individually citable — "cite everything"
discipline doesn't save you when the citations disagree and you only found one.

**Why it was LIVE, not historical (RESOLVED 2026-07-12).** L1X-v1 §6 step 3 and §8-H1
commanded a hard fail: "Anything but 0xEEAC = debug the shim." Had first-light code been
typed from L1X-v1, a perfectly good sensor returning 0xEACC or 0xEEAA would read as a
broken shim, stalling bring-up on a bug that doesn't exist. **The harmonized rule
(Appendix A item A-1, applied 2026-07-12 across all four docs): log and record any
stable unexpected ID; only 0x0000/0xFFFF or no-ACK is a hard-fail bus condition.**

**The check.** For identity values: **log, don't hard-fail** (L1X-v2 §6-L3 semantics).
For config defaults: never rely on defaults for mode-relevant registers — write them
explicitly (D-1 resolution). For file names in prose: trust `.cproject`/`.launch`/actual
files over README text.

### 1.8 Tool reports success, artifact is dead (signing/flashing class)

**The trap.** `STM32_SigningTool_CLI` without `-align` under CubeProgrammer ≥ 2.21
produces a signed-looking binary that does not boot — no error at sign time, no error at
flash time, the board just doesn't come up (Red Zone 1, CLAUDE.md §4). Sibling failure:
flashing a **stale** binary — "committed ≠ built ≠ flashed" cost three flash cycles on
2026-07-05 (CLAUDE.md §9).

**The pattern.** A multi-stage artifact pipeline where each stage exits 0 but the
composition is wrong (missing flag, stale input); the only failure signal is downstream
silence.

**Why it's invisible.** Every stage's own success indicator is green. The failure
presents as a hardware mystery (dead board) three steps removed from the cause.

**The check.** The mechanical checklist in §6 of this document, run every time. Never
debug a dead board before re-running §6.

### 1.9 Contract drift across a tool boundary

**The trap (near-miss class, standing).** `FEAT_COUNT` (`app_tasks.c:89`, = 15) and the
Edge Impulse feature spec are one contract with no compiler between them — a mismatch
silently degrades the NPU model (Red Zone 6). Already nearly hit at the documentation
level: the CLAUDE.md headline said "13 features" while its own enumeration counted 15
(corrected 2026-07-05; the enumeration is authoritative).

**The pattern.** Two representations of one contract live in systems that can't see
each other (firmware ↔ browser tool; headline ↔ enumeration); each edit site looks
locally complete.

**Why it's invisible.** No shared compiler, no shared test. The model still produces
outputs — just worse ones, indistinguishable from "the model isn't great yet."

**The check.** Single-source the contract: the enumeration is authoritative, headlines
are derived. Any change to either side of the firmware/Edge-Impulse boundary touches
both in the same session and says so in the commit message. The NPU verify order
(CPU inference → test vectors → NPU → compare outputs; CLAUDE.md §6) is the runtime
backstop.

---

## 2. INTEGRATION-PHASE LANDMINES NOT YET FIRED

Where the next silent failures most likely hide. Each entry names the mechanism, why it
will be silent, and the tripwire to install. Phase 6 (NPU/TinyML + pipeline integration)
is the concentration point: it is the first phase where all three drivers, the feature
pipeline, and the NPU run against each other.

### 2.1 NPU silent CPU fallback (Red Zone 6)

**The mechanism.** ST Edge AI / Neural-ART compiles unsupported ops to CPU with **no
error and no warning** — the model runs, 10–30× slower (CLAUDE.md §6). The locked model
(3-layer FC 15→32→16→1 sigmoid) uses only NPU-safe ops, but any "improvement" during
training iterations (an LSTM layer, an attention block, an exotic activation) silently
reintroduces this.

**Why it will be silent.** Inference still returns correct answers. The only symptom is
`inference_ms` — and nobody watches a number they haven't baselined.

**The compounding hazard nobody has written down yet:** the inference task runs at
TK_PRI 2, **above** the sensor task (TK_PRI 3). A CPU-fallback inference doesn't just
run slow — it occupies the CPU at P2 for its full duration, **starving P3 underneath
it**. Sensor frames drop, the stale-data path engages, and the visible symptom is
"flaky sensors," two layers away from the real cause. (P1 is unaffected — it still
preempts — but the pipeline degrades.)

**The tripwires.**
1. Follow the locked verify order: CPU inference → validate test vectors → enable NPU →
   **compare NPU vs CPU outputs** → measure `inference_ms` via `tk_get_otm()`
   (CLAUDE.md §6).
2. Record the first measured NPU `inference_ms` in CLAUDE.md as a baseline; assert
   against a generous multiple of it (e.g., 3×) in the heartbeat line. A 10–30× jump is
   unmissable against a recorded baseline and invisible without one.
3. Any model architecture change re-runs the whole verify order. No exceptions —
   op-support is a property of the *graph*, not the project.

### 2.2 D-cache enablement — three latent items fire at once

**The mechanism.** D-cache is OFF today (`app_config.h:21`, per `app_i2c.c:65-66`), so
three known cache-coherency defects are inert. Phase 6 NPU work is the likely trigger
for enabling it. The day that happens, all three go live simultaneously:

| ID (audit) | Latent defect | Where recorded |
|---|---|---|
| F-6a | Write-side `SCB_CleanDCache_by_Addr` **absent** before `HAL_I2C_Mem_Write_DMA` — DMA reads stale RAM | L1X-v1 §4 item 1; `app_i2c.c:326-335` (read path only) |
| F-6b | ULD passes unaligned stack buffer (`Temp[17]`, `VL53L1X_api.c:590`) — invalidate can clobber adjacent stack; named fix = bounce buffer in shim `ReadMulti` | L1X-v1 §4 item 2 |
| F-6c | DRV2605L transfers use 1-byte stack locals with **no alignment rule stated in the DRV doc** — same class as F-6b, unacknowledged in the design (audit: VIOLATION RISK) | Audit item 3; Appendix A item A-3 |

NPU-managed buffers are separately covered by `--cache-maintenance` in
`user_neuralart.json` (CLAUDE.md §3) — **that flag covers only NPU buffers, not the I2C
DMA buffers above.** Assuming it covers everything is the trap.

**Why it will be silent.** Cache incoherency is intermittent and data-dependent:
occasional stale bytes in sensor frames, corrupted adjacent stack variables. It will
present as sensor noise or random crashes, nowhere near the `USE_DCACHE` line that
caused it.

**The tripwire.** One gate, stated once: **enabling D-cache is gated on closing F-6a,
F-6b, and F-6c first.** (The audit recommends recording this line in CLAUDE.md — Appendix
A item A-9.) The MPU6050 side is already safe by design: `imu_buf[32]
__attribute__((aligned(32)))` (MPU doc §2.3; addendum closure note).

**Soak-test caveat and mitigation ordering (added 2026-08-26).** D-cache is currently
DISABLED. The CLAUDE.md §3 cache-maintenance rule (`SCB_InvalidateDCache_by_Addr` after
I2C DMA) has therefore never been exercised — **including through the Phase 4 91k-frame
soak** (`docs/evidence/phase4/phase4_soak.log`): that soak's `canary_err=0` result says
nothing about cache coherency, because the code path it would have stressed was never
live. Phase 6 NPU deployment requires D-cache ON (`--cache-maintenance` in
`user_neuralart.json`, CLAUDE.md §3/§5) — enabling it will activate every latent
coherency bug in the I2C path simultaneously (F-6a/F-6b/F-6c above), with no soak
evidence to fall back on. **MITIGATION: enable D-cache during single-sensor bring-up,
not during Phase 6** — closing F-6a–F-6c against one sensor at a time, on hardware,
before the NPU pipeline adds its own concurrent buffer traffic on top. See §3.2 L1-*
below (L1-10 through L1-14) for the specific open items this gates.

### 2.3 The M-4 staleness flag — F-1's race, queued for Phase 6

**The mechanism.** The MPU6050 design's Option A marks an IMU frame "stale" when INT is
low at poll time, feeding the 12-feature fallback (MPU doc §4). Who reads that mark and
how it crosses P3→P2 is **deliberately unspecified** — it is Phase 6 scope. If Phase 6
implements it as a bare file-static flag read by the inference task, it is a second
F-1-class unprotected cross-task race (audit M-4; recorded as a binding constraint in
MPU doc §7, commit `f804281`).

**Why it will be silent.** Same as §1.5 — microsecond window, no trace, and the failure
mode is subtle: inference consuming a 15-feature vector whose IMU features are stale
while believing them fresh (or the reverse), degrading predictions with no error.

**The tripwire (already locked as a constraint, not yet designed).** The staleness
indicator **MUST travel inside the semaphore-protected P3→P2 feature-frame buffer** — a
validity field in the frame struct — never as a side-channel flag (MPU doc §7). This
closes together with audit F-6d: the Phase 6 handoff design must specify the P3→P2
buffer's **owner, alignment, and a statement that no DMA engine touches it** (if it's a
CPU-copy assembly buffer, no cache maintenance is needed — but that must be *stated*,
not assumed; audit item 3, F-6d). **Refuse to review any Phase 6 handoff design that
lacks these two paragraphs.**

### 2.4 Timing-budget interactions at 50 Hz + the < 1 ms guarantee

**The mechanism.** Phase 4 proved 3.375 µs worst-case P1 preemption under synthetic CPU
chatter (CLAUDE.md §8 RZ3). Phase 6 changes the load profile in four ways at once:
three sensors on one I2C bus, NPU inference with its own AXI/memory traffic against the
XSPI RAM the CPU executes from, the DRV readback-verify poll, and the feature-frame
assembly. Individually budgeted, collectively unmeasured.

**The specific interactions to watch:**
1. **Poll-mode transaction multiplication.** ToF `CheckForDataReady` costs **2 full I2C
   transactions per call**; polling every 1–2 ms across a 20 ms frame ≈ 40 extra
   transactions/frame (L1X-v1 §6.1). Bring-up polls by design — fine alone, but with the
   IMU's 14-byte burst and DRV readback on the same bus, the frame budget erodes. The
   EXTI switch (2 transactions/frame total) is the designed fix; don't defer it past the
   point where three-sensor polling saturates the frame.
2. **Clock-domain beat.** The ToF's 20 ms intermeasurement timer, the MPU's ±1% internal
   sample clock (PS §6.6), and the kernel tick all free-run. Polls will occasionally
   find no fresh sample or two samples elapsed (~1% of frames, MPU doc §4 Option A).
   This is *expected* — the stale-frame path handles it. The landmine is treating the
   beat as a bug during integration and "fixing" it into a race.
3. **The ~145 ms failure envelope.** One I2C failure through the primitive's
   retry+recovery costs up to ~145 ms ≈ 7 frames (`app_i2c.c:357-362`). The stale-data
   policy bounds detection (L1 doc §4.3), but Phase 6 must decide what inference does
   during a 7-frame gap — that decision doesn't exist yet.
4. **Re-run the preemption campaign after NPU integration.** The 3.375 µs figure was
   measured without NPU AXI traffic contending for the memory system the CPU executes
   from. The claim "< 1 ms, hardware-verified" must be re-evidenced under real Phase 6
   load before it goes in contest material. The method and tooling already exist
   (`docs/evidence/phase4/`, `analyze_timing.py`, GPIO D0/D1 method) — this is a re-run,
   not new work.

**Why it will be silent.** Every component is individually within budget; the
interactions only show up as occasional missed frames or jitter — attributed to sensors,
not to scheduling.

**The tripwire.** Instrument frame timing from day one of Phase 6 (`tk_get_otm()` per
stage, heartbeat prints worst-case per minute), and gate the contest latency claim on
the re-run campaign, not the Phase 4 numbers.

### 2.5 Smaller queued landmines

| Landmine | Mechanism | Tripwire |
|---|---|---|
| GPDMA1 secure-world RIF attributes | Reference app grants RIMC master attributes to NPU/DMA2D/DCMIPP/LTDC but **not GPDMA1** (`main.c:330-351`); if required, first DMA transfer faults or never completes | Already ledgered (L1-4, §3.2): if first transfer hangs, add GPDMA1 to RIF config mirroring `main.c:336`; check RM0486 RIFSC table |
| EXTI migration (ToF PD0, IMU PE9) | GPIO1 pull-up voltage domain: init-table reg 0x2F (and 0x2E) bit 0 may need =1 for AVDD-level pull-ups on the 7SEMI breakout (L1X-v1 §6.1) | LA trace of GPIO1 idle level before the switch (TOF-2); both EXTI handlers `tk_sig_sem`-only at kernel level 1–15, zero I2C |
| VL53L5CX upgrade path (if ever) | The primitive casts `len` to `uint16_t` for HAL (`app_i2c.c:329/333`) — harmless for ULD's ≤17-byte transfers, **would truncate the L5CX's 32 KB firmware chunks** (L1X-v2 §4) | Flag stands in L1X-v2; do not integrate MB1854B before core pipeline works (CLAUDE.md §2) |
| Power budget with camera module | Type-A→C USB cannot power board + camera (~550 mA limit) — boot failure that looks like bricked hardware (CLAUDE.md §1) | C-to-C cable or powered source; part of §6 checklist |
| DRV standby-window I2C wedge | Device's I2C watchdog recovers hung transactions in all states *except standby*; init rows 0–1 execute in power-on standby; EN-toggle is shutdown, not power removal (audit F-9, low probability) | Note in DRV design (Appendix A item A-6); bench power-cycle if ever observed |

---

## 3. CONSOLIDATED VERIFICATION LEDGER

**This is the single source of truth for open items through September.** Built from
ground truth: `grep -rn "UNVERIFIED\|\[V-\|H-D\|F-[0-9]\|M-[0-9]" docs/` (58 hits,
2026-07-12), both audit reports read in full, plus targeted firmware-tree verification.

**Re-keying.** The three driver docs use colliding ID namespaces (three different
"V-1"s). New keys: `TOF-*` (VL53L1X), `IMU-*` (MPU6050), `HAP-*` (DRV2605L), `L1-*`
(I2C primitive), `BUS-*` (shared bus), `PH6-*` (Phase 6 gates), `DOC-*` (documentation
actions). Old IDs are cross-referenced in every row. Audit findings keep their original
F/M/U/A IDs in §3.1.

### 3.1 Audit finding register — LIVE disposition (verified against repo 2026-07-12)

The audit reports are snapshots of 2026-07-11; several findings were fixed the same
night. The report text's "F-1 and F-4 remain the open high-severity items" is **stale**
— both are fixed. Live status:

| ID | Severity | One-line | Disposition (evidence) |
|---|---|---|---|
| A-1 | blocker | Wrong file uploaded as MPU6050 doc | **CLOSED** — addendum audited the real doc; file recovered (`81e3548`, `77a9c9c`) |
| A-2 | med | Superseded L1X-v1 text still circulates | **CLOSED 2026-07-12** — F-4 half fixed in place (`108fb60`); F-3 half (sensor-ID) harmonized via Appendix A item A-1 |
| F-1 | high | DRV kill/re-init flag race → haptics silently dead | **FIXED** (`226651a`) — flag eliminated; readback-verify of reg 0x01 is the sole mechanism (DRV doc §2, §6.4) |
| F-2 | low | "~88 KB" contamination figure in CLAUDE.md | **FIXED** (`226651a`) — CLAUDE.md §2 now "~84 KB (86,016 B)"; L1X-v1 Gate-1 item 9 closed 2026-07-12 |
| F-3 | med | 0xEEAC vs 0xEACC conflicting first-light pass criteria | **FIXED 2026-07-12** — V-6 executed (in-repo `api.h:197` = 0xEEAC); log-don't-hard-fail rule harmonized across all four docs (Appendix A item A-1, edits a–e). Final observed value lands at first light (TOF-9 hardware half) |
| F-4 | high | Literal `2` vs `I2C_REG16` symbol in L1X-v1 table | **FIXED** (`108fb60`) — table now symbol-based; V-2 closed (`app_i2c.h:20` = `2u`) |
| F-5 | med | N6 `HAL_I2C_Mem_*_DMA` entry may contain internal busy-flag spin | **OPEN** → L1-8 |
| F-6a | med (latent) | Write-side D-cache clean absent from primitive | **OPEN-LATENT** → L1-10; fires at D-cache enable (§2.2) |
| F-6b | med (latent) | ULD unaligned `Temp[17]` vs invalidate | **OPEN-LATENT** → TOF-13; same milestone |
| F-6c | med (latent) | DRV doc has no buffer-alignment rule | **FIXED 2026-07-12** (Appendix A item A-3) — `drv_buf[32]` aligned rule added to DRV §4; hoist-into-primitive alternative remains an open decision; latent-defect *code* work still lands at D-cache enable (HAP-8) |
| F-6d | med | P3→P2 feature-handoff buffer unspecified | **CLOSED 2026-09-05** — `PH6-1_feature_frame_validity.md`; `feature_frame_t` w/ `imu_valid`/`tof_valid` |
| F-7 | med | IN/TRIG configure-low-before-arm ordering not an explicit rule | **FIXED 2026-07-12** (Appendix A item A-4) — ordering rule merged into DRV R-EN-1 |
| F-8 | low | DEV_RESET poll budget in wall-clock ms, incompatible with ~145 ms failure envelope | **FIXED 2026-07-12** (Appendix A item A-5) — DRV §4 row 0 respecified ≤5 attempts, matching the M-2 fix |
| F-9 | low | I2C wedge in power-on standby window unrecoverable by EN-toggle | **FIXED 2026-07-12** (Appendix A item A-6) — note added to DRV §6.2; bench only if observed |
| U-1 | med | `I2C_BUS_HZ` ≤ 400 kHz gates all three devices | **Desk half CLOSED 2026-07-12**: `app_i2c.h:15` = `400000u`; all three doc ledgers annotated (Appendix A item A-7) → BUS-1 done; wire-level LA capture half remains → BUS-2 |
| U-2 | — | Everything MPU6050-specific | **CLOSED** by addendum |
| M-1 | high | ±4 g paired with 4096 LSB/g (correct: 8192) | **FIXED** (`58753d2`) — CLAUDE.md §2 corrected with RM §4.18 cite; residual "grep docs for stray 4096" not evidenced as run → DOC-1 |
| M-2 | low-med | MPU DEVICE_RESET poll unbounded/unpaced | **FIXED** (`f804281`) — MPU doc §3 row 1: ≤5 attempts, `tk_dly_tsk(1)` pacing, fail loudly |
| M-3 | low | `tk_wup_tsk` vs `tk_sig_sem` — two ISR→P3 wake mechanisms | **FIXED** (`f804281`) — standardized `tk_sig_sem`, recorded CLAUDE.md §3 + MPU doc §4 Option B |
| M-4 | med | Stale-IMU-frame indicator transport unspecified | **CLOSED 2026-09-05** together with F-6d — `PH6-1_feature_frame_validity.md`; NOT gated on INT (found unreliable this board) |

### 3.2 Verification ledger — every open, closeable, and recently-closed item

Gate legend: **desk** = file read/grep, no hardware · **doc** = design-doc or comment
edit (requires Kavin's approval per Appendix A) · **hardware** = bench/LA/meter ·
**decision** = Kavin sign-off. "Blocks" names the milestone the item gates.

#### Shared bus

| ID | Old ID(s) | Item | Closure step | Gate | Status / Blocks |
|---|---|---|---|---|---|
| BUS-1 | L1X-v2 V-4 · MPU V-5 · DRV H-D8 (desk half) · audit U-1 | Bus clock ≤ 400 kHz for all three parts | Read `I2C_BUS_HZ` | desk | **CLOSED 2026-07-12**: `app_i2c.h:15` = `400000u`; all three doc ledgers annotated (Appendix A item A-7) |
| BUS-2 | DRV H-D8 (LA half) · audit U-1 | Actual SCL frequency on the wire ≤ 400 kHz | LA capture of SCL during any transfer | hardware | OPEN / first light (piggybacks on TOF-8 capture) |

#### VL53L1X (TOF-*)

| ID | Old ID(s) | Item | Closure step | Gate | Status / Blocks |
|---|---|---|---|---|---|
| TOF-1 | L1X-v1 H1 | First hardware proof of the `regsz==I2C_REG16` path (gate test only ever ran `I2C_REG8`, `app_i2c.c:435`) | `GetSensorId` read at first light; interpret per harmonized ID rule (TOF-9) | hardware | OPEN / **first light** |
| TOF-2 | L1X-v1 H2 · L1X-v2 H2 | GPIO1 idle level & pull-up voltage domain on 7SEMI breakout; possibly reg 0x2F/0x2E bit 0 = 1 for AVDD pull-ups | LA trace of GPIO1 around a ranging cycle | hardware | OPEN / **blocks EXTI switch** |
| TOF-3 | L1X-v1 H3 | Soft-reset register sequence + hold time (ULD defines `SOFT_RESET` but never uses it) | Read VL53L1X datasheet soft-reset section — **now in-repo**: `docs/datasheets/vl53l1x_datasheet.pdf` | desk | OPEN (was datasheet-gated; datasheet now available) / blocks recovery step 2 (L1X-v1 §6.2) |
| TOF-4 | L1X-v2 V-1 | N6 HAL emits 16-bit index MSB-first | — | desk | **CLOSED** — L1X-v1 §2.2 cites in-repo `stm32n6xx_hal_i2c.c:3117-3120` (write), `:3297-3300` (read) |
| TOF-5 | L1X-v2 V-2 | `I2C_REG16` numeric value | — | desk | **CLOSED** — `app_i2c.h:20` = `2u` (recorded L1X-v1 §3) |
| TOF-6 | L1X-v2 V-3 | Uploaded templates ≡ in-repo `API/platform/` | Overtaken by events — implementation targets in-repo files directly (`firmware/Appli/Lib/STSW-IMG009/`) | desk | **MOOT** — record as such |
| TOF-7 | L1X-v2 V-7 | `VL53L1X_GetResult` exists in v3.5.5 | — | desk | **CLOSED** — L1X-v1 §6 cites `VL53L1X_api.c:587-604` |
| TOF-8 | L1X-v2 V-5 | Index MSB-first **on the wire** (hardware half of H1) | LA capture of one `WrByte`: expect `[0x52+W][idx MSB][idx LSB][data]` (L1X-v2 §6-L5) | hardware | OPEN / first light; doubles as contest evidence + closes BUS-2 |
| TOF-9 | L1X-v2 V-6 · audit F-3 | Expected sensor-ID value | Desk half **DONE 2026-07-12**: in-repo `api.h:197` = 0xEEAC, vs UM2510's 0xEACC, vs field-reported 0xEEAA — ST doc drift confirmed. Doc harmonization **APPLIED 2026-07-12** (Appendix A item A-1, four docs). Remaining: record whatever stable word the actual unit returns at first light | hardware | OPEN (hardware half only) / first light |
| TOF-10 | L1X-v2 V-8 | SensorInit loop bounds / byte count | — | desk | **CLOSED** — 91 one-byte writes (L1X-v1 §0.1, `VL53L1X_api.c:184-186`) |
| TOF-11 | L1X-v2 H3 | 50 Hz polling timing budget on hardware | `tk_get_otm()` instrumentation during first-light ranging loop | hardware | OPEN / first light |
| TOF-12 | (new, from L1X-v1 §6.1) | Poll→EXTI migration: PD0 EXTI handler `tk_sig_sem`-only, kernel level 1–15 | Implementation review at the EXTI switch | doc/review | OPEN / EXTI switch (after TOF-2) |
| TOF-13 | audit F-6b | ULD unaligned `Temp[17]` bounce buffer in shim `ReadMulti` | Implement at D-cache-enable milestone | doc/code | OPEN-LATENT / **gates D-cache enable** |

#### MPU6050 (IMU-*)

| ID | Old ID(s) | Item | Closure step | Gate | Status / Blocks |
|---|---|---|---|---|---|
| IMU-1 | MPU V-1 | GY-521 AD0 strap state → 0x68 vs 0x69 | F1 bus scan via existing `app_i2c_gate_test()`; if unstable across power cycles, strap AD0 to GND | hardware | OPEN / **first light** (also closes CLAUDE.md §2 "verify AD0" item) |
| IMU-2 | MPU V-2 | `I2C_REG8` symbol value | Read `app_i2c.h` | desk | **CLOSED 2026-07-12**: `app_i2c.h:19` = `1u`; MPU doc ledger annotated (Appendix A item A-7) |
| IMU-3 | MPU V-3 | GY-521 VLOGIC = 3.3 V; INT swings 0/3.3 V into PE9 | Meter/LA on INT at first light (F4 step): expect 50 Hz edges | hardware | OPEN / first light; **blocks EXTI Option B** |
| IMU-4 | MPU V-4 | GY-521 regulator presence → correct VCC rail | Physical inspection of the unit | hardware | OPEN / **wiring (F0)** |
| IMU-5 | (addendum action) | `app_i2c.c:421-424` WHO_AM_I comment said UNVERIFIED — claim was verified against RM rev 4.0 (`{RM §4.34}`) | Update comment with cite | doc | **CLOSED 2026-07-12** (Appendix A item A-8) — comment updated. **Source file changed → §6 checklist applies: rebuild before any flash** |
| IMU-6 | (MPU §5, §6 F3) | Rest-state scale sanity: Z ≈ +1000 mg flat on bench (±80 mg), X/Y ≈ 0 | First-light burst read + reassembly check | hardware | OPEN / **GATES Edge Impulse data collection** (the §1.2 physical gate) |

#### DRV2605L (HAP-*)

| ID | Old ID(s) | Item | Closure step | Gate | Status / Blocks |
|---|---|---|---|---|---|
| HAP-1a | H-D1(a) | Breakouts expose IN/TRIG on a header pin | — | — | **CLOSED** (CLAUDE.md §2, 2026-07-10: Adafruit `INT` pin — 1.8 V max, needs divider from 3.3 V GPIO; SmartElex separate `IN`/`EN` pins). DRV doc §9 row not yet annotated → Appendix A item A-9 |
| HAP-1b | H-D1(b) · DRV §10 item 2 | Allocate a free Arduino-header GPIO for IN/TRIG; add to CLAUDE.md §2 pin map (+ voltage divider for Adafruit board) | Pin-map decision + CLAUDE.md edit | decision + doc | OPEN / **BLOCKING for trigger Option A** — the P1 fire path does not exist without it |
| HAP-2 | H-D2 | EN-rise state ambiguity (Fig. 17 vs §8.4.1.4): does EN low→high return to armed state without I2C? | Bench: configure → click → EN low 100 ms → EN high → trigger again (DRV doc §9) | hardware | OPEN / informs whether readback-verify re-init fires routinely or rarely |
| HAP-3 | H-D3 | Effect ID 1 duration in Library B on this ERM → sets T-1 min inter-pulse period | LA on OUT+ (or scope + RC filter): trigger-to-quiescent time | hardware | OPEN / **blocks P1 pulse-rate grading constants** — the clamp must be a named constant in `app_drv2605l.h`, not folklore (audit item 6) |
| HAP-4 | H-D4 | Library choice (B assumed): measured rise/brake vs Table 1 | Same capture as HAP-3; rise > 80 ms ⇒ consider C/D | hardware | OPEN / same capture as HAP-3 |
| HAP-5 | H-D5 | Breakout VDD rail (3.3 V assumed; OD_CLAMP headroom) | Re-measure — July 10 attempt inconclusive (unstable meter reading, likely breadboard contact) | hardware | OPEN / wiring |
| HAP-6 | H-D6 | ERM coil resistance > 4 Ω OC threshold | Multimeter across motor terminals — provisionally cleared (est. 25–37.5 Ω from vendor 3 V / 80–120 mA); direct measurement pending | hardware | OPEN (provisional) / wiring |
| HAP-7 | H-D7 | DEV_RESET self-clear time (unspecified in SLOS854D) | Instrument poll loop on first hardware run; record in CLAUDE.md | hardware | OPEN / first light |
| HAP-8 | audit F-6c | Buffer-alignment rule absent from DRV design | Add static aligned scratch-buffer rule to DRV §4 (or hoist bounce buffer into primitive — decision) | doc + decision | **Doc rule ADDED 2026-07-12** (Appendix A item A-3, `drv_buf[32]` aligned); hoist-into-primitive alternative still an open decision / **gates D-cache enable** |
| HAP-9 | audit F-7 | IN/TRIG configured push-pull LOW **before** EN rises and before row 8 arms MODE=1 (floating trigger = spurious alerts = functional safety event for this product) | One-sentence ordering rule merged into R-EN-1 | doc | **CLOSED 2026-07-12** (Appendix A item A-4) |
| HAP-10 | audit F-8 | DEV_RESET poll timeout respecified in attempts (wall-clock 10 ms impossible through the ~145 ms failure envelope) | Edit DRV §4 row 0 to match the M-2-style fix already applied to the MPU doc | doc | **CLOSED 2026-07-12** (Appendix A item A-5) — ≤5 attempts, fail loudly |
| HAP-11 | audit F-9 | Standby-window I2C wedge not covered by recovery ladder | Add note to DRV §6.2; bench only if observed | doc | **CLOSED 2026-07-12** (Appendix A item A-6) — note added |
| HAP-12 | DRV §10 item 3 | Readback-verify polling frequency (every loop vs throttled) | Decide after 50 Hz pipeline timing measured (TOF-11 + IMU first light) | decision | OPEN / Phase 6 |
| HAP-13 | DRV §10 item 1 | Loop mode sign-off | — | — | **CLOSED by lock** — open-loop baseline recorded in CLAUDE.md §2 locked decisions; DRV §10 row stale → Appendix A item A-9 |

#### L1 primitive / platform (L1-*)

| ID | Old ID(s) | Item | Closure step | Gate | Status / Blocks |
|---|---|---|---|---|---|
| L1-1 | L1 doc §6 | I2C1 kernel-clock reset-default source + actual frequency | Log `HAL_RCCEx_GetPeriphCLKFreq(I2C1)` + `HAL_RCC_GetPCLK1Freq()` at first boot (plumbing exists: `app_i2c.c:202` consumes `stats.clk_pclk1_hz`) | hardware | OPEN / first light |
| L1-2 | L1 doc §6 | 400 kHz rise time on 1.5 kΩ pull-ups | Scope SCL/SDA edges; 100 kHz fallback pre-approved | hardware | OPEN / first light |
| L1-3 | L1 doc §6 | GPDMA request-macro spelling | First compile | desk | **CLOSED** — L1 code committed (`fc6ff88`) and the project builds (CLAUDE.md RZ2) |
| L1-4 | L1 doc §6 | GPDMA1 TrustZone/RIF attributes in secure world | If first DMA transfer faults/hangs: add GPDMA1 to RIF config mirroring `main.c:336`; check RM0486 RIFSC table | hardware | OPEN / **first light** (no evidence the gate test has run on hardware — treat first sensor transfer as the test) |
| L1-5 | L1 doc §6 | `tk_def_int` + dynamic IVT (`USE_STATIC_IVT=0`) interaction | WHO_AM_I over the sem-wrapped path exercises def_int → IRQ → callback → sem end-to-end | hardware | OPEN / first light |
| L1-6 | L1 doc §6 | 9-pulse bus recovery on this board | Fault-injection: hold SDA low at a safe moment; confirm recovery + counter increment | hardware | OPEN / bring-up (after first light) |
| L1-7 | L1 doc §6 | DWT/CPU clock cross-check | Log `HAL_RCC_GetSysClockFreq()` at boot alongside I2C clocks | hardware | OPEN / first light (cheap, same log line) |
| L1-8 | audit F-5 | `HAL_I2C_Mem_Write_DMA`/`Read_DMA` **entry** may busy-wait on BUSY-flag before starting (IT-variant precedent on ST forum) | `grep -n "WaitOnFlag\|WaitOnTXIS\|WaitOnSTOP" stm32n6xx_hal_i2c.c` within both function bodies; record max spin bound in CLAUDE.md if nonzero | desk | OPEN / before Phase 6 timing analysis (a bounded P3 spin is legal but must be *known*) |
| L1-9 | audit item 1 note | `HAL_GPIO_WritePin` uses BSRR (atomic per-pin) on the N6 HAL — assumed for the dual-writer EN pin (P3 init + P1 kill) | One grep in `stm32n6xx_hal_gpio.c` | desk | OPEN / before DRV implementation |
| L1-10 | audit F-6a | Write-side `SCB_CleanDCache_by_Addr` before TX DMA | Add to primitive at D-cache-enable milestone | code | OPEN-LATENT / **gates D-cache enable** |
| L1-11 | (new, `app_i2c.h` review 2026-08-26) | 7-bit/8-bit address-width contract not stated in `app_i2c.h` — only the `dev7` parameter name hints at it; the known failure mode (VL53L1X ULD's native 8-bit `0x52` vs. this primitive's 7-bit-only input, `0x52<<1=0xA4` → NACK on every transaction) is documented only in `vl53l1x_port_design.md` §2.1, never in the header itself | Add an explicit header comment on `i2c_rd`/`i2c_wr` stating the 7-bit-only contract and citing the failure mode | doc | OPEN / before any driver author reads only the header |
| L1-12 | (new, `app_i2c.h` review 2026-08-26) | Return-code contract undocumented in the header: `E_IO` conflates DMA-start failure with device NACK (`app_i2c.c:334-335`, `:82-88`); `E_TMOUT` already reflects a completed retry+recovery cycle costing up to ~145 ms (`app_i2c.c:357-392`); `E_OBJ` (single-client reentrancy guard, `app_i2c.c:374-375`) is unmentioned; `app_i2c_init` can also return `E_PAR` or a raw semaphore-creation error, likewise unmentioned | Add a return-code table to the header's doc comment | doc | OPEN / before driver error-handling code is written |
| L1-13 | (new, `app_i2c.h` review 2026-08-26); relates to L1-10/F-6a, TOF-13/F-6b, HAP-8/F-6c | Header states no buffer-alignment/cache-maintenance contract for callers — the read-invalidate-only asymmetry (no write-side clean, `app_i2c.c:349-353`) and the implicit 32-byte-alignment expectation are invisible from `app_i2c.h` | Add a header comment stating alignment/cache expectations, cross-referencing the D-cache-enable gate (§2.2) | doc | OPEN / **gates D-cache enable**, same milestone as L1-10 |
| L1-14 | (new, `app_i2c.h` review 2026-08-26); relates to TOF-5 (value question, closed) | `I2C_REG16` header comment (`app_i2c.h:20`, "== I2C_MEMADD_SIZE_16BIT") is numerically false — `2u` vs. HAL's `0x10`; harmless today because `app_i2c.c:316` dispatches symbolically on the project's own symbol, not the literal, but the comment recreates the literal-vs-symbol ambiguity F-4 already had to fix once | Correct or remove the comment's false HAL-equivalence claim | doc | OPEN / low urgency, doc-hygiene |

#### Phase 6 gates & documentation actions

| ID | Old ID(s) | Item | Closure step | Gate | Status / Blocks |
|---|---|---|---|---|---|
| PH6-1 | audit F-6d + M-4 | P3→P2 feature-frame handoff spec: buffer owner, alignment, no-DMA statement, **validity field inside the protected buffer** (never a bare flag) | Written `PH6-1_feature_frame_validity.md`; `app_tasks.c`/`app_mpu6050.{c,h}` implement it | doc+code | **CLOSED 2026-09-05** — build-verify on hardware still outstanding (no cross-compiler in this working environment); Phase 6 implementation start unblocked |
| PH6-2 | §2.1 | NPU `inference_ms` baseline recorded + heartbeat assert | First NPU run after verify order | hardware | OPEN / Phase 6 |
| PH6-3 | §2.4 item 4 | Re-run preemption campaign under NPU load before contest claims | Existing Phase 4 method (`docs/evidence/phase4/`) | hardware | OPEN / **blocks contest latency claim** |
| DOC-1 | addendum M-1 residual | Sweep docs for stray "4096" transplants | `grep -rn "4096" docs/ CLAUDE.md` and check each hit against RM §4.18 | desk | OPEN (audit named it; not evidenced as run) |
| DOC-2 | audit A-2 + user directive 2026-07-12 | Sensor-ID harmonization across L1X-v1 / L1X-v2 / MPU doc (+ DRV doc, found during application) | Appendix A item A-1, edits a–e | doc | **CLOSED 2026-07-12** — first-light pass criteria unblocked (TOF-9 hardware half remains) |

**Reading of the board as of 2026-07-12 (post-Appendix-A):** VL53L1X shim
implementation is **unblocked** (pass criteria harmonized; TOF-3 datasheet read
optional before recovery work). DRV2605L implementation is blocked **only on HAP-1b**
(IN/TRIG pin allocation — HAP-9's ordering rule is now in R-EN-1). Phase 6 design is
blocked on PH6-1. **Edge Impulse data collection is gated on IMU-6** (and was gated on
M-1, now fixed). D-cache enable is gated on TOF-13 + HAP-8 (code half) + L1-10.
`app_i2c.c` was touched by A-8 → rebuild per §6 before the next flash.

---

## 4. LOCKED DECISIONS REGISTRY

Before changing anything in the "Decision" column, read its "Why" and reversibility.
**Reversible** = change it if evidence says so, one edit. **Reversible-with-cost** =
possible but has a named blast radius — re-read the source doc first.
**Irreversible-after-milestone** = after the named milestone, changing it destroys work;
treat as physics. If you find yourself "improving" one of these without having read its
rationale, that is the silent-un-decide failure this table exists to prevent.

| # | Decision | Why (with source) | Reversibility |
|---|---|---|---|
| L1 | MPU6050 DLPF_CFG=4 (accel 21 Hz BW, 8.5 ms delay) | Anti-aliasing: only setting fully under the 25 Hz Nyquist of the 50 Hz pipeline; group delay is constant, so ML sees a consistent shift {RM §4.3; MPU doc §3 Option A} | **Irreversible after Edge Impulse data collection begins** — changes the spectral content of every sample |
| L2 | MPU6050 ±4 g, AFS_SEL=1, **8192 LSB/g** | Torso-height walking/impact transients can exceed ±2 g; clipping corrupts features worse than halved resolution (0.122 mg/LSB ≪ noise floor, PS §6.2). Constant corrected by audit M-1 — 4096 is the ±8 g row | **Irreversible after data collection begins** — a range change rescales every raw count |
| L3 | MPU6050 SMPLRT_DIV=19 → exactly 50 Hz | 1000/(1+19)=50, exact match to the locked pipeline {RM §4.2} | Irreversible-after-data-collection (same coupling as L1/L2) |
| L4 | MPU6050 CLKSEL=1 (PLL, X-gyro reference); **gyro stays powered although no gyro features are used** | Standby-ing the clocking axis silently falls back to the ±5% internal oscillator and drags the whole 50 Hz timebase {RM §4.30/§4.31, PS §6.6; MPU doc §3 notes} | Reversible-with-cost — "optimizing gyro power off" is the trap; re-read the note first |
| L5 | DRV2605L open-loop ERM + ROM Library B, effect ID 1 (Strong Click 100%) | Datasheet's own recommendation for ERM+ROM (SLOS854D §9.3.1 step 7); no calibration, no per-unit state. Closed-loop is a **deferred add-on with a written upgrade recipe** (DRV doc §4.2), not a rejection | Reversible — revisit after HAP-3/HAP-4 if click sharpness inadequate |
| L6 | DRV2605L readback-verify (reg 0x01) is the *sole* config-validity mechanism; P1 signals nothing (R-EN-3/R-EN-4) | Replaces the flag-based kill signaling that audit F-1 showed to be a lost-update race; the device register is ground truth | Reversible-with-cost — any replacement must re-solve F-1; **never reintroduce a cross-task flag** |
| L7 | DRV2605L OD_CLAMP=0x8B (3.001 V) | Eq. 6: 3.0/0.02159=138.96→139; stays ≤3.000 V for the 3 V-class ERM; mode-independent peak clamp = the safety bound (§8.5.2.2) | Reversible (recompute per DRV doc §4.1 if actuator changes) |
| L8 | All sensor data-ready ISR→P3 wakes use `tk_sig_sem`, never `tk_wup_tsk` (audit M-3, CLAUDE.md §3) | Count-carrying: an event firing mid-frame is never lost (wake-counts can saturate/collapse); consistent with the paired-semaphore architecture | Reversible in principle — but there is no upside; treat as permanent |
| L9 | `DEVCNF_USE_HAL_IIC=0` **permanently** (Phase 5 Option A: app owns HAL I2C directly) | BSP wrapper defines the strong `HAL_I2C_*CpltCallback` symbols our driver must own (`hal_i2c.c:134-172`); flag=1 + Option A = duplicate-symbol link errors; wrapper also has a compile-breaking typo under `TK_SUPPORT_MEMLIB=0` (CLAUDE.md §3) | Reversible-with-cost — requires abandoning Option A entirely, not just flipping a flag |
| L10 | Feature vector = **15** features; `FEAT_COUNT` (`app_tasks.c:89`) ↔ Edge Impulse spec is ONE contract | Enumeration is authoritative (the "13" headline was the error); a mismatch silently degrades the NPU model (RZ6) | **Irreversible after data collection**; before that, both sides change together or neither |
| L11 | Hazard label: distance < 80 cm AND closing velocity > 20 cm/s | Locked labeling semantics for all training data (CLAUDE.md §6) | Irreversible-after-data-collection (relabeling = recollecting) |
| L12 | VL53L1X: Short mode + 15 ms budget + 20 ms IMP; **mode before budget** | 15 ms exists only in the short-mode table (`VL53L1X_api.c:280`); IMP ≥ budget is NOT checked by the API (`api.h:179-181`); 5 ms margin under the 50 Hz cadence (L1X-v1 §6 step 5, D6) | Reversible — 20/20 is a two-line change; dropping to 40 Hz is NOT (reopens L10) |
| L13 | I2C1 is the ONLY sensor bus (PH9/PC1); onboard 1.5 kΩ pull-ups; **never external pull-ups, never I2C2** | I2C2 is occupied (LCD touch 0x5D, codec, STLINK); 1.5 kΩ is in every device's legal range (CLAUDE.md §2, schematic-verified) | Hardware-locked |
| L14 | `I2C_BUS_HZ = 400000`, 100 kHz fallback pre-approved (`app_i2c.h:15`) | Fast-mode max for all three devices (BUS-1); fallback fits the bandwidth budget (L1 doc §1) | Reversible — the fallback is pre-approved, no re-review needed to drop |
| L15 | NPU model ops: FC/ReLU/Sigmoid (safe list also allows Conv/DepthwiseConv/BatchNorm); **never LSTM/GRU/attention** | Unsupported ops fall back to CPU silently, 10–30× slower, no error — and starve P3 (§2.1) | Reversible only by re-running the full NPU verify order for the new graph — never by assumption |
| L16 | `msdrvif.c` static-pool patch (commit `7a91f51`) — **never re-copy this file from the reference repo** | Preserves `USE_IMALLOC=0`; re-copying reintroduces `Kmalloc`/`Kfree` (CLAUDE.md §5) | Reversible-with-cost — re-copying is the *accident* this row guards against, not an option |
| L17 | I2C IRQs (and any future EXTI) at kernel level 1; **level-0 ISRs must never call tk_*** | Level 0 is never masked by the kernel (`INTPRI_MAX_EXTINT_PRI=1`); `tk_sig_sem` from ISR context is legal only at levels 1–15 (L1 doc §3) | Kernel-architecture-locked |
| L18 | Per-frame ToF read = `VL53L1X_GetResult` (one 17-byte transaction), status gates frame validity | Status+distance in one transaction vs two; invalid ranges must not feed the v/a derivatives (L1X-v1 §6.4) | Reversible (GetDistance+GetRangeStatus fallback documented) |

---

## 5. PROCESS DISCIPLINES THAT CATCH SILENT FAILURES

Do these every time. Each traces to a §1 pattern that actually bit.

1. **Read content before trusting a filename.** Title block + first section vs
   filename, every doc you cite, attach, audit, or commit. (§1.6 — bit twice.)
2. **Design doc before implementation; review gate per driver.** No driver code until
   its design is reviewed; no hardware until the implementation is reviewed against the
   design. (The review gates are where F-1/F-4/M-1 were caught — all pre-hardware.)
3. **Every hardware claim cited or tagged.** Exact source with section/line, or
   `[UNVERIFIED → named step]`. "Recalled from the datasheet" is a tag, not a cite.
   (§1.1, §1.2.)
4. **Anti-contamination header in every new-part doc.** Table of axes-that-differ from
   the last part, each with its failure mode named. (§1.1.)
5. **Adversarial framing, not confirmation-seeking.** Reviews ask "where does this fail
   silently," carry verdict vocabulary (PASS / VIOLATION / VIOLATION RISK / UNVERIFIED),
   and treat not-in-evidence as UNVERIFIED — never assumed, never filled from memory.
   Never generalize from partially tested cases (the gate test proving `I2C_REG8` proves
   nothing about `I2C_REG16` — TOF-1).
6. **Symbols, not literals, at contract boundaries.** And read the dispatch code your
   call site lands in. (§1.3.)
7. **Identity read first; readback-verify after config writes.** ACKs prove the bus,
   not the device. (§1.4.)
8. **Fail loudly, never guess.** Bounded polls (attempts, not wall-clock — the ~145 ms
   failure envelope makes wall-clock budgets fiction), no fallback addresses, loud init
   failure. (M-2/F-8.)
9. **Lock-before-data.** Anything that scales, filters, or labels training data
   (L1/L2/L3/L10/L11) is frozen before the first Edge Impulse sample and never touched
   after. Run the physical sanity gate (IMU-6) first.
10. **Correct superseded text in place.** A correction that lives only in a newer doc
    leaves the old doc as a contamination source (audit A-2). Annotate or strike the
    stale text where it stands.
11. **No cross-task bare flags, ever.** Shared state rides in semaphore-protected
    buffers or is re-derived from device ground truth. (§1.5, §2.3.)

---

## 6. BUILD / FLASH VERIFICATION CHECKLIST

Run top to bottom after **every** Claude Code commit and before **every** flash.
"Committed ≠ built ≠ flashed" (2026-07-05: three wasted flash cycles). Never debug a
dead board before re-running this list.

**After any commit:**
- [ ] 1. Rebuild in STM32CubeIDE (a commit does not build anything).
- [ ] 2. `Debug/<ProjName>-Trusted.bin` exists and is non-zero.
- [ ] 3. `-Trusted.bin` **timestamp is NEWER than the commit** you intend to flash.
- [ ] 4. Signing command (post-build step, `Appli/.cproject`) contains **`-align`** —
      mandatory for CubeProgrammer ≥ 2.21 (we run 2.23). Missing `-align` = signed-looking,
      non-booting binary, zero errors (RZ1).

**Flashing (order is strict):**
- [ ] 5. External loader: `MX66UW1G45G_STM32N6570-DK.stldr`.
- [ ] 6. `ai_fsbl.hex` @ `0x70000000`.
- [ ] 7. `network_data.hex` @ `0x70380000` (model weights — note: README calls it
      `network_weights.hex`, which doesn't exist; trust the actual file).
- [ ] 8. `<ProjName>-Trusted.bin` @ `0x70100000`.

**Boot:**
- [ ] 9. Boot pins: Flash Boot = BOOT0 LOW + BOOT1 LOW. (Dev Boot = BOOT0 LOW +
      BOOT1/PA6/SW1 HIGH.)
- [ ] 10. Power: with camera module attached, Type-A→C USB **cannot** power the board
      (~550 mA limit, silent boot failure). C-to-C cable or powered source.
- [ ] 11. Confirm the heartbeat line on USART1 VCP (115200) before concluding anything
      about new code.

**New board / new cable / new programmer install:**
- [ ] 12. Flash the reference repo's PREBUILT binaries first to validate
      board+cable+programmer before building anything.

---

## 7. POST-FABLE WORKFLOW

**The tool split (unchanged in shape, degraded in depth):**

| Tool | Owns | Never does |
|---|---|---|
| Chat (Opus) | Architecture, design docs, **adversarial review passes**, datasheet analysis | On-disk edits, commits |
| Claude Code | On-disk implementation, greps/file-verification, commits, evidence collection | Unreviewed design decisions |
| Bench | Everything §3.2 marks `hardware`; all H-D/first-light items | — |

**Approximating the lost adversarial capacity on Opus.** Fable-class review found F-1,
F-4, and M-1 in wide multi-file passes. Opus can reach similar findings with the same
*method* at tighter scope:

1. **Scope one slice per pass** — one driver × one audit item class (task boundary /
   blocking / cache / semaphores / contamination), not six items × three drivers.
   The cross-cutting audit's structure (items 1–6) is the menu; order by §3.2's open
   items.
2. **Attach few files, verify each.** 2–3 files max per pass; confirm each attachment's
   first lines match its name before the review starts (§1.6 — this exact failure
   blocked the last audit).
3. **Demand the verdict vocabulary.** PASS / VIOLATION / VIOLATION RISK / UNVERIFIED,
   every claim with file:line, "NOT in evidence" declared up front, nothing filled from
   memory. Refuse prose reviews without verdicts.
4. **Hand it the §1 catalogue as the hunting list.** "For each pattern in
   PROJECT_DEFENSE.md §1, where could this design instance it?" is a stronger prompt
   than "review this."
5. **Expect to iterate.** Two or three narrow passes replace one wide pass. Budget the
   extra turnaround; don't compress by widening scope — wide+shallow is how silent
   failures survive review.
6. **Attach this document** (at minimum §1 and §5) to every review session so the
   reviewer inherits the failure classes without inheriting the sessions.

**Standing order of work for September:** §3.2's "Reading of the board" paragraph is
the queue. Re-derive it after every closed item; keep this file's ledger updated as
items close (updating THIS doc's status columns is allowed and expected — it exists to
be maintained).

---

## APPENDIX A — SOURCE-FILE DISCREPANCIES FOUND WHILE WRITING THIS

Found during the 2026-07-12 synthesis pass. **STATUS: all 12 items APPLIED 2026-07-12
with Kavin's approval** (A-1/A-2/A-5 approved on individual diffs; the rest as
reference/consistency fixes). During application, A-1 gained a fifth edit — (e) a stray
`0xEEAC` hard-value reference in the DRV doc's post-init paragraph, also harmonized.
The per-item text below is retained as the record of what was found and why; "proposed
fix" now reads as "applied fix."

**A-1 — Sensor-ID rule harmonization (user-directed 2026-07-12; audit F-3 / A-2 residual; TOF-9, DOC-2).**
- Files: `docs/design/vl53l1x_port_design.md` (§6 step 3, §8 remaining-opens H1);
  `docs/design/vl53l1x_port_design_v2_reconciliation.md` (§8 V-6 row);
  `docs/design/mpu6050_port_design_v1.md` (§0 table, "Identity check" row).
- Conflict: L1X-v1 hard-fails on ≠0xEEAC ("Anything but 0xEEAC = debug the shim") citing
  in-repo `api.h:197` (citation verified accurate 2026-07-12); L1X-v2 says 0xEACC per
  UM2510 with log-don't-hard-fail; MPU doc §0 repeats 0xEACC in isolation. ST's own docs
  are inconsistent across the family (field report: 0xEEAA on real silicon). Good
  hardware could read as a broken shim at first light.
- Proposed fix: (a) L1X-v1 §6 step 3 and §8-H1 — replace the hard-fail sentence with the
  harmonized rule: *expected values are documented inconsistently by ST (in-repo
  `api.h:197`=0xEEAC; UM2510=0xEACC; field-reported 0xEEAA); log and record any stable
  ID; hard-fail only on 0x0000/0xFFFF or no-ACK (bus conditions)* — cross-referencing
  L1X-v2 §6-L3; mark the old sentence superseded in place. (b) L1X-v2 §8 V-6 row —
  mark executed 2026-07-12 with the result (in-repo comment says 0xEEAC). (c) MPU doc
  §0 identity-check cell — point to the harmonized rule instead of bare "0xEACC".

**A-2 — v1 §6 step 3 pass-criterion text also asserts "0xEEAC (`api.h:197`)" with Confidence: certain.**
Subsumed by A-1(a) — listed separately only so the "Confidence: certain" tag is also
struck: the *citation* is certain; the *pass criterion built on it* is not.

**A-3 — DRV2605L buffer-alignment rule missing (audit F-6c; HAP-8).**
- File: `docs/design/drv2605l_port_design_v1.md` §4.
- Conflict: every DRV transfer hands 1-byte stack locals (`&val`) to the DMA primitive;
  no alignment/padding rule appears anywhere in the doc, unlike the MPU doc's
  `imu_buf[32] __attribute__((aligned(32)))`. Latent until D-cache enables.
- Proposed fix: add to §4 the same rule: one file-static
  `drv_buf[32] __attribute__((aligned(32)))` used for all DRV2605L reads/writes —
  OR (developer decision) hoist a bounce buffer into the primitive so no driver can
  get this wrong; note the D-cache-enable gate either way.

**A-4 — IN/TRIG arming-order rule missing (audit F-7; HAP-9).**
- File: `docs/design/drv2605l_port_design_v1.md` §2 (R-EN-1) and §3 wiring requirement.
- Conflict: the doc requires IN/TRIG "driven low at init — never floating" but no
  ordering rule ties GPIO-configured-low strictly BEFORE EN rises and BEFORE row 8 arms
  MODE=1. A floating trigger input in edge-trigger mode can noise-fire spurious clicks —
  a false hazard alert to a visually impaired user is a functional safety event.
- Proposed fix (audit's one-sentence rule, merged into R-EN-1): "IN/TRIG GPIO is
  configured push-pull and driven low before EN(PE7) rises; row 8 must never execute
  with IN/TRIG unconfigured."

**A-5 — DEV_RESET poll budget still wall-clock (audit F-8; HAP-10).**
- File: `docs/design/drv2605l_port_design_v1.md` §4 row 0.
- Conflict: "≤10 ms timeout" is unachievable through the primitive's ~145 ms per-failed-
  attempt envelope (`app_i2c.c:357-362`). The equivalent MPU6050 poll was already fixed
  attempts-based (M-2, `f804281`); the DRV row the lesson originated from was not.
- Proposed fix: respecify as ≤N attempts (suggest ≤5, matching the MPU fix), `tk_dly_tsk(1)`
  pacing, fail init loudly on exhaustion; note per-attempt worst case. HAP-7 (measure
  actual self-clear time) unchanged.

**A-6 — Standby-window I2C wedge note missing (audit F-9; HAP-11).**
- File: `docs/design/drv2605l_port_design_v1.md` §6.2 (independent bounds) or §6 tail.
- Conflict: doc records the I2C watchdog exception ("only a power cycle recovers I2C"
  in standby) but doesn't note that init rows 0–1 execute inside that window and that
  the L1 recovery ladder + EN-toggle cannot clear it.
- Proposed fix: one note: "the L1 bus-recovery ladder does not cover an I2C wedge
  occurring while the device is in power-on standby (rows 0–1); recovery is bench
  power-cycle; probability low (two-transaction window)."

**A-7 — BUS-1/IMU-2 evidence found; three ledgers still carry the items as open.**
- Files: `docs/design/vl53l1x_port_design_v2_reconciliation.md` §8 (V-4);
  `docs/design/mpu6050_port_design_v1.md` §8 (V-2, V-5);
  `docs/design/drv2605l_port_design_v1.md` §9 (H-D8 desk half).
- Conflict: verified 2026-07-12 — `app_i2c.h:15` `#define I2C_BUS_HZ 400000u`,
  `app_i2c.h:19` `#define I2C_REG8 1u`. All three parts are 400 kHz-max class → the
  desk halves of V-4/V-5/H-D8 and V-2 are closed by evidence; the docs still list them
  open.
- Proposed fix: annotate each row closed-with-cite (400 kHz target confirmed ≤ every
  device max; wire-level capture remains open as BUS-2).

**A-8 — `app_i2c.c:421-424` WHO_AM_I comment still says UNVERIFIED (addendum action; IMU-5).**
- File: `firmware/Appli/Core/Src/app_i2c.c:421-424`.
- Conflict: the claim was verified against RM rev 4.0 (`{RM §4.34}`: reg 0x75 default
  0x68, AD0 not reflected) — MPU doc §0 and the addendum both direct the comment update;
  the comment is unchanged (verified 2026-07-12).
- Proposed fix: update the comment to verified-with-cite at the next code commit
  (then rebuild per §6 before any flash).

**A-9 — Design-doc ledger rows stale against CLAUDE.md locks.**
- Files: `docs/design/drv2605l_port_design_v1.md` §9 (H-D1 row) and §10 (item 1).
- Conflict: CLAUDE.md records H-D1(a) CONFIRMED (Adafruit `INT`, SmartElex `IN`, with
  the Adafruit 1.8 V-max divider requirement) and loop-mode = open-loop LOCKED; the DRV
  doc still shows H-D1 fully open and loop mode awaiting sign-off.
- Proposed fix: annotate H-D1 row "(a) CLOSED per CLAUDE.md §2 2026-07-10 — (b) GPIO
  allocation still open"; annotate §10 item 1 "decided — open-loop locked (CLAUDE.md §2)".

**A-10 — D-cache gate line not yet in CLAUDE.md (audit recommendation).**
- File: `CLAUDE.md` §3 (cache-maintenance block).
- Conflict: the audit recommends a single recorded line gating D-cache enable on
  F-6a/b/c; CLAUDE.md's cache section doesn't carry it. Without it, a future session
  can flip `USE_DCACHE` innocently and arm all three latent defects (§2.2).
- Proposed fix: add one line: "D-cache enable is GATED on closing F-6a (write-side clean
  in primitive), F-6b (ULD bounce buffer), F-6c (DRV buffer rule) — see
  PROJECT_DEFENSE.md §2.2."

**A-11 — CLAUDE.md haptic-semantics wording vs DRV baseline (minor).**
- Files: `CLAUDE.md` §6; `docs/design/drv2605l_port_design_v1.md` §3.
- Conflict: CLAUDE.md says "intensity/pattern-graded urgency (pulse rate ∝ closing
  velocity)"; the DRV design's baseline grades by pulse **rate only**, with
  intensity-band retuning (rewriting 0x04 from P3) explicitly deferred. Not a
  contradiction, but a reader could implement the deferred half prematurely on
  CLAUDE.md's wording.
- Proposed fix (optional): CLAUDE.md §6 wording → "urgency graded by pulse rate
  (∝ closing velocity); intensity banding deferred (DRV design §3)."

**A-12 — Audit-report reading aid (no edit to the reports themselves proposed).**
- Files: both reports under `docs/audits/`.
- Conflict: the reports' finding registers reflect 2026-07-11; six findings have since
  been fixed. Rewriting dated audit reports would be historical falsification — not
  proposed. The risk is a future reader acting on the stale register.
- Proposed fix: add a two-line header note to each report — "Snapshot of 2026-07-11.
  For live finding dispositions see `docs/PROJECT_DEFENSE.md` §3.1." (Alternatively:
  no edit, and rely on this doc's prominence. Kavin's call.)

---

*End of PROJECT_DEFENSE.md. Maintenance rule: §3's status columns and §3.2's
"Reading of the board" are living state — update them as items close. §1, §4, §5 change
only when a new failure class is caught or a new decision is locked.*
