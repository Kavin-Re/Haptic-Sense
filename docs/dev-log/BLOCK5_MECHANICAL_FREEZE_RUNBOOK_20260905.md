# BLOCK 5 — MECHANICAL FREEZE RUNBOOK
**Written 2026-09-05. Round-trip shipment constraint incorporated.**
Supersedes `PLAN_TO_SUBMISSION_20260830.md` Block 5 (steps 21–24), which was four lines.
Everything electrical is fixed by `CLAUDE.md` §2. **Nothing here rewires anything.**

Inputs assumed, confirmed by the developer 2026-09-05:
- **No mechanical build materials in hand.** Not the protoboard, not the consumables.
- **Spares: SmartElex DRV2605L ×1 only.** No spare VL53L1X, no spare GY-521, no spare ERM.
- **Two bench days allotted (6–7 Sep).**

---

## 0. THE FINDING THAT REORDERS THIS BLOCK

**Block 5 cannot start on 6 Sep. There is nothing to build with.** The 6–7 Sep window was
sized for assembly, not for procurement, and procurement is now the gate.

`HANDOFF_20260903.md` §6 listed the mechanical BOM as "order now, not on 17 Sep" on 2026-09-03.
It was not ordered. That is the actual state; the plan should reflect it rather than carry a
date it cannot meet.

| | |
|---|---|
| Today | Sat 5 Sep, evening |
| Physical component market | **Mon 7 Sep** — realistic first buying day |
| Online (Robu / Amazon / local) | 2–4 days ⇒ 8–10 Sep, and it does not fail gracefully |
| **Revised assembly window** | **Mon 7 Sep (evening) – Wed 9 Sep** |
| Slip to downstream blocks | **~2–3 days**, absorbed by Blocks 6–8, not by Block 9 |

**Buy in person on 7 Sep. Every line in §3 is generic stock.** An online order is the wrong
risk shape here: a single missing item on 10 Sep costs more than a day of shopping does.

**Non-negotiable:** Block 9 (evidence capture) does not move. If Blocks 6–8 compress, they
compress. `HANDOFF_20260903.md` §9 is right — protect the archive above the features.

### 0.1 What to do on 6 Sep instead (zero-material work, all of it gating)

The rebuild has three preconditions that need no parts, and doing them on 6 Sep means 7 Sep
evening is assembly rather than decision-making:

1. **The firmware change in §1** — written, built, flashed, and left on the board. It must be
   on the board *before* the post-rebuild soak, or the soak validates a binary that is not the
   shipping binary. See §1.3.
2. **Draw the deck.** A physical placement drawing on paper: where each breakout sits, which
   way the ToF points, which way the IMU's +X points, where the motor leads exit, where the
   decoupling caps go. **You cannot improvise this with a hot iron in your hand and no spares.**
3. **Commit the untracked evidence.** `ADVERSARIAL_REVIEW_I2C_DECISION_20260905.md` §2.8: the
   30-minute soak logs backing the 09-05 decision are untracked and in the repo root. One
   command. Do it before touching hardware.

---

## 1. THE TWO FIRMWARE GAPS — JUDGMENT, AND THE SEQUENCING RULE

You asked whether the DRV2605L kill path and the fault-response gap should close before the
unit ships. **One yes, one no, and they are not close calls.**

### 1.1 The kill path (STANDBY write from the hazard task) — DO NOT IMPLEMENT

The thing a kill path defends against is a motor that will not stop. **This design cannot
produce one.**

- Playback is a finite one-shot ROM effect. `H-D3` measured it at **58,605–58,708 µs**, and
  §8.4.3 says GO clears itself when the sequence completes. There is no continuous-drive mode
  in this configuration.
- The only path to sustained output is repeated TRIG edges, and those are already bounded in
  firmware by `DRV_R3_FLOOR_MS = 75` — a hard ceiling of ~13 pulses/s, with `suppressed`
  counting every refusal.
- EN is on PE7 and initialised LOW. **The emergency path already exists in copper**; it is
  simply never exercised, which is exactly what Decision 4 chose (`PLAN` §1, R-1).

