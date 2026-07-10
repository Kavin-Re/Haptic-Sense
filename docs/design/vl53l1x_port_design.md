# PHASE 5 DESIGN — VL53L1X ULD Platform-Layer Port

**Project:** Haptic-Sense (TRON Forum Contest 2026)
**Status:** DESIGN ONLY — no implementation. Review before build.
**Scope:** Port the ST VL53L1X ULD (STSW-IMG009 v3.5.5) platform layer onto the
existing async `i2c_rd`/`i2c_wr` primitive (`app_i2c.c`, commit `fc6ff88`), so the
ULD core (`VL53L1X_api.c`, `VL53L1X_calibration.c`) runs **unmodified**.

**Provenance of hardware/API claims in this doc (grounding pass 2026-07-07):**
- ULD source is now IN-REPO: `firmware/Appli/Lib/STSW-IMG009/STSW-IMG009_v3.5.5/`.
  All `API/...` and `Example/...` cites below are relative to that directory.
  Full source read this pass: `API/core/VL53L1X_api.{c,h}`, `API/core/VL53L1X_calibration.c`,
  `API/platform/vl53l1_platform.{c,h}`, `Example/Inc/vl53l1_platform.h`,
  `Example/Src/vl53l1_platform.c`, `API/LICENSE.txt`.
- Committed primitive read: `firmware/Appli/Core/Src/app_i2c.c`, `Core/Inc/app_i2c.h` (commit `fc6ff88`).
- N6 HAL read: `firmware/STM32Cube_FW_N6/Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_i2c.c`.
- I2C addresses / pin map: CLAUDE.md §2 (schematic-verified MB1939, Nov 2024).
- Every item labelled **[UNVERIFIED]** in the 2026-07-06 draft is now resolved against
  source; the few remaining opens are HARDWARE checks, collected in §8.

---

## 0. TWO CORRECTIONS (updated 2026-07-07)

Item 1 still needs pasting into `docs/PHASE5_DESIGN_sensor_bringup_i2c.md` before
implementation. Item 2 now ALSO corrects this doc's own 2026-07-06 rationale.

1. **The "~88 KB firmware upload at init" does NOT apply to the VL53L1X.**
   `VL53L1X_SensorInit` is a loop of single-byte `VL53L1_WrByte` calls over registers
   0x2D–0x87 (`API/core/VL53L1X_api.c:184-186`) — that is **91** one-byte register
   writes (the `VL51L1X_DEFAULT_CONFIGURATION[]` array, `VL53L1X_api.c:60-152`, has
   91 entries; the `api.h:113` docstring's "135 bytes" is ST's own doc drift, both
   numbers are tiny either way). The 88 KB blob upload is a **VL53L5CX** property
   (the MB1854B camera-module ToF, CLAUDE.md §2 upgrade path), which
   cross-contaminated the VL53L1X section. No large-transfer buffer or chunking
   concern exists here. **Evidence** (source read). Confidence: certain.

2. **Copy the platform contract from `API/platform/`, and copy NOTHING from `Example/Inc/`.**
   CORRECTION to the 2026-07-06 draft, which claimed the two `vl53l1_platform.h`
   files differ (`Example/Inc` allegedly a fat `I2C_HandleTypeDef`-bound struct):
   **in v3.5.5 the two headers are byte-identical except one line of whitespace**
   (verified by `diff`; both define `VL53L1_Dev_t {uint32_t dummy;}` and pass
   `uint16_t dev` — `API/platform/vl53l1_platform.h:28-32` ==
   `Example/Inc/vl53l1_platform.h:29-33`). The fat HAL-bound struct the draft
   remembered lives in `Example/Inc/vl53l1_platform_user_data.h:32`
   (`I2C_HandleTypeDef *I2cHandle`) — a **full-API leftover** that neither
   platform header includes. The conclusion stands unchanged, with the correct
   rationale: `API/platform/` contains exactly the ULD contract
   (`vl53l1_platform.h` + `vl53l1_types.h`) and nothing else; `Example/Inc/`
   is littered with full-API leftovers (`vl53l1_platform_user_data.h`,
   `vl53l1_platform_user_config.h`, `vl53l1_error_codes.h`,
   `vl53l1_platform_log.h`) that pull in HAL and a different error-type scheme
   (`VL53L1_Error`, `vl53l1_error_codes.h:89`) if copied alongside.
   **Our shim targets `API/platform/vl53l1_platform.h` — and since the headers
   are identical, the §1 signature table needed no correction** (re-verified
   line-by-line, §1). **Evidence** (both headers diffed this pass). Confidence: certain.

---

## 1. THE 9 PLATFORM FUNCTIONS (verified signatures)

