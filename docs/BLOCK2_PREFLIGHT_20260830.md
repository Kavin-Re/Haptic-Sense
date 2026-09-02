# BLOCK 2 PRE-FLIGHT — ADVERSARIAL PASS BEFORE THE VL53L1X IS SOLDERED
**2026-08-30, repo at `0a82205`. No hardware was touched. One code change made
(the L1 probe, §10); everything else is a finding or a decision.**

Labelled per CLAUDE.md §9: **evidence** (measured or read in a source),
**inference** (follows from evidence), **speculation** (plausible, unverified).

Scope: `app_vl53l1_port.c`, `app_i2c.c`, ST's ULD `VL53L1X_api.c`, the build
configuration, and the plan's Block 2 procedure. Twelve findings; the first six
are on or beside the Block 2 critical path.

---

## SUMMARY TABLE

| # | Finding | Severity | Where |
|---|---|---|---|
| F-1 | DWT→µs conversion still divides by 600 on an 800 MHz CPUCLK | **HIGH — latent, arms in Block 9** | `app_tasks.c:572-575` |
| F-2 | `VL53L1X_SensorInit` discards the status of all 91 config writes | **HIGH** | `VL53L1X_api.c:184-197` |
| F-3 | `VL53L1X_SetDistanceMode` keeps only the last of its six writes | **HIGH** | `VL53L1X_api.c:423-436` |
| F-4 | `VL53L1X_SetTimingBudgetInMs` never captures its writes at all | **HIGH** | `VL53L1X_api.c:270-371` |
| F-5 | `SetInterMeasurementInMs` discards its write; also the project's first FP in a task | MED-HIGH | `VL53L1X_api.c:460-471` |
| F-6 | `VL53L1X_GetResult` fills the caller's struct from uninitialised stack on a failed read | **HIGH** | `VL53L1X_api.c:587-601` |
| F-7 | The shim writes a 0xA5 sentinel it never checks | MED | `app_vl53l1_port.c:163` |
| F-8 | A plain NACK costs a 45 ms bus recovery + retry | MED — **FIXED, see §9** | `app_i2c.c:445-447` |
| F-9 | The 7SEMI XSHUT pull-up has never been measured | MED (procedural) | CLAUDE.md §2 `[pending V-W-3]` |
| F-10 | Sensor ID: ST's datasheet contradicts ST's header. 0xEACC is right | doc | `VL53L1X_api.h:197` |
| F-11 | The `ok = 24 + 2×polls` accounting model breaks in Block 2 | LOW | handoff §2.5 |
| F-12 | `VL53L1X_calibration.o` is in the build though the port is out of scope | INFO | `Debug/.../VL53L1X_calibration.o` |

**Two things verified and found CORRECT**, recorded so they are not re-opened:
the bounce-buffer worst case of 17 (§8), and the FPU context configuration (§5).

---

## 1. F-1 — THE DWT→µs CONVERSION STILL DIVIDES BY 600 *(evidence)*

```c
/* app_tasks.c:570-577, inside #ifdef DEBUG_TIMING */
if (dwt_dt_cnt > 0) {
        /* cycles / 600 = µs at 600 MHz (CLAUDE.md §3) */
        tm_printf((UB *)"[HB] dt_us min=%u max=%u mean=%u n=%u\n",
                  dwt_dt_min / 600u, dwt_dt_max / 600u,
                  (dwt_dt_sum / dwt_dt_cnt) / 600u, dwt_dt_cnt);
```

**What `dwt_dt` is.** `app_tasks.c:158-166`: *"dwt_t0 written by inference_task
at D0-set, read by hazard_task at D1-set — strictly ordered by the
result_ready_sem handshake."* The accumulation is at `:229-238`, the first
statement after `tk_wai_sem(result_ready_sem, ...)` returns. **This is the Red
Zone #3 preemption latency** — the number the contest claim rests on.

**Why 600 is wrong.** G-8 closed 2026-08-30: CPUCLK = **800 MHz**, and
`DWT->CYCCNT` counts the processor clock (CLAUDE.md §1 and §3; evidence
`docs/evidence/phase5/PHASE5_T1_CYCCNT_CLOCK_20260830.md`). CLAUDE.md §3 says
in as many words: *"Corrected 2026-08-30 from /600000, which was wrong by
800/600 = 1.333×."* That correction was applied to `cyc_per_ms` at
`app_tasks.c:505-535`. **It was not applied here.** The stale comment at
`app_tasks.c:161` still reads "µs at 600 MHz".