Against that, implementing it now means adding a **new I2C write path invoked from a fault
condition** to a binary you will never be able to debug again after packing. `CLAUDE.md` §3
also forbids I2C from TK_PRI 1, so it would have to be a P1→P3 request hop — new shared state,
new race surface, in the last week. That is a worse trade than the risk it removes.

**Ship it as a documented, designed, deliberately unexercised path.** That is a defensible
sentence in the write-up and a correct engineering position. The judge's actual kill path is
the USB cable, and the printed card says so.

### 1.2 Fault response — IMPLEMENT EXACTLY ONE: bounded re-arm on `cfg_lost`

The two counters are not the same kind of thing and should not get the same treatment.

**`cfg_lost` (MODE no longer 0x01) → re-arm. This one matters, and it is on the
disqualification criterion.**

`cfg_lost` means the DRV2605L has lost its configuration — the mechanism is documented in
`CLAUDE.md` §8 H-D2: the part **retains registers across an MCU reset but loses them across a
true power cycle**, so any VDD dip at the breakout resets it to 0x40 defaults. In that state
TRIG edges do nothing. The demo does not error. It does not log a failure a judge can see.
**The motor just silently stops working, permanently, for the rest of that power-on.**

`CONTEST_LOGISTICS.md` §3, verbatim: *"if we are unable to confirm that your submitted program
works properly, your entry will be disqualified."* A unit that sits unattended, browns out once
on a shared power strip in Tokyo, and is thereafter a board with a dead motor is that condition
exactly. The fix is calling `drv2605l_init()` — code that is already written, already proven on
hardware, already idempotent (it issues DEV_RESET and confirms MODE reads 0x40 first).

Constraints, so this does not become the next silent failure:

- **From TK_PRI 3 only.** It is I2C. `drv2605l_poll()` already runs there.
- **Bounded.** `DRV_REARM_MAX = 8` per boot. A device genuinely gone must not turn into an
  unbounded I2C hammer that takes the ToF path down with it.
- **Rate-limited.** No more than one re-arm attempt per poll cycle (~1 Hz), never in a loop.
- **Counted and printed.** New `rearm` and `rearm_fail` fields in `drv2605l_stats_t`, on the
  `[HLT]` heartbeat line. A re-arm that happens silently is worth nothing as evidence.
- **`check_soak.py` gate:** `rearm == 0` on a healthy bench run. Fix the empty-set defect first
  (`ADVERSARIAL_REVIEW` §4.4 — `set() <= {0}` is `True`, so a check on an absent counter passes),
  or you are adding a check that cannot fail.

**`faults_seen` (OVER_TEMP | OC_DETECT) → keep logging. Do not act.**

The asymmetry runs the other way here. A fault-driven shutdown that fires spuriously produces
a permanently dead demo — the same disqualification-adjacent outcome, arrived at by your own
code. And OC_DETECT is the **DRV2605L's own protection already having acted**: the part limits
its own drive, which is precisely how the 2026-08-30 twisted-lead short was survived without
damage (`CLAUDE.md` §8: coil undamaged, effect duration unchanged across three boots). Adding a
software shutdown on top of working hardware protection buys nothing and can only subtract.

Also: after §2 of this document, the wire fault that produced OC_DETECT cannot recur — every
motor conductor is soldered, sleeved and offset. Do not write software to defend against a
failure mode you are about to delete mechanically.

### 1.3 The sequencing rule this creates

**Any change to the shipping binary lands before the post-rebuild 30-minute soak, not after.**

The post-rebuild soak (§4, Gate G5) is the last full-system validation this project gets. If
firmware changes after it, that soak characterises a binary that is not on the board — the
exact `CLAUDE.md` §9 trap ("committed ≠ built ≠ flashed") that cost three flash cycles on
2026-07-05, and the same question `ADVERSARIAL_REVIEW` §3.6 had to raise about
`soak_30min_20260905_postsolder4.log`.

So: **§1.2 on 6 Sep. Rebuild 7–9 Sep. Soak the rebuilt hardware running the shipping binary.
Firmware freeze on the board is effectively 6 Sep**, independent of the source-code freeze on
30 Sep (`CONTEST_LOGISTICS.md` §9.6).

