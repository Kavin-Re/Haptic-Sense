# WHILE YOU WAIT — 1 SEP → 3 SEP 14:30

Ordered by *what happens if you don't do it*, not by what is interesting.
Nothing in §1–§4 needs an AI. §1 needs no rebuild either.

---

## 1. TONIGHT, NO REBUILD, NO SOLDERING — the free discriminator

**The binary already on your board can tell you which of the two main causes it
is.** I gave you `[PRT]` and the write-gap switch last time and missed this.

When I made a NACK return `E_NOEXS`, I also made it skip **both** the bus
recovery **and** the retry. `E_IO` still takes both. So the two candidate causes
have wildly different *durations*, and duration is free to observe:

| cause | per failure | 55 failures cost | `vl53l1x_init()` total |
|---|---|---|---|
| **NACK** (`E_NOEXS`) — no recovery, no retry | ~0.1 ms | ~6 ms | **~2 s** |
| **`E_IO`** — 45 ms recovery + 50 ms retry | ~145 ms | **~8.0 s** | **~10 s** |

(The ~2 s floor is the poll loop itself: 999 iterations x `tk_dly_tsk(1)`,
which rounds up to 1–2 ms per tick.)

`tstats.step` is written **before** each stage runs, and `heartbeat_task`
(TK_PRI 10) reads it live while `sensor_task` is still inside init. So:

> **Boot the board with picocom already open and count how many heartbeats
> print `[RNG] ... step=3` before it settles.**
>
> - **~2 heartbeats at step=3** → the failures are NACKs. The sensor is
>   refusing writes. Cause 2.
> - **~10 heartbeats at step=3** → the failures are `E_IO`, each dragging a
>   full DeInit / nine-pulse / re-init / retry through the middle of your
>   configuration sequence. Cause 1, and the write-gap experiment is the fix.

**And read `[I2C] ... err= tmo= recov=` and `[NAK] nacks=` on the same
heartbeat.** They answer it directly:
- `recov` jumped by ~55 → `E_IO`. Cause 1.
- `nacks` jumped by ~55, `recov` still 0 → genuine NACKs. Cause 2.
- `tmo` jumped → transfers armed and never completed. Cause 3, different fault.

**Also quote `[DRV]`.** If `init=0 ... armed=1` still holds with the 7SEMI on
the bus, the problem is specific to 0x29. If `[DRV]` has also degraded, adding
the second device hurt the bus for everything and it is a wiring problem, not a
driver problem. That one line halves the search space.

Write all six lines into `BLOCK2_LOG.md` verbatim. **Ten minutes, no tools.**

---

## 2. THE TWO THINGS THAT CAN ACTUALLY END THIS PROJECT

Both have been open since the plan was written on 30 Aug. Neither involves the
sensor. Both are more urgent than the ToF.

### 2.1 Contest logistics — one hour, Tuesday morning
- https://www.tron.org/programming_contest-2026/
- https://www.tron.org/programming_contest-2026/programming_contest_entry-2026/

**The one question that matters most: does entry registration close before the
work deadline?** That is the classic way to lose a contest you had working code
for, and you currently do not know the answer. If it is in early September you
need to know today, not Thursday.

Also settle: the real deadline (`CLAUDE.md:4` says 30 Sep, `PROJECT_DEFENSE.md`
says 25 Sep — one is wrong); what must be submitted; whether the hardware must
**arrive** by a date or merely be **sent**; the shipping address and recipient.

Write `docs/CONTEST_LOGISTICS.md` with the URL and the date you read each fact.

### 2.2 Customs — start the clock, do not finish it
An international electronics shipment out of India needs a proforma invoice with
a declared value and an HS code. **The lead time is other people's response
times, and those do not compress.** Email a courier business desk (DHL/FedEx
India) today with three questions: HS code for a development-board assembly,
transit time to Japan, and whether a lithium-free prototype needs anything
special. Get the answer in writing.

**Deliverable by Thursday: an email thread exists.** That is all. It is the
highest-variance risk in the project and it is entirely non-technical.

### 2.3 Order the 100 µF today — it is now a shipping-unit part, not a bench part
You logged it as unavailable and moved on, which was right for the bench. But
the shipped unit runs the motor, two sensors **and the NPU** on that rail, and
after 18 Sep you cannot add it. **Indian delivery is 1–3 days; ordering on
17 Sep is ordering too late.**

Any 47–470 µF electrolytic works. They are also in every dead phone charger, PC
power supply, router and motherboard — desolder one tonight and you are done.
**Stripe (negative band) to GND.**

While ordering, consider cheap spares of the things Block 5 soldering could
kill: a second 7SEMI VL53L1X and a second ERM motor. You cannot spare the DK,
but you can spare those, and G-6 says you get exactly one attempt at the
mechanical build.

---

## 3. THE DECISIONS — paper only, and they gate irreversible work

These do not need hardware, an AI, or a working sensor. They need an hour and a
text editor. **Answer the questions; do not wait for someone to draft prose.**

### 3.1 IMU — due 6 Sep. Decide it this week.
This is your largest schedule valve and it costs almost nothing in capability.
- Keeping it means desoldering two 0402 pull-ups on a GY-521 you cannot replace,
  or the MPU6050 sinks ~3.55 mA against a 3 mA spec.
- Dropping it means **nothing on the bus needs modifying at all**, and the
  12-feature fallback is already defined in CLAUDE.md §6.
- The hazard rule does not use accelerometer data.

Questions to answer in writing: does any feature you actually need come from the
IMU? What does the write-up say about the descope? If Block 2 is not ranging by
6 Sep, is this decision automatic?

