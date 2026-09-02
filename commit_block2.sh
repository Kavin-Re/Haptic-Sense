#!/usr/bin/env bash
# Block 2 commit split. REWRITTEN 2026-09-02 to cover everything now in the tree.
#   bash commit_block2.sh          # dry run
#   bash commit_block2.sh --go     # commit
#
# WHAT IS ON THE BOARD: the 08-31 19:56 build = the write-retry build.
# The 0x2E/0x2F pad fix and the own-config-loop are NEWER THAN THE BINARY
# and have never been compiled. Refresh (F5) + Build before flashing.
set -e
GO=0; [ "$1" = "--go" ] && GO=1
run(){ if [ $GO -eq 1 ]; then "$@"; else echo "  would: $*"; fi }
# NOTE 2026-09-03: this was `eval "$@"`, which re-parsed the argument list and
# split '...Appli Debug.launch' into two pathspecs. Direct execution preserves
# the quoting. eval was never needed -- run() only ever calls `git add`.
msg(){ if [ $GO -eq 1 ]; then git commit -q -F -; else cat >/dev/null; echo "  would commit"; fi }

echo "== 1/4  L1 primitive: NACK != bus fault, + the L1 raw probe  -- ALREADY COMMITTED =="
echo "  done 2026-09-03 as afca4ce. Skipped: re-committing with nothing staged\n        returns non-zero and set -e would abort the run."

echo "== 2/4  VL53L1X shim + L2 driver  (NEWER THAN THE FLASHED BINARY) =="
run git add firmware/Appli/Core/Src/app_vl53l1_port.c firmware/Appli/Core/Inc/app_vl53l1_port.h \
            firmware/Appli/Core/Src/app_vl53l1x.c firmware/Appli/Core/Inc/app_vl53l1x.h \
            firmware/Appli/.cproject \
            'firmware/Appli/STM32N6_MTK_Person_Detection_Appli Debug.launch'
msg <<'MSG'
feat(phase5): VL53L1X L2 driver -- every ULD silent failure turned into a gate

NOT YET COMPILED FOR THE TARGET. Syntax-checked only (gcc -Wall -Wextra
against stub typedefs). Refresh the CubeIDE project (F5) then Build.

ST's ULD discards write status in four places and reads uninitialised
stack in a fifth (docs/BLOCK2_PREFLIGHT_20260830.md):
  F-2 SensorInit overwrites the accumulated status of its 91 writes at
      VL53L1X_api.c:190 and :197 -- replaced entirely by our own config
      loop over VL51L1X_DEFAULT_CONFIGURATION[] (:60, non-static), every
      write checked, the first refusal naming its register.
  F-3 SetDistanceMode keeps only the last of six writes -- gated by a
      GetDistanceMode readback, DM pre-set so an untouched *DM is caught.
  F-4 SetTimingBudgetInMs never captures its writes -- gated by readback.
  F-5 SetInterMeasurementInMs discards its WrDWord, skips the IMP >=
      budget check and uses a double literal -- replaced by an integer
      write (1.075 == 43/40 exactly, max intermediate 879,780).
  F-6 GetResult uses Temp[0] and Temp[13..14] from uninitialised stack --
      the frame loop checks transfer status, then xfer_err delta, then
      result.Status, in that order.

Two entries in ST's default table are WRONG FOR THIS BOARD. Verbatim,
VL53L1X_api.c:62-63: 0x2e and 0x2f are "bit 0 if I2C/GPIO pulled up at
1.8V, else set bit 0 to 1 (pull up at AVDD)" and ST ships both as 0x00.
This board's pull-ups measure 2 x 9.9 kOhm to VIN and VIN is the 3.32 V
rail, so bit 0 must be 1 in both. Patched in our config loop.

Bounded write AND read retry in the shim, counted on [RTY], never silent:
the part refuses its own address for a measured 12.8 ms mid-config.

