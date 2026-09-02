# THE VL53L1X REFUSES ITS OWN ADDRESS FOR 12.8 ms, MID-CONFIGURATION
**2026-09-01. Logic-analyzer capture, decoded. This overturns two earlier
hypotheses, one of them mine.**

Capture: `sensorinit_fail_20260901.sr`, `fx2lafw`, 4 MHz, 8.000 s, SCL on D2,
SDA on D3. Decoded with `docs/evidence/phase5/decode_sr_i2c.py`.
32,000,000 samples, 8,887 bus frames, 4,610 logical transactions.

Labelled per CLAUDE.md §9: **evidence**, **inference**, **speculation**.

---

## 1. WHAT THE WIRE SAYS *(evidence)*

```
=== transaction census ===
  0x29 ADDR-NACK   207          <-- the fault
  0x29 RD  ok     4236
  0x29 WR  ok       91
  0x29 WR  DATA_NACK   2
  0x5A RD  ok       41
  0x5A WR  ok       33
```

**Per boot, the configuration block gets exactly 30-31 registers in and then
the sensor stops answering its address entirely.**

```
   t=2.196631  52 00 44 00      \
   t=2.196920  52 00 45 00       |  writes 0x002D..0x004B all ACK,
   t=2.197205  52 00 46 20       |  spaced 287 us apart
   t=2.197491  52 00 47 0B       |
   t=2.197868  52 00 48 00       |
   t=2.198154  52 00 49 00       |
   t=2.198440  52 00 4A 02       |
   t=2.198727  52 00 4B 0A      /   <-- last one that lands
        gap 161.25 us
   t=2.199103  52!               \
   t=2.199213  52!                |  69 consecutive address-NACKs,
   t=2.199322  52!                |  ~185 us apart, 12.8 ms total,
   t=2.199431  52!               /   nothing else on the bus between them
```

The burst is unbroken — the decoded frame kinds run
`NNNNNNNN...N` for 69 frames and then `iririr...` (index-write, read) as the
poll loop starts working again.

**It happens three times in the capture and it is nearly identical each time:**

| boot | config writes that ACKed | first..last | NACKs | span |
|---|---|---|---|---|
| t=2.184 | **31** | 0x002D..0x004B | 69 | **12.8 ms** |
| t=5.114 | **30** | 0x002D..0x004A | 69 | **12.8 ms** |
| t=6.111 | **31** | 0x002D..0x004B | 69 | **12.8 ms** |

69 ≈ the 60 remaining config writes + `StartRanging` + the first few poll
reads. The 60 missing registers are exactly **0x004C .. 0x0087**.

## 2. WHAT THIS KILLS

### 2.1 It is NOT back-to-back transfer spacing / `HAL_BUSY` *(evidence)*
That was my leading hypothesis and it is wrong.

- The gap before the first NACK is **161 µs**, not microseconds.
- Config writes are spaced **287 µs** apart (transaction ~100 µs + ~187 µs idle).
- Across the **entire 8 s capture**, real STOP→START gaps: min **10.75 µs**,
  median 80.75 µs, and **zero** gaps below the 1.3 µs fast-mode `t(BUF)` minimum.

The master's bus timing is clean everywhere. `VL53L1_PORT_WRITE_GAP_MS` was
added for this hypothesis; it is still worth running, but for a different
reason (§4.2).

### 2.2 It is NOT "the sensor is held in reset" in the simple sense *(evidence)*
The sensor ACKs **4,236 reads** in the same capture, before and after each
burst, including `0x010F -> EA`, `0x0110 -> CC`, `0x010F -> EA CC 10` and
`0x00E5 -> 03`. It is alive, addressable and correct either side of the window.

### 2.3 My "do not fit the XSHUT pull-up" advice was over-generalised *(correction)*
I argued from `XSHUT = 2.62 V`, `boot=1` and `id=0xeacc` that the part could not
be in reset. **Every one of those observations was taken under light bus load.**
The failure only appears after ~8.7 ms of sustained traffic, so concluding
"XSHUT is not the problem" from them broke CLAUDE.md §9's own rule: *never
generalize from partially tested cases.* XSHUT is back on the suspect list —
see §4.1, which is the test that settles it.

## 3. WHY THE INIT THEN TIMES OUT — fully explained *(evidence → inference)*

The poll loop reads, 2,109 times each, exactly two values:

```
  reg 0x0030 (GPIO_HV_MUX__CTRL)    -> 0x11
  reg 0x0031 (GPIO__TIO_HV_STATUS)  -> 0x03
```

Trace it through ST's code:
- `VL53L1X_GetInterruptPolarity`: `0x11 & 0x10 = 0x10`, `>>4 = 1`, `!1` ⇒
  **IntPol = 0**
- `VL53L1X_CheckForDataReady`: `0x03 & 1 = 1`; `1 != 0` ⇒
  **`isDataReady = 0`, on every one of 1000 iterations**

So the `VL53L1X_ERROR_TIMEOUT` is a **symptom**, not a second fault: 60 of 91
configuration registers were never written, so the part never ranges and never
asserts data-ready. Fix the NACK window and the timeout goes with it.

