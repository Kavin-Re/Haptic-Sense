# FOUR DAYS WITHOUT AI — 30 AUG (night) → 3 SEP 14:30
**Written 2026-08-30 ~21:00 IST, at commit `0a82205` + uncommitted work.
This document is meant to be the only thing you need open.**

You are not losing four days of Block 2. Block 2 in plan v2 is **1–4 Sep**, so
these four days **are** Block 2, and almost all of Block 2 is bench work and
decisions — neither of which needs an AI. The parts that genuinely needed one
are already written and sitting in your tree.

Read §0 first. If you only get one hour this week, spend it on §2.1.

---

## 0. STATE — WHAT IS TRUE RIGHT NOW

### 0.1 On the board
The binary flashed at ~15:30 today. It contains **the NACK fix and the L1 raw
probe, and nothing else**. It has been soaked 9.9 minutes, 567 heartbeats,
16 checks, one expected failure (`[TOF] res=-42`, the VL53L1X is not on the
bus). `docs/evidence/phase5/soak_10min_20260830_nackfix.log`.

**Block 1 is intact.** The haptic chain still works and is still your fallback
demo. Do not break it.

### 0.2 In the tree, uncommitted
| file | state | compiled for target? |
|---|---|---|
| `app_i2c.c` / `.h` | NACK classification, `nacks` counter, L1 probe | **YES — flashed and soaked** |
| `check_soak.py` | 14 checks → 16 | n/a (Python, re-run and verified) |
| `app_vl53l1x.c` / `.h` | **NEW** — the L2 driver, ~280 lines | **NO — syntax-checked only** |
| `app_tasks.c` | `[RNG]` prints, driver call sites, the `/600u` fix | **NO** |
| `docs/BLOCK2_PREFLIGHT_20260830.md` | twelve findings, full citations | n/a |
| `commit_block2.sh` | the commit split, ready to run | n/a |

**`app_vl53l1x.c` and the `app_tasks.c` changes have never been through
`arm-none-eabi-gcc`.** They passed `gcc -Wall -Wextra` against stub typedefs,
every ULD function used is declared in `VL53L1X_api.h`, and braces balance —
but that is not the same as compiling. **Build tonight** (§1). A compile error
found tonight gives you four days to fix it; one found Wednesday does not.

### 0.3 Not on the bench yet
- VL53L1X (7SEMI) — unsoldered. This week's job.
- MPU6050 (GY-521) — unsoldered, and **do not solder it** (§6).
- **100 µF bulk capacitor across 3V3/GND — still not fitted.** T3 step 5, owed
  since 30 Aug, and it is owed *before* the sensors join that rail.
- **V-W-1 — the 3V3 rail has never been metered.** 60 seconds. Never done.

---

## 1. TONIGHT, 30 MINUTES. DO NOT SKIP THIS.

The single highest-value 30 minutes of the week, because everything else
depends on the code compiling.

```bash
cd ~/haptic-sense
bash commit_block2.sh          # dry run — read what it will do
bash commit_block2.sh --go     # three commits, you as the author
```

Then in **CubeIDE**:

1. **Right-click the project → Refresh (F5).** The managed build generates
   `Debug/**/subdir.mk` by scanning the source folders; without a refresh it
   will not see `app_vl53l1x.c` and you will get undefined-reference errors at
   link time that look like a code problem and are not.
2. **Project → Build.**
3. If it builds: confirm `Debug/STM32N6_MTK_Person_Detection_Appli-Trusted.bin`
   exists, is non-zero, and its timestamp is newer than the commit (CLAUDE.md
   §9). **Do not flash yet** — flash after the sensor is on (§3.4).

**If it does NOT build**, the errors will almost certainly be one of these, and
all three are two-minute fixes:

| symptom | cause | fix |
|---|---|---|
| `undefined reference to vl53l1x_init` | CubeIDE never saw the new file | Refresh (F5), then Project → Clean, then Build |
| `unknown type name 'vl53l1x_stats_t'` in `app_tasks.c` | include order | the `#include "app_vl53l1x.h"` line is right after `app_drv2605l.h` — check it survived |
| `implicit declaration of HAL_RCC_GetCpuClockFreq` | it is in `stm32n6xx_hal_rcc.h`, pulled in by `stm32n6xx_hal.h`, already included in `app_tasks.c` | if it still complains, that print is inside `#ifdef DEBUG_TIMING` which is off — it should not even compile. Check you did not define `DEBUG_TIMING`. |

