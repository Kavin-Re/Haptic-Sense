# Phase 5 — L1 `i2c_xfer()` Design: Semaphore-Wrapped DMA I2C Primitive

**Project:** Haptic-Sense · TRON Forum Contest 2026
**Status:** DESIGN — no driver code written. For review before implementation.
**Context:** Option A decided (commit `b1b51c8`): the app owns ST HAL I2C directly; `DEVCNF_USE_HAL_IIC` stays 0 permanently. This doc designs the L0/L1 layers of the Phase 5 stack (`docs/PHASE5_DESIGN_sensor_bringup_i2c.md` §4).
**Evidence rule:** every hardware-specific below cites an on-disk source (path:line) or is explicitly labeled UNVERIFIED with its verification step. Nothing is stated from memory alone.

---

## 0. What L1 must provide (from the Phase 5 design §4.1, unchanged)

```c
/* task: sensor_task, TK_PRI 3 — ONLY CALL FROM PRIORITY 3 SENSOR TASK */
ER i2c_xfer(...);   /* synchronous to the caller, non-blocking to the CPU */
```
Single client (sensor_task), single bus (I2C1), hard-real-time environment: priority 1–2
tasks must preempt freely during any transfer. The BSP's gated-off `hal_i2c.c` is the
reference for the wait pattern (IT + `tk_wai_flg`); we reimplement as **semaphore + DMA**.

Proposed home: one new file pair `Core/Src/app_i2c.c` + `Core/Inc/app_i2c.h`
(Apache 2.0, own code), holding L0 (handles, MSP, IRQ plumbing) and L1 (`i2c_xfer`).

---

## 1. `hi2c1` handle + HAL init

**Decision:** `static I2C_HandleTypeDef hi2c1;` file-scope in `app_i2c.c`. Static — never
dynamic (CLAUDE.md §7), and **file-local, not global**: with `DEVCNF_USE_HAL_IIC=0`, the
BSP's `devinit.c` `IMPORT I2C_HandleTypeDef hi2c1` is compiled out (`devinit.c`,
`knl_start_device()`, gated by `#if DEVCNF_USE_HAL_IIC`), so nothing external links
against the name. The import trap found on 2026-07-06 is fully defused by flag=0;
grep confirms no other reference to `hi2c1` in compiled sources.

**Who calls `HAL_I2C_Init`, and when:** `app_i2c_init()`, called at the **top of
`sensor_task_fct` before its loop** (TK_PRI 3 context, after kernel start, before the
first frame).
- Justification: the strictest reading of "ALL I2C lives in the Priority 3 task"
  (CLAUDE.md §3). `HAL_I2C_Init` itself produces no bus traffic, so running it from
  main_thread (TK_PRI 15, where GPIO init lives) would also be defensible — but
  initializing from sensor_task costs nothing (no other task touches the bus, and
  inference blocks on `data_ready_sem` until the first frame anyway) and keeps every
  I2C-adjacent line in one task context. **Open decision #1** (below) records the
  alternative.
- The `i2c_done_sem` creation also happens inside `app_i2c_init()` — kernel is running
  by then (`tk_cre_sem` needs it), and it must exist before the first transfer starts.

**400 kHz Fast-mode timing (TIMINGR):**
- The I2C kernel clock source options on this part are PCLK1 / CLKP / IC10 / IC15 /
  MSI / HSI (`stm32n6xx_hal_rcc_ex.h:239-244`). `main.c`'s `SystemClock_Config` does
  **not** configure an I2C1 kernel clock → the reset-default selection applies.
  UNVERIFIED which source that is (expected PCLK1; the board BSP assumes PCLK1 —
  see next bullet). **Verification step:** log `HAL_RCCEx_GetPeriphCLKFreq()` for I2C1
  and `HAL_RCC_GetPCLK1Freq()` at bring-up, first heartbeat line.
- ST's own board support computes TIMINGR **at runtime**:
  `MX_I2C1_Init(&hbus_i2c1, I2C_GetTiming(HAL_RCC_GetPCLK1Freq(), BUS_I2C1_FREQUENCY))`
  (`stm32n6570_discovery_bus.c:228`), with the calculator at `bus.c:1050` and a
  128-entry valid-timing table (`bus.c:39-40,150`).
