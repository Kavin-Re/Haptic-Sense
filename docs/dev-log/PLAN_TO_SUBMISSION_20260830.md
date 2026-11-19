# HAPTIC-SENSE — PLAN TO SUBMISSION
**v2 · 2026-08-30 · revised after the shipping constraint was stated.**
Supersedes the forward-looking sections of every prior handoff, and v1 of this document
(committed `76f196d`, superseded within the hour — see §0.1).

Status source: `docs/evidence/phase5/PHASE5_I2C_FIRST_LIGHT_20260829.md`, `docs/PROJECT_DEFENSE.md`,
`docs/audits/*`, `docs/design/*`, `CLAUDE.md`.

---

## 0. THE CONSTRAINT THAT DRIVES EVERYTHING

The physical prototype must be **couriered to Japan**. Ship date target: **Fri 18 Sep 2026**,
leaving ~12 days of transit and customs margin before 30 Sep. Documentation and written
submission continue until **Wed 30 Sep 2026**.

| | Date | Days from today |
|---|---|---|
| Today | Sun 30 Aug 2026 | — |
| **BOX CLOSES — hardware ships** | **Fri 18 Sep 2026** | **19** |
| Documentation / submission | Wed 30 Sep 2026 | 31 |

### 0.1 What this changes

**18 September is not an internal target. It is a hard, external, irreversible deadline.**
The moment the box is sealed:

- No reflash. **The binary on the board at pack time is the binary that gets judged.**
- No bench. No logic analyzer, no meter, no serial terminal, no soak run, no photograph.
- No debugging. If a reviewer's question during the write-up needs a measurement, the answer is
  whatever you captured before 18 Sep or nothing.

Three consequences, and they reorder the plan:

1. **Every hardware-dependent evidence item moves before 18 Sep.** v1 of this plan put the PH6-3
   preemption re-run under NPU load in the 18–25 Sep window. That was wrong — it needs the board.
   Same for every logic-analyzer capture, every soak log, and any demo video.
2. **A new mechanical block exists, and it must land before data collection.** See §0.2.
3. **The 25–30 Sep buffer is now documentation buffer only.** It absorbs nothing hardware-shaped.
   The real buffer is whatever slack exists before 18 Sep, and today there is roughly none.

### 0.2 The thing that was missing entirely: the prototype has to survive a courier

Right now this is a breadboard with jumper wires. **Courier handling will pull those wires out.**
Vibration, drops, pressure from stacked parcels — a breadboard prototype does not arrive working.

This is fabrication work that appeared nowhere in v1 and it is not optional.

**And it must happen BEFORE data collection, not after.** The reason is not mechanical, it is
statistical: the MPU6050's axes and the VL53L1X's aim are physically fixed by how the sensors are
mounted. Collect training data on a breadboard, then rebuild onto protoboard with the IMU rotated,
and every accelerometer feature in the training set describes a geometry that no longer exists.
That is a silent invalidation of the same class as changing `DLPF_CFG` or `AFS_SEL` after
collection — no error, no warning, just a model that does not work.

**Mechanical freeze precedes data freeze.**

### 0.3 The judges will power it on with nothing attached

Assume the board is switched on in a room in Japan with no serial terminal, no debugger and no
instructions beyond what is in the box. The demo must be self-evident:

- Boots from flash unaided (Flash Boot, BOOT0/BOOT1 both LOW), runs the full pipeline with no host.
- Its behaviour is legible without a terminal — LD1 heartbeat plus the motor firing when a hand
  approaches is a complete, understandable demo on its own.
- **Put a printed card in the box:** what it is, how to power it (C-to-C cable — a Type-A-to-C
  cable is current-limited to ~550 mA and will not boot it, UM3300 §6.1 note 1), what to wave a
  hand at, what the vibration means, and a contact address.

**Verify before packing:** unplug everything, power-cycle from cold, and confirm the demo runs with
no host connected. Firmware that only works with picocom open is firmware that does not work.

---

## 1. FOUR DECISIONS — make these before writing more code

### Decision 1 — Deadline and shipping logistics
`CLAUDE.md:4` says 30 Sep; `docs/PROJECT_DEFENSE.md` says **25 Sep**. Fix whichever is wrong.
- https://www.tron.org/programming_contest-2026/
- https://www.tron.org/programming_contest-2026/programming_contest_entry-2026/

While there, establish: **whether entry registration closes earlier than the work itself**
(the classic way to lose a contest you had working code for); what must be submitted; whether the
hardware must *arrive* by a date or merely be *sent*; and the exact shipping address and recipient.