Anything else: write the **exact** error text into `docs/BLOCK2_LOG.md` (§7)
and keep going — the L1 probe binary is already flashed and works, so a broken
L2 build costs you nothing until Tuesday.

---

## 2. THE WORK THAT IS WORTH MORE THAN CODE

Three of these have been open since the plan was written. All three are pure
human work. **They are the reason this week can be a net gain.**

### 2.1 D1 — CONTEST LOGISTICS. Do this Monday morning, before anything else.

The risk analysis calls this *"the highest-value hour in the project right now,
and it involves no soldering."* It is still open.

- https://www.tron.org/programming_contest-2026/
- https://www.tron.org/programming_contest-2026/programming_contest_entry-2026/

Find and **write down verbatim, with the URL and the date you read it**, in
`docs/CONTEST_LOGISTICS.md`:

1. **Does entry registration close before the work deadline?** This is the
   classic way to lose a contest you had working code for. If yes, and the date
   is inside September, **stop everything else and register.**
2. The actual submission deadline. `CLAUDE.md:4` says 30 Sep;
   `docs/PROJECT_DEFENSE.md` says 25 Sep. **One of them is wrong.** Fix the
   wrong one in the same sitting.
3. What must be submitted — document format, length, language, video, source?
   This sizes Block 11 and you are currently guessing.
4. Must the hardware **arrive** by a date, or merely be **sent**? These are
   very different shipping decisions and it changes your 18 Sep target.
5. Exact shipping address and recipient name.
6. Whether µT-Kernel API breadth is judged (G-13 depends on this answer).

### 2.2 CUSTOMS — start Monday, not on 17 September.

An international electronics shipment out of India needs a **proforma invoice
with a declared value and an HS code**. It is multi-day, bureaucratic, and
entirely capable of ending this project while your firmware works perfectly.

- Get the HS code for a development board / electronic assembly. Ask a courier
  (DHL/FedEx India business desk) directly — they will tell you.
- Draft the proforma invoice with a declared value. Low declared value reduces
  duty but also reduces insurance; decide deliberately and write the reason
  down.
- Ask the courier explicitly: **does a lithium-free electronics prototype to
  Japan need anything special?** Get the answer in writing (email).
- Ask about transit time **to a residential/office address in Japan**, and what
  happens if customs holds it.

**Deliverable by Wednesday:** a draft proforma invoice and an email thread with
a courier. That is it. It is not glamorous and it is the highest-variance risk
in the project.

### 2.3 G-1 — THE ML LABEL. Decide by 9 Sep; decide it this week instead.

This is the one that could invalidate the submission, and it costs nothing but
thinking.

Right now `Hazard = distance < 80 cm AND closing velocity > 20 cm/s`, evaluated
on the **current** frame — and the feature vector contains `d(t)` and `v`. So
the network is being trained to reproduce an if-statement it is being fed the
inputs of. A judge will ask why a 15→32→16→1 network beats the two comparisons
it was trained on, and today the honest answer is "it doesn't."

The project is called **predictive**. A predictive model labels the future.

**Recommended: shift the label in time.** Label frame *t* with the rule
evaluated at *t+k*, k ≈ 5–15 frames (100–300 ms at 50 Hz). The network then does
something an if-statement provably cannot: infer from 200 ms of distance history
that a hazard is *about to* exist. It costs one column shift in the training CSV
and it is the difference between a demo and a result.

**What to actually do this week:** pick k, write one page in
`docs/ML_LABEL_DECISION.md` saying what the label is, why, and what you will say
to a judge who asks "why not an if-statement". You cannot collect data until
this is fixed, and collection is Block 6, 9 Sep. **Deciding this is on the
critical path; the code for it is not.**

### 2.4 G-4 — THE DATA COLLECTION PROTOCOL. Nothing defines one. Write it.

The VL53L1X's returned range depends on target reflectance and ambient IR.
Training on a white wall and demonstrating against a dark sleeve are different
problems. Collection is **irreversible** (Block 6).

Write `docs/DATA_COLLECTION_PROTOCOL.md` covering, concretely:

- How many samples, and how many of each class. Rough target and why.
- Approach speeds — slow / normal / fast, roughly what cm/s each means, how you
  will keep them repeatable (a metronome app, a marked distance on the desk).