### 3.2 The ML label (G-1) — must be fixed before collection starts, 9 Sep
Today the label is a deterministic function of two of its own input features, so
the network reproduces an if-statement it is fed the inputs of. The project is
called *predictive*; a predictive model labels the future.

Questions: what is k, in frames, for labelling frame *t* with the rule evaluated
at *t+k*? (5–15 frames = 100–300 ms at 50 Hz.) Why that k and not double it?
What is your one-sentence answer to a judge who asks "why not an if-statement"?
What does the training CSV column layout look like after the shift?

### 3.3 Collection protocol (G-4) — nothing defines one, and collection is irreversible
Questions: how many samples per class? What approach speeds, and how do you make
them repeatable (a metronome, marks on the desk)? What angles? **Which surfaces
— hand, dark sleeve, light wall at minimum**, because reflectance changes the
returned range and the demo uses a hand. What lighting? What distance sweep,
given the 80 cm threshold and short mode's ~1.3 m ceiling?

### 3.4 Frame-drop policy (G-7) — part of the same document
`v = (d_prev - d) * 5` assumes exactly one frame of separation. After a drop the
real interval is 40 ms and the computed velocity is **half** the true value — in
the direction that makes a hazard look safe. Pick one of hold-last, interpolate,
or mark-invalid-and-skip, write down why, and note that **the identical policy
must be active during collection and during inference.**

### 3.5 Feature parity and INT8 scaling (G-2, G-3) — these change what Block 4 builds
G-2: features computed on-device and logged over UART as raw rows, uploaded to
Edge Impulse with **no DSP block**, so the firmware is the single definition of
the feature space. That needs a CSV-shaped UART logging mode which no design
document currently mentions.

G-3: distance 0–4000 mm, velocity ~±100 cm/s, acceleration ±thousands, accel
±4000 mg — one per-tensor INT8 scale across that range crushes velocity, which
is the discriminative feature. Normalisation constants belong in the locked
contract alongside `FEAT_COUNT`.

Questions: what are the normalisation constants? Where do they live in the code?
What does the UART CSV line look like, exactly?

---

## 4. BENCH WORK THAT DOES NOT NEED ME

### 4.1 Put the logic analyzer on the failing writes
You own the instrument, the failure is on the wire, and this answers
NACK-vs-`HAL_BUSY` **definitively** — no inference required.

```
sigrok-cli --driver fx2lafw --config samplerate=4m --time 8s \
           --channels D2,D3 -o docs/evidence/phase5/sensorinit_fail_$(date +%Y%m%d).sr
```
SCL on **D2** (silkscreened **CH3** — the clone labels CH1–CH8, the driver names
them D0–D7), SDA on **D3**, ground to **CN8 pin 7**. 4 MHz is plenty for
*decoding* (you are not measuring periods this time) and halves the data.

Start the capture, then reset the board so `SensorInit` lands inside the window.
Open it in PulseView, add the I²C decoder, and look for:

- **Writes reaching the wire at all**, as `0x52 idxMSB idxLSB data`.
- **NACK bits.** A NACK after the address byte is a different fault from a NACK
  after a data byte, and both are different from no transaction at all.
- **The gap between one STOP and the next START.** If it is single-digit
  microseconds, that is the `HAL_BUSY` hypothesis made visible.
- **A ~45 ms hole with nine clock pulses in it** = a bus recovery, which
  confirms `E_IO` on its own.

This capture is worth taking **whether or not it succeeds**, because it is also
most of L5 (the 16-bit-addressing wire proof) and **you cannot retake it after
18 Sep**. Archive the `.sr`.

### 4.2 The rebuild-and-capture cycle from my last message
`[PRT]` and `VL53L1_PORT_WRITE_GAP_MS` are already in your tree. Refresh (F5),
build, flash, read `ler` and `lidx`. If `lidx` is in `0x002D`–`0x0087`, set the
gap to 1, rebuild, and compare `xerr`. **There is a real chance this simply
fixes it and you spend Thursday on Block 3 instead of Block 2.**

### 4.3 Photographs
Block 5 photos are submission material and cannot be retaken after 18 Sep. The
bench is open and accessible right now. Take them badly-lit and redundant rather
than not at all.

---

## 5. CAN YOU AFFORD THE PAUSE? — honest arithmetic

From 1 Sep to the box closing on 18 Sep is **17 days**. Plan v2 said, on 30 Aug,
that the real buffer before 18 Sep was "roughly none". That has not improved.

**A 1.5-day pause on the ToF debug is affordable — but only on these terms:**

1. **You do not also pause §2.** If Thursday arrives with the ToF unresolved
   *and* no contest logistics *and* no customs thread, you are 15 days out on
   three fronts and that is the bad scenario.
2. **You invoke the IMU valve if Block 2 is not ranging by 6 Sep.** Dropping the
   IMU deletes Block 3 (4–6 Sep) outright — two days back, for a sensor the
   hazard rule does not use. That valve is what makes this pause safe, and it is
   free to decide on paper.
3. **You keep Block 1 working.** Hazard → buzz is your fallback demo. If
   everything else slid, a µT-Kernel submission with the Phase 4 timing evidence
   and the Block 9 archive is still defensible and on-topic. **Protect the
   archive above the features.**

What you must *not* do is spend Tuesday and Wednesday re-reading the same six
log lines waiting for Thursday. The bench work in §4 has a real chance of
resolving Block 2 without me, and §2 and §3 are worth more than the ToF either
way.

---

## 6. IF YOU GET ONE HOUR AND ONE HOUR ONLY

Do §2.1. Not the sensor.

Working firmware with a missed registration deadline is the worst possible
outcome here, and it is the only failure mode on this list that no amount of
later effort can undo.
