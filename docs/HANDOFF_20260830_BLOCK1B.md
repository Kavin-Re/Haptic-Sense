# HAPTIC-SENSE — HANDOFF FOR 30 AUGUST
**Written 2026-08-30 ~02:00 IST at the end of the Block 0 / Block 1a session.**
Read `CLAUDE.md` first. Plan: `docs/PLAN_TO_SUBMISSION_20260830.md` (v2).
Risks: `docs/RISK_ANALYSIS_20260830.md`. Last night: `docs/evidence/phase5/PHASE5_I2C_FIRST_LIGHT_20260829.md`.

---

## 1. STATE

**Hardware on the bench, working and untouched:**
one SmartElex DRV2605L on I2C1. VCC/GND to CN8 pins 4/7, SDA/SCL to CN12 pins 9/10,
**EN on CN12 pin 1 (D8/PE7)** — the 3V3 bench jumper is gone. IN still jumpered to GND.
No motor. VL53L1X and MPU6050 not soldered. Camera FFC unplugged from CN14. CN4 empty.

**Last serial line (flashed binary, verified):**
```
[I2C] init=0 gate=0 whoami=0x140e0 wr=0 wrseen=0x27 addr=0x5a ok=9 err=0 tmo=0 recov=0
```

**Closed:** RZ9 (GPDMA channel security attributes), H-D8 (400 kHz legal), H-D9 (write
direction), H-D10 (EN on PE7). **Block 0 complete.**

**Written but NOT YET BUILT OR FLASHED:** Block 1a — `app_drv2605l.c/.h` register init and
arming (commit `d9e4a8e`). Compiles clean under `-Wall -Wextra` against stub types; never
seen a compiler with real headers, never run. **First job tomorrow is to build and flash it.**

**Branch:** `phase5/i2c-first-light`, 7 commits ahead of `master`. Merge is a fast-forward:
`git checkout master && git merge phase5/i2c-first-light`.
Housekeeping: stray git lock files are parked in `.git/_stale_locks/` — safe to `rm -rf`.

---

## 2. DECISIONS TAKEN (silence = accepted, per plan v2 §1)

- **D2 — D-cache stays OFF** for the contest. Write F-6a/b/c, do not flip the flag.
- **D3 — ONE haptic channel.** EN-arbitration for channels B/C is out of scope.
- **D4 — R-1:** EN is init-only, never toggled at runtime. Kill path is a STANDBY write from
  TK_PRI 3. EN remains wired to PE7 as an emergency path that is never exercised.
  OD_CLAMP = 0x8B. MODE=6 diagnostics dropped from init.
- **D1 — STILL OPEN.** Contest deadline, submission format, entry-registration date. See §4.

---

## 3. TOMORROW, IN ORDER

### T0 — Build and flash Block 1a (first, before anything else)
CubeIDE: select project, **F5** (a header changed), Ctrl+B, confirm `-Trusted.bin` is fresh.
Flash: Dev Boot → external loader `MX66UW1G45G_STM32N6570-DK` → `0x70100000` → Flash Boot →
picocom. Expect:
```
[DRV] init=0 id=7 mode=0x1 lib=0x2 seq=0x1 armed=1
```
`ok=` on the `[I2C]` line should reach **21** (9 + 1 ID read + 7 config writes + 1 arm + 3 readbacks).

Failure map: `init=-42` device id not 7 · `init=-41` a readback disagreed, and which of
mode/lib/seq is wrong names the register the device ignored · `init=-57` a transfer failed.

### T1 — Enable DWT unconditionally, and measure CYCCNT's real clock
**Blocks T2, and closes G-8 with hardware evidence rather than a datasheet reading.**

`app_tasks.c:472-479` enables `DEMCR.TRCENA` + `DWT->CTRL.CYCCNTENA` **inside
`#ifdef DEBUG_TIMING`, which is not defined in this build.** So `DWT->CYCCNT` is not running,
and `dwt_spin_cycles()` — which T2 needs for the trigger pulse — cannot work.

1. Move the three DWT enable lines outside the `#ifdef`. Costs nothing to leave the counter on.
2. Add `cyc=%u` to the heartbeat, printing `DWT->CYCCNT`.
3. **Confirm it advances at all** — the in-code comment flags this sequence as UNVERIFIED on
   this TrustZone/FSBL configuration. A frozen counter means the enable sequence is wrong here.
4. Take two heartbeat lines and compute `(cyc2 - cyc1) / (up_ms2 - up_ms1)` = cycles per ms.
   **600000 ⇒ CYCCNT runs at 600 MHz** and CLAUDE.md §3's `/600000` is right.
   **400000 ⇒ it runs off the 400 MHz bus clock** and every DWT-derived figure in the project,
   including the Phase 4 corroboration, is out by 1.5×. Record the answer in CLAUDE.md §3.

### T2 — Trigger path: replace the level with an edge pulse
Currently `drv2605l_trig_set()` drives PE13 as a hazard *level*. In edge mode a second rising
edge before GO clears **cancels** the waveform (SLOS854D §8.6.2 Table 5, verbatim), so a level
is wrong and a too-fast repeat rate makes the buzz *weaker*, not stronger.