**Start the customs paperwork this week, not on 17 September.** An international electronics
shipment out of India needs a proforma invoice with declared value and HS code. This is a
multi-day, bureaucratic, entirely non-technical path to missing the deadline.

### Decision 2 — D-cache: ON or OFF?
**Recommendation: OFF.** `PROJECT_DEFENSE` §2.2 sequences it into bring-up, which puts three code
fixes (F-6a write-side clean, F-6b ULD bounce buffer, F-6c DRV buffer rule) plus a soak plus a
coherency failure class onto the critical path, to buy performance nothing has asked for at 50 Hz.
Write the fixes — they are correct regardless and cheap — leave the flag off, and record the
decision as deliberate. "D-cache disabled; coherency preconditions implemented and documented but
unexercised" is a defensible engineering statement in a contest write-up. Enabling it in the last
week, on hardware you can no longer debug, is not.

Buys back ~3 days and removes one failure class.

### Decision 3 — How many haptic channels?
**Recommendation: ONE.** The Adafruit board does not break out EN so it cannot participate in EN
arbitration (max 2); **the arbitration scheme is not designed in any document you own** — both
DRV2605L design docs are single-device at fixed 0x5A; and a single-zone ToF measures **no
direction**, so extra channels encode nothing the sensor can sense. One channel is also one fewer
thing to survive a courier.

### Decision 4 — DRV2605L kill path
Your two design documents contradict each other outright:

| | `drv2605l_port_design_v1.md` | `DRV2605L_P3_INIT_ARMING_DESIGN.md` |
|---|---|---|
| Runtime kill | **R-EN-2**: P1 drives EN low, hard GPIO kill | **R-1**: EN init-only; kill = P3 writes 0x01←0x41 (STANDBY) |
| OD_CLAMP (0x17) | 0x8B (139 → 3.001 V, computed) | 0x8C (140, "keep default") |
| Diagnostics in init | absent | MODE=6 diag pass — **spins the motor at every boot** |

**Recommendation: R-1, with EN still wired to PE7 as an emergency path you never exercise.**
Playback is a finite one-shot ROM effect, not continuous drive, so "motor stuck on" — the thing
R-EN-2 defends against — is close to unreachable by construction. Meanwhile EN low forbids register
access entirely (SLOS854D §8.4.1.3), so a hard kill implies a re-init race on the way back up.
Keep P1 GPIO-only on TRIG.

Take **OD_CLAMP = 0x8B** (it shows its arithmetic) and **drop the MODE=6 diagnostics from init** —
run it once by hand as HAP-T12 instead of spinning the motor on every power-up in front of a judge.

---

## 2. CRITICAL PATH (revised)

Two orderings changed from v1:

- **Haptics moved early.** The DRV2605L is the only device already soldered and proven, and the
  Phase 4 synthetic pipeline *already emits hazard events*. Building the haptic path now gives a
  complete, demonstrable hazard→buzz chain in week one, de-risks the actuator while it is alone on
  the bus, and means that if later blocks slip you still have something that works.
- **Mechanical build moved before data collection**, per §0.2.

```
writes proven ─▶ HAPTICS (synthetic front end) ─────────────┐
                                                             │
                 VL53L1X ─┐                                  │
                          ├─▶ real frames ─▶ MECHANICAL ─▶ COLLECT ─▶ train ─▶ CPU ─▶ NPU ─▶ EVIDENCE ─▶ PACK
                 MPU6050 ─┘                    FREEZE       (irreversible)                  (needs board)
```

### Block 0 — Close the driver (today, ~2 h, no new hardware)
The bench is wired and working. Do not disturb it until these are done.

1. **H-D9 — prove the write direction.** Scratch-write DRV2605L **0x02 (RTP_INPUT, reset 0x00)**,
   read back, restore. Safe: MODE reset 0x40 has STANDBY=1 and no motor is attached. First byte
   `hdma_i2c1_tx` will ever move. **Every driver is blocked behind it** — `VL53L1X_SensorInit`
   alone is 91 writes.
2. **H-D10 — move EN from the 3V3 jumper to CN12 pin 1 (D8/PE7).** ~10 lines: push-pull,
   initialised **LOW**, raised only after the rail is up (CLAUDE.md §2 derived rule).
3. **Meter while the bench is up:** CN8 pin 4 → pin 7 = 3.25–3.35 V (**V-W-1, still never done** —
   CubeProgrammer's 3.29 V is the ST-LINK's own sense, not your rail), and VOL on SDA/SCL under
   traffic (**V-W-7 baseline**, before the bus gets crowded).

