# PHASE 5 T1 — WHICH CLOCK FEEDS `DWT->CYCCNT`
**2026-08-30, ~03:24 IST. G-8 CLOSED with hardware evidence.**
Firmware: commit `0bbde4e`. Board: STM32N6570-DK, Flash Boot, no host beyond the VCP.

**Answer: CPUCLK = 800 MHz. `DWT->CYCCNT` counts it. `cycles / 800000 = ms`.**
Neither candidate offered in `HANDOFF_20260830_BLOCK1B.md` §3 T1 (600000 or 400000) was right.

---

## 1. Raw capture

Five consecutive heartbeats, `picocom /dev/ttyACM0 115200`, verbatim:

```
[HB] up_ms=8558  frames=393 inf=393 hazard=40 canary_err=0 q=0 qovr=0
[I2C] init=0 gate=0 whoami=0x140e0 wr=0 wrseen=0x27 addr=0x5a ok=22 err=0 tmo=0 recov=0 pclk1=200000000 sysclk=400000000
[CLK] cpu=800000000 sysb=400000000 pclk1=200000000
[DRV] init=0 id=7 mode=0x1 lib=0x2 seq=0x1 odc=0x8b sts=0xe0 armed=1
[DWT] ok=1 cyc=2421324800 cyc_per_ms=800000
[HB] up_ms=9590  frames=443 inf=443 hazard=43 ...
[DWT] ok=1 cyc=3246924216 cyc_per_ms=799999
[HB] up_ms=10622 frames=492 inf=492 hazard=80 ...
[DWT] ok=1 cyc=4072572996 cyc_per_ms=800047
[HB] up_ms=11654 frames=541 inf=541 hazard=80 ...
[DWT] ok=1 cyc=603205744  cyc_per_ms=800000
[HB] up_ms=12686 frames=590 inf=590 hazard=80 ...
[DWT] ok=1 cyc=1428805744 cyc_per_ms=800000
```

## 2. Two independent methods agree

**Method A — the RCC tree, reported by the HAL.** `[CLK] cpu=800000000`. This is
`HAL_RCC_GetCpuClockFreq()` (IC1, `stm32n6xx_hal_rcc.c:1351`), added in this build precisely
because the project had only ever logged `HAL_RCC_GetSysClockFreq()` (IC2, `:1440`) and been
comparing a bus clock against a CPU figure. It matches the arithmetic derived from `main.c`
before the board was powered: PLL1 = HSI 64 MHz / M 2 × N 25 / P1 1 / P2 1 = 800 MHz;
CPUCLK = IC1 ÷ 1 = 800 MHz; sysb_ck = IC2 ÷ 2 = 400 MHz; HCLK = ÷2 = 200 MHz = PCLK1.
All three printed values match the prediction exactly.

**Method B — counting cycles against the kernel clock.** `cyc_per_ms`, computed in firmware
from `(CYCCNT delta) / (up_ms delta)`. Every printed value reproduces exactly from the
previous line, including across a 32-bit wrap:

| interval | Δcyc | Δms | computed | printed | |
|---|---|---|---|---|---|
| 8558→9590 | 825,599,416 | 1032 | 799,999 | 799999 | match |
| 9590→10622 | 825,648,780 | 1032 | 800,047 | 800047 | match |
| 10622→11654 | 825,600,044 | 1032 | 800,000 | 800000 | match, **CYCCNT wrapped** |
| 11654→12686 | 825,600,000 | 1032 | 800,000 | 800000 | match |

Spread is 799,999–800,047, i.e. **800.00 MHz ± 60 ppm**, which is the ±1 ms quantisation of
`tim.lo` and not a clock instability. The two methods agree to within that.

**The wrap at 10622→11654 is worth keeping.** `cyc` fell from 4,072,572,996 to 603,205,744 and
the `UW` subtraction still produced the correct 800,000. That exercises, on hardware, the
wrap-safety asserted in the code comment. CYCCNT wraps every 2^32 / 800e6 = **5.37 s**;
`HEARTBEAT_PERIOD_MS` is 1000. **Raising the heartbeat period above ~5 s silently turns this
number into garbage.**

## 3. Consequences