**`calls=3093` is confirmed exactly** — 1 BootState + 1 GetSensorId + 91 config
+ 1 StartRanging + 999×3 + 2 = 3093. Every stage ran its full count.

## 4. WHAT IT COULD BE, RANKED, AND THE TEST FOR EACH

The trigger is **~8.6-8.9 ms of sustained bus activity**, not a specific
register: the count was 31, 30, 31 across three boots. A register-triggered
fault would be 31 every time. *(inference, high confidence)*

The recovery is **12.8 ms ± 0.1 ms, three times.** That is a fixed duration, not
analog noise. Note that DS12385 §3.6 says **"The boot duration is 1.2 ms
maximum"**, so 12.8 ms is **ten times** a normal power-on boot — a plain
reset-and-reboot does not explain it on its own. *(evidence)*

### 4.1 Sensor is being reset or browned out — **test this first**
XSHUT reads **2.62 V**, not the 3.32 V rail. That gap says XSHUT is pulled to
something other than VIN, or through a high impedance against real leakage. If
the 7SEMI runs the die from an onboard regulator at ~2.62 V, that is **20 mV
above the DS12385 AVDD minimum of 2.6 V**, with no bulk capacitance anywhere on
the board. *(evidence for the numbers; inference for the mechanism)*

> **THE TEST: put the logic analyzer on XSHUT and capture again.**
> D0 → XSHUT, D1 → GPIO1, D2 → SCL, D3 → SDA.
> ```
> sigrok-cli --driver fx2lafw --config samplerate=4m --time 8s \
>   --channels D0,D1,D2,D3 -o docs/evidence/phase5/xshut_probe_$(date +%Y%m%d).sr
> ```
> - **XSHUT goes LOW during the 12.8 ms window** ⇒ the part is being reset.
>   Fit 10 kΩ from XSHUT to VIN (ST's recommended value, DS12385 §3.6 note) and
>   re-run. This is the one case where the resistor is the fix.
> - **XSHUT stays HIGH** ⇒ the part is powered and out of reset and is refusing
>   anyway. Go to §4.2/§4.3.

Also measure, with the board running: **VIN at the 7SEMI pin itself** (not at
CN8 — you have never metered it), and XSHUT again during init with the meter in
min/max hold if it has one.

### 4.2 Sustained-activity threshold — **free, one rebuild**
`VL53L1_PORT_WRITE_GAP_MS` is already in `app_vl53l1_port.c`. Set it to **1**
and rebuild. That stretches the config block from 8.7 ms to ~130 ms and drops
the bus duty cycle roughly fivefold.

- **NACK burst disappears** ⇒ the trigger is sustained activity. Spacing is a
  legitimate mitigation and you have a ranging sensor immediately.
- **Still NACKs after ~30 writes** ⇒ the trigger is the write **count**, not
  elapsed time, and this is a device state-machine behaviour.

Either answer is worth the rebuild. **Put it back to 0 afterwards** — it costs
the frame loop 1-2 ms per `ClearInterrupt`.

### 4.3 Bus loading / signal integrity *(speculation)*
Weakest of the three. The pull-up budget is measured and in spec (818 Ω,
3.57 mA against a 4 mA limit) and SCL low-pulse widths look healthy: median
1.500 µs, p99 2.500 µs, 719 pulses over 10 µs and 10 over 100 µs across
207,130 pulses — mild clock stretching, nothing pathological.

## 5. THE THREE REBOOTS ARE PROBABLY YOURS *(inference)*

`drv2605l_init()` writes DEV_RESET (`B4 01 80`) once per boot. It appears at
**t=2.184, 5.114, 6.111** — three boots in 8 s, at irregular 2.9 s and 1.0 s
intervals, with ~0.6-0.9 s of total silence before each. That is the shape of a
person pressing reset, not a watchdog or a brownout loop, and you were told to
reset the board to land `SensorInit` inside the capture window.

**Confirm it anyway:** take one capture with exactly one reset press. If more
than one DEV_RESET appears, the board is resetting itself and that is a
separate and more serious problem than the NACK window.

## 6. WHAT IS ALREADY PROVEN AND SHOULD BE BANKED

This capture is **submission evidence** — it is most of L5 and it cannot be
retaken after 18 Sep. Archive the `.sr`.

- **16-bit addressing reaches the wire, verbatim:** `52 01 0F` → `53 EA`, and
  `52 01 0F` → `53 EA CC 10`. That is the `0x52 idxMSB idxLSB data` proof L5
  exists to obtain. **V-5/H1 closes.**
- The REG8 negative control is on the wire too: `52 0F` → `53 00`. One index
  byte, not two, returning a different value. The 8-bit and 16-bit branches are
  provably distinct on this bus.
- `52 00 E5` → `53 03` — `FIRMWARE__SYSTEM_STATUS` non-zero, the part booted.
- Bus timing is clean: zero STOP→START gaps below the 1.3 µs `t(BUF)` minimum
  across 4,609 measured gaps.
- **The 0x5A traffic is untouched: 41 reads, 33 writes, zero NACKs.** Adding the
  7SEMI did not harm the DRV2605L. Block 1 is intact and the fault is specific
  to 0x29.
