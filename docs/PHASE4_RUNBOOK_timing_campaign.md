# Phase 4 Runbook — Preemption Measurement Campaign (Red Zone #3)

**Project:** Haptic-Sense · TRON Forum Contest 2026
**Prerequisite state:** Phase 4 correctness gate PASSED (commit `90d65b7` — ~91k events, zero torn reads, soak archived at `docs/evidence/phase4/phase4_soak.log`). `DEBUG_TIMING` currently compiled out.
**Goal of this session:** produce hardware evidence that **worst-case** hazard-path preemption latency is **< 1 ms over >= 100 events under CPU load**.
**Claim wording (locked, CLAUDE.md §3):** "deterministic sub-millisecond latency (< 1 ms, hardware-verified)". Never "zero-latency".

---

## 0. What is being measured (read once before touching anything)

Two GPIO edges, one interval:

| Signal | Pin | Arduino header | Driven by | Meaning |
|---|---|---|---|---|
| TIMING_D0 | PH5 | **D4** | `inference_task` (TK_PRI 2) at the instant it signals `result_ready_sem` | "hazard result is ready" |
| TIMING_D1 | PD6 | **D7** | `hazard_task` (TK_PRI 1), first action after waking from `tk_wai_sem` | "hazard task is running" |

**Δt(D0↑ → D1↑) = scheduler preemption + context-switch latency.** The hazard task clears D1 before looping back to its wait (per the Phase 4 implementation), so each event is a clean rising-edge pair. The contest number is the **maximum** Δt across >= 100 pairs while the CPU is deliberately kept busy — the tail matters because the app executes from XSPI RAM and a cache miss on the hazard path is exactly the kind of thing that ruins the worst case while leaving the average pretty.

Event supply is not a concern: the soak showed hazard fires ~12.5% of frames ≈ **6 events/s** → 100 events in ~17 s of capture. One 60 s capture gives ~350 events with margin.

---

## 1. Pre-flight checklist (nothing powered yet)

- [ ] Board on the bench, USB-C to host, no CubeProgrammer session left connected from earlier
- [ ] Logic analyzer (24 MHz sigrok clone) + probe leads
- [ ] `sigrok-cli` and PulseView installed on Mint (`sigrok-cli --version`; install `sigrok-cli pulseview` via apt if missing)
- [ ] Free the serial port habit: exit picocom with `Ctrl-A Ctrl-X`, and `pkill picocom` if a stale lock ever appears
- [ ] This runbook + `docs/PHASE4_DESIGN_task_architecture_preemption.md` open

## 2. Firmware prep (Claude Code task, then YOUR rebuild/flash)

1. **Enable the instrumentation defines** in the Appli `.cproject` (CubeIDE: Project → Properties → C/C++ Build → Settings → MCU GCC Compiler → Preprocessor):
   - `DEBUG_TIMING` — compiles in the PH5/PD6 pulses and DWT capture
   - `DEBUG_CHATTER` — compiles in the heartbeat busy-load mode (both configurations were syntax-checked at Phase 4 implementation time; they are separate flags so the baseline run can disable chatter without touching timing)
2. **Rebuild** in CubeIDE. §9 rule: confirm `Debug/*-Trusted.bin` timestamp is **newer than the enabling commit** and note the size (it will grow slightly vs 65,856 B).
3. **Flash** @ `0x70100000` (Dev Boot → Connect → Program → verify OK → **Disconnect**), BOOT1 → L, NRST.
4. **Sanity gate before any wiring:** open picocom, confirm the `[HB]` line still shows `canary_err=0 q=0 qovr=0` and frames/inf in lockstep **with instrumentation compiled in**. Instrumentation itself perturbs timing; if the pipeline degrades with DEBUG_TIMING on, stop and investigate before measuring. Also confirm the heartbeat now prints the DWT min/mean/max fields.
5. **DWT check (flagged unverified under TrustZone since design):** the heartbeat's DWT max must be a plausible nonzero number that changes between lines. If CYCCNT reads 0 or never advances, TrustZone/FSBL is gating DWT — **note it and proceed**; the LA is the primary evidence and carries the claim alone.

## 3. Wiring (board powered off, or at least before starting capture)

| LA channel | Connect to | Signal |
|---|---|---|
| CH0 (D0) | Arduino header **D4** | PH5 / TIMING_D0 |
| CH1 (D1) | Arduino header **D7** | PD6 / TIMING_D1 |
| CH2 (optional) | Arduino header **D8** | PE7 / DRV_EN — shows the full chain to the actuator pin in the same trace; nice contest visual, not required for the number |
| GND | Any board GND pin on the Arduino header | common reference — **do not skip**; floating ground = garbage edges |

Rules: probe **only** the Arduino header pins listed (3.3 V logic). Nothing connects to port O or any XSPI-adjacent pin.

