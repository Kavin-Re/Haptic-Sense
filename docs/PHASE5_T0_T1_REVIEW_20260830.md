# PHASE 5 — BLOCK 1a REVIEW + T1 CLOCK ANALYSIS
**2026-08-30. Adversarial pass over Block 1a before its first build, plus T1.**
Code: commit `0bbde4e`. Predecessor: `d9e4a8e` (Block 1a, written and committed, never built).

Labelled per CLAUDE.md §9: **evidence** (measured or read in a source), **inference**
(follows from evidence), **speculation** (plausible, unverified).

---

## 0. BRANCH STATE — the handoff was stale

`phase5/i2c-first-light` and `master` were already at `9e1b4c4`; the fast-forward merge
the handoff §1 describes as pending had already happened. Both branches now sit on
`0bbde4e`. Nothing was lost, but the handoff's "7 commits ahead of master" is no longer
true and should not be trusted next session. *(evidence: `git branch -vv`)*

`.git/_stale_locks/` did **not** exist and has been recreated. The bridge to this machine
cannot unlink files, so every git operation leaves `index.lock`, `HEAD.lock` and
`tmp_obj_*` behind. They are parked in `.git/_stale_locks/` and are safe to `rm -rf`
from a normal shell. `git fsck` reports only dangling blobs. *(evidence)*

---

## 1. DEFECTS FOUND IN BLOCK 1a

### D-1. `drv2605l_init()` verified the one register that proves nothing. *(evidence)*

The block comment above `drv_cfg[]` is correct and explicit:

> Only three registers actually change from reset, and those are the ones
> `drv2605l_init()` verifies: `0x17 OD_CLAMP 0x8C -> 0x8B`, `0x03 LIBRARY 0x01 -> 0x02`,
> `0x01 MODE 0x40 -> 0x01`

The code did not do that. The pass condition read **MODE, LIBRARY and WAVSEQ1 (0x04)** and
never read OD_CLAMP at all. WAVSEQ1's expected value `0x01` **is its reset value**
(SLOS854D Table 3, Register Map Overview, `docs/datasheets/drv2605l_datasheet.pdf`), so its
readback cannot distinguish "my write landed" from "the part reset" — which is exactly what
the comment two lines above says. **The code disagreed with its own comment, and the comment
was right.**

Worse, the WAVSEQ1 test was **sentinel-blind**. `(seq_rb & 0x7F) == 0` is false for:

| buffer content | `& 0x7F` | verdict |
|---|---|---|
| `0xA5` DRV_SENTINEL — DMA never wrote | `0x25` | **passes** |
| `0x01` reset value — write never landed | `0x01` | **passes** |
| `0x01` genuine correct value | `0x01` | passes |

Every failure mode it existed to catch passed it. The other two checks (MODE, LIBRARY) do
discriminate, so a *total* write failure was still caught — but a *single-register* OD_CLAMP
failure was invisible, and OD_CLAMP is the register that sets the full-scale drive voltage.

**Fixed:** pass condition is now MODE, LIBRARY and `0x17 == 0x8B`. `0x8B` is neither the
`0x8C` reset value nor the `0xA5` sentinel, so it discriminates all three states. `seq_rb`
is still read and printed, as observability only, and is no longer load-bearing.

**Rule this generalises to, worth adding to CLAUDE.md §3 next to the sentinel rule:**
*a readback may only appear in a pass condition if its expected value differs from BOTH the
register's reset value AND the pre-fill sentinel.* Otherwise it is decoration.

### D-2. STATUS fault bits were read three times a boot and thrown away. *(evidence)*

SLOS854D Table 4 (§8.6.1), verbatim on all three:

- bit 3 `DIAG_RESULT` — "The flag clears upon read."
- bit 1 `OVER_TEMP` — "This bit clears upon read."
- bit 0 `OC_DETECT` — "This bit clears upon read." Set when "the load impedance is below the
  load-impedance threshold, the device shuts down and periodically attempts to restart".

Register `0x00` is read three times at boot — once in the `app_i2c_gate_test()` probe, once
in its `drv_regs[]` sweep, once at the top of `drv2605l_init()` — and **every one of those
reads discarded bits 4:0**, keeping only `v >> 5`. Because the flags clear on read, each read
also consumes them for the next.

Consequence, and it is the requested failure class: **an overcurrent or overtemperature event
on the DRV2605L can never be reported by this firmware.** It presents as a weak or silent
motor with `armed=1`, `ok=` climbing normally, and no error anywhere — indistinguishable from
a wiring fault, a wrong library, or a dud ERM. This matters from T3 onward: `OC_DETECT` is the
device's own verdict on the coil, and V-W-6's multimeter reading is not a substitute for it.

**Fixed (partly):** the full STATUS byte is now captured into `dstats.status_rb` and printed
as `sts=`. **Still owed before the motor is connected (T3):** a periodic P3 re-read of `0x00`
alongside the `0x01` re-read the plan already specifies (Block 1 step 5), reporting
`OC_DETECT`/`OVER_TEMP` on the heartbeat. The init-time byte is weak evidence on its own
because the two earlier gate-test reads have already cleared anything latched.