**Why it is not visible today, and why that is the problem.** `DEBUG_TIMING`
is not in the build's define list (`Debug/Core/Src/subdir.mk` — the `-D` set is
`DEBUG`, `STM32N657xx`, `USE_FULL_ASSERT`, … and no `DEBUG_TIMING`). So this
code is not compiled. **It arms the moment Block 9 step 34 turns `DEBUG_TIMING`
on for the PH6-3 preemption re-run under NPU load, 16–17 Sep — after which
there is no bench, ever.** Every `[HB] dt_us` figure captured that day would be
1.333× too large, in the one campaign that cannot be redone.

**It has already produced a wrong number in a locked document.** CLAUDE.md §8
RZ3 records *"DWT cross-check consistent (~4 µs integer-truncated firmware
reading)"*. That 4 came from this code. 3.375 µs at 800 MHz is 2700 cycles;
2700/600 = 4 (integer), 2700/800 = 3. **The corrected reading is 3 µs, and it
agrees with the 3.375 µs logic-analyzer figure better than the 4 did.**

**Fix — and do not just change 600 to 800.** There is already a correct
in-repo pattern for this, in `app_drv2605l.c:97-106`:

```c
UW cpu_hz = (UW)HAL_RCC_GetCpuClockFreq();
drv_pulse_cycles = cpu_hz / 500000u;    /* 2 us */
```

Derive one `dwt_cyc_per_us` from `HAL_RCC_GetCpuClockFreq()` at init (the value
is already logged into `stats.clk_cpu_hz`, `app_i2c.c:261`) and use it at every
conversion site. A hardcoded divisor is what failed here, twice.
**And print raw cycles alongside the µs.** The cycles are the measurement; the
divisor is a derived constant that has now been wrong once in this project.

---

## 2. F-2 — `VL53L1X_SensorInit` DISCARDS THE STATUS OF ALL 91 WRITES *(evidence)*

```c
/* VL53L1X_api.c:178-203 */
for (Addr = 0x2D; Addr <= 0x87; Addr++){
        status |= VL53L1_WrByte(dev, Addr, VL51L1X_DEFAULT_CONFIGURATION[Addr - 0x2D]);
}
status |= VL53L1X_StartRanging(dev);
while (tmp == 0)
{
        status = VL53L1X_CheckForDataReady(dev, &tmp);   /* <-- '=', not '|=' */
        ...
        status = VL53L1_WaitMs(dev, 1);                  /* <-- '=', not '|=' */
}
```

The 91 writes accumulate into `status` with `|=`; line 190 then **overwrites**
it, and line 197 overwrites it again on every loop iteration. `VL53L1X_SensorInit`
can therefore return **0 with an arbitrary number of the 91 configuration
writes having failed**. This is ST's code, on the Block 2 critical path, and it
is the same shape as the five defects this project has already found: the call
returns success and the device is misconfigured.

**Mitigation already exists in the shim — use the other half of it.** The
handoff (B1 step 3) says to call `vl53l1_port_reset_stats()` first. What is
missing is the assertion on the far side:

> **Bring-up rule.** `vl53l1_port_reset_stats()` immediately before
> `VL53L1X_SensorInit()`, and afterwards treat
> `vl53l1_port_stats()->xfer_err != 0` as a **hard failure regardless of the
> returned status**. Print `calls`, `xfer_err`, `last_er`, `last_index`.
> `calls` should be **exactly 91 + the CheckForDataReady/WaitMs traffic** —
> a `calls` count below 91 means the loop did not complete.

---

## 3. F-3 — `VL53L1X_SetDistanceMode` KEEPS ONLY THE LAST OF SIX WRITES *(evidence)*

```c
/* VL53L1X_api.c:422-429, case 1 = short mode */
case 1:
        status = VL53L1_WrByte(dev, PHASECAL_CONFIG__TIMEOUT_MACROP, 0x14);
        status = VL53L1_WrByte(dev, RANGE_CONFIG__VCSEL_PERIOD_A, 0x07);
        status = VL53L1_WrByte(dev, RANGE_CONFIG__VCSEL_PERIOD_B, 0x05);
        status = VL53L1_WrByte(dev, RANGE_CONFIG__VALID_PHASE_HIGH, 0x38);
        status = VL53L1_WrWord(dev, SD_CONFIG__WOI_SD0, 0x0705);
        status = VL53L1_WrWord(dev, SD_CONFIG__INITIAL_PHASE_SD0, 0x0606);
```