---

## 2. THE BUILD — TOPOLOGY AND THE FOUR RULES

### 2.1 Topology: one deck, one separable interface

```
        rigid base plate (3 mm acrylic / ply / FR4)
              |  nylon standoffs, M3, screwed
        STM32N6570-DK
              |  ONE male-pin interface into CN7/CN8/CN11/CN12, seated once, then locked
        PROTO DECK  ── VL53L1X (7SEMI)   direct-soldered, aim fixed
                    ── MPU6050 (GY-521)  direct-soldered, axes fixed
                    ── DRV2605L (SmartElex) direct-soldered
                    ── local decoupling at each VIN/GND
                    ── motor leads terminate here, strain-relieved
              |
        ERM  ── bonded to the BASE PLATE, not to the deck  (see Rule 3)
```

**Rule 1 — Direct-solder the breakouts. No sockets.**

This is decided by the spares answer, not by taste. Sockets exist to make a device swappable;
with **no spare VL53L1X, no spare GY-521 and no spare ERM**, there is nothing to swap in, so a
socket buys zero recoverability and costs a fretting/unseating interface across a round trip
and 2–3 months of storage. The breakouts already carry male header pins from the breadboard
build — those pins go through the deck and get soldered on the underside. That joint is
gas-tight, vibration-immune, and still recoverable with a solder sucker if a board ever dies —
which is far less destructive than the 0402 rework this project has (correctly) refused twice.

**Rule 2 — Every motor connection soldered, sleeved, offset, strain-relieved.**

This is the H-D9 rule, earned on 2026-08-30, and the round trip makes it stricter, not looser.
Concretely: motor leads terminate in **through-hole pads on the deck**, not in a mid-air joint;
each joint individually heat-shrunk; the two conductors **offset along the wire run so their
joints are not adjacent**; a service loop; and a cable-tie anchor within ~10 mm of the pads so
no mechanical load ever reaches solder. `CLAUDE.md` §8 records what the alternative looks like:
two bare joints a centimetre apart that shorted *only while the motor vibrated*, with every
other indicator on the bus reading clean.

**Rule 3 — Mechanically decouple the ERM from the deck. This is not a robustness point.**

Two independent reasons, and the second is the one that has not been written down anywhere in
this project yet:

1. *Robustness.* Every intermittent-contact fault this project has found — the OC_DETECT short,
   and the twisted SDA/SCL joint — appeared **only under motor vibration**. Bonding the ERM to
   the same rigid deck that carries all three I2C devices routes that vibration straight into
   every joint you are about to make.
2. *Training-data integrity, and this one is irreversible.* If the ERM is rigidly coupled to the
   GY-521, the accelerometer sees the motor every time the motor fires. The motor fires on
   hazard. So `ax, ay, az` acquire a component that is **causally downstream of the label**, and
   Block 6 bakes it into the training set. The model can then learn "vibration ⇒ hazard", which
   is present in training, circular at inference, and completely invisible in every metric this
   project currently collects. `DLPF_CFG = 4` (~21 Hz) attenuates a ~200 Hz ERM strongly but
   **attenuation is not rejection** — rectification and clipping at ±4 g can still leave a DC
   term. Collection is marked IRREVERSIBLE in `PLAN` Block 6; this confound is not detectable
   after the fact.

So: ERM bonded to the **base plate** (foam pad as damper, cable tie as primary retention —
adhesive alone creeps over months in a warm hold), on a rigid tab so it is still clearly
**audible and palpable** for the judge. Then measure the residual (§4, Gate G4b).

**Rule 4 — The deck↔DK pin interface is inserted once and locked.**

`CN8 pin 5 is +5 V and adjacent to pin 4.` Count twice, meter pin 4→pin 7 for 3.25–3.35 V
before the deck is ever powered (V-W-1 discipline). Then seat it once, bolt the deck to the DK
through the mounting holes with nylon standoffs, and put a small bead of hot-melt at two
opposite corners so the **pins carry no load and cannot walk out under vibration**. Lifetime
insertion count: 1. The card in the box says *do not separate the boards.*

