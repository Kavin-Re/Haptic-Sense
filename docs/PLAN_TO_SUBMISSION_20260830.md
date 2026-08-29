# HAPTIC-SENSE — PLAN TO SUBMISSION
**Written 2026-08-30, early hours. Supersedes the forward-looking sections of every prior handoff.**
Status source: `docs/evidence/phase5/PHASE5_I2C_FIRST_LIGHT_20260829.md`, `docs/PROJECT_DEFENSE.md`,
`docs/audits/*`, `docs/design/*`, `CLAUDE.md` (all read 2026-08-30).

---

## 0. THE CLOCK

| | Date | Days from today |
|---|---|---|
| Today | Sun 30 Aug 2026 | — |
| Internal hardware freeze | Fri 18 Sep 2026 | **19** |
| Contest submission | Wed 30 Sep 2026 | **31** |

**Both dates are disputed inside this repo.** `CLAUDE.md:4` says 30 Sep with an 18 Sep freeze;
`docs/PROJECT_DEFENSE.md` header says **25 Sep**. That is a five-day swing on a schedule with no
slack. **Resolve it first** — see Decision 1.

### The honest position

First light was **31 hours ago** and it was one device, reads only. Against that:
- **L2 and L3 do not exist.** No `vl53l1x.c`, no `mpu6050.c`, no real `sensor_task` body. The
  frame pipeline in `app_tasks.c` is still synthetic.
- **The I2C write direction has never moved a byte** (`hdma_i2c1_tx`, H-D9). Every driver init in
  this project is writes.
- **`I2C_REG16` has never run on hardware.** The VL53L1X is 16-bit-addressed and its failure mode
  is silent wrong data — the same class as the GPDMA defect, which cost a full bench session.
- **Phase 6 has not started and has no design document.** PH6-1 (the P3→P2 handoff spec) is a
  hard prerequisite and is unwritten.
- **The NPU has never been touched on this project.** Not once. It is the single largest unknown.
- Two of three sensors are **not soldered**.

~35 items are open across the ledgers, ~20 of them needing bench time. This plan exists to spend
the 31 days on the ones that are actually on the critical path.

---

## 1. FOUR DECISIONS — make these before writing any more code

### Decision 1 — What is the real deadline?
Two dates in your own repo. Check the contest pages and fix whichever document is wrong:
- https://www.tron.org/programming_contest-2026/
- https://www.tron.org/programming_contest-2026/programming_contest_entry-2026/

**Also read what must actually be submitted** — source, report, video, entry registration, and
whether entry registration closes earlier than the work itself. Block 8 below is sized blind
because this is unknown, and an early entry-registration deadline is the classic way to lose a
contest you had working code for.

### Decision 2 — D-cache: ON or OFF for the contest?
**Recommendation: OFF. Do not flip `USE_DCACHE` before submission.**

`docs/PROJECT_DEFENSE.md` §2.2 sequences D-cache enablement into single-sensor bring-up. That
inserts three code fixes (F-6a write-side clean, F-6b ULD bounce buffer, F-6c DRV buffer rule)
plus a soak plus a whole coherency failure class into the critical path, to buy performance
nobody has measured a need for. The pipeline runs at 50 Hz on a 600 MHz M55.

Do this instead: **write** the three fixes (they are correct regardless and cheap), leave the flag
off, and record the decision as deliberate in the contest write-up. "D-cache disabled; the
coherency preconditions are implemented and documented but unexercised" is a defensible engineering
statement. Flipping it in week four is not.

Cost if you accept: ~3 days back, one failure class removed.

### Decision 3 — How many haptic channels?
**Recommendation: ONE.**

CLAUDE.md §2 describes three EN-arbitrated channels. But: the Adafruit board does not break out EN
so it cannot participate (max 2), **the EN-arbitration sequencing scheme is not designed in any
document** — both DRV2605L design docs are single-device at fixed 0x5A — and a single-zone ToF
gives **no direction**, so extra channels encode nothing the sensor can measure. Multi-channel is
scope with no signal behind it.

Cost if you accept: an undesigned subsystem leaves the critical path entirely.

### Decision 4 — DRV2605L kill path: R-EN-2 or R-1?
Your two design documents **directly contradict each other** and this must be settled before
`app_drv2605l.c` is written:

| | `drv2605l_port_design_v1.md` | `DRV2605L_P3_INIT_ARMING_DESIGN.md` |
|---|---|---|
| Runtime kill | **R-EN-2**: P1 drives EN low, hard GPIO kill | **R-1**: EN init-only; kill = P3 writes 0x01←0x41 (STANDBY) |
| OD_CLAMP (0x17) | 0x8B (139 → 3.001 V, computed) | 0x8C (140, "keep default") |
| Diagnostics in init | absent | MODE=6 diag pass — **spins the motor** |