Six plain assignments. **Only the last one's status survives.** Identical at
`:431-436` for long mode. This is handoff step B1.3's `SetDistanceMode(1)`, and
the writes that can fail silently include both VCSEL periods — i.e. the
short-mode ranging configuration itself.

**Verification that costs one call:** `VL53L1X_GetDistanceMode` (`:448-458`)
reads `PHASECAL_CONFIG__TIMEOUT_MACROP` and returns 1 if it is 0x14. Call it
after `SetDistanceMode(1)` and require `DM == 1`. Note its own defect: if the
register reads neither 0x14 nor 0x0A it leaves `*DM` **untouched**, so
initialise `DM` to 0 before the call or it reads whatever was on the stack.

---

## 4. F-4 — `VL53L1X_SetTimingBudgetInMs` NEVER CAPTURES ITS WRITES *(evidence)*

```c
/* VL53L1X_api.c:280-284, and the same shape in all 13 cases */
case 15: /* only available in short distance mode */
        VL53L1_WrWord(dev, RANGE_CONFIG__TIMEOUT_MACROP_A_HI, 0x01D);
        VL53L1_WrWord(dev, RANGE_CONFIG__TIMEOUT_MACROP_B_HI, 0x0027);
        break;
```

The return values are not assigned to anything. The status this function
returns describes only the `GetDistanceMode` read at `:275`. Both timing-budget
writes can fail with a returned status of 0.

**Verification:** `VL53L1X_GetTimingBudgetInMs` (`:372-411`) reads
`RANGE_CONFIG__TIMEOUT_MACROP_A_HI` back and maps 0x001D → 15. Require 15.

---

## 5. F-5 — `SetInterMeasurementInMs`: DISCARDED WRITE, AND THE FIRST FLOATING POINT IN A TASK

```c
/* VL53L1X_api.c:460-471 */
status |= VL53L1_RdWord(dev, VL53L1_RESULT__OSC_CALIBRATE_VAL, &ClockPLL);
ClockPLL = ClockPLL & 0x3FF;
VL53L1_WrDWord(dev, VL53L1_SYSTEM__INTERMEASUREMENT_PERIOD,
                (uint32_t)(ClockPLL * InterMeasMs * 1.075));
return status;
```

Two separate problems. *(evidence)*

1. The `WrDWord` return value is discarded. The returned status is the read's.
2. `1.075` is a **double**. This is the first floating-point arithmetic
   executed inside a task in this project.

**The FPU question, VERIFIED rather than assumed — and the answer is that it is
safe.** Two `config.h` files in the tree disagree:

| file | `USE_FPU` |
|---|---|
| `mtk3_bsp2/config/config.h:126` | **1**, with `ALWAYS_FPU_ATR (1)` at `:129` |
| `mtk3_bsp2/mtkernel/config/config.h:143` | **0** |

They have different include guards (`_MTKBSP_TK_CONFIG_` vs `__TK_CONFIG__`),
so nothing prevents both being seen. The build resolves it: the only
config-bearing `-I` in `Debug/mtk3_bsp2/mtkernel/kernel/sysdepend/cpu/core/armv7m/subdir.mk`
is `-I".../mtk3_bsp2/config"`. **`USE_FPU = 1` is the active value.**
`dispatch.S:91-103` and `:143-156` therefore save and restore S16–S31 for
TA_FPU tasks that have actually executed FP instructions, and
`reset_hdl.c:109-113` enables CPACR and sets `FPCCR.ASPEN|LSPEN`.

Two things to carry forward anyway:

- **A landmine for later.** The two `config.h` files disagree on `USE_FPU`,
  `USE_DSP` and more. Anyone who ever adds `mtkernel/config` to the include
  path silently turns FPU context saving **off**. *(inference)*
- **A PH6-3 note.** Once sensor_task executes one FP instruction, `CONTROL.FPCA`
  latches for that task, and every subsequent preemption of it stacks S16–S31.
  That is a small, real addition to the preemption path the Block 9 re-run
  measures. *(inference)*

**Recommendation: do not call this API function.** Write the register directly
and keep the arithmetic integer:

```
1.075 = 43/40 exactly.
ClockPLL <= 0x3FF = 1023 (masked at :466); InterMeasMs = 20.
max intermediate = 1023 * 20 * 43 = 879,780  — no uint32 overflow.
(ClockPLL * InterMeasMs * 43u) / 40u  ==  (uint32_t)(ClockPLL * InterMeasMs * 1.075)
        1023*20 = 20460;  20460*43/40 = 21994  ==  trunc(20460 * 1.075) = 21994
```

