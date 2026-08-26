# DRV2605L — P3 Init & Arming-Readback Sequence (Design Document)

**Project:** Haptic-Sense · Phase 5 driver bring-up
**Status:** DESIGN — reviewed against TI SLOS854D (Rev. D, Mar 2018). No code committed. Implement via Claude Code, then hardware-verify.
**Task context:** ALL I2C below runs in the **Priority 3 Sensor Acquisition task** via `i2c_wr`/`i2c_rd` (DMA, semaphore-wrapped). Priority 1 touches GPIO only.
**Device:** DRV2605L @ 7-bit 0x5A (SLOS854D §8.5.3.1) · Adafruit breakout (primary channel) · ERM coin 10 mm, 3 V class, 60–90 mA · open-loop ERM + ROM library (locked decision, July 2026)

> **Recovery note (2026-08-26):** this file was produced 2026-07-19 and never saved to disk. Restored verbatim from the chat transcript of that session. Content is unchanged from the original, including the HAP-T10 resolution applied later the same day.

---

## 0. Audit findings resolved this session (updates to HAP-T ledger)

| ID | Prior status | Resolution | Source |
|---|---|---|---|
| HAP-T2 | [UNVERIFIED] EN register retention | **RESOLVED.** EN low = shutdown; **registers are NOT reset**, but I2C read/write is impossible while EN is low (device may still ACK — deceptive). STANDBY bit (0x01[6]) defaults to **1** at power-up; asserting STANDBY **immediately stops any playing waveform** and retains registers + I2C. | SLOS854D §8.4.1.3, §8.4.1.4, Table 5 |
| HAP-T3 | [UNVERIFIED] retrigger semantics | **RESOLVED — design-changing.** In edge mode, a second rising edge on IN/TRIG **while GO is still high CANCELS the playing waveform** (clears GO, does not fire a new effect). Falling edge does nothing. | SLOS854D §8.3.5.6.2, Table 5 MODE=1 |
| HAP-T4 | [UNVERIFIED] min pulse width | **RESOLVED.** Trigger pulse width **≥ 1 µs** to ensure detection. | SLOS854D §8.3.5.6.2 |
| — (new) | Trigger-to-output latency | t(start) = **0.7 ms typ** from external trigger to output signal. | SLOS854D §6.7 |
| — (new) | First-light ID check | Status reg 0x00 default 0xE0; **DEVICE_ID[2:0] = 7** identifies DRV2605L. | SLOS854D §8.6.1 |
| — (new) | VIH | IN/TRIG VIH min = **1.3 V** → 3.3 V GPIO drive valid. | SLOS854D §6.3 |

**HAP-T3 consequence (critical):** cadence-based urgency grading is viable ONLY if the inter-trigger period always exceeds the total effect duration (rise + body + brake). Otherwise alternating edges fire/cancel and **higher urgency produces WEAKER output** — an inverted safety response. Mitigation is rule R-3 below.

**Remaining [UNVERIFIED] (hardware-gated):**
- **HAP-T1:** SmartElex ×2 breakout of IN/TRIG — visual silkscreen inspection.
- **HAP-T8 (new):** Power-state path after EN low→high cycle: registers retained, but ambiguous whether device re-enters standby *state* regardless of retained STANDBY=0 bit (SLOS854D Fig. 17 diagram vs §8.4.1.3 text). **Sidestepped by rule R-1** (EN never toggled at runtime); verify on bench anyway.
- **HAP-T9 (new):** Total effect duration of chosen waveform (drives R-3 floor). Measure OUT+/OUT− envelope or accelerometer on bench.
- ~~HAP-T10~~ **RESOLVED (2026-07-19):** Effect ID 1 = **"Strong Click - 100%"** per the §11.2 Waveform Library Effects List, cross-confirmed by TI E2E (TI staff response, valid ID range 1–123), Adafruit CircuitPython guide, and the Adafruit Arduino library effect table. Fallback candidate if bench feel is too soft/long for cadence grading: **ID 4 = "Sharp Click - 100%"** (same table). IDs 15/16 are 750 ms / 1000 ms alerts — useful later for a distinct "sensor-fault" pattern, NOT for cadence grading (duration >> R-3 floor).
- **HAP-T11 (new):** Exact delay required between EN-high and first I2C write — no explicit spec found; 1 ms interim guard. Confirm none needed / adjust.