### Block 1 — Haptics end-to-end (31 Aug – 1 Sep)
4. **V-W-6 before the motor touches anything:** meter ERM coil DC resistance. Must clear the 8 Ω
   floor (SLOS854D §6.3, specified at VDD = 5.2 V, "ensured by design"). Expect 25–37.5 Ω.
5. `app_drv2605l.c/.h` per Decision 4. Library B (0x03←0x02), effect 1 Strong Click (0x04←0x01),
   arm 0x01←0x01, **readback-verify after every 0x01 write** (mode==0x01, lib&7==2, seq0&0x7F!=0).
   P3 re-reads 0x01 periodically as the sole authority on config validity.
6. Wire P1's `hazard_fire_haptic()` to the **existing synthetic hazard events**: TRIG high,
   `dwt_spin_cycles(1200)` ≈ 2 µs, low. GPIO only, no I2C, no printf.
7. **HAP-T9 — scope OUT+ and measure the real effect duration.** This sets the R-3 minimum
   inter-pulse floor (duration × 1.2). Without it a second rising edge while GO is high **cancels**
   playback, so higher urgency yields *weaker* output — a silent inversion of the entire product
   thesis. Capture it now; you cannot capture it in October.
8. End of this block you have a board that buzzes on hazard. Keep that capability working from here
   on — it is your fallback demo.