### 2.2 Round-trip degradation — what actually changes

You asked what degrades over 2–3 months in transit and storage plus a return leg. Three things,
and only one of them costs money:

- **Connector fatigue** — addressed by construction: exactly one connector in the whole
  assembly, inserted once, mechanically locked (Rule 4). Nothing else mates.
- **Solder-joint stress from handling** — addressed by Rules 1–3: nothing flexes, no wire is
  load-bearing, the vibration source is off the deck. A joint that never moves does not fatigue.
- **Corrosion / humidity** — this is the one that needs a consumable. Monsoon-season India →
  cargo hold → a Tokyo office → months of storage → back again. Two mitigations:
  - **Clean the flux.** No-clean residue is mildly hygroscopic; at 593 Ω pull-up impedances it
    is irrelevant, but across the motor pads at 60–90 mA it is not. IPA 99% and a brush, then
    dry, after every soldering session. Costs nothing.
  - **Desiccant.** 2–3 silica-gel sachets in the ESD bag with the unit. Costs ~₹50 and is the
    entire humidity answer for a benchtop prototype.
  - **Conformal coating: no.** It is a one-shot irreversible process on unspared boards, it
    needs masking around every connector and the ToF's optical window, and a bad coat is a new
    failure mode you cannot see. The risk/benefit is wrong with zero spares and two days.

---

## 3. BOM — buy all of this in person, 7 Sep

| Item | Qty | Note |
|---|---|---|
| Arduino UNO-shape prototyping shield | **2** | ×2 because it is now the one-of-one part. **Dry-fit before soldering anything** — verify it seats CN7/CN8/CN11/CN12 (UM3300 §8.7 Table 16). If none is stocked, plain perfboard + male header strips cut to length is the fallback and is universally available. |
| Male header strip, 2.54 mm, long-pin | 2 strips | For the fallback path, and spares |
| **0.1 µF ceramic** | 10 | §4 Stage 2 — fit at *every* breakout VIN/GND |
| **10 µF ceramic** | 5 | VL53L1X VCSEL pulse current (`HANDOFF_20260903.md` §4.1) |
| **47–100 µF electrolytic** | 3 | DRV2605L VCC, motor inrush |
| Silicone-jacket **stranded** hookup wire, 24 AWG | 2 m ea, 2 colours | **Stranded, not solid.** Corrects the 2026-09-03 list: solid core work-hardens and cracks at a vibrating termination, which is precisely the motor lead |
| Heat-shrink, 1.5 / 2.4 / 3.2 mm | 1 pack | Per-joint sleeving, Rule 2 |
| Nylon standoffs M3 + screws + washers | 1 set | Deck→DK and DK→base plate |
| Rigid base plate, 3 mm acrylic / ply / FR4 | 1 | ~1.5× the DK footprint |
| Cable ties 2.5 mm + adhesive tie mounts | 1 pack | Strain relief and ERM retention |
| Hot-melt glue sticks | few | Reworkable. Prefer over epoxy everywhere except the standoff bond |
| Kapton tape | 1 roll | Masking, and insulating under breakouts |
| IPA 99% + brush | 1 | Flux removal, §2.2 |
| Silica-gel sachets | 3–5 | §2.2 |
| ESD bag, closed-cell foam, rigid carton | 1 ea | Block 10 |
| **Spare VL53L1X · spare GY-521 · spare 10 mm ERM** | 1 ea | **Order these even if they land after the freeze.** A spare arriving 12 Sep still protects Blocks 6–9, which are the irreversible ones |

**The decoupling caps are the line item that cannot be added later.** The rebuild is the last
moment they can be fitted. `HANDOFF_20260903.md` §4.1 has been carrying this as a conditional
("if `[RTY]` is still non-zero") since 2026-09-03; with the board about to be soldered shut,
fit them unconditionally. They are ₹20 and they close the VIN-brownout class permanently.

---

## 4. THE RUNBOOK — STAGES AND GATES

**The single most important sequencing rule: verify after every device, not at the end.** With
no spares, a fault found after all three are soldered is not localisable. Each stage below has
its gate, and you do not proceed past a failed gate.