From `API/platform/vl53l1_platform.h:37-97` — re-verified line-by-line against the
in-repo copy 2026-07-07; the table below matches exactly (`WriteMulti` h:37,
`ReadMulti` h:45, `WrByte` h:53, `WrWord` h:60, `WrDWord` h:67, `RdByte` h:74,
`RdWord` h:81, `RdDWord` h:88, `WaitMs` h:95). All return `int8_t` (0 = success,
nonzero = error). `dev` is `uint16_t`. `index` is `uint16_t` (16-bit register
index → `regsz = 2`). `count` is `uint32_t`; `wait_ms` is `int32_t`.

| ULD function | Signature (abbrev) | Direction | Payload width |
|---|---|---|---|
| `VL53L1_WrByte`   | `(dev, index, uint8_t data)`        | write | 1 |
| `VL53L1_WrWord`   | `(dev, index, uint16_t data)`       | write | 2 |
| `VL53L1_WrDWord`  | `(dev, index, uint32_t data)`       | write | 4 |
| `VL53L1_RdByte`   | `(dev, index, uint8_t *pdata)`      | read  | 1 |
| `VL53L1_RdWord`   | `(dev, index, uint16_t *pdata)`     | read  | 2 |
| `VL53L1_RdDWord`  | `(dev, index, uint32_t *pdata)`     | read  | 4 |
| `VL53L1_WriteMulti`| `(dev, index, uint8_t *pdata, uint32_t count)` | write | count |
| `VL53L1_ReadMulti` | `(dev, index, uint8_t *pdata, uint32_t count)` | read  | count |
| `VL53L1_WaitMs`   | `(dev, int32_t wait_ms)`            | delay | — |

**Evidence** (header read this session). Confidence: certain.

---

## 2. TWO NON-OBVIOUS CONTRACTS — GET THESE RIGHT OR NOTHING WORKS

### 2.1 `dev` is the 8-bit address; `i2c_rd`/`i2c_wr` take 7-bit → **right-shift by 1**

`VL53L1X_api.h` documents the default sensor address as **0x52**. That is the
**8-bit** (write-frame) form of the 7-bit address **0x29** (`0x29 << 1 = 0x52`).
CLAUDE.md §2 locks the VL53L1X at 7-bit **0x29**. The ULD passes the 8-bit value
around as `dev`. Your primitive signature is `i2c_rd(dev7, reg, regsz, buf, len)`
taking a **7-bit** address.

**The shim MUST convert:** `uint8_t dev7 = (uint8_t)(dev >> 1);`

> **This is the single most error-prone line in the port.** A wrong shift = every
> transaction NACKs and looks identical to "sensor not wired." Put an assertion in
> the shim: `/* dev is 8-bit per ULD; our bus is 7-bit */`.

**Evidence — RESOLVED against source 2026-07-07.** Default 0x52 documented at
`API/core/VL53L1X_api.h:108`. The core **never** shifts or interprets `dev`: the
grep over `VL53L1X_api.c` finds exactly one shift, and it is `new_address >> 1`
being written INTO the device's address register by `SetI2CAddress`
(`VL53L1X_api.c:174`) — not a manipulation of `dev` itself. `dev` is passed
opaquely to every platform call, so whatever the app hands the API is what the
shim receives. ST's own reference shim confirms the 8-bit convention: it passes
`Dev` straight into HAL's 8-bit-address calls (`HAL_I2C_Master_Transmit(&hi2c, Dev, …)`,
`Example/Src/vl53l1_platform.c:61`; read frame `Dev|1`, `:73`). The app therefore
passes `0x52`, and the shim's `dev >> 1 = 0x29` matches our 7-bit primitive.
Confidence: certain.

### 2.2 Register index is 16-bit, **big-endian on the wire (MSB first)** — RESOLVED

Every register constant in `VL53L1X_api.h` is 16-bit (`0x0000`…`0x013E`,
`VL53L1X_api.h:35-75`). The VL53L1 wire protocol sends the index **high byte first**.

**RESOLVED 2026-07-07 — the byte order is ST HAL's job, not `app_i2c.c`'s, and
HAL does it correctly.** Our primitive does not hand-pack the index at all: it
maps `regsz == I2C_REG16` to `I2C_MEMADD_SIZE_16BIT` and calls
`HAL_I2C_Mem_Read_DMA`/`HAL_I2C_Mem_Write_DMA` (`Core/Src/app_i2c.c:316-333`).
The N6 HAL transmits the 16-bit memory address **MSB first, LSB second**:
`HAL_I2C_Mem_Write_DMA` (fn at `stm32n6xx_hal_i2c.c:3060`) prefetches
`I2C_MEM_ADD_MSB(MemAddress)` into TXDR and queues the LSB for the ISR
(`stm32n6xx_hal_i2c.c:3117-3120`); `HAL_I2C_Mem_Read_DMA` (fn at `:3240`) does the
same (`:3297-3300`). Macros: `I2C_MEM_ADD_MSB`/`_LSB`, `stm32n6xx_hal_i2c.h:801-803`.
Independent cross-check: ST's reference shim packs `index>>8` then `index&0xFF`
(`Example/Src/vl53l1_platform.c:87-88`) — same order. Confidence: certain from source.