- Add `drv2605l_trig_pulse(void)`: TRIG high → `dwt_spin_cycles(~2 us worth)` → TRIG low.
  GPIO only, TK_PRI 1, no I2C, no printf. Gate it on `drv_armed` as `trig_set` already is.
- Enforce the **R-3 minimum inter-pulse interval** in the hazard task: tick-compare against
  `tk_get_otm()`, never blocking. **Interim floor 125 ms** until T4 measures the real duration.
- Urgency is encoded as pulse *rate*, clamped to `[floor, 1000 ms]`. The effect content never
  changes at runtime.

### T3 — Bench: motor, in this order
1. **V-W-6 FIRST, motor disconnected from everything.** Meter ERM coil DC resistance. Must
   clear the 8 Ω floor (SLOS854D §6.3, specified at VDD = 5.2 V, "ensured by design, not
   production tested"). Expect 25–37.5 Ω. **Below 8 Ω the motor does not go on this driver.**
2. Power off. **Remove the IN→GND jumper** — PE13 is a driven push-pull output and leaving that
   jumper in place is the same fault as the old EN jumper.
3. Wire **CN11 pin 7 (D6/PE13) → IN** on the SmartElex.
4. Motor to the **O− / O+** pads.
5. **Add a 100 µF electrolytic across the 3V3/GND rails next to the DRV2605L.** The ERM start
   transient will otherwise dip the shared rail that the sensors sit on.
6. Power up. The board should buzz on the synthetic hazard pattern.

### T4 — HAP-T9: measure the effect, the one measurement that decides the design
Scope OUT+ (RC 100 kΩ / 470 pF per SLOS854D Fig. 11). Measure effect-1 duration.
**Predicted 45–75 ms** from Table 1 (Library B: rise 40–60 ms, brake 5–15 ms).
- Set the R-3 floor to measured duration × 1.2.
- **H-D4:** if rise exceeds 80 ms the library is wrong — move to C or D.
- Save the capture. It is submission evidence and it cannot be retaken after 18 Sep.

### T5 — Non-bench work, do it while builds run
- **D1 (highest-value hour in the project, no hardware):** the two contest pages. Deadline,
  what must be submitted, **whether entry registration closes before the work does**, shipping
  address and recipient.
- **Start the customs paperwork.** Proforma invoice, declared value, HS code. Multi-day and
  entirely non-technical.
- **Decide G-1 (risk doc, Tier 1):** the ML label is currently a deterministic function of two
  of its own input features, which makes the network reproduce an if-statement. Recommended fix
  is a time-shifted label — label frame *t* with the rule evaluated at *t+k*, k ≈ 5–15 frames.
  Costs a column shift in the training CSV **if decided before collection starts (9 Sep)**, and
  a full recollection afterwards.

---

## 4. WHAT NOT TO DO

- Do not wire TRIG before T2 ships the pulse — a hazard-level on PE13 cancels playback.
- Do not connect the motor before V-W-6.
- Do not enable D-cache (D2).
- Do not touch DLPF_CFG or AFS_SEL once collection begins.
- Do not re-run the Phase 4 correctness soak — RZ4 is closed, 91k events, zero torn reads.
- Do not plug anything into CN4 or CN10, or reconnect the camera FFC to CN14.

---

## 5. OPENING PROMPT FOR THE NEXT SESSION

Paste from here down, with this file attached.

---

I'm continuing the Haptic-Sense build (TRON Forum Contest 2026, STM32N6570-DK, µT-Kernel 3.0).
Repo is at `~/haptic-sense`, branch `phase5/i2c-first-light`. Read `CLAUDE.md` first — it holds
the locked architecture, the hardware map with connector numbers, and the red zones including
RZ9 (GPDMA channel security attributes), which cost a full bench session on 29 August.

Attached is the handoff from last night's session. Also in the repo:
`docs/PLAN_TO_SUBMISSION_20260830.md` (the schedule — hardware ships 18 Sep, documentation
until 30 Sep), `docs/RISK_ANALYSIS_20260830.md` (fourteen ranked gaps), and
`docs/evidence/phase5/PHASE5_I2C_FIRST_LIGHT_20260829.md`.

State: the I2C L1 primitive is proven on hardware in both directions. One SmartElex DRV2605L
answers at 0x5A with STATUS/MODE/LIBRARY_SEL reading 0xE0/0x40/0x01, EN driven by PE7. Block 1a
(the DRV2605L register driver) is written and committed but **has never been built or flashed**.

Work through §3 of the handoff in order, starting with T0. T1 in particular unblocks T2 and
closes an open question about which clock feeds DWT->CYCCNT.

Two things I want throughout:

Be adversarial. Look for defects rather than summarising what looks fine. Two silent-failure
defects were found in the last week — a GPDMA channel with no security attributes that
transferred nothing while reporting success, and a DMA destination buffer pre-filled with a
value that was indistinguishable from a real reading. Assume there are more of that class.

Apply citation discipline. Every hardware specific — pin number, register address, connector
label, register value — must trace to a file in the repo, the local datasheet copies under
`docs/datasheets/`, or a live documentation lookup, and you name the source. Anything from
memory gets tagged `[UNVERIFIED]` with the verification step named. This project has already had
three cross-sensor spec contaminations; don't add a fourth.