## 4. Analyzer bring-up

1. `sigrok-cli --scan` → the clone should appear (typically as the `fx2lafw` driver). **Record the scan output** — it is itself a checklist item in CLAUDE.md §3. If nothing appears: try another USB port/cable, check `dmesg`, and if needed install the sigrok udev rules.
2. **Sample rate: use 8 MHz, not the max 24 MHz.** Rationale: the bound is 1 ms; 8 MHz gives 125 ns resolution — ~8000x finer than the pass line — while staying far inside the USB streaming budget of a cheap fx2lafw clone on 2–3 channels. Maxing the rate risks dropped samples, which corrupt exactly the long-capture worst-case hunt you are doing. *(If the clone drops samples even at 8 MHz, 2 MHz is still 500x margin.)*
3. Quick visual check in PulseView: 2 s capture with the app running — you should see D0/D1 pulse pairs arriving in bursts (~6/s average, clustered in approach spans). If a channel is flat, re-seat that probe.

## 5. The campaign — two runs

### Run A — chatter ON (the contest run)
The heartbeat's chatter mode keeps the CPU busy so priority-1 wakeups must genuinely preempt running code — an idle-system latency number is meaningless.

Headless capture (repeatable, scriptable):
```
sigrok-cli --driver fx2lafw --config samplerate=8m \
  --channels D0,D1 --time 60s -o docs/evidence/phase4/timing_chatter.sr
```
*(driver name per your `--scan` output; add the third channel if wiring CH2)*

### Run B — chatter OFF (baseline)
Rebuild with `DEBUG_CHATTER` removed (or a runtime switch if implemented), reflash, capture 60 s to `timing_baseline.sr`. Both datasets go in the write-up: baseline shows the floor, chatter shows the guaranteed bound.

During both runs keep picocom logging (`--logfile timing_runA_uart.log` etc.) so the DWT-reported min/mean/max rides along as corroboration.

## 6. Analysis — worst case or nothing

Extract per-event Δt from each `.sr` file. Algorithm (hand to Claude Code as a small Python script over the CSV export):

1. `sigrok-cli -i timing_chatter.sr -O csv > timing_chatter.csv`
2. Find every rising edge on D0; for each, take the **next** rising edge on D1; Δt = t(D1) − t(D0).
3. Sanity rules: discard nothing silently — if a D0 has no D1 before the next D0, that is a **missed hazard event and a finding, not noise**; count and report it (expected count: zero). Event count must be >= 100 (expect ~350).
4. Report: N, min, mean, sigma, **max**, plus a histogram. The pass/fail line is `max Δt < 1 ms` on **Run A**.
5. Cross-check the LA max against the DWT max from the UART log (`cycles / 600000 = ms` at 600 MHz). Agreement within measurement error is itself evidence; disagreement is a bug to chase before publishing either number.

## 7. Evidence package (into `docs/evidence/phase4/`, committed with the CLAUDE.md update — no dangling pointers)

- [ ] `sigrok-cli --scan` output (text)
- [ ] `timing_chatter.sr` + `timing_baseline.sr` (raw captures)
- [ ] CSV exports + the analysis script + its printed stats
- [ ] One annotated PulseView screenshot of a representative D0→D1 pair (and CH2 if wired)
- [ ] UART logs with DWT stats
- [ ] CLAUDE.md: RZ3 → RESOLVED only if Run A passes, with N, max Δt, and file pointers

## 8. If max Δt >= 1 ms — ranked triage (do not panic-tune)

1. **Cache miss on the hazard path** (XSPI execution) — *likely if the tail is rare and large*. Test: histogram shape — a tight cluster plus a few large outliers points here. Mitigation to investigate (verify against RM0486 before claiming): pin the hazard task's hot path into ITCM/fast RAM.
2. **Interrupt-masked section in BSP/HAL** holding off the scheduler — *likely if outliers correlate with heartbeat prints*. Test: disable the heartbeat print for a run; if the tail vanishes, `tm_printf`'s critical section is the culprit — move printing or accept and document.
3. **Chatter implementation starving at the wrong priority** — *speculative*. Test: baseline (Run B) clean but Run A wild beyond explanation → inspect the chatter loop for unintended critical sections.
4. **LA artifact (dropped samples)** — *check first, cheapest*. Test: rerun at 2 MHz; if the outliers move or vanish, it was the capture, not the board.

Whatever the outcome: the number reported is the measured worst case under load. If it is 0.4 ms, the claim is "< 1 ms, hardware-verified, worst case 0.4 ms over N events" — never rounded down, never averaged up.

---
*Session exit: RZ3 resolved (or triaged with data), evidence committed, CLAUDE.md updated. Then Phase 4 is fully closed and Phase 5 (flip `DEVCNF_USE_HAL_IIC`, sensor bring-up) is next.*