### D-3. `dwt_spin_cycles()` does not exist. *(evidence)*

Handoff §3 T2 reads as though the function exists but is inert for want of a running counter.
`grep -rn dwt_spin` over `Core/Src` and `Core/Inc` returns nothing. T2 has to write it, not
just enable it. Scope, not risk — but it is one more unbuilt function.

---

## 2. T1 — WHICH CLOCK FEEDS `DWT->CYCCNT`

### 2.1 The handoff offered two answers and both are wrong. *(inference, high confidence)*

Handoff §3 T1 step 4: "600000 ⇒ CYCCNT runs at 600 MHz … 400000 ⇒ it runs off the 400 MHz
bus clock". **The predicted answer is 800000.**

The N6 has independent internal-clock (IC) dividers per domain, and the two HAL getters read
two different trees:

| HAL function | source | file |
|---|---|---|
| `HAL_RCC_GetCpuClockFreq()` | **IC1 → CPUCLK** | `stm32n6xx_hal_rcc.c:1351` |
| `HAL_RCC_GetSysClockFreq()` | **IC2 → sysb_ck** | `stm32n6xx_hal_rcc.c:1440` |
| `HAL_RCC_GetNPUClockFreq()` | IC6 → sysc_ck | `stm32n6xx_hal_rcc.c:1477` |

The firmware logged only `HAL_RCC_GetSysClockFreq()`, labelled it `sysclk`, and the project
then compared its 400 MHz against a CPU figure in CLAUDE.md §1. **Those are two different
clock domains, and the comparison was never meaningful.**

### 2.2 The arithmetic, from `main.c` *(evidence → inference)*

`HSI_VALUE = 64000000` (`Appli/Core/Inc/stm32n6xx_hal_conf.h:130`).
PLL1, `main.c:206-212`: source HSI, `PLLM 2`, `PLLN 25`, `PLLFractional 0`, `PLLP1 1`, `PLLP2 1`.

```
PLL1 = HSI / PLLM x PLLN / PLLP1 / PLLP2
     = 64 MHz / 2 x 25 / 1 / 1
     = 800 MHz
```
Formula from the HAL's own comment, `stm32n6xx_hal_rcc_ex.c:2820-2822`.

`main.c:249-255`:
```
CPUCLK  = IC1 <- PLL1, divider 1  =  800 MHz
sysb_ck = IC2 <- PLL1, divider 2  =  400 MHz
```
`main.c:266`, `AHBCLKDivider = RCC_HCLK_DIV2`, `APB1CLKDivider = RCC_APB1_DIV1`:
```
HCLK  = 400 / 2 = 200 MHz
PCLK1 =           200 MHz
```

**Two independent observed numbers corroborate this chain**, which is why it is inference and
not speculation: the board printed `sysclk=400000000` (= IC2, exact) and CLAUDE.md H-D8
records I2C timing "measured working at pclk1 = 200 MHz" (exact). A third: PLL2 =
64/8 x 125 = 1000 MHz feeds IC6 divider 1, matching ST's "Neural-ART up to 1 GHz".

`DWT->CYCCNT` counts processor clock cycles, i.e. **CPUCLK**. Therefore:

- **The board's `sysclk=400000000` is correct and expected.** It is not a symptom. G-8's
  premise — that 400 vs 600 was a discrepancy needing explanation — was itself the error.
- **CLAUDE.md §1's "Cortex-M55 @ 600 MHz" is wrong.** ST gives the STM32N657X0 as "frequency
  up to 800 MHz" [st.com product page, fetched 2026-08-30], and this board's PLL configures
  exactly that.
- **CLAUDE.md §3's `cycles / 600000 = ms` is wrong by 800/600 = 1.333x.** Every DWT-derived
  figure in the project reads **1.333x too large**.

### 2.3 This makes the existing Phase 4 evidence *better*, not worse *(inference)*

RZ3 recorded a DWT cross-check of "~4 µs (integer-truncated firmware reading)" against a
logic-analyzer worst case of **3.375 µs**, and the 0.6 µs gap was written off as truncation.
Recompute at 800 MHz: a firmware reading of `cycles/600 ≈ 4` means 2400–2999 raw cycles;
at 800 MHz that is **3.00–3.75 µs**, which brackets 3.375 µs. **At 600 MHz the two methods
disagreed; at 800 MHz they agree.** The existing data already pointed at 800 MHz.

**The contest claim is unaffected.** 3.375 µs is a logic-analyzer number and no clock constant
enters it. What must change is the parenthetical "(2,025 cycles)" in CLAUDE.md §8 RZ3 — that
is 3.375 µs x 600 MHz. At 800 MHz it is **2,700 cycles**.

### 2.4 Consequence for T2 *(inference)*

Handoff §3 T2 specifies `dwt_spin_cycles(1200)` "≈ 2 µs". At 600 MHz, 1200 cycles is 2.0 µs;
**at 800 MHz it is 1.5 µs.** Still legal — SLOS854D §8.4.5.1: "The pulse width should be at
least 1 µs to ensure detection" — but the margin is 1.5x, not 2x. Prefer deriving the spin
count from `HAL_RCC_GetCpuClockFreq()` at runtime over hardcoding either constant.