Every gate uses the tooling that already exists: `picocom -q -b 115200 /dev/ttyACM0 | tee`,
then `python3 docs/evidence/phase5/check_soak.py <log>`.

### Stage 0 — 6 Sep, no parts needed
- [ ] `git add` the untracked soak logs into `docs/evidence/phase5/` (`ADVERSARIAL_REVIEW` §2.8)
- [ ] Implement §1.2 (bounded re-arm on `cfg_lost` + `rearm`/`rearm_fail` counters + soak gate)
- [ ] Fix `check_soak.py`'s empty-set defect **first** (`ADVERSARIAL_REVIEW` §4.4)
- [ ] CubeIDE **Refresh (F5) → Build → verify `-Trusted.bin` timestamp is newer → flash**
- [ ] 10-minute soak on the *existing breadboard* to prove the new binary is not a regression
- [ ] Draw the deck placement on paper (§0.1 item 2)
- **Gate G0: `check_soak.py` no worse than the `pullupfix2` baseline, and `rearm = 0`.**
      This is the reference the rebuild will be measured against. Save the log.

### Stage 1 — 7 Sep evening: dry-fit, no solder
- [ ] Dry-fit the proto shield onto CN7/CN8/CN11/CN12. Confirm it seats fully and squarely.
- [ ] Meter **CN8 pin 4 → pin 7 = 3.25–3.35 V** before anything is connected. Count twice —
      pin 5 is +5 V and adjacent, pin 3 is NRST.
- [ ] Lay out all three breakouts on the deck against the paper drawing. Check that the ToF's
      optical window has clear line of sight and nothing overhangs it.
- [ ] Mark the deck permanently with a paint-pen **+X arrow** and a **ToF aim arrow**. A photo
      is only interpretable later if the axes are physically written on the object.
- **Gate G1: rail in spec, mechanical fit confirmed, geometry decided and marked.**

### Stage 2 — 7–8 Sep: solder, one device at a time, gate after each

Order is deliberate: **the spared board first.** The SmartElex DRV2605L is the only device with
a spare and is already the most-proven device on the bus, so it is both the technique rehearsal
and the lowest-consequence mistake.

For each device, in this order — **DRV2605L → VL53L1X → GY-521**:
1. Solder its header pins through the deck. Underside joints.
2. Fit its local decoupling (0.1 µF + 10 µF at VIN/GND; add the 47–100 µF at the DRV2605L VCC).
3. Run its I2C wiring to the deck's SDA/SCL rail. Soldered joints only — **no twisted joints
   anywhere, ever again** (`PHASE5_I2C_TWISTED_JOINT_20260905.md` is the whole reason).
4. Clean flux with IPA. Let it dry.
5. **Gate G2.x — power up and run the existing probe path before soldering the next device:**
   - Bus scan: only the expected addresses answer
   - `[DRV] init=0 id=7 mode=0x1 lib=0x2 odc=0x8b armed=1` (after DRV2605L)
   - `[RNG] init=0 id=0xeacc boot=1 cfg=91 cfgfail=0x0` and `[RTY] all zero` (after VL53L1X)
   - `[ACC] pm=0x1 drift=0 rderr=0` and non-zero accel (after GY-521)
   - **Also re-measure SDA↔SCL resistance, power off, after the SmartElex is mounted** —
     confirm the desoldered `I2C-PU` jumper is still open post-rework (expect ~174 kΩ, not
     ~4.3 kΩ). Reheating that board during mounting is exactly how it would come back.

### Stage 3 — 8 Sep: motor, then geometry lock
- [ ] Bond the ERM to the **base plate** per Rule 3. Foam pad + cable tie. Not the deck.
- [ ] Motor leads to deck pads per Rule 2: soldered, individually sleeved, joints offset,
      service loop, cable-tie anchor within ~10 mm of the pads.
- [ ] **V-W-6 re-check:** meter ERM coil DC resistance with the motor disconnected. Expect
      ~32.5 Ω (`CLAUDE.md` §8 H-D6). Any large deviation means a damaged coil or a bad joint.