---

## 1. Locked design rules (add to PROJECT_DEFENSE.md registry)

- **R-1 — EN is init-only.** PE7 driven high once during P3 init and **never toggled at runtime**. Rationale: I2C is dead while EN is low (silent-failure trap: device may still ACK, SLOS854D §8.4.1.3), and post-toggle power-state is ambiguous (HAP-T8). Reversible: yes, after HAP-T8 bench result.
- **R-2 — Runtime kill = STANDBY bit, owned by P3 exclusively.** Write 0x01 ← 0x41 stops playback immediately and retains everything; re-arm by rewriting 0x01 ← 0x01 **followed by mandatory arming readback (§4)**. P1 never kills (it has no I2C); P1's only "stop" is withholding further trigger edges. Reversible: no (safety-architectural).
- **R-3 — Minimum inter-trigger period floor in P1.** Interim floor **125 ms** (max 8 Hz pulse rate) until HAP-T9 characterizes effect duration; then floor = t_effect_total + 20 % margin. Enforced in P1 with tick-count comparison — no blocking calls. Rationale: HAP-T3 cancel-on-retrigger. Irreversible while edge mode is the trigger architecture.
- **R-4 — Standby minimized.** If I2C halts unexpectedly *during standby*, only a power cycle recovers the bus (SLOS854D §8.3.11 — the 4.33 ms I2C watchdog is inactive in standby). Normal operation keeps the device in ready state (quiescent 0.5 mA typ, §6.5 — acceptable).

---

## 2. Register configuration table (all writes P3, `i2c_wr(0x5A, reg, I2C_REG8, &val, 1)`)

| Order | Reg | Value | Meaning | Source |
|---|---|---|---|---|
| 1 | 0x01 | 0x00 | Exit standby (STANDBY=0), MODE=0 internal trigger (temporary, for diagnostics) | §8.6.2 |
| 2 | 0x1A | 0x36 | N_ERM_LRA=0 (ERM) + datasheet defaults for FB/LOOP/BEMF fields — explicit write for determinism | §8.6.20 |
| 3 | 0x1D | 0xA0 | ERM_OPEN_LOOP=1, NG_THRESH=4 % — matches power-on default; explicit for determinism | §8.6.23 |
| 4 | 0x17 | 0x8C | OD_CLAMP default. Open-loop ERM full-scale: V = 21.59 mV × OD_CLAMP. 21.59 mV × 140 = **3.02 V** ≈ motor 3 V rating. Keep default. | §8.5.2.2, Eq. (6) |
| 5 | 0x03 | 0x02 | **Library B** — TS2200 ERM, rated 3 V / overdrive 3 V, rise 40–60 ms, brake 5–15 ms. Best match for 3 V-class coin ERM. (Library A = 1.3 V rated w/ fixed overdrive — fallback if B feels weak.) | §8.3.5.2 Table 1, §8.6.4 |
| 6 | 0x04 | 0x01 | Waveform slot 0 = effect ID 1 = "Strong Click - 100%" (verified, §11.2 effect list + TI E2E) | §8.6.5 |
| 7 | 0x05 | 0x00 | Slot 1 = 0 → sequence terminator (single-effect playback) | §8.6.5 |
| 8 | — | — | *(Diagnostics pass — §3 step D)* | |
| 9 | 0x01 | **0x01** | **ARM: STANDBY=0, MODE[2:0]=1 external edge trigger** | §8.6.2 |

Registers 0x0D–0x10 (time offsets): power-on default 0x00, not written.
RATED_VOLTAGE (0x16): **ignored in open-loop** (§8.6.16) — not written. Auto-calibration: not run (closed-loop deferred, locked).

## 3. Init sequence (P3 task body, pseudocode against real `app_i2c.c` API)