**Residual hardware check (not a source question):** the `regsz==2` path has never
run on hardware — the L1 gate test used `I2C_REG8` against the MPU6050
(`app_i2c.c:435`). The first `VL53L1X_GetSensorId` read (§6) is the hardware proof.

### 2.3 Multi-byte payload endianness (Wr/RdWord, Wr/RdDWord) — RESOLVED

The VL53L1 sends **data** big-endian too. `WrWord(dev, index, data)` must place
`data >> 8` at the lower address. `RdWord` must reassemble `(buf[0] << 8) | buf[1]`.
The shim owns this byte-swap — the primitive moves raw bytes and must not swap.

**RESOLVED 2026-07-07 against ST's reference shim** (authoritative for the wire
contract even though its blocking-HAL transport is unusable for us):
- `WrWord`: `data >> 8` first, `data & 0xFF` second (`Example/Src/vl53l1_platform.c:144-145`).
- `WrDWord`: `data>>24, >>16, >>8, >>0` (`:161-164`).
- `RdWord`: `(buf[0]<<8) + buf[1]` (`:228`).
- `RdDWord`: `(buf[0]<<24) + (buf[1]<<16) + (buf[2]<<8) + buf[3]` (`:252`).
Exactly the §3 mapping. Confidence: certain.

---

## 3. PLATFORM-FUNCTION → PRIMITIVE MAPPING

All shim functions run **only in `sensor_task` (TK_PRI 3)** — same context that
owns `i2c_rd`/`i2c_wr`. This satisfies CLAUDE.md §3 ("Any I2C call outside the
Priority 3 task = red-zone violation"). Every shim function gets the mandatory
comment `// ONLY CALL FROM PRIORITY 3 SENSOR TASK`.

Notation: `dev7 = dev >> 1`. `i2c_wr(dev7, reg, regsz, buf, len)` /
`i2c_rd(dev7, reg, regsz, buf, len)` are the committed semaphore-wrapped DMA
primitives. `regsz = 2` for all VL53L1X access.

```
WrByte (dev,index,data):        b[0]=data;                         i2c_wr(dev7, index, 2, b, 1)
WrWord (dev,index,data):        b[0]=data>>8; b[1]=data;           i2c_wr(dev7, index, 2, b, 2)
WrDWord(dev,index,data):        b[0..3]=data>>24..data (BE);       i2c_wr(dev7, index, 2, b, 4)
RdByte (dev,index,*pdata):      i2c_rd(dev7,index,2,b,1); *pdata=b[0]
RdWord (dev,index,*pdata):      i2c_rd(dev7,index,2,b,2); *pdata=(b[0]<<8)|b[1]
RdDWord(dev,index,*pdata):      i2c_rd(dev7,index,2,b,4); *pdata=BE32(b)
WriteMulti(dev,index,p,count):  i2c_wr(dev7, index, 2, p, count)   // pass-through, no swap
ReadMulti (dev,index,p,count):  i2c_rd(dev7, index, 2, p, count)   // pass-through, no swap
WaitMs(dev, wait_ms):           tk_dly_tsk(wait_ms)                // NOT a busy loop
```

**Return-code translation:** map primitive success → `0`, any primitive
error/timeout → nonzero. Recommend a single `#define` (e.g. `-1` or a project error
code) so the ULD's `!= 0` checks fire correctly. (The core ORs platform returns
into a `VL53L1X_ERROR`, which is `uint8_t` — `VL53L1X_api.h:28` — so any fixed
nonzero `int8_t` works; avoid values colliding with the named codes 253/254/255,
`VL53L1X_api.h:30-33`.)

**Local buffers:** each Wr/Rd shim needs a tiny stack buffer (max 4 bytes for the
DWord case). Static-only rule (CLAUDE.md §7) — these are fixed-size stack locals,
no malloc; acceptable. WriteMulti/ReadMulti pass the caller's buffer straight
through (the ULD supplies it), so no extra buffer.

### 3.1 `WriteMulti`/`ReadMulti` — max transfer size — RESOLVED