- [ ] Bolt the deck to the DK, bolt the DK to the base plate, hot-melt the two corners (Rule 4).
- **Gate G3 — effect duration and the R-3 floor.** This is the check you asked about, and it
  has a number. Run enough hazard events to fill `eff_n`, then:
  - **`eff_max_us` must stay below 62,500 µs.** The R-3 floor is 75 ms and the rule is
    floor ≥ 1.2 × measured max (`CLAUDE.md` §8 H-D3). `check_soak.py` line ~205 already enforces
    `floor_ratio >= 1.2` — it will fail on its own if this shifts.
  - Baseline to compare against: **58,505–58,760 µs** across three boots, 0.44% spread. A
    rebuilt motor mount should not move this at all. If it does, the coil or the joint changed.
  - `eff_late = 0`, `eff_stuck = 0`, `eff_rderr = 0`, `faults_seen = 0x0`.
- **Gate G4a — flat-bench IMU gate, and it belongs here, not in Block 6.** The IMU's
  orientation just changed, so re-run IMU-6 **now**: flat bench, **Z ≈ +1000 mg ± 80 mg**.
  Record the actual reading. `PLAN` step 25 puts this before sample one; doing it at the end of
  the mechanical build is what makes the mounting itself the thing being verified.
- **Gate G4b — the vibration-coupling measurement (new, from Rule 3).** Board stationary on the
  bench. Log `ax, ay, az` across ~30 s of *no* motor activity, then across ~30 s of firing at
  the R-3 floor. **Compare the two distributions.** If firing shifts the accel statistics
  measurably, the ERM is still coupled to the IMU and Rule 3's second reason is live — re-mount
  before Block 6, because collection cannot be undone. Record the numbers either way; "we
  measured the confound and it was N mg" is a submission-grade sentence.

### Stage 4 — 8–9 Sep: the courier simulation, then the baseline soak

Do the mechanical provocation **before** the long soak, so the soak characterises hardware that
has already been shaken rather than hardware that is about to be.

- [ ] **Tap and shake test, running, watching live:** with the demo running and the serial log
      open, tap the deck, tap each breakout, flex the base plate gently, tilt through all
      orientations, and shake the assembly at courier-plausible amplitude for ~60 s. Watch
      `recov`, `nacks`, `err`, `rderr`, `faults` in real time.
      **Any counter that moves during this test is a joint to redo, and you find it now while a
      soldering iron is still an option.** This is the actual thing being verified in Block 5 —
      the 30-minute soak measures a stationary board and cannot see it.
- [ ] **Gate G5 — fresh RESET, 30-minute soak, gap-free, captured inside `tmux`** (two captures
      on 09-05 were killed by a desktop idle event; `tmux` made it immune regardless of cause).
      **10 minutes under-samples this fault class — §1 of the twisted-joint document establishes
      that directly. 30 minutes minimum.**
      Pass criteria, stated as numbers so this is not a vibe check:
      | counter | 09-05 baseline (`postsolder4`) | 09-06 baseline (`pullupfix2`) | rebuild must not exceed |
      |---|---|---|---|
      | `err` (hard failures) | 1 | 1 | **1** |
      | `[ACC] rderr` | 1 | 1 | **1** |
      | `nacks` | 1 | 3 | **3** |
      | `recov` | 4 | 11 | **11** |
      | `faults_seen` / `cfg_lost` / `rearm` | 0 | 0 | **0** |
      | `canary_err` / `qovr` / `frames==inf` | clean | clean | **clean** |

### Stage 5 — 9 Sep: the things that cannot be redone after packing
- [ ] **Photograph everything, thoroughly, while it is open and lit.** Every face, every joint,
      the axis markings, the ERM mount, the deck underside. Submission material, and
      unrepeatable.
- [ ] **Write the geometry down in words**, in a dated evidence file, next to the photos: the
      IMU's +X/+Y/+Z relative to the deck, the ToF's aim vector, the measured flat-bench Z. A
      photo without a written axis definition is not a record.