- **Decision:** copy the `I2C_GetTiming()` runtime-calculator approach (adapted, ST-SLA
  respected by calling into the BSP bus source or re-deriving; see Open decision #2)
  rather than hardcoding a TIMINGR constant. Justification: it is the exact mechanism
  ST validated on this exact board and clock tree, and it stays correct if the clock
  tree changes. A fixed constant from CubeMX is the fallback if the calculator's
  footprint offends (it costs ~128 × sizeof(I2C_Timings_t) RAM).
- 400 kHz target per Phase 5 design §3; rise-time margin on the 1.5 kΩ pull-ups is
  UNVERIFIED until scoped — fallback to 100 kHz is pre-approved by the §7 bandwidth
  budget (20 % utilization, still fits).

---

## 2. MSP init — pins, clocks, power domain, GPDMA routing

**Pin configuration (grounded in ST's board support for this exact board):**
`stm32n6570_discovery_bus.h:62-80` defines I2C1 on precisely our CLAUDE.md §2 pins:

| Item | Value | Source |
|---|---|---|
| SCL | **PH9**, `GPIO_AF4_I2C1`, AF open-drain, no pull, speed HIGH | `discovery_bus.h:74,78` + `discovery_bus.c:577-583` |
| SDA | **PC1**, `GPIO_AF4_I2C1`, AF open-drain, no pull, speed HIGH | `discovery_bus.h:75,79` + `discovery_bus.c:585-591` |
| AF number | AF4 for both (`GPIO_AF4_I2C1 = 0x04`, `stm32n6xx_hal_gpio_ex.h:97`) | ST board header, not datasheet-from-memory |
| GPIO clocks | `__HAL_RCC_GPIOH_CLK_ENABLE()` + `__HAL_RCC_GPIOC_CLK_ENABLE()` | `discovery_bus.h:66-68` |
| I2C1 clock | `__HAL_RCC_I2C1_CLK_ENABLE()` + FORCE/RELEASE_RESET | `discovery_bus.h:63,70-71` |
| **Power domain** | **`HAL_PWREx_EnableVddIO4()` before touching the pins** | `discovery_bus.c:568` |

That last row is the kind of step that silently bricks a bring-up if coded from memory:
ST's own MspInit enables the VddIO4 domain first. Our MSP copies this order exactly.
No pull-up configuration — the 1.5 kΩ onboard resistors are the pull-ups (CLAUDE.md §2:
add nothing).

**GPDMA routing (the item flagged UNVERIFIED in the sensor design doc §4.2 — now
grounded):** the N6 uses GPDMA (not the older DMA1/DMA2 stream model). On-disk request
IDs:

```
LL_GPDMA1_REQUEST_I2C1_RX = 95   (stm32n6xx_ll_dma.h:1287)
LL_GPDMA1_REQUEST_I2C1_TX = 96   (stm32n6xx_ll_dma.h:1288)
```
These are the only I2C1 request defines in the HAL tree (grep of
`Drivers/STM32N6xx_HAL_Driver/Inc/` returns only the `LL_`-prefixed names — the HAL
DMA `Init.Request` field takes these values; confirm the exact identifier spelling at
first compile).

**Channel assignment — Open decision #3:** GPDMA1 has 8 channels, each with its own
IRQ (`GPDMA1_Channel0..7_IRQn = 84..91`, `stm32n657xx.h:140-147`). Nothing else in
this app uses GPDMA (camera pipeline excluded from build). **Recommendation:
Channel 0 = I2C1_RX, Channel 1 = I2C1_TX**, linked with `__HAL_LINKDMA(&hi2c1, hdmarx/
hdmatx, ...)`; two `static DMA_HandleTypeDef` in `app_i2c.c`. UNVERIFIED: whether any
GPDMA channel has TrustZone/RIF security attribute requirements in our FSBL-configured
secure world (the reference app's `Security_Config` grants RIMC master attributes to
NPU/DMA2D/DCMIPP/LTDC — `main.c:330-351` — but not GPDMA1). **Verification step:** if
the first DMA transfer faults or never completes, add GPDMA1 to the RIF/RIMC config
mirroring `main.c:336` and retest; check RM0486 RIFSC peripheral index table.

---

## 3. IRQ integration

**Which interrupts fire (all from `stm32n657xx.h`):**

| IRQ | Number | Role |
|---|---|---|
| `I2C1_EV_IRQn` | 100 (`:156`) | I2C event — HAL uses it even in DMA mode (transfer mgmt, NACK) |
| `I2C1_ER_IRQn` | 101 (`:157`) | I2C error — feeds `HAL_I2C_ErrorCallback` |
| `GPDMA1_Channel0_IRQn` | 84 (`:140`) | RX DMA complete |
| `GPDMA1_Channel1_IRQn` | 85 (`:141`) | TX DMA complete |

Handlers are thin: call `HAL_I2C_EV_IRQHandler(&hi2c1)` / `HAL_I2C_ER_IRQHandler(&hi2c1)` /
`HAL_DMA_IRQHandler(&hdma_rx/tx)`; HAL then invokes our completion/error callbacks.

**Registration — via the kernel, not bare NVIC:** `tk_def_int()` +
`EnableInt()`, the BSP's first-class mechanism:
- `tk_def_int(intno, T_DINT{.intatr=TA_HLNG, .inthdr=our_handler})` lands in
  `knl_define_inthdr()` (`sysdepend/stm32_cube/cpu/core/armv8m/interrupt.c:65-81`),
  which writes the handler into the kernel's RAM vector table and — because of
  `TA_HLNG` — wraps it in `knl_hll_inthdr()` (`interrupt.c:33-46`), which brackets the
  call in `ENTER_TASK_INDEPENDENT`/`LEAVE_TASK_INDEPENDENT`. That bracket is what makes
  `tk_sig_sem` from the handler legal and makes the kernel defer dispatch correctly.
  (The BSP's own `hal_i2c.c:120,131` does the same bracketing manually — same
  requirement, different mechanism.)
- `EnableInt(intno, level)` → `EnableInt_nvic()` (`int_armv8m.c:95-107`): writes
  NVIC_IPR from the level and sets NVIC_ISER. We never touch NVIC registers directly.

**The priority constraint (the critical number):** the kernel's critical sections mask
interrupts by writing BASEPRI to `INTPRI_VAL(INTPRI_MAX_EXTINT_PRI)`
(`disint()`, `int_armv8m.c:55-64`), where `INTPRI_MAX_EXTINT_PRI = 1`
(`include/sys/sysdepend/stm32_cube/cpu/stm32n6/sysdef.h:77`) and
`INTPRI_VAL(x) = (x) << (8-INTPRI_BITWIDTH)` with `INTPRI_BITWIDTH = 4`
(armv8m core sysdef family; same file line 71 for the bitwidth). Consequences:
- An ISR at **level 0 is never masked by the kernel and must not call any tk_* API.**
- **Safe range for ISRs that call `tk_sig_sem`: level 1 .. 15.** Level 1 is the
  highest kernel-safe level; SysTick itself runs there (`INTPRI_SYSTICK = 1`,
  `cpu/stm32n6/sysdef.h:79`).
- **Decision: all four IRQs at level 1** via `EnableInt(intno, 1)`. Justification:
  I2C completion wakes the Priority 3 task feeding the hazard chain — no reason to
  rank it below anything else; and `main.c:121-123` already blanket-sets every
  peripheral IRQ (PVD_PVM_IRQn=0 .. LTDC_UP_ERR_IRQn=194, covering 84/85/100/101) to
  SysTick's priority, so level 1 is also consistent with the system as it runs today.
- Precedent that this pattern works on this port: the reference app signals
  `isp_sem` with `tk_sig_sem` from a DCMIPP frame-event callback (ISR context) —
  `app.c:417-421` — under exactly this blanket NVIC configuration.

**Registration timing:** inside `app_i2c_init()` (sensor_task context), before the
first transfer, after `HAL_I2C_Init`. UNVERIFIED: interaction between `tk_def_int`'s
RAM vector table and the app's `USE_STATIC_IVT=0` configuration (`config.h:79` sets
dynamic IVT — expected to Just Work since that is the mode `tk_def_int` is built for).
**Verification step:** first bring-up test is WHO_AM_I over the sem-wrapped path
(Phase 5 design §9, second checkbox) — it exercises def_int → IRQ → callback → sem
end-to-end.

---

## 4. The transfer protocol

### 4.1 Objects (all static, `USE_IMALLOC=0`, CLAUDE.md §7)

```c
/* app_i2c.c — all file-scope static */
static I2C_HandleTypeDef hi2c1;
static DMA_HandleTypeDef hdma_i2c1_rx, hdma_i2c1_tx;
static ID  i2c_done_sem;              /* tk_cre_sem: isemcnt = 0, maxsem = 1, TA_TFIFO */
static volatile ER   i2c_xfer_err;    /* written by callbacks, read by caller  */
static volatile BOOL i2c_busy;        /* single-client assert, not a lock      */
```
`i2c_done_sem` **init count 0** — the caller always waits for a completion that hasn't
happened yet; producer is the ISR-context callback, consumer is sensor_task (named per
CLAUDE.md rule): I2C/DMA completion callback (P) → `i2c_done_sem` → `sensor_task` (C).

DMA buffers: every DMA-target buffer `__attribute__((aligned(32)))` and padded to a
32-byte multiple (M55 cache line), per Phase 5 design §4.2. Grounded note: **D-cache is
currently OFF in this build** — `USE_DCACHE` is commented out (`app_config.h:21`), so
`main.c:62-65` never enables it. DMA coherency is therefore a non-issue *today*; the
alignment rule and the invalidate call are enforced anyway so that enabling D-cache
later (Phase 6 NPU work may want it) cannot introduce a silent corruption. This state
is recorded here deliberately: it is also why Phase 4's synthetic-path invalidate was
harmless.

### 4.2 The sequence (happy path)

```
i2c_xfer(dir, dev_addr, reg, buf, len):            /* TK_PRI 3 only */
  1. assert !i2c_busy; i2c_busy = TRUE; i2c_xfer_err = E_OK
  2. start transfer:
       write: HAL_I2C_Mem_Write_DMA(&hi2c1, addr<<1, reg, regsz, buf, len)
       read:  HAL_I2C_Mem_Read_DMA (&hi2c1, addr<<1, reg, regsz, buf, len)
       (Mem_* variants: VL53L1X needs 16-bit reg index -> I2C_MEMADD_SIZE_16BIT;
        MPU6050 8-bit -> I2C_MEMADD_SIZE_8BIT. Both supported by the same calls.)
       HAL status != HAL_OK  ->  release busy, return E_IO (bus never started)
  3. err = tk_wai_sem(i2c_done_sem, 1, I2C_XFER_TMO_MS)   /* task sleeps; CPU free */
  4. IRQ side (already ran by now on success):
       HAL_I2C_MemTx/RxCpltCallback  -> i2c_xfer_err = E_OK   -> tk_sig_sem(i2c_done_sem, 1)
       HAL_I2C_ErrorCallback         -> i2c_xfer_err = E_IO   -> tk_sig_sem(i2c_done_sem, 1)
       HAL_I2C_AbortCpltCallback     -> i2c_xfer_err = E_ABORT-> tk_sig_sem(i2c_done_sem, 1)
       (error paths SIGNAL TOO — the caller must always wake; mirrors hal_i2c.c:164-172,
        which maps the same three callbacks to E_OK/E_IO/E_ABORT)
  5. on read success: SCB_InvalidateDCache_by_Addr(buf, padded_len)
       (inert while D-cache is off — kept per CLAUDE.md §3, same policy as Phase 4)
  6. i2c_busy = FALSE; return (err == E_TMOUT) ? E_TMOUT : i2c_xfer_err
```

### 4.3 Timeout — Open decision #4

`#define I2C_XFER_TMO_MS 50` in `app_i2c.h`. Justification: the longest legitimate
transfer in the Phase 5 §7 budget is ≈ 1 ms at 400 kHz (≈ 4 ms at the 100 kHz
fallback); 50 ms is > 10× margin over the slowest case including clock stretching,
yet detects a stuck bus within 2.5 frames — against the BSP's 500 ms
(`hal_i2c_cnf.h:23`), which would silently eat 25 frames per fault. The stale-data
policy (§8 of the Phase 5 design: `stale_frames > 10` → `data_valid = false`) bounds
end-to-end detection at ~200 ms regardless.

### 4.4 Error path and bus recovery

Per-call: one retry on `E_IO` (NACK class); second consecutive failure → bus recovery.
`E_TMOUT` → `HAL_I2C_Abort_IT()` first (defined HAL state), then bus recovery.

Bus recovery (all from sensor_task, TK_PRI 3):
```
1. HAL_I2C_DeInit(&hi2c1)
2. re-mux PH9 as GPIO output-OD; SDA (PC1) as input
3. clock out up to 9 SCL pulses @ ~100 kHz until SDA reads high   /* frees a stuck slave */
4. generate STOP: SDA low -> SCL high -> SDA high
5. re-mux PH9/PC1 back to AF4; HAL_I2C_Init(&hi2c1); re-register nothing
   (tk_def_int table survives; NVIC state survives)
6. increment recovered-error counter (heartbeat prints it — Phase 5 design §8)
```
The 9-pulse sequence is standard I2C practice (slave releasing SDA mid-byte);
UNVERIFIED on this board until fault-injected. **Verification step:** during bring-up,
induce a stuck bus (hold SDA low via probe at a safe moment) and confirm recovery +
counter increment. Note the XSHUT-to-rail consequence (Phase 5 design §3): recovery
cannot power-cycle the ToF, so this routine plus the VL53L1X soft-reset register is
the entire recovery arsenal.

### 4.5 What L1 explicitly does NOT do

- No queueing, no multi-client arbitration — single caller by CLAUDE.md rule;
  `i2c_busy` is an assert, not a mutex.
- No calls from any ISR, ever. No calls from any task but sensor_task (TK_PRI 3).
- No dynamic allocation anywhere.

---

## 5. Open decisions (explicit, with recommendations)

| # | Decision | Options | Recommendation |
|---|---|---|---|
| 1 | Where `app_i2c_init()` runs | (a) top of sensor_task (TK_PRI 3); (b) `app_tasks_run()` (TK_PRI 15, with GPIO init) | **(a)** — strictest reading of the P3 rule, zero cost |
| 2 | TIMINGR source | (a) runtime `I2C_GetTiming()` calculator like ST's BSP (`bus.c:1050`); (b) fixed CubeMX constant | **(a)** — board-proven, clock-tree-proof; (b) if RAM table offends |
| 3 | GPDMA channels | any of ch0–7 | **ch0=RX, ch1=TX** — nothing else uses GPDMA1 in this build |
| 4 | Timeout | 50 ms vs BSP's 500 ms | **50 ms** — bounded fault detection within 2.5 frames |
| 5 | `Mem_*` vs raw `Master_*` HAL calls | `Mem_Read/Write_DMA` do write-index + repeated-start-read in one HAL call; raw needs two calls | **`Mem_*`** — matches both sensors' register model (VL53L1X 16-bit, MPU6050 8-bit index) |

## 6. Consolidated UNVERIFIED list (each with its verification step)

| Item | Verify by |
|---|---|
| I2C1 kernel-clock reset-default source + actual frequency | log `HAL_RCCEx_GetPeriphCLKFreq(I2C1)` + `HAL_RCC_GetPCLK1Freq()` at first boot |
| 400 kHz rise time on 1.5 kΩ pull-ups | scope SCL/SDA edges at bring-up; fallback 100 kHz pre-approved |
| Exact HAL spelling of the GPDMA request macros (LL_ prefix or alias) | first compile |
| GPDMA1 TrustZone/RIF attributes needed in secure world | if first DMA transfer faults: add GPDMA1 to RIF config mirroring `main.c:336`; check RM0486 RIFSC table |
| `tk_def_int` + dynamic IVT (`USE_STATIC_IVT=0`) interaction | WHO_AM_I test exercises the full chain |
| 9-pulse bus recovery on this board | fault-injection test during bring-up |
| (carried) DWT/CPU-clock question: log `HAL_RCC_GetSysClockFreq()` at boot alongside the I2C clocks — cheap to record while we're logging clocks anyway | first boot log |

---
*Review gate: no driver code until this design is approved. After approval, implementation order = app_i2c.c (L0+L1) → bus scan (§2.4 of the sensor design) → WHO_AM_I/model-ID reads → L2 device drivers.*