That (a) lets the write status be captured, (b) lets **IMP ≥ timing budget** be
enforced — the handoff already notes the API does not check it — and (c) keeps
floating point out of the sensor task entirely.

---

## 6. F-6 — `VL53L1X_GetResult` READS UNINITIALISED STACK WHEN THE TRANSFER FAILS *(evidence → inference)*

```c
/* VL53L1X_api.c:587-601 */
uint8_t Temp[17];
uint8_t RgSt = 255;

status |= VL53L1_ReadMulti(dev, VL53L1_RESULT__RANGE_STATUS, Temp, 17);
RgSt = Temp[0] & 0x1F;                        /* <-- unconditional */
if (RgSt < 24)
        RgSt = status_rtn[RgSt];
pResult->Status = RgSt;
...
pResult->Distance = Temp[13] << 8 | Temp[14]; /* <-- unconditional */
```

`Temp[17]` is an uninitialised stack array. Our shim returns **before**
`memcpy(pdata, port_buf, count)` when the transfer fails
(`app_vl53l1_port.c:165-169`), which is the correct thing for a shim to do — so
`Temp[]` keeps whatever the stack held, and `GetResult` then populates the
caller's struct from it.

**Plan v2 step 12 and handoff B1.4 both say "Gate `d(t)` on `result.Status`".
That is not sufficient, and this is the finding that changes the procedure.**

- `status_rtn[24] = {255,255,255,5,2,4,1,7,3,0,255,255,9,13,255,255,255,255,10,6,255,255,11,12}`
  (`VL53L1X_api.c:154-157`). Exactly one of the 32 possible values of
  `Temp[0] & 0x1F` — the value **9** — maps to status **0**, i.e. a valid
  range. *(evidence)*
- One in 32 sounds rare. It is not, because stack contents are not a fresh draw
  per frame: the failing call reuses the same stack frame at the same depth as
  the previous **successful** call. **The most likely content of `Temp[]` after
  a failed read is the previous frame's 17 bytes** — so a failed read presents
  as the last good measurement, at 50 Hz, indefinitely, with no error anywhere.
  *(inference, high confidence)*

That is defect #2 of this project's five ("a DMA destination buffer pre-filled
with a value indistinguishable from a real reading") reappearing in a new place.

**Required frame-loop policy, replacing "gate on `result.Status`":**

1. Check **`VL53L1X_GetResult`'s return value** first. Non-zero ⇒ drop the
   frame; do not look at `pResult` at all.
2. Then check `result.Status`.
3. Then check that `vl53l1_port_stats()->xfer_err` did not move across the
   call — the belt to the braces, and free.

Do **not** "fix" this by having the shim copy the sentinel out on failure. A
buffer of 0xA5 reads as distance 42405 mm, which is implausible — but it is
still failure wearing the costume of data, and that is the mistake this project
already made once.

---

## 7. F-7 — THE SHIM WRITES A SENTINEL IT NEVER CHECKS *(evidence)*

`app_vl53l1_port.c:159-163` pre-fills `port_buf` with 0xA5 before every read,
with a comment citing the rule that surfaced the GPDMA defect. Nothing then
tests it. CLAUDE.md §3 states the rule in two parts: *"Pre-fill with a sentinel
… **and verify against a known non-zero reset value** rather than against 'the
call returned OK'."* The shim implements the first half.

**Suggested:** a `sentinel_hits` counter in `vl53l1_port_stats_t`, incremented
when a **successful** read of ≥ 2 bytes comes back entirely 0xA5. Do not fail on
it — a single register could legitimately hold 0xA5 — count it and print it. For
the 17-byte `GetResult` read, all-0xA5 is not a possible reading, so any
non-zero count is proof of an untransferred buffer. Cost: one loop, one UW.

---

## 8. VERIFIED CORRECT — THE BOUNCE BUFFER SIZING *(evidence)*

`app_vl53l1_port.c:74-83` justifies `PORT_BUF_SZ = 32` from "the ULD's own worst
case, `VL53L1X_api.c:593` … 17". Checked exhaustively:

```
grep -rn "VL53L1_ReadMulti\|VL53L1_WriteMulti" API/  (excluding headers)
  -> VL53L1X_api.c:593  (the only line in the entire ULD)
```

`VL53L1X_calibration.c` contains **no** multi-byte call sites. The 91-byte
configuration table does go out one `WrByte` at a time (`:184-186`). **The claim
holds, and 17 is the true worst case.** `max_count` in the stats keeps it
observable rather than assumed, which is the right design.

---

## 9. F-8 — A PLAIN NACK COSTS A 45 ms BUS RECOVERY. **THIS IS A DECISION.**