**RESOLVED 2026-07-07:** `SensorInit` is a `WrByte` loop over registers 0x2D–0x87
(`VL53L1X_api.c:184-186`) — 91 individual one-byte writes, no block transfer.
The largest single transfer anywhere in the ULD core is **17 bytes**:
`VL53L1X_GetResult`'s `VL53L1_ReadMulti(dev, VL53L1_RESULT__RANGE_STATUS, Temp, 17)`
(`VL53L1X_api.c:590-593`). Largest write payload is 4 bytes (`WrDWord`). Both are
trivial for the DMA primitive. Note `GetResult` passes a **stack** buffer
(`uint8_t Temp[17]`, `VL53L1X_api.c:590`) — fine today with D-cache off, but see §4
for the alignment caveat if D-cache is ever enabled.

---

## 4. CACHE MAINTENANCE (Cortex-M55 + XSPI RAM)

CLAUDE.md §3 mandates `SCB_InvalidateDCache_by_Addr` after DMA reads before the
data is consumed. **RESOLVED 2026-07-07 — Option A is already implemented:** the
primitive invalidates its own destination buffer after every successful DMA read,
inside `i2c_xfer_once` (`Core/Src/app_i2c.c:349-353`). The call is inert today
because D-cache is OFF in this build (`app_config.h:21`, per the app_i2c.c comment)
but is kept per CLAUDE.md §3 policy. The shim adds nothing.

Two latent items to carry forward (both inert while D-cache stays off — become
real work items only if D-cache is ever enabled):

1. **Write-side clean is ABSENT from the primitive.** `i2c_xfer_once` never calls
   `SCB_CleanDCache_by_Addr` on the TX source buffer before kicking
   `HAL_I2C_Mem_Write_DMA` (`app_i2c.c:326-335` — read path only has maintenance).
   With D-cache on, the DMA engine could read stale XSPI RAM. Owner: the primitive
   (same seam as the invalidate).
2. **The ULD passes unaligned stack buffers.** `GetResult`'s `Temp[17]`
   (`VL53L1X_api.c:590`) is not 32-byte aligned/padded; invalidating it with
   D-cache on can clobber adjacent stack. The L1 design's aligned-buffer rule
   (`app_i2c.c:64-67`, `gate_buf`) cannot be imposed on unmodified ULD code — the
   fix at that point would be a bounce buffer in the shim's `ReadMulti`.

---

## 5. LICENSING SEPARATION

Goal: ULD core stays ST-licensed and **byte-for-byte unmodified**; our platform
implementation is Apache-2.0.

| File | Origin | License | Modify? |
|---|---|---|---|
| `VL53L1X_api.{c,h}` | ST ULD v3.5.5 | ST (see below) | **NO** — copy verbatim |
| `VL53L1X_calibration.{c,h}` | ST ULD v3.5.5 | ST | **NO** — copy verbatim |
| `vl53l1_platform.h` (from `API/platform/`) | ST ULD v3.5.5 | ST | **NO** — copy verbatim |
| `vl53l1_types.h` (from `API/platform/`) | ST ULD v3.5.5 | ST | **NO** — copy verbatim |
| `vl53l1_platform.c` | **OURS** | **Apache-2.0 + SPDX** | authored fresh |