**Recommendation: R-1 (EN init-only), with EN still wired to PE7 so a hard kill remains physically
available as an emergency path.** Reasoning: playback here is a one-shot ROM effect of finite
duration, not continuous drive, so "motor stuck on" is close to unreachable by construction — which
is what R-EN-2 exists to defend against. Meanwhile EN low loses nothing but *does* forbid register
access (SLOS854D §8.4.1.3), so a hard kill implies a re-init race on the way back. Keep P1
GPIO-only on TRIG, keep the EN wire as insurance you never use.

Take **OD_CLAMP = 0x8B** (computed, cites its own arithmetic) and **drop the MODE=6 diagnostics from
init** — it spins the motor at every boot for a check you can run once by hand as HAP-T12.

---

## 2. CRITICAL PATH

The long pole is **not** the firmware. It is **Edge Impulse data collection**, because it cannot
start until both sensors stream correctly at the locked settings, and because collecting against
the wrong settings silently invalidates everything downstream (DLPF_CFG=4, AFS_SEL=1 / 8192 LSB/g
are locked for exactly this reason). Everything before it is in service of hitting that gate.

```
writes proven ──▶ VL53L1X streaming ──┐
                                       ├──▶ real feature frames ──▶ COLLECT ──▶ train ──▶ CPU inf ──▶ NPU ──▶ freeze
                  MPU6050 streaming ──┘         (PH6-1 first)      (irreversible)
                                                                                    haptics ──┘
```

### Block 0 — Close the driver (today, ~2 h, no new hardware)
The board is wired and working right now. Do not disturb it until these are done.

1. **H-D9 — prove the write direction.** Write a scratch value to DRV2605L **0x02 (RTP_INPUT,
   reset 0x00)**, read it back, restore. Safe: MODE reset 0x40 has STANDBY=1, so nothing drives the
   motor, and no motor is connected. This is the first byte `hdma_i2c1_tx` will ever move. **Every
   driver in this project is blocked behind it** — 91 `WrByte` calls in `VL53L1X_SensorInit` alone.
2. **H-D10 — move EN from the 3V3 bench jumper to CN12 pin 1 (D8/PE7)** and re-read the three
   registers. Needs ~10 lines of GPIO init: PE7 push-pull, **initialised LOW**, raised only after
   the 3V3 rail is up (CLAUDE.md §2 derived rule). Pass on the rail but fail on PE7 ⇒ GPIO config,
   not the board.
3. **Meter two things while the bench is up:** CN8 pin 4 → pin 7 = 3.25–3.35 V (**V-W-1, still
   never done** — CubeProgrammer's "3.29 V" is the ST-LINK's own sense, not this), and VOL on
   SDA/SCL during traffic, target < 0.6 V (**V-W-7 baseline**, worth having before the bus gets
   crowded and the I2C-PU jumpers have to come off).

### Block 1 — VL53L1X (30 Aug – 2 Sep) ← highest risk in the whole plan
4. Solder the 7SEMI board. **XSHUT unconnected** (CLAUDE.md §2 — do not tie to 3V3). GPIO1 → PD0,
   no internal pull.
