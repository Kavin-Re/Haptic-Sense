# OC_DETECT — THE FIRST REAL FAULT, AND THE POLL THAT CAUGHT IT
**2026-08-30 ~19:27–19:35 IST. Found, diagnosed and fixed inside ten minutes.**
Firmware: `052135b`. No code changed — this is a hardware-and-instrumentation record.

---

## 1. What was seen

```
[HLT] polls=9 faults=0x1 cfglost=0
[EFF] n=1 last=58652 min=58652 max=58652 late=3 stuck=1
[I2C] ... ok=2565 err=0 tmo=0 recov=0
[DRV] init=0 id=7 mode=0x1 lib=0x2 seq=0x1 odc=0x8b sts=0xe0 armed=1
```

`faults=0x1` is **bit 0, `OC_DETECT`**. SLOS854D Table 4, verbatim: *"Latching overcurrent
detection flag. If the load impedance is below the load-impedance threshold, the device shuts
down and periodically attempts to restart until the impedance is above the threshold."*

**Every other indicator was clean.** `init=0`, `armed=1`, `mode/lib/seq/odc` all correct,
`cfglost=0`, `err=0 tmo=0 recov=0`, `canary_err=0`, frame rate normal. Without the health poll
the only symptom available to a human would have been *"the buzz feels weaker sometimes."*

## 2. What `[EFF]` added

| | healthy | during the fault |
|---|---|---|
| n | 5 | **1** |
| late | 0 | **3** |
| stuck | 0 | **1** |

`late` = GO already clear on the first read; `stuck` = GO never cleared. **That is the exact
fingerprint of the shutdown-and-restart cycle**: playback aborts, so GO drops early or hangs
mid-restart. And the one sample that did complete read **58652 µs**, squarely inside the
healthy band — proving the fault was **intermittent, not a hard short**, before anything was
touched.

Those two counters exist only because HAP-T9's discarded-sample bookkeeping refused to turn a
failed measurement into a plausible number. They were written as measurement hygiene and turned
out to be a diagnostic.

## 3. Cause

**Two bare twisted joints where the motor's thin enamelled leads met the jumper wires** —
unsupported, in free air, roughly a centimetre apart, no sleeving or strain relief
(bench photos, 2026-08-30 19:30).

The intermittency was the clue that identified it: **the short only occurred while the motor was
running, because the motor's own vibration was what closed it.** Nothing else on the bench moves
when the motor fires. Fires → vibrates → joints touch → OC → shutdown → playback aborts →
joints separate → next attempt succeeds.

## 4. Fix and confirmation

Joints separated and offset so they cannot meet. Power-cycled (confirmed by
`whoami=0x140e0`, the Table 3 reset values — the DRV2605L loses its configuration only on a
true power cycle, per H-D2).

**22 consecutive heartbeats, up_ms 10741 → 32707:**
```
[HLT] polls=9..30  faults=0x0  cfglost=0        (every line)
[EFF] n=5 last=58539 min=58539 max=58760 late=0 stuck=0
[I2C] err=0 tmo=0 recov=0    [HB] canary_err=0 q=0 qovr=0
pulses 8 -> 17, and pulses + suppressed == hazard on every line
```

**The coil is undamaged.** Effect duration across three independent boots:

| boot | min | max |
|---|---|---|
| 2026-08-30 early | 58605 | 58708 |
| 2026-08-30 midday | 58505 | 58660 |
| 2026-08-30 post-fault | 58539 | 58760 |

Combined **58505–58760 µs, spread 255 µs = 0.44%**, across a power cycle and a hardware fault.
A partially shorted coil would have changed the drive; it did not. The R-3 floor of 75 ms
remains **1.276×** the worst observed maximum, still above the ×1.2 rule.

## 5. What this does and does not prove

**Proves:** the runtime STATUS poll works, catches a real fault, and is the only thing on this
bench that could have. The `late`/`stuck` counters localise the failure mode, not just its
existence.

**Does NOT prove the fix is permanent.** 22 seconds is roughly three hazard bursts. Intermittent
mechanical faults recur. **Leave the board running ten minutes and confirm `faults` is still
`0x0`** before treating this as closed. `faults_seen` is sticky for the life of a boot, so any
single recurrence in that window will be visible.

## 6. Consequences to carry forward

- **Block 5 (mechanical freeze) now has a worked example of why it exists.** Every motor
  connection gets soldered and sleeved, offset so no two conductors can meet, and strain-relieved.
  The courier will do far more to this assembly than the ERM's own vibration did.
- **The 100 µF bulk capacitor across 3V3/GND is still not fitted** (T3 step 5, confirmed absent
  by bench photo). It is **not** the cause of this fault — `OC_DETECT` measures output current,
  and a sagging rail cannot trip it — but it is owed before the VL53L1X and MPU6050 join that
  rail. See §7.
- **Do not solder the VL53L1X onto a bench with a known-intermittent motor lead.** Confirm the
  ten-minute clean run first.

## 7. CLOSED: the USB disconnects were manual unplugs

Both long captures today ended with:
```
FATAL: read zero bytes from port
term_exitfunc: reset failed for dev UNKNOWN: Input/output error
```
**Confirmed by the developer 2026-08-30: the cable was unplugged by hand both times.** Not a
fault. Recorded because the message reads like one, and the hypothesis it would otherwise have
triggered — ERM transient into a 3V3 rail with no bulk capacitance, glitching the VCP — is
plausible enough that a later session could waste an hour on it.

**The 100 µF is still owed regardless** (T3 step 5, confirmed absent by bench photo). It played
no part in this fault, but it belongs on the rail before the VL53L1X and MPU6050 join it.