- Angles: straight on, and off-axis by roughly how much.
- **Surfaces: at minimum a hand, a dark sleeve, and a light wall.** These have
  very different reflectance and the demo will use a hand.
- Lighting: the room you collect in, and the fact that the demo room in Japan
  is unknown (G-11).
- Distances: the sweep range. Short mode tops out around 1.3 m; the label
  threshold is 80 cm, so you need plenty either side of it.
- **The frame-drop policy (G-7), and it must be identical during collection and
  inference.** `v = (d_prev - d) * 5` assumes exactly one frame of separation;
  after a drop the real interval is 40 ms and the computed velocity is *half*
  the true value — in the direction that makes a hazard look safe. Pick one:
  hold-last, interpolate, or mark-invalid-and-skip. Write down which, and why.

This is a one-page document and it protects the single irreversible step in the
whole project.

---

## 3. BLOCK 2 AT THE BENCH — THE RUNBOOK

### 3.1 Before the iron is hot: DMM on the 7SEMI, board disconnected

**The 7SEMI's XSHUT pull-up has never been measured** (CLAUDE.md §2,
`[pending V-W-3]`). If the board does not fit one, XSHUT floats, the part sits
in reset, nothing ACKs at 0x29 — and it looks exactly like a bad solder joint,
which is where your next two hours would go.

With the 7SEMI **not connected to anything**, meter and write the numbers into
`docs/BLOCK2_LOG.md`:

| from | to | what it tells you |
|---|---|---|
| XSHUT | VIN | a few kΩ ⇒ pulled up to VIN |
| XSHUT | GND | should be high (no pull-down) |
| GPIO1 | VIN | tells you PD0's real input level instead of assuming |
| SDA | SCL | expect ~19.8 kΩ (confirms it is the board you measured before) |

Then **add two rows to the table in CLAUDE.md §2** and mark V-W-3 closed. That
table already has the I2C pull-ups; this completes it.

**If XSHUT has no pull-up anywhere**, do not hard-tie it to 3V3 — CLAUDE.md §2's
rule stands: most VL53L1X carriers run the die from a 2.8 V LDO and XSHUT abs
max is VDD+0.3 = 3.1 V. Use a 10 kΩ from XSHUT to VIN, or leave XSHUT
unconnected and see whether it works anyway (many carriers pull it internally
at the die). Record what you did.

### 3.2 Two things owed before the sensor joins the rail

1. **Fit the 100 µF** across 3V3/GND next to the DRV2605L, stripe (the negative
   band) to GND. T3 step 5. It played no part in the OC_DETECT fault, but it is
   owed before a second and third device join that rail.
2. **V-W-1, 60 seconds.** Black probe on a GND pin of CN8, red on 3V3.
   **The double-GND is the landmark — back one is 5V, back two is 3V3.**
   Expect **3.25–3.35 V**. Then meter across the DRV2605L breakout's own
   VCC/GND; the difference is the wiring drop and closes H-D5 for free.
   **CN8 pin 5 is +5V and physically adjacent to pin 4. Count twice.**

### 3.3 Solder and wire the 7SEMI

| 7SEMI pin | goes to | connector |
|---|---|---|
| VIN | 3V3 rail | (shared with the DRV2605L) |
| GND | GND rail | |
| SDA | PC1 | **CN12 pin 9** |
| SCL | PH9 | **CN12 pin 10** |
| GPIO1 | PD0 | **CN11 pin 3** |
| XSHUT | **nothing** | leave unconnected |

- **Label the wires.** The OC_DETECT fault came from two unlabelled bare joints.
- GPIO1 → PD0 is **not needed for Block 2** — the frame loop polls
  `CheckForDataReady`. Wire it now because solder time is now and the mechanical
  freeze is Block 5, but do not let it block you if it is awkward.
- Do not disturb the DRV2605L wiring. Block 1 is your fallback demo.

### 3.4 Flash and read the L1 probe

Flash the build from §1. Then:

```bash
timeout 60 picocom -q -b 115200 /dev/ttyACM0 | tee /tmp/l1probe.log
```

**PASS is exactly:**
```
[TOF] res=0 step=0 id=0xeacc blk=0xeacc10 ctl=0x??      (ctl must NOT be 0xea)
[I2C] ... err=0 tmo=0 recov=0     [NAK] nacks=0
```

**If it does not pass, use this tree. Do not guess.**