Boot gate bounded and paced (50 x 1 ms) -- the MPU6050 audit's M-2 rule.
StartRanging confirmed by reading SYSTEM__MODE_START back as 0x40.
Gated on the raw L1 probe, so one binary works before and after soldering.
MSG

echo "== 3/4  DWT divisor, [PRT]/[RTY]/[RNG] instrumentation, CLAUDE.md =="
run git add firmware/Appli/Core/Src/app_tasks.c CLAUDE.md
msg <<'MSG'
fix(timing): DWT cycles->us divisor was still 600 on an 800 MHz CPUCLK

app_tasks.c printed [HB] dt_us as dwt_dt / 600u. dwt_dt IS the Red Zone #3
preemption latency -- dwt_t0 set by inference_task at D0, read by
hazard_task at D1, ordered by the result_ready_sem handshake.

G-8 closed 2026-08-30: CPUCLK is 800 MHz and DWT->CYCCNT counts CPUCLK.
That correction reached cyc_per_ms and MISSED here, so every figure this
line printed was 1.333x too large. Dead today (DEBUG_TIMING undefined) --
which is the danger: Block 9 step 34 turns it on for the PH6-3 re-run on
16-17 Sep and there is no bench afterwards.

The divisor now derives from HAL_RCC_GetCpuClockFreq() at print time, as
app_drv2605l.c:101-103 already does, with a zero guard. Raw cycles print
alongside: a log carrying cycles can be re-derived, one carrying only
microseconds cannot.

Adds [PRT] (shim counters incl. last_er/last_index), [RTY] (write and read
retries) and [RNG] (L2 bring-up and frame loop) heartbeat lines.

CLAUDE.md gains a VL53L1X Verification Ledger: V-W-3, V-W-1 and H-D5
closed; the GPIO1 pull-up correction; the measured pull-up budget; and
V-5/H1 CLOSED -- 16-bit addressing proven on the wire.

STILL OWED BY HAND: CLAUDE.md section 8 RZ3 records "~4 us" for the DWT
cross-check. The correct figure is 3 us (2700 cycles / 800).
MSG

echo "== 4/4  Evidence and documents =="
run git add docs/ commit_block2.sh .gitignore
msg <<'MSG'
evidence: Block 2 -- 16-bit addressing proven, and the root cause found

Four logic-analyzer captures, a decoder, and the investigation end to end.

decode_sr_i2c.py decodes I2C straight out of a sigrok .sr without
PulseView: address/NACK census, which register indices were written and
which are missing, NACK bursts and spans, real STOP->START gaps, SCL
low-pulse widths.

V-5/H1 CLOSED. On the wire: 52 01 0F -> 53 EA, 52 01 10 -> 53 CC,
52 01 0F -> 53 EA CC 10, plus the REG8 control 52 0F -> 53 00. That is
the L5 deliverable. Bus timing clean: zero STOP->START gaps below the
1.3 us t(BUF) minimum across 4,609 measured gaps.

PHASE5_SENSORINIT_NACK: the part refuses its own ADDRESS for 12.8 ms
+/- 0.1 ms after 30-31 config writes, identical across three boots, then
serves 4,236 reads without a failure.

PHASE5_VIN_BROWNOUT: root cause. VIN at the 7SEMI measures 2.04 V against
the DS12385 AVDD minimum of 2.6 V -- 560 mV under, never once in spec.
The die is phantom-powered through its own 2 x 9.9 kOhm I2C pull-ups
(255 uA at the measured voltage), and both mystery timings are that RC:
12.8 ms / 4.95 kOhm = 2.6 uF of carrier decoupling, and the ~8.7 ms
collapse implies 359 uA of load. Not a device state machine. A capacitor.

Also: the four-day plan, the pre-flight audit (twelve findings), and
HANDOFF_20260903 with the fix, the pass criteria and the procurement list.
MSG

echo
echo "Done. Nothing above is on the board yet except commit 1."
echo "Next: fix VIN (>= 3.2 V at the 7SEMI pin), then Refresh(F5) + Build + flash."