```c
/* File: app_drv2605l.c — ONLY CALL FROM PRIORITY 3 SENSOR TASK */
/* All i2c_wr/i2c_rd are DMA + semaphore primitives (commit fc6ff88). */

ER drv2605l_init(void)
{
    UB v;

    /* [A] EN high (PE7), then guard delay — HAP-T11 interim 1 ms */
    HAL_GPIO_WritePin(DRV_EN_PORT, DRV_EN_PIN, GPIO_PIN_SET);
    tk_dly_tsk(1);

    /* [B] First-light ID check — expect DEVICE_ID[2:0] = 7 (0x00 reads 0xE0 mask 0xE0) */
    if (i2c_rd(0x5A, 0x00, I2C_REG8, &v, 1) != E_OK) return E_IO;
    if ((v >> 5) != 0x07) {
        tm_printf("DRV2605L ID FAIL: 0x%02X\n", v);   /* log-don't-hard-fail policy */
    }

    /* [C] Configuration writes — table §2, orders 1–7 */
    v = 0x00; i2c_wr(0x5A, 0x01, I2C_REG8, &v, 1);    /* wake, internal trig */
    v = 0x36; i2c_wr(0x5A, 0x1A, I2C_REG8, &v, 1);    /* ERM */
    v = 0xA0; i2c_wr(0x5A, 0x1D, I2C_REG8, &v, 1);    /* open-loop */
    v = 0x8C; i2c_wr(0x5A, 0x17, I2C_REG8, &v, 1);    /* OD_CLAMP ~3.02 V */
    v = 0x02; i2c_wr(0x5A, 0x03, I2C_REG8, &v, 1);    /* Library B */
    v = 0x01; i2c_wr(0x5A, 0x04, I2C_REG8, &v, 1);    /* effect ID 1 */
    v = 0x00; i2c_wr(0x5A, 0x05, I2C_REG8, &v, 1);    /* terminator */

    /* [D] Actuator diagnostics — catches disconnected/shorted motor = the
       silent-dead-haptics detector. MODE=6, GO, poll self-clear, read DIAG_RESULT.
       NOTE: briefly spins the motor. */
    v = 0x06; i2c_wr(0x5A, 0x01, I2C_REG8, &v, 1);
    v = 0x01; i2c_wr(0x5A, 0x0C, I2C_REG8, &v, 1);
    for (int i = 0; i < 100; i++) {                    /* ≤1 s timeout */
        tk_dly_tsk(10);
        i2c_rd(0x5A, 0x0C, I2C_REG8, &v, 1);
        if ((v & 0x01) == 0) break;
    }
    i2c_rd(0x5A, 0x00, I2C_REG8, &v, 1);
    if (v & 0x08) tm_printf("DRV2605L DIAG FAIL (actuator open/short)\n");
    /* DIAG_RESULT clears on read (§8.6.1) — read exactly once, act on it. */

    /* [E] ARM — edge-trigger mode */
    v = 0x01; i2c_wr(0x5A, 0x01, I2C_REG8, &v, 1);

    /* [F] ARMING READBACK — §4. Never declare armed without it. */
    return drv2605l_verify_armed();
}
```

## 4. Arming readback (the HAP-T2 mitigation — mandatory after EVERY write to 0x01)

```c
/* ONLY CALL FROM PRIORITY 3 SENSOR TASK */
ER drv2605l_verify_armed(void)
{
    UB mode, lib, seq0;
    if (i2c_rd(0x5A, 0x01, I2C_REG8, &mode, 1) != E_OK) return E_IO;
    if (i2c_rd(0x5A, 0x03, I2C_REG8, &lib,  1) != E_OK) return E_IO;
    if (i2c_rd(0x5A, 0x04, I2C_REG8, &seq0, 1) != E_OK) return E_IO;

    /* ARMED iff: STANDBY=0, DEV_RESET=0, MODE=1, library & effect intact */
    if (mode == 0x01 && (lib & 0x07) == 0x02 && (seq0 & 0x7F) != 0x00) {
        drv_armed = TRUE;    /* single flag, written ONLY here and in kill path */
        return E_OK;
    }
    drv_armed = FALSE;
    tm_printf("DRV2605L ARM FAIL: mode=%02X lib=%02X seq=%02X\n", mode, lib, seq0);
    return E_OBJ;
}
```