| what you see | what it means | what to do |
|---|---|---|
| `res=-42 step=1 id=0xa5` | **No ACK at 0x29.** The bus itself is fine — `[I2C] addr=0x5a` proves the DRV2605L still answers. | 1. Meter VIN at the breakout: 3.25–3.35 V. 2. Continuity from each breakout pad to its CN12/CN11 pin. 3. **Measure XSHUT's voltage while powered — it must be above ~2 V.** If it is low the part is held in reset (§3.1). 4. Inspect for solder bridges between adjacent pads under a phone camera zoom. |
| `res=-57` (E_IO) | The part **ACKed and then the transfer failed.** A different and more interesting problem. | Check `[I2C] tmo=` and `recov=`. A marginal joint, or bus contention. Reflow the joints. |
| `res=0`-ish but `id=0xffff` | Nothing driving the data phase. | Same as `-42`: XSHUT / reset / power. |
| `id=0xa5a5` | **The DMA never wrote.** This is RZ9's shape on a new path. | You have hit a sixth silent-failure defect. Write it down in full and stop — this is worth the $10 (§8). |
| `id=0x0000` | The part genuinely drove zeros. | Unusual. Power-cycle and retry once, then record. |
| `id=` some other value | 16-bit addressing is reaching a **different register** than you think. | Compare with `ctl=`. If they are equal, see the next row. |
| `res` non-zero **and `ctl=0xea`** | **STOP.** The negative control failed: the REG8 and REG16 branches are indistinguishable on this bus, so nothing about address width is proven and every VL53L1X read downstream is suspect. | Do not proceed to L2. Record everything and stop. This is the defect the whole probe exists to catch. |

**If the L1 probe passes, you have completed the highest-risk single step in
Block 2 without any AI help at all.** Write the exact `[TOF]` line into
`docs/BLOCK2_LOG.md` and commit it.

### 3.5 The L2 driver — one flash, then read `[RNG]`

The driver is **gated on the L1 probe**, so it only runs once §3.4 passes. Same
binary, no rebuild needed between §3.4 and here.

**PASS is:**
```
[RNG] init=0 step=99 id=0xeacc boot=1 calls=~100 xerr=0 dm=1 tb=15
[RNG] osc=<nonzero> imp=<nonzero> imprb=<same as imp> mstart=0x40
[RNG] frames=<climbing ~40-50/s> last=<mm> min=.. max=.. nrdy=.. dxfer=0 dstat=..
```

`step` names the **first gate that did not hold**:

| step | gate | most likely cause | what to try |
|---|---|---|---|
| 1 | boot | `FIRMWARE__SYSTEM_STATUS` never went non-zero in 50 × 1 ms | Power-cycle. If it persists, raise `TOF_BOOT_POLL_MAX` in `app_vl53l1x.c` to 200 and rebuild. |
| 2 | id | read 0x0000 or 0xFFFF | Same causes as an L1 failure — but note L1 passed, so this is odd. Record it. |
| 3 | SensorInit | `xerr` > 0, or `calls` < 91 | A write in the 91-write block failed — **this is F-2, the defect this gate exists for.** `calls` < 91 means the loop aborted early. Retry once; if reproducible, record `xerr` and `calls` exactly. |
| 4 | distance mode | `dm` ≠ 1 | **F-3 biting.** One of six writes failed silently. Retry; if reproducible it is a bus quality problem, not a code problem. |
| 5 | timing budget | `tb` ≠ 15 | Usually means you are not in short mode — but step 4 passed, so suspect the WrWord. 15 ms is legal **only** in short mode. |
| 6 | inter-measurement | `osc=0`, or `imprb` ≠ `imp` | `osc=0` means `RESULT__OSC_CALIBRATE_VAL` read zero — the sensor is not configured. `imprb ≠ imp` means the DWord write did not land. |
| 7 | StartRanging | `mstart` ≠ 0x40 | The mode register did not take the start bit. |

**Runtime expectations, so you do not chase a non-problem:**
- `nrdy` climbing alongside `frames` is **normal**. The sensor's
  inter-measurement period is 20 ms and `sensor_task` also runs at 20 ms, so the
  two alias and roughly half the polls find no new data. Expect `frames` around
  40–50/s and `nrdy` a similar order.
- `dstat` non-zero with nothing in front of the sensor is **normal** — that is
  `result.Status` reporting out-of-range, exactly what it is for.