### Block 2 — VL53L1X (1 – 4 Sep) ← highest technical risk in the plan
9. Solder the 7SEMI board. **XSHUT unconnected.** GPIO1 → PD0, no internal pull.
10. **Replace ST's ULD platform stub.** `Lib/STSW-IMG009/.../API/platform/vl53l1_platform.c` is the
    unfilled ST template — **all nine bodies `return 255`** — and is already compiled into the
    build. Write a fresh Apache-2.0 file (do not fill ST's in place; license mixing). ~150–200
    lines. Two lines carry nearly all the risk:
    - `dev7 = dev >> 1` — the ULD passes 8-bit 0x52; the primitive takes 7-bit and re-shifts
      internally (`app_i2c.c:327,331,342`).
    - `I2C_REG16` **as the symbol, never the literal 2** — dispatch at `app_i2c.c:316` is symbolic;
      a literal falls silently to the 8-bit branch, the sensor still ACKs, every read is wrong, and
      nothing errors.
    ~~Fix the build split first: `firmware/Lib/` and `firmware/Appli/Lib/` are two real duplicate
    trees, the build uses the former, the ULD lives in the latter.~~ **WRONG — struck 2026-08-30.**
    The build uses both correctly: `Appli/Lib/` for the ULD (which is the only real thing in it;
    the rest are empty shells) and `firmware/Lib/` for the reference libraries. `vl53l1_platform.o`
    is already in `Appli/Debug/`. **There is nothing to fix — skip this step.**
11. **L1 raw probe, before any shim code runs:** `i2c_rd(0x29, 0x010F, I2C_REG16, buf, 1)`, expect
    `0xEA`. **First 16-bit-addressed transfer in the project's history.** One variable at a time.
12. L2 `BootState` → L3 `GetSensorId` (**log, don't hard-fail**: 0xEACC per UM2510 vs 0xEEAC in the
    in-repo header; only 0x0000/0xFFFF/no-ACK is real failure) → L4 `SensorInit` →
    `SetDistanceMode(1)` **before** `SetTimingBudgetInMs(15)` (15 ms exists only in short mode) →
    `SetInterMeasurementInMs(20)` (**the API does not check IMP ≥ budget — enforce it**) →
    `StartRanging`. Frame loop: `CheckForDataReady` → `GetResult` (one 17-byte `ReadMulti`) →
    `ClearInterrupt` **every frame, mandatory**. Gate `d(t)` on `result.Status`.
13. **L5 — LA capture of one `WrByte`.** Wire bytes must read `0x52 idxMSB idxLSB data`. Only
    hardware proof that 16-bit addressing reaches the wire. Closes V-5/H1 and, same capture,
    BUS-2 (SCL ≤ 400 kHz). **Archive the `.sr` file — it is submission evidence.**

### Block 3 — MPU6050 (4 – 6 Sep)
14. Solder GY-521. **AD0 unconnected** → 0x68. INT → PE9. **Open the I2C-PU jumper on the SmartElex
    now** — three sets of pull-ups on the bus is 5.2 mA and out of budget (CLAUDE.md §2).
15. `app_mpu6050.c/.h`, init per `mpu6050_port_design_v1.md` §3: 0x6B←0x01 → `tk_dly_tsk(50)` →
    0x1A←4 → 0x19←19 (50 Hz) → 0x1B←0x00 → **0x1C←0x08 (±4 g, 8192 LSB/g)** → 0x37←0x30 →
    0x38←0x01 → **read back and compare**. `I2C_REG8`, 7-bit address **unshifted** — the opposite
    convention to the VL53L1X shim, and exactly the cross-sensor trap that has bitten this project
    twice already.
16. Runtime: **one 14-byte burst from 0x3B.** Correctness requirement, not an optimisation — the
    registers are double-banked and six single reads tear across sample instants.
17. **Polling, not EXTI.** Read PE9 at the 20 ms cadence; the latch makes level-polling safe and the
    burst read clears INT. EXTI + `tk_sig_sem` is fully specced but it is an optimisation, and every
    hour spent on it before the box ships is stolen from the NPU.

### Block 4 — Real frames (6 – 7 Sep)
18. **Write PH6-1 first.** Buffer owner, alignment, an explicit "no DMA touches this buffer", and
    **IMU staleness as a validity field inside the semaphore-protected buffer, never a bare flag**
    (audit M-4). Twenty minutes, and Phase 6 is blocked on it.
19. Replace the synthetic generator. Feature vector **locked at 15** — `FEAT_COUNT` and the Edge
    Impulse spec are one contract.
20. Soak: `frames==inf` lockstep, `canary_err=0`, plus plausible distance tracking a hand.

### Block 5 — MECHANICAL FREEZE (7 – 9 Sep) ← new, and it gates everything downstream
21. Move off the breadboard onto a **soldered, courier-survivable build**. Protoboard/perfboard
    seated on the Arduino headers is the robust option; if you keep the breadboard, every jumper
    gets strain-relieved and the whole assembly bonded to a rigid base. Either way: nothing that a
    drop or a vibration can unseat.
22. **Fix the sensor geometry permanently now.** ToF aim and IMU axis orientation must be exactly
    what the shipped unit has, because the training data you are about to collect encodes them.
23. Re-run the full validation on the rebuilt hardware: bus scan, all three register readbacks,
    50 Hz frame soak, hazard→buzz. **Every solder joint is a new failure mode** — treat the rebuild
    as a new board, not a move.
24. **Photograph the build thoroughly** while it is open and accessible. These photos are
    submission material and you cannot retake them after 18 Sep.

### Block 6 — Data collection (9 – 11 Sep) ← IRREVERSIBLE
25. **Gate before sample one: flat bench, Z ≈ +1000 mg ± 80 mg** (IMU-6). Wrong scaling here makes
    every subsequent sample unrecoverable.
26. Collect against the locked label: **hazard = distance < 80 cm AND closing velocity > 20 cm/s.**
27. **Do not touch DLPF_CFG, AFS_SEL, or the physical mounting after this point.**

### Block 7 — Model, CPU first (11 – 14 Sep)
28. Edge Impulse → 15→32→16→1 sigmoid, INT8. **NPU-safe ops only** — FullyConnected/ReLU/Sigmoid.
    Never LSTM/GRU/attention: unsupported ops fall back to CPU silently, 10–30× slower, no error.
29. `generate-n6-model.sh` → `network.c` + `network_data.hex`. `user_neuralart.json` verbatim.
30. **CPU inference first**, validated against saved test vectors, before the NPU is touched.

### Block 8 — NPU (14 – 16 Sep) ← largest unknown in the project
31. Enable the NPU. **Compare NPU output to CPU output on identical vectors.** Divergence, or an
    `inference_ms` that looks too slow, means silent CPU fallback (Red Zone #6).
32. Measure `inference_ms` via `tk_get_otm()`.
33. **Hard call point, 16 Sep:** if the NPU is not working, ship the CPU-only pipeline and describe
    it accurately. A working CPU inference path beats a broken NPU one, and there is no version of
    this where you debug the NPU after the box ships.

### Block 9 — EVIDENCE CAPTURE (16 – 17 Sep) ← everything that needs the board, ever
This block exists because after it there is no hardware. Nothing here can be deferred.

34. **PH6-3 — re-run the preemption campaign under NPU load.** The 3.375 µs figure was measured
    with the NPU idle. The contest latency claim is not valid until this is redone. *(v1 of this
    plan scheduled this after the ship date. That was a mistake.)*
35. Long soak with the full real pipeline. Save the log.
36. **Demo video.** Hand approaching, motor firing, urgency rising with closing speed. Film it
    properly, multiple takes, good light. You cannot reshoot this.
37. Archive every artefact: `.sr` captures, soak logs, serial transcripts, build photos, the exact
    `-Trusted.bin` that is on the board, and the flash procedure that produced it.
38. **Cold-boot test, host disconnected** (§0.3). Then leave that binary on the board.

### Block 10 — PACK AND SHIP (17 – 18 Sep)
39. Printed card in the box (§0.3). C-to-C cable in the box.
40. Anti-static bag, foam, rigid outer carton. Assume it will be dropped.
41. Customs paperwork completed days earlier per Decision 1. Book the courier. **Keep the tracking
    number and a photo of the sealed package.**
42. **BOX CLOSES 18 SEP.**

### Block 11 — Documentation (18 – 30 Sep)
43. Written submission, assembled entirely from the archive captured in Block 9.
44. Locked claim wording, verbatim: *"deterministic sub-millisecond latency (< 1 ms,
    hardware-verified), worst case 3.375 µs over 2,229 events under CPU load"* — updated with the
    Block 9 NPU-load figures. Never "zero-latency". **Never claim directional feedback** — a
    single-zone ToF cannot measure direction.
45. This window is documentation buffer only. It absorbs nothing hardware-shaped.

---

## 3. RISK REGISTER

| Risk | Why it is real | Early-warning signal | Mitigation |
|---|---|---|---|
| **Prototype does not survive the courier** | It is currently a breadboard with jumpers | — | Block 5. Rigid build, strain relief, foam. Assume a drop |
| **Evidence not captured before the box closes** | After 18 Sep there is no board, forever | Writing a doc section and reaching for a number you do not have | Block 9 is mandatory and non-deferrable. Archive everything |
| **Mechanical rebuild after data collection** | Silently invalidates every accelerometer feature | — | Mechanical freeze precedes data freeze. Non-negotiable |
| **NPU never works** | Never attempted once. Largest unknown | `inference_ms` far off; NPU/CPU divergence | Hard call point 16 Sep (Block 8.33). Ship CPU-only and say so |
| **`I2C_REG16` silently wrong** | Never run. Fails as wrong data, not as an error — same class as the GPDMA defect | Distances that ACK but are implausible | L1 raw probe and L5 LA capture. Skip neither |
| **Write direction broken** | `hdma_i2c1_tx` has never moved a byte | Everything downstream | Block 0.1, today |
| **Data collected against wrong scaling** | Unrecoverable | Flat-bench Z ≠ 1000 mg | IMU-6 gate before sample one |
| **Customs / courier delay** | Non-technical, multi-day, entirely capable of ending this | — | Paperwork this week. 12 days of transit margin is the plan, not the fallback |
| **Entry registration closes early** | Unknown, and separate from the work deadline | — | Decision 1, today |
| **Second silent-failure surprise** | You have had one this week; it cost a full session | — | Slack before 18 Sep is the only defence, and there is almost none. This is what Decisions 2 and 3 buy |
| **Competing commitments** | VLSI workshop, IoT lab, active manuscript | Any block slipping >1 day | Two slips ⇒ invoke Decisions 2 and 3 immediately, not in week four |

---

## 4. EXPLICITLY OUT OF SCOPE

Recorded so none of it gets re-litigated at 1 a.m. in week three:

- **D-cache enablement** (Decision 2) — fixes written, flag stays off.
- **Haptic channels B and C** (Decision 3), and the undesigned EN-arbitration scheme with them.
- **MB1854 camera module / VL53L5CX / ISM330DLC.** FFC stays unplugged from CN14 for the whole
  project (0x29 collision).
- **DRV2605L closed-loop operation.** Open-loop ERM + ROM library is the locked baseline.
- **VL53L1X calibration port** (`VL53L1X_calibration.c`).
- **MPU6050 EXTI wake.** Designed and specced; polling ships.
- **Wearable enclosure.** The deliverable is a benchtop prototype — but see Block 5: benchtop still
  has to survive a courier.

---

## 5. WHAT I DO NEXT, GIVEN A GO

1. H-D9 scratch-write test + PE7 GPIO init (Block 0.1, 0.2).
2. `app_drv2605l.c/.h` and the P1 trigger path (Block 1).
3. The ULD platform shim, nine functions, plus the build-system fix (Block 2.10).
4. `vl53l1x.c/.h` (Block 2.12).
5. `app_mpu6050.c/.h` (Block 3.15).
6. PH6-1, then the real `sensor_task` body (Block 4).

Reply with the four decisions and I start at 1.