Failure semantics: `drv_armed == FALSE` means haptic channel is DOWN — P2/P3 escalate (e.g., UART alert); P1 keeps pulsing the GPIO regardless (harmless when unarmed, and recovers automatically the instant P3 re-arms).

## 5. P1 trigger primitive (GPIO only — zero I2C, zero blocking)

```c
/* Runs in PRIORITY 1 HAZARD TASK. GPIO + DWT only. */
/* Pulse width: spec ≥1 µs (SLOS854D §8.3.5.6.2).
   cycles = 1 µs × 600 MHz = 600; ×2 margin = 1200 cycles ≈ 2 µs. */
static UW last_trig_tick = 0;

void hazard_fire_haptic(UW now_tick)
{
    if ((now_tick - last_trig_tick) < HAPTIC_MIN_PERIOD_TICKS)  /* R-3: 125 ms interim */
        return;                       /* retrigger would CANCEL playback — HAP-T3 */
    last_trig_tick = now_tick;

    HAL_GPIO_WritePin(DRV_TRIG_PORT, DRV_TRIG_PIN, GPIO_PIN_SET);
    dwt_spin_cycles(1200);            /* 2 µs — bounded, no RTOS call */
    HAL_GPIO_WritePin(DRV_TRIG_PORT, DRV_TRIG_PIN, GPIO_PIN_RESET);
}
```

Urgency grading: P1 maps closing velocity → trigger period, clamped to **[HAPTIC_MIN_PERIOD (R-3), 1000 ms]**. Faster approach = faster clicks. Effect content itself never changes at runtime (that would be I2C).

**DRV_TRIG pin: NOT YET ASSIGNED (HAP-T5, open).** Candidates D5/D6 — verify MCU port against MB1939 schematic before implementation. **Blocker for coding §5; §§3–4 are unblocked.**

## 6. Runtime watchdog (P3, low cadence — e.g., every 100th sensor cycle ≈ 2 s)

Read 0x00 once; OVER_TEMP (bit 1) and OC_DETECT (bit 0) are **latching, clear-on-read** (§8.6.1). On either flag: log via `tm_printf`, set `drv_armed = FALSE`, attempt §3[E]–[F] re-arm next cycle. Note §8.4.4.4: an output short latches OC_DETECT and requires the short removed (+ mode change if short pre-existed playback, §8.4.1.6 note) — re-arm loop covers this.

## 7. Hardware verification ledger additions

| ID | Test | Pass criterion |
|---|---|---|
| HAP-T8 | EN low→high cycle, then GPIO edge, no I2C | Effect fires (else R-1 becomes permanent) |
| HAP-T9 | Scope OUT± envelope for effect ID 1, Library B | Duration measured; R-3 floor recomputed = dur × 1.2 |
| ~~HAP-T10~~ | ~~Effect table lookup~~ | CLOSED 2026-07-19: ID 1 = Strong Click 100%; fallback ID 4 = Sharp Click 100% |
| HAP-T11 | Shrink EN→I2C delay until first write NAKs/fails | Minimum guard documented |
| HAP-T12 | Full §3 init on bench, UART log | ID=7, DIAG pass, ARM readback E_OK |
| HAP-T13 | P1 edge at max cadence (125 ms) under CPU load | Every pulse produces a felt click; none cancelled |

## 8. Claude Code handoff instructions

1. Create `app_drv2605l.c/.h` (Apache 2.0 + SPDX) implementing §3–§6 against `app_i2c.c` primitives. Comment every function `// ONLY CALL FROM PRIORITY 3 SENSOR TASK` except §5 (P1, GPIO-only).
2. `dwt_spin_cycles()` goes in the existing `#ifdef DEBUG_TIMING` DWT module or a small always-on util — it's functional here, not instrumentation; keep it outside the ifdef.
3. Do NOT wire §5 until HAP-T5 pin is verified and added to CLAUDE.md §2 pin map.
4. After commit: **rebuild → confirm `-Trusted.bin` timestamp → flash** (standing rule).
5. Update CLAUDE.md: HAP-T2/T3/T4 resolutions, rules R-1–R-4, new ledger items T8–T13.