- **`dxfer` non-zero is NOT normal.** That is a failed transfer, and the whole
  reason it is counted separately is that ST's `GetResult` would otherwise have
  handed you stack garbage (F-6). Any `dxfer` at all is worth recording.
- **Wave your hand at it.** `last=` should track distance in millimetres and
  `min`/`max` should spread. That is your first real sensor reading in this
  project — photograph the terminal.

### 3.6 Then: the 10-minute soak, and L5

```bash
timeout 600 picocom -q -b 115200 /dev/ttyACM0 \
  | tee docs/evidence/phase5/soak_10min_$(date +%Y%m%d)_block2.log
python3 docs/evidence/phase5/check_soak.py docs/evidence/phase5/soak_10min_*_block2.log
```
With the sensor on the bus you should now get **16/16 PASS** — `nacks=0`
expected, and the L1 probe check green.

**L5 — the logic-analyzer capture of one `WrByte`.** Workflow and probe mapping
are in the handoff §2.6: `sigrok-cli --driver fx2lafw --config samplerate=8m
--time 5s`, SCL on **D2** (silkscreened **CH3** — the clone labels CH1–CH8 while
the driver names them D0–D7), SDA on **D3**, ground to **CN8 pin 7**. Decode with
PulseView's I²C decoder.

**Wire bytes must read `0x52  idxMSB  idxLSB  data`.** That is the only hardware
proof that 16-bit addressing reaches the wire. It is now also *predicted* from
HAL source: `HAL_I2C_Mem_Read_DMA` writes `I2C_MEM_ADD_MSB` to TXDR and sends
the LSB from the interrupt. **Archive the `.sr` file — it is submission
evidence** and you cannot retake it after 18 Sep.

---

## 4. DAY BY DAY

### Sunday night (tonight) — 30 min
- [ ] `bash commit_block2.sh --go`
- [ ] CubeIDE **Refresh (F5)** → Build. Fix any compile error (§1).
- [ ] Do not flash. Go to sleep.

### Monday 31 Aug
**Morning, at a desk, no hardware:**
- [ ] §2.1 contest logistics — the two URLs. Write `docs/CONTEST_LOGISTICS.md`.
- [ ] §2.2 customs — email a courier. Get the HS code question moving.

**Afternoon/evening, at the bench:**
- [ ] §3.1 DMM on the 7SEMI. Update CLAUDE.md §2, close V-W-3.
- [ ] §3.2 fit the 100 µF, do V-W-1.
- [ ] §3.3 solder and wire the 7SEMI.
- [ ] §3.4 flash, read the L1 probe.

### Tuesday 1 Sep
- [ ] §3.5 read `[RNG]`, work the step table if needed.
- [ ] Wave a hand at it. Photograph the terminal.
- [ ] §3.6 the 10-minute soak → `check_soak.py` → expect 16/16.
- [ ] §3.6 the L5 logic-analyzer capture. Archive the `.sr`.
- [ ] Commit everything, including the log and the capture.

### Wednesday 2 Sep — the thinking day
- [ ] §2.3 the ML label. Write `docs/ML_LABEL_DECISION.md`. **Pick k.**
- [ ] §2.4 the collection protocol. Write
      `docs/DATA_COLLECTION_PROTOCOL.md`, including the frame-drop policy.
- [ ] §5 — the IMU decision. It is due 6 Sep and you now have the information.
- [ ] Chase the courier if they have not replied.

### Thursday 3 Sep, before 14:30
- [ ] Buffer. Whatever slipped.
- [ ] §7 — write the handoff so the next session is cheap.

---

## 5. THE IMU DECISION — decide it Wednesday, it is free

Schedule valve #2, due 6 Sep. You now have a **second, independent** argument
that was not available when the valve was written:

- **With the MPU6050:** both 2.2 kΩ pull-up pairs must come off the bus — the
  SmartElex jumper **and two desoldered resistors on the GY-521**, which has no
  jumper. Removing either alone leaves the MPU6050 sinking ~3.55 mA against its
  3 mA spec. That is desoldering 0402-ish parts on a board you cannot replace.
- **Without it:** *nothing needs modifying at all*, the 12-feature fallback is
  already defined in CLAUDE.md §6, and the hazard rule does not use accelerometer
  data anyway.

Dropping the IMU removes a sensor, a solder job, a driver, three features, a
pull-up modification and a whole class of mounting-orientation risk — and it is
the single largest schedule valve in the project.