- **`sysclk=400000000` was never a symptom.** It is IC2 and it is correct. G-8's premise —
  that 400 vs 600 was a discrepancy needing explanation — was itself the error.
- **CLAUDE.md §1 "600 MHz" was wrong** → corrected to 800 MHz.
- **CLAUDE.md §3 `/600000` was wrong by 800/600 = 1.333×** → corrected to `/800000`. Every
  DWT-derived figure recorded before 2026-08-30 reads **1.333× too large**.
- **RZ3's "(2,025 cycles)"** was 3.375 µs × 600 MHz. At 800 MHz it is **2,700 cycles**.
  **The 3.375 µs itself is a logic-analyzer measurement and is unaffected**, so the locked
  contest claim stands unchanged.
- **The Phase 4 DWT cross-check now agrees with the LA where it previously did not.** RZ3
  recorded a firmware reading of "~4 µs (integer-truncated)" against an LA worst case of
  3.375 µs, and the 0.6 µs gap was attributed to truncation. `cycles/600 ≈ 4` means 2400–2999
  raw cycles; at 800 MHz that is 3.00–3.75 µs, which brackets 3.375 µs. The existing data had
  been pointing at 800 MHz all along.
- **`[DWT] ok=1`** — the counter is implemented and the enable was accepted under TZEN. The
  secure-non-invasive-debug failure mode did not materialise. T2 is unblocked.

## 4. Also confirmed in the same capture

- **`ok=22`** — exactly the count predicted after the OD_CLAMP readback was added
  (9 gate-test + 1 STATUS + 7 config + 1 arm + 4 readbacks). No transfer is missing.
- **`odc=0x8b`** — the OD_CLAMP write does land. The defect fixed in `0bbde4e` was a missing
  *check*, not a broken write; the check is now real. `err=0 tmo=0 recov=0`.
- **`sts=0xe0`** — DEVICE_ID 7, and `DIAG_RESULT`/`OVER_TEMP`/`OC_DETECT` all clear.
  **This is a snapshot taken once in `drv2605l_init()`, not a live read**, and it will print
  0xe0 for ever regardless of what happens to the part. The runtime poll is still owed.
- **`canary_err=0`, `frames == inf` lockstep, `q=0 qovr=0`** across all five heartbeats —
  the RZ4 paired-semaphore handshake is still clean with the new code in the loop.
- **Frame rate**: 50 frames per 1032 ms = **48.4 Hz**, consistent with the Phase 4 ~47.6 Hz.

## 5. `hazard=` is not stalled — the sawtooth is slower than the log

Worth recording because it looks alarming: hazard went 40, 43, 80, 80, 80 and then stopped
moving. It is correct. With `SYN_D_FAR_MM 2000`, `SYN_D_NEAR_MM 400`, `SYN_STEP_MM 10`, the
synthetic generator is a 320-frame triangle (160 approach + 160 retreat ≈ **6.6 s**), and
`hazard = d < 800 && v > 20` is true only while approaching below 800 mm — the last
**40 frames** of each approach. Modelling it frame by frame reproduces the board:

| frames | model | board |
|---|---|---|
| 393 | 40 | 40 |
| 443 | 44 | 43 |
| 492 | 80 | 80 |
| 541 | 80 | 80 |
| 590 | 80 | 80 |

The single-count difference at 443 is heartbeat sampling skew — `stat_frames` and
`stat_hazard_events` are read at slightly different instants by a TK_PRI 10 task. The three
flat readings are the retreat half; the next burst is due around up_ms ≈ 16.1 s, past the end
of the capture.

**This matters for T2 far more than it matters for T1.** The synthetic pattern delivers
**40 consecutive hazard frames ~20.7 ms apart**, and effect-1 is predicted at 45–75 ms
(SLOS854D Table 1, Library B). One trigger per hazard frame would put edges 2 through 40
inside the previous playback window, and SLOS854D §8.6.2 Table 5 says a second rising edge
before GO clears **cancels** the waveform. Ungated, this pattern produces one partial buzz out
of forty triggers — which at the bench looks exactly like a dead motor or a bad driver. The
R-3 rate limit is not a refinement; it is the difference between the demo working and not.