### 2.5 What was changed in code

- DWT enable moved out of `#ifdef DEBUG_TIMING` (which this build does not define, so the
  counter has never run outside the Phase 4 campaign).
- Liveness check added: `DWT_CTRL_NOCYCCNT_Msk` (`core_cm55.h:1314`) reads 1 if the cycle
  counter is not implemented; two spaced reads catch an enable that was rejected. Prime
  suspect for the latter on this target is secure non-invasive debug being disabled under
  TZEN, which the FSBL owns. Result prints as `[DWT] ok=`.
- `CoreDebug`/`CoreDebug_DEMCR_TRCENA_Msk` kept deliberately, though marked `\deprecated` in
  this CMSIS (`core_cm55.h:3194,3620`) in favour of `DCB`/`DCB_DEMCR_TRCENA_Msk` — same
  register, and this is the exact sequence that produced the RZ3 evidence.
- `HAL_RCC_GetCpuClockFreq()` now logged alongside sysb_ck.
- Heartbeat computes `cyc_per_ms` in firmware, so the answer arrives on the wire with no
  arithmetic at the bench. **Wrap constraint, now in the code comment:** CYCCNT wraps every
  2^32/800e6 = 5.37 s; `HEARTBEAT_PERIOD_MS` is 1000, so the UW subtraction is safe. Raising
  the heartbeat period above ~5 s silently turns this number into garbage.

---

## 3. AT THE BENCH — what to expect, and what each outcome means

Build in CubeIDE (**F5 first, headers changed**), confirm `-Trusted.bin` is newer than
`0bbde4e` per CLAUDE.md §9, flash, open picocom.

### 3.1 `ok=` is now 22, not 21

The handoff predicted 21 = 9 + 1 ID read + 7 config writes + 1 arm + 3 readbacks.
The OD_CLAMP readback makes it **4** readbacks: **22**.
`ok=21` after this build means one transfer did not happen.

### 3.2 Expected lines

```
[DRV] init=0 id=7 mode=0x1 lib=0x2 seq=0x1 odc=0x8b sts=0xe0 armed=1
[DWT] ok=1 cyc=<large, changing> cyc_per_ms=800000
[CLK] cpu=800000000 sysb=400000000 pclk1=200000000
[I2C] init=0 gate=0 whoami=0x140e0 wr=0 wrseen=0x27 addr=0x5a ok=22 err=0 tmo=0 recov=0
```

### 3.3 Failure map

| observation | meaning |
|---|---|
| `init=-42` | STATUS bits 7:5 ≠ 7. `sts=` names what was actually read. `sts=0xa5` ⇒ DMA never wrote |
| `init=-41` | a readback disagreed — **`odc=` is the new one**; `odc=0x8c` means the OD_CLAMP write did not land (D-1's defect, now visible); `odc=0xa5` means that read did not transfer |
| `init=-57` | a transfer failed outright |
| `sts=` bit 0 set | `OC_DETECT` — load impedance below threshold. Nothing is connected yet, so this would be surprising and worth stopping for |
| `sts=` bit 1 set | `OVER_TEMP` |
| `[DWT] ok=0` | CYCCNT not counting. Either not implemented (`NOCYCCNT`) or the enable was rejected — secure non-invasive debug under TZEN. **T2 is blocked and every DWT number in the project is void until this is resolved.** The LA method is unaffected |
| `cyc_per_ms=800000` | predicted. Confirms §2.2. Update CLAUDE.md §1 and §3 |
| `cyc_per_ms=400000` | CYCCNT is on sysb_ck, contradicting the Arm architecture. Do not accept without a second method |
| `cyc_per_ms` ≈ 0 or wild | first heartbeat only (prev=0); read the second line onward |

### 3.4 Do not update CLAUDE.md until `cyc_per_ms` prints

§2 is inference from source files, not a measurement. It is strong inference — three
independently observed numbers fit it — but the project's own §9 rule is that a hardware
figure gets recorded when hardware produces it. **Then** change CLAUDE.md §1 (600 → 800 MHz),
§3 (`/600000` → `/800000`) and §8 RZ3's "(2,025 cycles)" → 2,700.

---

## 4. NOT DONE

- **Not built, not flashed.** There is no ARM toolchain reachable from this session — the
  bridge mounts the repo, not the CubeIDE install. `Appli/Debug/app_drv2605l.su` still lists
  only `drv2605l_gpio_init`/`power_up`/`trig_set` at lines 36/60/67, which is the **Block 0**
  version of the file: the register driver has never been through a compiler with real
  headers, and now neither have these changes. *(evidence)*
- **T2 deliberately not started.** Stacking a second unbuilt change on an unbuilt one is how
  Block 1a got here. T2 is written the moment `[DWT] ok=1` and `cyc_per_ms` land.
- **Runtime STATUS poll (D-2) still owed** before the motor is connected at T3.