```c
/* app_i2c.c:445-447 */
err = i2c_xfer_once(...);
if (err != E_OK) {
        if (err == E_TMOUT || err == E_IO)
                i2c1_bus_recover();
        err = i2c_xfer_once(...);
}
```

Every HAL error maps to `E_IO` (`app_i2c.c:99`, `HAL_I2C_ErrorCallback`), so an
address NACK — a device simply not being there — triggers a full DeInit,
nine SCL pulses paced by `tk_dly_tsk(1)`, a STOP, a re-init and a retry:
**~145 ms and `recoveries` += 1** per absent address. This is already recorded
as known debt in the gate-test comment block ("COST NOTE … deferred to the
bus-scan work"). *(evidence)*

**Block 2 is when the debt comes due.** With the VL53L1X not yet soldered, the
new L1 probe (§10) costs ~145 ms at boot and pushes `recoveries` off zero — and
`docs/evidence/phase5/check_soak.py` asserts `recov == 0` as one of its
fourteen checks. **A Block 1 fallback soak run today would fail a check for a
reason that is not a fault**, which is exactly the kind of noise that trains a
person to ignore a real one.

The fix is small and every piece of it is verified against the HAL source in
this repo:

- `HAL_I2C_ERROR_AF = 0x00000004U` — `stm32n6xx_hal_i2c.h:166`.
- `HAL_I2C_Mem_Read_DMA` sets `hi2c->ErrorCode = HAL_I2C_ERROR_NONE` at entry
  (`stm32n6xx_hal_i2c.c:3266`), so `ErrorCode` is **per-transaction** and safe
  to read in the error callback.
- `E_NOEXS` (-42, "Object does not exist") is free —
  `mtk3_bsp2/mtkernel/include/tk/errno.h:55`.

In `i2c_complete`'s error path, map `ErrorCode == HAL_I2C_ERROR_AF` exactly
(no other bits set) to `E_NOEXS`; in `i2c_xfer`, skip both recovery and retry
for `E_NOEXS`. An absent device then costs one NACK, `recoveries` stays 0, and
"not present" stops being indistinguishable from "bus wedged".

### 9.1 DECIDED 2026-08-30: Option A — fixed. **Block 1 owes a re-soak.**

Applied, not committed:

- `HAL_I2C_ErrorCallback` now inspects `hi2c->ErrorCode`. **Exact** equality
  with `HAL_I2C_ERROR_AF` (nothing else set) yields `E_NOEXS` and increments a
  new `stats.nacks`; anything with a BERR/ARLO/DMA bit alongside is still
  `E_IO` and still gets the recovery.
- `i2c_xfer` takes neither recovery nor retry for `E_NOEXS`.
- `[NAK] nacks=%u` added to the heartbeat. **`nacks` is a subset of `err`** —
  `err == nacks` means every failure was an absent device.

**Why skipping the recovery is safe, verified in the HAL source rather than
assumed.** `I2C_ITError` sets `hi2c->State = HAL_I2C_STATE_READY` before it
touches either DMA channel (`stm32n6xx_hal_i2c.c:6823 ff`), and where a DMA
abort is required it defers `HAL_I2C_ErrorCallback` to `I2C_DMAAbort`, which
runs after the abort completes — so the handle and both channels are READY by
the time our callback is entered. **And there is a net underneath:** if the
peripheral were somehow left busy, the next `HAL_I2C_Mem_*_DMA` returns
`HAL_BUSY`, `i2c_xfer_once` maps that to `E_IO`, and `E_IO` still takes the
recovery path. A genuinely wedged bus is still recovered — one transfer later.

**One behavioural change to watch.** A NACK no longer gets a retry. Neither
the DRV2605L nor the VL53L1X NACKs when busy (both stretch the clock), so this
should never bite — but if `nacks` starts climbing during ranging **with the
sensor present**, that is a new and real signal, not noise.

### 9.3 CONFIRMED ON HARDWARE 2026-08-30 ~20:54 IST *(evidence)*

Flashed and running. Five consecutive heartbeats, verbatim from picocom:

```
[I2C] init=0 gate=0 whoami=0x140e0 wr=0 wrseen=0x27 addr=0x5a ok=2464 err=1 tmo=0 recov=0 ...
[NAK] nacks=1
[CLK] cpu=800000000 sysb=400000000 pclk1=200000000
[TOF] res=-42 step=1 id=0xa5 blk=0x0 ctl=0x1ff
[DRV] init=0 id=7 mode=0x1 lib=0x2 seq=0x1 odc=0x8b sts=0xe0 armed=1
[TRG] pulses=8 suppressed=72 cyc=1600 rst=1 rstmode=0x40
[HLT] polls=10 faults=0x0 cfglost=0
[EFF] n=5 last=58613 min=58568 max=58644 late=0 stuck=0
[DWT] ok=1 cyc=776880118 cyc_per_ms=800087
[HB] up_ms=12903 frames=586 inf=586 hazard=80 canary_err=0 q=0 qovr=0
```

**`err=1 tmo=0 recov=0` with `nacks=1` is the whole result.** Before this
change the same absent device produced `recov=1` as well, because `E_IO` took
the DeInit / nine-pulse / re-init path and then retried. The classification
works on real silicon: **`err == nacks`, so every failure was an absent device
and there were no bus faults.**

Four independent things the same lines confirm:

1. **The sentinel discipline discriminates.** `id=0xa5` is the pre-fill,
   untouched — "nothing ACKed". Had the part ACKed and the DMA moved nothing,
   this would read `0xa5` too, but `res` would be `0`-with-wrong-byte rather
   than `-42`. The two failure modes stay separable, which is the entire point
   of CLAUDE.md §3's rule.
2. **The probe stopped at step 1 as designed** — `blk=0x0` and `ctl=0x1ff` are
   the initialised "did not run" values, not readings. One transfer spent, not
   four.
3. **`res=-42` is `E_NOEXS`** (`tk/errno.h:92`), not `E_IO` (`-57`). If the
   VL53L1X had ACKed and *then* failed, this would read `-57` — a different and
   much more interesting problem.
4. **H-D2's discipline is visibly working.** `whoami=0x140e0` unpacks as
   LIBRARY 0x01 / MODE 0x40 / STATUS 0xE0 — the SLOS854D Table 3 reset values,
   read by the gate test *before* `drv2605l_init()` writes anything. So the
   part had genuinely lost its configuration to a power cycle, and the
   `[DRV] mode=0x1 lib=0x2` that follows is **this boot's** writes, not a
   readback passing on a previous boot's state.

Everything else is unchanged and nominal: `faults=0x0`, `cfglost=0`,
`canary_err=0`, `q=0 qovr=0`, `frames == inf` on every line, `rst=1
rstmode=0x40`, `cyc_per_ms` 799999–800087, and `pulses + suppressed == hazard`
exactly (8 + 72 = 80). `[EFF] 58568–58644 µs` is a **fourth** independent boot
inside the established band; combined 58505–58760, spread 0.44%, so the R-3
floor of 75 ms remains 1.276× the worst maximum.

**This is five heartbeats, ~17 s. It is not the re-soak.** §9.1 owes a
10-minute run because the change sits in the path every DRV2605L transfer
takes, and five heartbeats cannot see an intermittent.

### 9.2 `check_soak.py` updated — 14 checks became 15, and none was loosened

The old single `I2C err` check would now fail on an absent VL53L1X, which is a
true statement told badly. It is split:

- **`I2C bus faults (err - nacks)`** must be 0 — this is the old check's real
  intent.
- **`I2C nacks`** must be 0, and when it is not, the detail line says that 1 is
  expected while the VL53L1X is unsoldered and is not a bus fault.
- **`VL53L1X L1 probe`** — parses the `[TOF]` line and requires
  `res=0 step=0 id=0xeacc blk=0xeacc10` with `ctl != 0xea`. When there is no
  `[TOF]` line at all it reports **N/A explicitly** rather than skipping
  silently.

Backward compatible: a log with no `[NAK]` line is treated as `nacks = 0`,
which reproduces the original check exactly. Re-run against
`soak_10min_20260830.log` — still **PASS, 15/15**.

**Expected log while the VL53L1X is not yet soldered:**

```
[I2C] ... ok=N err=1 tmo=0 recov=0
[NAK] nacks=1
[TOF] res=-42 step=1 id=0xa5 blk=0x0 ctl=0x1ff
```

`err == nacks == 1`, `recov == 0`: one absent device, no bus fault. The soak
verdict will show exactly one FAIL, on the L1 probe, naming the cause.

---

## 10. F-9 — MEASURE THE 7SEMI XSHUT PULL-UP BEFORE SOLDERING *(evidence: it is untested)*

CLAUDE.md §2 says XSHUT should be left **unconnected** because of an "on-board
pull-up to the sensor's own VDD", and tags the whole item **`[pending V-W-3]`**.
That pull-up has never been measured. If the 7SEMI does not fit one, XSHUT
floats, the part sits in reset, nothing ACKs at 0x29 — and the failure looks
exactly like a bad solder joint, which is where the next hour would go.

**Sixty seconds with the DMM, board disconnected, before the iron is hot.** Add
two rows to the table CLAUDE.md §2 already has:

| measure | expect | closes |
|---|---|---|
| XSHUT ↔ VIN, and XSHUT ↔ the 2.8 V LDO output | one of them a few kΩ or less | V-W-3 |
| GPIO1 ↔ VIN, and GPIO1 ↔ the 2.8 V LDO output | tells you PD0's actual input level instead of assuming it | the "may be 2.8 V logic" note in §2 |

Two consequences worth writing down while the board is still in hand:

- **XSHUT unconnected means the only VL53L1X reset is a power cycle.** If
  `SensorInit` ever leaves the part wedged, recovery is unplugging the board.
  That is consistent with plan v2 §0.3 (the judge cold-boots it) — noting it so
  nobody is surprised at 1 a.m.
- **GPIO1 → PD0 is not needed for Block 2.** The frame loop the handoff
  specifies uses `CheckForDataReady`, i.e. polling. Wire GPIO1 anyway, because
  solder time is now and the mechanical freeze is Block 5 — but do not let it
  gate the probe.

---

## 11. F-10 — THE SENSOR ID: ST'S DATASHEET CONTRADICTS ST'S HEADER. 0xEACC IS RIGHT. *(evidence)*

The handoff carries this as an open contradiction. It is now settled, from the
local datasheet copy.

`docs/datasheets/vl53l1x_datasheet.pdf` — **ST DS12385 Rev 8, §4.2 "I²C
interface - reference registers", Table 8**, verbatim:

| Register name | Index | After fresh reset, without the driver loaded |
|---|---|---|
| Model ID | **0x010F** | **0xEA** |
| Module type | 0x0110 | 0xCC |
| Mask revision | 0x0111 | 0x10 |

ST introduces the table with *"The registers shown in the table below can be
used to validate the user I²C interface"*, and the note under it reads
*"Multibyte read/writes are always addressed in ascending order with the MSB
first."*

`VL53L1X_GetSensorId` (`VL53L1X_api.c:497-505`) does `VL53L1_RdWord(dev, 0x010F)`,
and our shim assembles `b[0] << 8 | b[1]` (`app_vl53l1_port.c:221`), so the ULD
path yields **0xEACC**. The *"sensor Id must be 0xEEAC"* at `VL53L1X_api.h:197`
is a typo in ST's own header, contradicted by ST's own datasheet.

**Keep the handoff's rule — log, don't hard-fail — but the expected value is
0xEACC, and it is now cited rather than remembered.** Only 0x0000, 0xFFFF or a
no-ACK is real failure.

---

## 12. F-11 / F-12 — TWO SMALL ONES

**F-11. The `ok` accounting model breaks in Block 2.** Handoff §2.5 records
`ok = 24 + 2×polls`. `VL53L1X_CheckForDataReady` calls `GetInterruptPolarity`
first (`VL53L1X_api.c:258-259`), so it is **two** reads, not one. A ranging frame
is 2 reads (CheckForDataReady) + 1 read (`GetResult`, 17 bytes) + 1 write
(`ClearInterrupt`) = 4 transfers, plus another CheckForDataReady each time data
is not ready. `ok` will climb by roughly 200/s once ranging starts. Update the
model in the handoff before someone reads a healthy counter as a stall.

**F-12. `VL53L1X_calibration.o` is in the build** (`Debug/.../VL53L1X_calibration.o`)
although plan v2 §4 lists the calibration port as explicitly out of scope. It
contains no multi-byte platform calls (§8), so it does not widen the bounce
buffer's worst case and is harmless. Noted only so the `PORT_BUF_SZ` justification
stays true if the file is ever changed.

---

## 13. THE CODE CHANGE THAT WAS MADE — THE L1 RAW PROBE

`app_i2c_tof_probe()` in `app_i2c.c`, five new fields in `app_i2c_stats_t`,
one call site at the end of the sensor-task init block, one `[TOF]` heartbeat
line. **Not committed** — review first, and see the §9 decision.

It runs on the L1 primitive **directly**: no shim, no ULD. If it fails, the
suspect list is the joints, the bus, and the 16-bit index path, and nothing else.

| step | transfer | expect | proves |
|---|---|---|---|
| 1 | `i2c_rd(0x29, 0x010F, I2C_REG16, buf, 1)` | `0xEA` | a 16-bit-addressed read completes and lands a byte |
| 2 | `i2c_rd(0x29, 0x0110, I2C_REG16, buf, 1)` | `0xCC` | the **low** index byte reached the wire — the two indices differ only there — and the bus is not stuck |
| 3 | `i2c_rd(0x29, 0x010F, I2C_REG16, buf, 3)` | `EA CC 10` | first multi-byte DMA receive; device auto-increment; the exact shape the 17-byte `GetResult` uses every frame |
| 4 | `i2c_rd(0x29, 0x010F, **I2C_REG8**, buf, 1)` | **≠ 0xEA** | negative control — see below |

**Step 4 is the one that matters, and it closes the risk register's
"`I2C_REG16` silently wrong" item in firmware rather than on a logic analyzer.**
The mechanism is not assumed; it is in the HAL source in this repo
(`STM32Cube_FW_N6/.../Src/stm32n6xx_hal_i2c.c`, `HAL_I2C_Mem_Read_DMA`,
lines 3285-3300):

```c
if (MemAddSize == I2C_MEMADD_SIZE_8BIT) {
        hi2c->Instance->TXDR = I2C_MEM_ADD_LSB(MemAddress);   /* ONLY the low byte */
} else {
        hi2c->Instance->TXDR = I2C_MEM_ADD_MSB(MemAddress);   /* LSB follows via IRQ */
}
```

An 8-bit-sized transfer of index 0x010F puts **0x0F** on the wire, the sensor
ACKs it like any register-mapped slave, and returns whatever lives at 0x000F.
If step 4 returns 0xEA as well, then the two branches are indistinguishable on
this bus and steps 1-3 proved nothing about address width. That failure is
invisible to every other test in the plan, including the L5 capture, which
shows what a **correct** call emits and not what an incorrect one would.

Note in passing: the same HAL lines are the citation for what the L5 capture
must show on the wire — `0x52 idxMSB idxLSB data` — so L5 now has a source-level
prediction to be checked against, not just an expectation.

**Failure modes are distinguishable by value, not by a flag.** Every read is
preceded by the `GATE_SENTINEL` (0xA5) pre-fill:

```
[TOF] res=0 step=0 id=0xeacc blk=0xeacc10 ctl=0x??   <- PASS (ctl must not be 0xea)
      id/blk = 0xa5a5..   the DMA never wrote; the transfer "succeeded" untransferred
      id/blk = 0xffff..   nothing driving the data phase (absent, or held in reset)
      id/blk = 0x0000..   the device really drove zeros
      res=-57 step=1      no ACK at 0x29 — joints, XSHUT (F-9), or the wrong bus
```

The probe **stops at the first failing step**, so an absent sensor costs one
transfer rather than four (see the §9 decision for why that still is not free).
Placed **last** in the init block, after `drv2605l_init()`, deliberately:
Block 1 is the fallback demo and is brought up before anything new can disturb
the bus.

Syntax-checked in isolation with `gcc -std=c99 -Wall -Wextra -Wconversion`
against stub typedefs — clean. **Not yet compiled for the target; CubeIDE is on
the developer's machine.** Per CLAUDE.md §9: rebuild and confirm the
`-Trusted.bin` timestamp is newer than the commit before flashing.

---

## 14. REVISED B1 ORDER

1. **DMM on the 7SEMI, before the iron** — XSHUT and GPIO1 pull-up destinations
   (§10). Two rows into CLAUDE.md §2.
2. Decide §9 (Option A or B). If A: apply, rebuild, **re-run the Block 1
   10-minute soak** while the bench is still one device.
3. Solder the 7SEMI. XSHUT unconnected. GPIO1 → PD0 (CN11 pin 3), input, no
   internal pull.
4. **L1 raw probe.** Pass is `res=0 step=0 id=0xeacc blk=0xeacc10` with
   `ctl != 0xea`.
5. Only then the shim path — with F-2/F-3/F-4/F-5 readbacks wired in as gates,
   not as prints: `xfer_err == 0` across `SensorInit`; `GetDistanceMode == 1`;
   `GetTimingBudgetInMs == 15`; IMP ≥ budget enforced in our code.
6. Frame loop with the F-6 policy: **`GetResult` return value first**, then
   `result.Status`, then `xfer_err` unchanged.
7. L5 logic-analyzer capture of one `WrByte` → `0x52 idxMSB idxLSB data`.
   Archive the `.sr`.

**Separately and not on this path: fix F-1 before anything turns on
`DEBUG_TIMING`.** It is a five-line change and it is the only one of these
twelve findings that can silently corrupt the contest's headline number.