**My read: drop it, and say so deliberately in the write-up.** But it is your
call, and "deliberately descoped, here is the pull-up arithmetic" reads as
competence while "ran out of time" does not. Write the decision down either way.

---

## 6. WHAT NOT TO DO THIS WEEK

- **Do not solder the GY-521** until §5 is decided and the pull-ups are off.
- **Do not enable D-cache.** Decision 2 stands: fixes written, flag off.
- **Do not lower `I2C_BUS_HZ`** and believe the bus clock changed. It does not —
  `I2C_GetTiming()` uses it only to pick a speed bucket. The delivered clock is
  `I2C_Charac[FAST].freq` in `i2c_timing.c`, currently 350000. Do not undo it.
- **Do not restore `vl53l1_platform.c`** into the build, and do not fill ST's
  template in place.
- **Do not plug anything into CN4 or CN10**, or reconnect the camera FFC to
  CN14 (its VL53L5CX also answers 0x29 — it would collide with the sensor you
  just soldered).
- **Do not flash anything you have not run `check_soak.py` against.**
- **Do not define `DEBUG_TIMING`** casually. It is off, and the `/600u` bug it
  would have armed is now fixed — but that code path has still never run since
  the fix.
- **Do not quote `analyze_scl.py`'s "SCL freq max 400.0 kHz"** anywhere. That is
  125 ns quantisation. Quote the median, 381 kHz.

---

## 7. WRITE THINGS DOWN — this is what makes Thursday cheap

Keep one file, `docs/BLOCK2_LOG.md`, and append to it as you go. Verbatim
terminal lines, not summaries. Specifically:

- The DMM numbers from §3.1, with what was connected to what.
- The V-W-1 rail voltage.
- The **exact** `[TOF]` and `[RNG]` lines at every stage — pass or fail.
- Any compile error, verbatim.
- Every decision from §2 and §5, with the reasoning, not just the conclusion.
- Photographs: the soldered 7SEMI, the terminal showing a real distance, the
  bench overall. **Block 5 photographs are submission material and you cannot
  retake them after 18 Sep** — start now while things are accessible.

Then, on Thursday morning, write `docs/HANDOFF_20260903.md` in the same shape as
the existing handoffs: state, what happened, what to do next, what not to do.
**Paste that handoff as the first message of the next session.** A session that
starts with the state in hand costs a fraction of one that has to rediscover it.

---

## 8. IS ANYTHING WORTH THE $10? — honest answer: hold it.

**Do not spend it on anything in §2, §3.1–§3.4, §4, §5 or §7.** All of that is
bench work, meter work, reading contest pages, and writing decisions down. None
of it is faster with an AI.

**Hold the $10 for exactly one contingency:** a failure whose row in the §3.4 or
§3.5 table does not fit what you are seeing, *or* the `id=0xa5a5` row — a
transfer that reports success with an untouched buffer, which would be a sixth
silent-failure defect on a path we thought was proven.

If you do spend it, spend it well:

- **One message. All context in that message.** Paste the full heartbeat block
  verbatim (all of `[I2C] [NAK] [CLK] [TOF] [RNG] [DRV] [TRG] [HLT] [HB]`), the
  DMM numbers, and what you have already tried from the table.
- **Ask one specific question.** "Step 3 fails with xerr=4 calls=91, here are
  the lines, what are the three most likely causes and the single test that
  separates them" — not "it doesn't work, help".
- **Do not ask for re-explanation** of anything already in
  `BLOCK2_PREFLIGHT_20260830.md` or this file. It is written down; re-reading
  costs you nothing and re-generating it costs you credits.
- Say up front that you are on a hard budget. That changes the answer shape.

A compile error is **not** worth $10. It is a missing semicolon or a stale
CubeIDE index, and you can fix it.

---

## 9. IF EVERYTHING GOES WELL

By Thursday 14:30 you could plausibly have: the VL53L1X soldered and ranging at
50 Hz with every ULD silent-failure gated, the L5 capture archived, a 16/16
soak, the contest logistics resolved, customs in motion, the ML label decided,
the collection protocol written, and the IMU question closed.

That is Block 2 complete, Block 3 decided, and the four Tier-1 risks from the
risk analysis closed — **ahead of plan v2, not behind it.**

The µT-Kernel result that this contest actually judges is already finished and
hardware-measured. Everything this week is turning it into a product. Protect
the fallback demo, protect the evidence archive, and do not let a courier form
be the thing that ends it.