5. **Replace ST's ULD platform stub.** `Lib/STSW-IMG009/.../API/platform/vl53l1_platform.c` is the
   unfilled ST template — **all nine bodies `return 255`** — and it is already compiled into the
   build. Write a fresh Apache-2.0 file (do not fill ST's in place; license mixing). Nine
   functions, ~150–200 lines. Two lines carry almost all the risk:
   - `dev7 = dev >> 1` — the ULD passes the 8-bit 0x52; `i2c_rd`/`i2c_wr` take 7-bit and re-shift
     internally (`app_i2c.c:327,331,342`).
   - `I2C_REG16` **as the symbol, never the literal 2** — the dispatch at `app_i2c.c:316` is
     symbolic; a literal silently falls to the 8-bit branch, the sensor still ACKs, every read is
     wrong, and nothing errors.
   Also fix the build-system split first: `firmware/Lib/` and `firmware/Appli/Lib/` are two real
   duplicate trees, the build uses the former, the ULD is in the latter. Point the includes at
   `../Lib/...` and drop the absolute paths from `.cproject`.
6. **L1 — raw probe before any shim code runs:** `i2c_rd(0x29, 0x010F, I2C_REG16, buf, 1)`,
   expect `0xEA`. **This is the first 16-bit-addressed transfer in the project's history.** Do not
   skip it and do not run it through the shim — you want one variable.
7. L2 `BootState` poll → L3 `GetSensorId` (**log, don't hard-fail**: 0xEACC per UM2510 vs 0xEEAC in
   the in-repo header; only 0x0000/0xFFFF/no-ACK is a real failure) → L4 `SensorInit` →
   `SetDistanceMode(1)` **before** `SetTimingBudgetInMs(15)` (15 ms exists only in short mode) →
   `SetInterMeasurementInMs(20)` (the API does **not** check IMP ≥ budget — enforce it yourself) →
   `StartRanging`. Frame loop: `CheckForDataReady` → `GetResult` (one 17-byte `ReadMulti`) →
   `ClearInterrupt` **every frame, mandatory**. Gate `d(t)` on `result.Status`.
8. **L5 — logic-analyzer capture of one `WrByte`.** Wire bytes must read `0x52 idxMSB idxLSB data`.
   This is the only hardware proof that 16-bit addressing is actually on the wire. Closes V-5/H1
   and, on the same capture, BUS-2 (SCL ≤ 400 kHz). **Budget it. Do not skip it.**

### Block 2 — MPU6050 (2 – 4 Sep)
9. Solder GY-521. **AD0 unconnected** → 0x68 via the onboard 4.7 kΩ pull-down. INT → PE9.
10. `app_mpu6050.c/.h`. Init exactly as `mpu6050_port_design_v1.md` §3: 0x6B←0x01 → `tk_dly_tsk(50)`
    → 0x1A←4 → 0x19←19 (50 Hz) → 0x1B←0x00 → **0x1C←0x08 (±4 g, 8192 LSB/g)** → 0x37←0x30 →
    0x38←0x01 → **read back and compare**. `I2C_REG8`, 7-bit address **unshifted** (opposite
    convention to the VL53L1X shim — this is the cross-sensor contamination trap that has already
    bitten this project twice).
11. Runtime: **one 14-byte burst from 0x3B.** This is a correctness requirement, not an
    optimisation — the registers are double-banked and six single reads tear across sample instants.
12. **Use polling (Option A), not EXTI, for now.** Read PE9 at the 20 ms cadence; the latch makes
    level-polling safe and the burst read clears INT. EXTI + `tk_sig_sem` is designed and specced
    (`M3_EDIT_SPEC_MPU6050_EXTI_SIGSEM.md`) but it is an optimisation, and every hour spent on it
    before data collection is an hour stolen from the NPU.

### Block 3 — Real frames (4 – 6 Sep)
13. **Write PH6-1 first** — the P3→P2 handoff spec. Two paragraphs: buffer owner, alignment, an
    explicit "no DMA touches this buffer" statement, and **IMU staleness as a validity field inside
    the semaphore-protected buffer, never a bare flag** (audit M-4). Phase 6 is blocked on this and
    it takes twenty minutes.
14. Replace the synthetic generator in `app_tasks.c` with real frames. Feature vector is **locked at
    15** (CLAUDE.md §6): d(t)…d(t−9), v, a, ax, ay, az. `FEAT_COUNT` and the Edge Impulse spec are
    one contract.
15. Soak it. `frames==inf` lockstep, `canary_err=0`, and now also plausible distance tracking a hand.

### Block 4 — Data collection (6 – 9 Sep) ← THE IRREVERSIBLE GATE
16. **Gate check before the first sample: flat bench, Z ≈ +1000 mg ± 80 mg** (IMU-6). If the scaling
    is wrong here, every sample collected afterwards is wrong and unrecoverable.
17. Collect against the locked label: **hazard = distance < 80 cm AND closing velocity > 20 cm/s.**
18. **Do not touch DLPF_CFG or AFS_SEL after this point.** Changing either rescales the entire set.

### Block 5 — Model, CPU first (9 – 13 Sep)
19. Edge Impulse → 15→32→16→1 sigmoid, INT8. **NPU-safe ops only** — FullyConnected/ReLU/Sigmoid.
    Never LSTM/GRU/attention: unsupported ops fall back to CPU silently, 10–30× slower, no error.
20. `generate-n6-model.sh` → `network.c` + `network_data.hex`. Copy `user_neuralart.json` verbatim.
21. **CPU inference first**, validate against saved test vectors. Do not enable the NPU until the
    CPU path gives the right answers on known input.

### Block 6 — NPU (13 – 16 Sep) ← largest unknown in the project
22. Enable the NPU. **Compare NPU output against CPU output on the same vectors.** Divergence, or a
    suspiciously slow `inference_ms`, means silent CPU fallback (Red Zone #6).
23. Measure `inference_ms` via `tk_get_otm()`.

### Block 7 — Haptics end-to-end + FREEZE (16 – 18 Sep)
24. **V-W-6 before the motor touches anything:** meter the ERM coil DC resistance. Must exceed the
    8 Ω floor (SLOS854D §6.3, specified at VDD = 5.2 V, "ensured by design"). Expect 25–37.5 Ω.
25. `app_drv2605l.c` per Decision 4. Library B (0x03←0x02), effect 1 Strong Click (0x04←0x01),
    arm with 0x01←0x01, **readback-verify after every 0x01 write** (mode==0x01, lib&7==2,
    seq0&0x7F!=0). P3 re-reads 0x01 periodically as the sole authority on config validity.
26. **HAP-T9 — scope OUT+ and measure the actual effect duration.** This sets the R-3 minimum
    inter-pulse floor (duration × 1.2). Without it, a second rising edge while GO is high
    **cancels** playback, so higher urgency produces *weaker* output — a silent inversion of the
    whole product thesis.
27. P1 trigger: TRIG high, `dwt_spin_cycles(1200)` ≈ 2 µs, low. GPIO only. HAP-T13 at max cadence
    under CPU load.
28. **HARDWARE FREEZE 18 Sep.** After this, firmware fixes only.

### Block 8 — Evidence and submission (18 – 25 Sep)
29. **PH6-3 — re-run the preemption campaign under NPU load.** The 3.375 µs figure was measured
    without the NPU running. The contest latency claim is not valid until this is redone.
30. Assemble the contest package per whatever Decision 1 turns up.
31. Locked claim wording, verbatim, no improvisation: *"deterministic sub-millisecond latency
    (< 1 ms, hardware-verified), worst case 3.375 µs over 2,229 events under CPU load."* Never
    "zero-latency". Never claim directional feedback — a single-zone ToF cannot measure direction.

### Block 9 — Buffer (25 – 30 Sep)
Do not plan work here. This is where the plan absorbs the surprise you have not had yet.

---

## 3. RISK REGISTER

| Risk | Why it is real | Early-warning signal | Mitigation |
|---|---|---|---|
| **NPU deployment fails or falls back to CPU** | Never attempted once. Largest unknown in the project | `inference_ms` far above expectation; NPU/CPU output divergence | Start Block 6 **early if Block 5 finishes early**. Fallback: ship CPU-only inference and say so — a working CPU pipeline beats a broken NPU one |
| **`I2C_REG16` silently wrong** | Never run. Fails as wrong data, not as an error — same class as the GPDMA defect | Distances that ACK but are implausible | The L1 raw probe and the L5 LA capture exist precisely for this. Do not skip either |
| **Write direction broken** | `hdma_i2c1_tx` has never moved a byte | Everything: 91 writes in `SensorInit` | Block 0 item 1, today, before anything else |
| **Data collected against wrong scaling** | Unrecoverable — invalidates every downstream artefact | Flat-bench Z ≠ 1000 mg | IMU-6 gate check before sample one |
| **Entry registration closes before you look** | Unknown deadline, separate from the work deadline | — | Decision 1, today |
| **Second silent-failure surprise** | You have had one this week and it cost a full session | — | Block 9 is the only defence. Protect it: do not schedule into it |
| **Competing commitments** | VLSI workshop, IoT lab, active manuscript | Blocks slipping right by >1 day each | If two blocks slip, invoke Decisions 2 and 3 immediately rather than at the end |

---

## 4. EXPLICITLY OUT OF SCOPE

Recording these so they are not re-litigated at 1 a.m. in week four:

- **D-cache enablement** (Decision 2) — fixes written, flag stays off.
- **Haptic channels B and C** (Decision 3) — and with them the undesigned EN-arbitration scheme.
- **MB1854 camera module / VL53L5CX / ISM330DLC.** Already off the critical path; keep the FFC
  unplugged from CN14 (0x29 collision) for the whole project.
- **DRV2605L closed-loop operation.** Open-loop ERM + ROM library is the locked baseline.
- **VL53L1X calibration port** (`VL53L1X_calibration.c`) — explicitly out per the port design.
- **MPU6050 EXTI wake.** Designed, specced, not needed before submission. Polling first.
- **Wearable enclosure.** The deliverable is a benchtop prototype.

---

## 5. WHAT I DO NEXT, GIVEN A GO

In order, and each is a self-contained piece of work:
1. Write the H-D9 scratch-write test into the gate (Block 0.1) and the PE7 GPIO init (Block 0.2).
2. Write the ULD platform shim, all nine functions, plus the build-system fix (Block 1.5).
3. Write `vl53l1x.c/.h` — the L2 device layer (Block 1.7).
4. Write `app_mpu6050.c/.h` (Block 2.10).
5. Write PH6-1, then the real `sensor_task` body (Block 3).

Reply with your four decisions and I start at 1.