**Key discipline:** we **implement** the prototypes in ST's `vl53l1_platform.h` but
we do **not** edit that header. Our `vl53l1_platform.c` is a fresh file (do NOT copy
ST's `API/platform/vl53l1_platform.c`, which is a stub/HAL impl) carrying our own
Apache SPDX header. This keeps the boundary clean: ST declares, we define.

**RESOLVED 2026-07-07 — exact ST license read.** `API/LICENSE.txt:1-6`: terms are
the `Package_license` file if one was delivered with a package, "If you received
this software component outside of a package or without applicable license terms,
the terms of the BSD OPEN SOURCE SLA0103 license shall apply"
(https://www.st.com/SLA0103). **No `Package_license` file exists anywhere in the
tree** (verified by `find` — the only license files are `API/LICENSE.txt` and the
Example-side CMSIS/HAL ones, which we don't copy) → **SLA0103 applies to the ULD
core files we take.** Record it as **"BSD Open Source SLA0103"** in CLAUDE.md §7 —
do NOT write the generic `BSD-3-Clause` SPDX tag; SLA0103 is ST's own identifier
and the honest one. ULD file headers keep their existing ST notice untouched
(they reference the LICENSE file; ship `API/LICENSE.txt` alongside the copied files).

**Do not mix headers** (CLAUDE.md §7): ULD files keep their ST notice untouched;
`vl53l1_platform.c` gets Apache SPDX only.

---

## 6. INIT SEQUENCE (sensor_task, TK_PRI 3)

All entry points below verified against `API/core/VL53L1X_api.{c,h}` in-repo
2026-07-07 (file:line cited per step). Order:

1. **XSHUT high** — VL53L1X XSHUT is tied to the 3.3 V rail (CLAUDE.md §2), so the
   sensor boots when the rail powers; no GPIO toggle needed or possible. The ULD
   core never touches XSHUT (grep over `API/`: zero hits — XSHUT is purely a board
   concern, appearing only in the Example's X-NUCLEO BSP). Recovery without XSHUT
   is specified in §6.2.
2. **Boot poll** — `tk_dly_tsk(2)` after rail-up, then poll
   `VL53L1X_BootState(dev, &state)` until `state == 1` (`VL53L1X_api.c:487-495`;
   `api.h:192-194`: "1:booted"). With XSHUT hardwired, the sensor has usually been
   powered for seconds before sensor_task runs — expect this to pass on the first
   read; poll with a bounded timeout anyway.
3. **Sensor ID sanity check** — `VL53L1X_GetSensorId(dev, &id)`
   (`VL53L1X_api.c:497-505`); **`id` must equal `0xEEAC`** (`api.h:197`). This
   single `RdWord` of a 16-bit register (`VL53L1_IDENTIFICATION__MODEL_ID` =
   0x010F, `api.h:74`) exercises the entire shim — address shift §2.1, 16-bit
   MSB-first index §2.2 (first hardware run of `regsz==2`), data byte order §2.3 —
   BEFORE any distance value is trusted. Anything but 0xEEAC = debug the shim,
   not the ranging config. **Best first-light test.**
4. `VL53L1X_SensorInit(dev)` (`VL53L1X_api.c:178-204`) — the 91-register `WrByte`
   loop (§0.1), then **internally**: `StartRanging` → poll `CheckForDataReady`
   with `VL53L1_WaitMs(dev,1)` per iteration, bounded at 1000 iterations
   (`:187-198`) → `ClearInterrupt` → `StopRanging` → two VHV config writes
   (`:199-202`). This means SensorInit blocks for one full first measurement at
   the sensor's **default** timing budget (100 ms default, `api.h:157`) — an
   init-only cost, fine under `tk_dly_tsk` (§6.3).
5. **Ranging config — ORDER MATTERS, mode before budget:**
   - `VL53L1X_SetDistanceMode(dev, 1)` → **Short** (`VL53L1X_api.c:413-446`, case 1
     at `:422-429`; `api.h:167-169`: short = max 1.3 m, better ambient immunity —
     covers the <80 cm hazard threshold with margin). Default is 2=Long.
     It reads the current timing budget first and re-programs it after the mode
     switch (`:418`, `:443-444`), so the budget survives.
   - `VL53L1X_SetTimingBudgetInMs(dev, 15)` — the function switches on the
     **current** distance mode (`VL53L1X_api.c:275-278`), and **the 15 ms budget
     exists only in the short-mode table** (`:280` — "only available in short
     distance mode"; the long-mode switch starts at 20, `:327-328`). Calling it
     with 15 while still in default Long mode returns error (default case
     `:364-366`). Hence mode first, budget second. Predefined set
     {15,20,33,50,100,200,500} (`api.h:157`).
   - `VL53L1X_SetInterMeasurementInMs(dev, 20)` (`VL53L1X_api.c:460-471`) —
     **HARD CONSTRAINT: IMP ≥ timing budget, and the API does NOT check it**
     (`api.h:179-181` verbatim: "Intermeasurement period must be >/= timing
     budget. This condition is not checked by the API"). Init code must enforce it.
   - **50 Hz decision (CLAUDE.md §6 lock):** IMP = 20 ms sets the 50 Hz cadence.
     Budget **15** (recommended) leaves 5 ms of margin under the IMP; budget 20
     satisfies the letter of `api.h:180` (`≥`, equality allowed) but with zero
     margin — any internal stretch of a ranging cycle silently pushes the
     effective period past 20 ms. The only constraint the source states is `≥`;
     any stronger rule (e.g. the "IMP ≥ budget + 4 ms" guidance seen in ST
     app-material) is **[EXTERNAL — not in this source tree]**. Short mode's 1.3 m
     ceiling is irrelevant to the 0.8 m hazard threshold. → **Short + 15 ms budget
     + 20 ms IMP.** See §7 D6.
6. `VL53L1X_StartRanging(dev)` (`VL53L1X_api.c:236-242`).
7. Ranging loop (per frame, 50 Hz):
   - Data-ready: poll `VL53L1X_CheckForDataReady(dev, &ready)`
     (`VL53L1X_api.c:252-268`) for bring-up; EXTI on **PD0/ARD_D2** for production
     (§6.1, §7 D2).
   - Read: **`VL53L1X_GetResult(dev, &result)`** (`VL53L1X_api.c:587-604`) — ONE
     17-byte `ReadMulti` returning `{Status, Distance, Ambient, SigPerSPAD,
     NumSPADs}` together (struct `api.h:94-100`). Preferred over separate
     `GetDistance` + `GetRangeStatus` (one transaction instead of two; validity
     arrives with the distance). `result.Distance` (mm) → `d(t)` feature.
   - `VL53L1X_ClearInterrupt(dev)` (`VL53L1X_api.c:206-212`) — **mandatory after
     every read** to re-arm the next data-ready event (`api.h:120-122`).

### 6.1 Data-ready: poll vs EXTI (resolves review issue B; supersedes old D2 text)

**What the ULD gives you:** its ONLY readiness primitive is a poll —
`CheckForDataReady` "checks if the new ranging data is available by polling the
dedicated register" (`api.h:150-153`). There is no callback/IRQ hook in the ULD;
interrupt mode is a pure hardware path (GPIO1 pin) that needs no ULD API beyond
`ClearInterrupt`.

**Cost of the poll path, grounded:** each `CheckForDataReady` call is **two full
I2C transactions** — `GetInterruptPolarity`'s `RdByte` of `GPIO_HV_MUX__CTRL`
plus the `RdByte` of `GPIO__TIO_HV_STATUS` (`VL53L1X_api.c:258-259`). Through our
L1 that is 2 × (DMA setup + IRQ + semaphore round-trip). Polling every 1–2 ms
tick across a 20 ms frame ⇒ up to ~20 poll calls ≈ **40 extra transactions per
frame**, plus up-to-one-tick latency jitter on frame delivery.

**What the hardware gives you:** GPIO1 fires on "new sample ready" — default
interrupt config 0x20 in the init table (`VL53L1X_api.c:86`, reg 0x46), polarity
default **active HIGH** (`api.h:126-127`: "1=active high (default)"; init table
reg 0x30 = 0x01, bit 4 = 0 = active high, `VL53L1X_api.c:64`) → **rising edge on
PD0**. `ClearInterrupt` re-arms it (`api.h:120-122`).

**EXTI path and the red zone:** the PD0 EXTI handler does **NO I2C** — it only
signals a semaphore (`tk_sig_sem`) that sensor_task (TK_PRI 3) waits on; the task
then runs `GetResult` + `ClearInterrupt`. All I2C stays in the Priority 3 task —
CLAUDE.md §3 holds. Per-frame I2C drops to exactly **2 transactions**
(17-byte read + 1-byte clear), and sampling locks to the sensor's own 20 ms
cadence instead of beating against a `tk_dly_tsk` loop.

**Recommendation (unchanged in shape, now grounded): two-stage.** Bring-up uses
the poll (fewer moving parts; isolates shim bugs from EXTI config); switch to
EXTI once ranging is proven. **Bring-up hardware check before the switch:** verify
GPIO1's idle level and pull-up situation on the 7SEMI breakout with the LA —
init-table reg 0x2F concerns the GPIO pull-up voltage domain
(`VL53L1X_api.c:63`: "bit 0 if GPIO pulled up at 1.8V, else set bit 0 to 1 (pull
up at AVDD)") and may need bit 0 = 1 for a 3.3 V-pulled breakout. Same question
applies to reg 0x2E for the I2C pads (`:62`). Evidence = LA trace of GPIO1
around a ranging cycle.

### 6.2 Recovery without XSHUT (resolves review issue C)

XSHUT is hard-wired to 3.3 V (CLAUDE.md §2): **no software power-cycle exists.**
Verified assumptions: the ULD core never references XSHUT (grep over `API/`,
zero hits), and it defines `SOFT_RESET` (reg 0x0000, `api.h:35`) **but never uses
it** — the `#define` is the only occurrence in `API/core/`. So the ULD has no
reset entry point at all; nothing in the ULD assumes a power-cycle either.
The recovery ladder is:

1. **L1 built-in:** retry + 9-pulse bus recovery inside the primitive
   (`Core/Src/app_i2c.c:365-392`, `:246-306`) — handles bus-level wedges.
2. **Sensor-level soft reset (authored by us, init-class):** raw register
   sequence via the shim — `VL53L1_WrByte(dev, SOFT_RESET, 0x00)`, brief hold,
   `VL53L1_WrByte(dev, SOFT_RESET, 0x01)`, then re-run the FULL init chain from
   step 2 (BootState poll → SensorInit → mode → budget → IMP → StartRanging).
   The 0x00/0x01 sequence and hold time are **[EXTERNAL — verify against the
   VL53L1X datasheet soft-reset section before implementing; not derivable from
   this source tree]**. Side effect: a soft reset returns the I2C address to the
   default 0x52 — irrelevant here since we never change it (single sensor).
3. **Manual power cycle** (bench) — last resort.

Item 2 runs only in sensor_task, only outside the frame loop (it costs a full
re-init including SensorInit's first-measurement wait, §6.3).

### 6.3 Blocking/WaitMs budget (resolves review issue A)

**Complete `VL53L1_WaitMs` inventory for the ULD tree — four call sites, all
`WaitMs(dev, 1)`, none on the frame path:**

| Call site | Function | Class |
|---|---|---|
| `VL53L1X_api.c:197` | `SensorInit` data-ready poll loop | ONE-TIME INIT |
| `VL53L1X_api.c:816` | `StartTemperatureUpdate` poll loop | OCCASIONAL MAINTENANCE (init-class) |
| `VL53L1X_calibration.c:47` | `CalibrateOffset` (50-sample loop) | BENCH CALIBRATION (out of scope, §9) |
| `VL53L1X_calibration.c:88` | `CalibrateXtalk` (50-sample loop) | BENCH CALIBRATION (out of scope, §9) |

**The per-frame path — `CheckForDataReady` (`:252-268`), `GetResult` (`:587-604`),
`ClearInterrupt` (`:206-212`) — contains ZERO `WaitMs` calls.** Verified by grep
plus reading each function body. No per-frame call can hit a long wait; the ~20 ms
frame budget is threatened only by I2C transaction count (§6.1), not by delays.

**Longest waits, located:** the `SensorInit` internal loop (`:188-198`) and the
identical `StartTemperatureUpdate` loop (`:807-817`) — each blocks for one first
measurement at the then-current timing budget (default 100 ms at SensorInit time,
`api.h:157`) and is bounded by a 1000-iteration timeout returning
`VL53L1X_ERROR_TIMEOUT` (`:192-196`). Each iteration ≈ `tk_dly_tsk(1)` (1–2 ms
with the 1 ms kernel tick, `CNF_TIMER_PERIOD 1`, `mtk3_bsp2/config/config.h:29`)
plus two `RdByte` transactions ⇒ worst-case timeout on the order of seconds —
acceptable, because both run **before/outside the frame loop** in sensor_task,
where `tk_dly_tsk` sleeps the task and priorities 1–2 preempt freely.
`StartTemperatureUpdate` is recommended by ST only after ≥8 °C drift following
extended idle (`api.h:341-345`) — if we ever schedule it, it is an init-class
operation (pause frame loop, run it, resume), never inline in a frame.

### 6.4 Feature-pipeline tie-in (CLAUDE.md §6)

`GetResult` yields `result.Distance` in mm — the input to the locked 15-feature
vector `[d(t)…d(t−9), v, a, ax, ay, az]`. Sampling target 50 Hz (CLAUDE.md §6),
set by **Short mode + 15 ms budget + 20 ms IMP** per step 5 above.

Use `result.Status` to gate frame validity: only feed `d(t)` into the vector when
Status indicates a valid range (0 = no error, 1 = sigma fail, 2 = signal fail,
7 = wrap-around — `api.h:233-235`; the raw device status is remapped through the
`status_rtn[]` table, `VL53L1X_api.c:154-157`, applied in `GetResult` at
`:594-597`). Feeding an invalid range into the velocity/acceleration derivatives
corrupts `v` and `a`.

---

## 7. DECISIONS (updated 2026-07-07 — most are now settled by source)

**D1 — Cache maintenance ownership. SETTLED:** Option A is already implemented in
the primitive (`app_i2c.c:349-353`). Two latent D-cache items carried in §4.

**D2 — Data-ready: interrupt vs poll.** Superseded by §6.1 — **two-stage: poll
for bring-up, EXTI (PD0, rising edge) for production.** Now grounded: poll costs
2 I2C transactions per check (`VL53L1X_api.c:258-259`); EXTI cuts the frame to
2 transactions total and the ISR never touches I2C (red zone preserved).
Pre-switch hardware check: GPIO1 idle level / pull-up voltage (reg 0x2F, §6.1).

**D3 — `WaitMs` granularity. SETTLED:** kernel tick is 1 ms (`CNF_TIMER_PERIOD 1`,
`mtk3_bsp2/config/config.h:29` — same citation the committed `app_i2c.c:279-280`
comment carries). `tk_dly_tsk(1)` = 1–2 ms; the ULD only ever asks for 1 ms
(§6.3, all four call sites) inside bounded init-class loops. Harmless.

**D4 — Where does the 8-bit→7-bit shift live.** In the shim (recommended, §2.1) vs
storing a pre-shifted 7-bit constant and ignoring `dev`.
→ Recommend **shift in shim** so multi-sensor address changes (if ever) still work
through the ULD's `SetI2CAddress` (`VL53L1X_api.c:170-176`). Ignoring `dev`
hardcodes the address and breaks that path.

**D5 — Return-code mapping.** Single sentinel vs propagating distinct error codes.
→ Recommend **single nonzero sentinel** — the ULD only tests `!= 0` / ORs into a
`uint8_t` (`api.h:28`); richer codes add no value. Avoid 253/254/255 (§3).

**D6 — Timing budget for 50 Hz: 15 ms (recommended) vs 20 ms.** Developer choice,
both legal per source (`api.h:179-181` only requires IMP ≥ budget):
- **Option A — 15 ms budget + 20 ms IMP** (recommended): 5 ms margin under the
  cadence; 15 exists only in short mode (`VL53L1X_api.c:280`), which we use anyway.
- **Option B — 20 ms budget + 20 ms IMP**: more integration time (SNR at range),
  but zero margin — equality is allowed by the docstring, and any cycle stretch
  silently degrades the 50 Hz cadence. The "+4 ms" IMP guidance sometimes cited
  is external to this source tree and unverified here.
At 0.8 m in short mode, 15 ms is expected to be ample; if bring-up shows noisy
`Status`/sigma at 15, switching to Option B is a two-line change (budget + IMP
stay 20/20) — or 15/25 at 40 Hz would need the feature-vector contract reopened
(locked, CLAUDE.md §6 — do NOT go there unilaterally).

---

## 8. GATE-1 CHECKLIST — CLOSED OUT 2026-07-07 (source pass)

All source-derivable items are RESOLVED with citations; what remains is hardware
or datasheet work, listed at the bottom.

1. ~~`app_i2c.c` `regsz==2` byte order~~ **RESOLVED — MSB-first via HAL**
   (`stm32n6xx_hal_i2c.c:3117-3120` write, `:3297-3300` read; §2.2).
2. ~~8-bit vs 7-bit `dev` in ULD core~~ **RESOLVED — core passes `dev` opaquely;
   only shift is `SetI2CAddress` writing `new_address>>1` to the device
   register** (`VL53L1X_api.c:174`; §2.1).
3. ~~cache invalidate/clean in primitive~~ **RESOLVED — read-side invalidate
   present** (`app_i2c.c:349-353`); write-side clean absent but inert with
   D-cache off — carried as latent items in §4.
4. ~~SensorInit block vs loop~~ **RESOLVED — WrByte loop, 91 × 1 byte; max ULD
   transfer = 17-byte `GetResult` read** (`VL53L1X_api.c:184-186`, `:590-593`; §3.1).
5. ~~Multi-byte data endianness~~ **RESOLVED — big-endian, confirmed against
   `Example/Src/vl53l1_platform.c:144-145,161-164,228,252`** (§2.3).
6. ~~Ranging-API names/sigs~~ **DONE** (§6, all cited).
7. ~~Exact ST LICENSE~~ **RESOLVED — BSD Open Source SLA0103**
   (`API/LICENSE.txt:1-6`, no `Package_license` in tree; §5). Record in
   CLAUDE.md §7 at commit time.
8. ~~µT-Kernel tick period~~ **RESOLVED — 1 ms** (`config.h:29`; §7 D3).
9. **Fix the 88 KB error in `docs/PHASE5_DESIGN_sensor_bringup_i2c.md` — STILL
   OPEN** (§0.1 has the corrected facts to paste).

**Remaining opens (hardware / datasheet, not source):**

- **H1 — First `regsz==2` hardware proof:** `GetSensorId` == 0xEEAC at first
  light (§6 step 3). The L1 gate test only exercised `I2C_REG8` (`app_i2c.c:435`).
- **H2 — GPIO1 idle level / pull-up voltage on the 7SEMI breakout** before the
  EXTI switch; possibly reg 0x2F (and 0x2E) bit 0 = 1 for AVDD-level pull-ups
  (`VL53L1X_api.c:62-63`; §6.1). Evidence = LA trace.
- **H3 — Soft-reset register sequence + hold time** from the VL53L1X datasheet
  before implementing recovery step 2 (§6.2). The ULD defines `SOFT_RESET`
  (`api.h:35`) but never uses it.

---

## 9. WHAT THIS DOC DELIBERATELY DOES NOT DO

- No implementation code (per instruction — design only).
- No calibration/xtalk port (`VL53L1X_calibration.c` copied verbatim, unused until
  offset calibration is needed; out of Phase 5 core scope — its two `WaitMs`
  sites are inventoried in §6.3 anyway).
- No EXTI register-level wiring detail (§6.1/D2 defer the EXTI switch to
  post-bring-up; the design constraint — ISR signals only, no I2C — is stated).
- No multi-sensor address arbitration (single VL53L1X on the bench).