- [ ] **Cold-boot test, host disconnected** (`CONTEST_LOGISTICS.md` §3 — this is the
      disqualification bar, not a nicety). Unplug everything, power from the C-to-C only,
      confirm LD1 heartbeat and hand-wave → buzz with no terminal attached.
- [ ] **Cold-boot with the GY-521 physically disconnected** (`ADVERSARIAL_REVIEW` §3.5). The
      MPU6050 driver postdates `PROJECT_AUDIT` §1.2's `E_OK == 0` / BSS-zero sentinel finding
      and has never been checked against it. Required behaviour: boots, says something a judge
      reads as "IMU absent", **and keeps the ToF→haptic chain running.** A hang here is on the
      disqualification criterion. Reconnect and re-verify afterwards.
- [ ] Leave that exact binary on the board. Archive the `-Trusted.bin` and the flash procedure.

---

## 5. PACK — the round-trip-specific items

Everything in `CONTEST_LOGISTICS.md` §7 "Before packing" stands. What the return leg adds:

- [ ] **Printed card**, in a sleeve so it survives handling both ways. Contents: what it is; how
      to power it (**C-to-C cable — note the A-to-C boot failure claim is NOT true for this
      build, measured 2026-08-30, so do not print it**); what to wave a hand at; what the
      vibration means; **"do not separate the two boards"**; **"unplug USB to stop"**; Entry ID
      **34476**; contact email and phone.
- [ ] **Return address on a label INSIDE the box as well as outside.** The hardware comes back
      after the December Awards Ceremony (`CONTEST_LOGISTICS.md` §10.3); an outer label will not
      survive two legs and months of storage.
- [ ] Entry ID 34476 written on the outside of the box.
- [ ] Silica-gel sachets in the ESD bag (§2.2).
- [ ] C-to-C cable in the box. Packing list. Cover letter.
- [ ] **Battery confirmation, for the customs declaration.** Physically inspect the
      STM32N6570-DK for any coin-cell/VBAT holder and confirm it is **empty**, then confirm the
      deck and the ERM carry no cell of any kind. The build is USB-powered by design
      (`CLAUDE.md:4`), so the lithium-battery declaration reduces to **"none"** — but write down
      that you looked, because the declaration is a signed statement.
- [ ] Photograph the sealed package. Keep the tracking number.

---

## 6. WHAT THIS REBUILD ALSO SETTLES — treat it as the experiment it is

The project has been unable to separate two competing explanations for the residual I2C fault:
**contact quality** versus **the pull-up budget**. `ADVERSARIAL_REVIEW` §2.7 points at contact
quality (4 timeout-class events vs 1 NACK-class) but with n=4 and n=1 that discriminates almost
nothing.

**Stage 2 replaces every I2C joint in the system with a fresh soldered one.** That makes Gate G5
the discriminating measurement this project has not been able to make:

- If `recov` drops sharply from 11 → low single digits, **contact quality was the driver**, the
  09-05 accept-as-is decision is retrospectively correct, and the GY-521's pull-ups stay
  untouched with a clear conscience.
- If `recov` stays at ~10 or higher **after every joint has been remade**, contact quality is
  ruled out and the pull-up budget is the live suspect again — at which point the GY-521
  desoldering question genuinely reopens, with an actual measurement behind it rather than n=1.

Write the answer into the evidence record either way. It is the cheapest experiment available
and it comes free with work already required.

---

## 7. EXPLICITLY NOT DOING — recorded so it is not re-litigated at 1 a.m.

- **DRV2605L kill path** (§1.1). Designed, wired in copper, deliberately unexercised.
- **Fault-driven shutdown on `faults_seen`** (§1.2). The part protects itself; a spurious
  shutdown is worse than the fault.
- **Desoldering the GY-521's pull-ups.** Unchanged from 2026-09-05 — unless §6 reopens it with
  data.
- **Conformal coating** (§2.2). Wrong risk shape with zero spares.
- **Sockets for the breakouts** (Rule 1). Nothing to swap in; a mating interface is pure
  round-trip risk.
- **Chasing `recov` further before the rebuild.** The rebuild is the experiment (§6).
- **Any rewiring.** `CLAUDE.md` §2 is locked. This block is mechanical realisation only.
