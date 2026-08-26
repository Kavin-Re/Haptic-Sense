# PHASE5_L2 — VL53L1X ULD Platform Port Design (v2)

Haptic-Sense · TRON Forum Contest 2026 · Review pass 2026-07-09 (chat/Opus)
Supersedes the Gate-1 design in the areas below; everything not restated here carries over unchanged.

**Review inputs (this pass):** `app_i2c.c` (uploaded, full 449 lines), `vl53l1_platform.h` / `vl53l1_platform.c` / `vl53l1_types.h` (uploaded STSW-IMG009 templates, unfilled).
**NOT available this pass:** `app_i2c.h` (⇒ `I2C_REG16` literal value unverifiable), `i2c_timing.h` (⇒ `I2C_BUS_HZ` unverifiable), in-repo `API/core/VL53L1X_api.c/.h`, VL53L1X/VL53L5CX datasheet PDFs (announced but not attached — closed via live lookups instead, sources in §9).

**Convention:** `[V-n]` = named verification step, listed in §8. Every hardware/technical claim carries either a source citation (§9 key in braces, e.g. `{DS}`) or an `[UNVERIFIED → V-n]` tag.

---

## 1. H1 RESOLVED (source level) — register-index byte order

### 1.1 What the sensor requires
- The VL53L1X register index is **16-bit** — datasheet: "the second byte received provides a 16-bit index, which points to one of the internal 8-bit registers" `{DS}`.
- The index goes on the wire **MSB first**. Grounding: ST's own reference `vl53l1_platform.c` (as reproduced verbatim on the ST community, ST staff engaged in-thread) serializes `_I2CBuffer[0] = index >> 8; _I2CBuffer[1] = index & 0xFF;` `{ST-PLAT}`. The identical rule is stated by ST staff for the sibling VL53L5CX: "First Register (… registers are 16 bit addresses)" with `data[0] = Reg >> 8` `{ST-L5-PLAT}`.

### 1.2 What the primitive emits — trace through `app_i2c.c`
1. `i2c_rd/i2c_wr(dev7, reg, regsz, buf, len)` → `i2c_xfer` → `i2c_xfer_once` (app_i2c.c:394-402, 365-392, 312).
2. `i2c_xfer_once` maps size **symbolically**: `memaddsz = (regsz == I2C_REG16) ? I2C_MEMADD_SIZE_16BIT : I2C_MEMADD_SIZE_8BIT` (app_i2c.c:316-317). **The primitive never compares against the literal 2** — it compares against the symbol `I2C_REG16`.
3. The mapped size is passed to `HAL_I2C_Mem_Read_DMA` / `HAL_I2C_Mem_Write_DMA` (app_i2c.c:327-333). Byte order is therefore **entirely delegated to ST HAL**.
4. ST HAL (I2C-v2 peripheral family, DMA Mem path): for `I2C_MEMADD_SIZE_16BIT` the driver prefetches `TXDR = I2C_MEM_ADD_MSB(MemAddress)` and stages `Memaddress = I2C_MEM_ADD_LSB(...)` for the follow-up TXIS interrupt — i.e. **MSB first, then LSB**; the blocking/IT paths (`I2C_RequestMemoryWrite/Read`) do the same ("Send MSB of Memory Address" … "Send LSB of Memory Address") `{HAL-DMA}` `{HAL-BLK}`. Cited source is the STM32H7 HAL (same I2C-v2 IP family as N6). **N6-file confirmation is one grep** `[UNVERIFIED for the N6 file specifically → V-1]`.

### 1.3 Verdict and the one design rule it produces
Sensor requires 16-bit MSB-first; HAL emits 16-bit MSB-first; the primitive is a transparent pass-through. **Match — H1 is resolved at source level, pending V-1 (grep) and V-5 (wire capture).**

**⚠ RULE (bold constraint): the shim must pass the symbol `I2C_REG16`, never the literal `2`.** The primitive's dispatch is `regsz == I2C_REG16`; if a literal `2` is passed and `I2C_REG16` is not defined as 2, the comparison silently falls to the 8-bit branch — only the index LSB is emitted, the sensor ACKs anyway (register-mapped slaves ACK any index, per the gate-test analysis at app_i2c.c:414-420), and every read returns wrong data with **no error**. `app_i2c.h` was not uploaded, so `I2C_REG16`'s value is `[UNVERIFIED → V-2]`. Using the symbol makes V-2 moot for correctness (it then only matters for documentation).

---

## 2. ADDRESS SHIM — verified

- ULD `dev` parameter carries the **8-bit** I2C address; the device default is **0x52** — datasheet: "uses a default device address of 0x52" `{DS}`; 7-bit equivalent 0x29 = 0x52 ≫ 1 (corroborated: community/ST staff describe 0x52 as the write address, 0x29 as the 7-bit form) `{ADDR}`.
- The primitive takes a **7-bit** address and re-shifts left at the HAL boundary: `(uint16_t)(dev7 << 1)` at app_i2c.c:327, 331, 342 (verified in uploaded source).
- Therefore the shim performs **exactly one right shift, in exactly one place** — the entry of each platform function:

  `dev(8-bit, 0x52) ── shim ≫1 ──> dev7(0x29) ── primitive ≪1 ──> 0x52/0x53 on the wire` ✓

  Arithmetic check: 0x52 ≫ 1 = 0x29; 0x29 ≪ 1 = 0x52 (write), | 1 = 0x53 (read, set by hardware from the direction bit).
- **Failure mode if omitted** (the known highest-risk line): passing `dev = 0x52` straight through as a 7-bit address makes the primitive emit `0x52 << 1 = 0xA4` — a NACK on every transaction, indistinguishable at the ULD level from an unpowered sensor.
- Derive `dev7` from the `dev` argument on **every call** — do not hard-code 0x29 — so `VL53L1X_SetI2CAddress` remains usable later `{ADDR}`. (Single sensor at default address is the locked project configuration; this is future-proofing, not scope creep.)

---

## 3. SECOND ENDIANNESS HAZARD — data words (raised proactively, same severity class as H1)

H1 covers the index bytes. `VL53L1_WrWord/WrDWord/RdWord/RdDWord` additionally move **multi-byte data**, and the Cortex-M55 is little-endian while the VL53L1X stores multi-byte values MSB-at-lower-address: the identification word read at 0x010F is 0xEACC, composed of MODEL_ID 0x010F = 0xEA (the MSB) followed by 0x0110 = 0xCC `{ID}` `{UM2510}`. ST's own template warns exactly here: "fields 'RegisterAdress' and 'value' need to be swapped" for mismatched endianness (uploaded `vl53l1_platform.c`, every function body).

**⚠ RULE: never `memcpy`/pointer-cast a `uint16_t`/`uint32_t` into the I2C buffer. Serialize explicitly, MSB first:**

```c
/* WrWord */  b[0]=(UB)(data>>8);  b[1]=(UB)data;
/* WrDWord */ b[0]=(UB)(data>>24); b[1]=(UB)(data>>16); b[2]=(UB)(data>>8); b[3]=(UB)data;
/* RdWord */  *pdata = ((uint16_t)b[0]<<8) | b[1];
/* RdDWord */ *pdata = ((uint32_t)b[0]<<24)|((uint32_t)b[1]<<16)|((uint32_t)b[2]<<8)|b[3];
```

This construction is endian-neutral C — correct regardless of host byte order, no `#ifdef` needed.

---

## 4. SHIM SPECIFICATION — `vl53l1_platform.c` (target file), all functions

Context header per CLAUDE.md §3/§7: every function `// ONLY CALL FROM PRIORITY 3 SENSOR TASK`; SPDX Apache-2.0 on our file **only if written clean-room** — the STSW-IMG009 template carries an ST license header; either keep ST's header and fill the bodies (component stays under ST's terms), or write a fresh file against the `API/platform/vl53l1_platform.h` contract under Apache-2.0. **Do not mix headers** (CLAUDE.md §7). License identifier check remains open (`SLA0103` vs `API/LICENSE.txt` body — pre-existing item, unchanged).

| ULD platform fn | Maps to | Notes |
|---|---|---|
| `VL53L1_WriteMulti(dev,index,pdata,count)` | `i2c_wr(dev>>1, index, I2C_REG16, pdata, count)` | ULD `pdata` used directly for TX DMA — see §5 buffer rules |
| `VL53L1_ReadMulti(dev,index,pdata,count)` | `i2c_rd(dev>>1, index, I2C_REG16, pdata, count)` | RX DMA into ULD buffer — see §5 (cache hazard note) |
| `VL53L1_WrByte` | `i2c_wr(..., &data, 1)` | via 1-byte static buffer or `&data` (stack OK — transfer is synchronous, buffer live until semaphore returns) |
| `VL53L1_WrWord` / `VL53L1_WrDWord` | serialize per §3 into static `xfer_buf`, then `i2c_wr(..., xfer_buf, 2/4)` | |
| `VL53L1_RdByte` | `i2c_rd(..., pdata, 1)` | |
| `VL53L1_RdWord` / `VL53L1_RdDWord` | `i2c_rd(..., xfer_buf, 2/4)`, then reassemble per §3 | |
| `VL53L1_WaitMs(dev,wait_ms)` | `tk_dly_tsk((RELTIM)wait_ms)` | µT-Kernel API only; kernel tick 1 ms ⇒ actual delay rounds up ≤ +1 tick (same rounding already documented at app_i2c.c:277-281). Never `HAL_Delay` (blocking-HAL red-zone). |

- **Error mapping:** return `0` on `E_OK`, else a nonzero `int8_t` (suggest `-13`, matching ST's own community-shown convention `{ST-PLAT}`; note the template's `status = 255` in an `int8_t` is just −1). The ULD treats any nonzero as failure — no finer granularity is consumed upstream. Do not retry in the shim; `i2c_xfer` already owns the one-retry + bus-recovery policy (app_i2c.c:357-392) and a second retry layer would multiply the bounded worst case.
- **`count` width:** ULD `count` is `uint32_t`; the primitive's `len` is `UW`, but app_i2c.c:329/333 casts to `uint16_t` for HAL. VL53L1X ULD transfers are tens of bytes at most (largest is the multi-register result block), so no truncation in practice — assert or comment the ≤65535 assumption. (Contrast: this cast **would** matter for the VL53L5CX upgrade path and its 32 KB `WrMulti` chunks — flag now, act never, per §7.)
- **Contract source:** implement against the uploaded template signatures (`uint16_t dev, uint16_t index, ...`), which the project has designated as the `API/platform/` authoritative contract; `Example/Inc/vl53l1_platform.h` (HAL-bound demo stub) stays excluded from the build `[assumed per locked project decision; V-3 confirms the uploaded files are byte-identical to in-repo API/platform/]`. The `VL53L1_Dev_t {uint32_t dummy}` struct in the header is dead weight for the ULD call path (all calls pass `uint16_t dev`) — keep for contract fidelity, never instantiate.

---

## 5. DMA / CACHE / TASK-CONTEXT CONSTRAINTS (inherited, restated for L2)

- **All nine platform functions run only in sensor_task (TK_PRI 3).** The ULD core is called only from that task, so the primitive's single-client tripwire (`i2c_busy`, app_i2c.c:369-376) is satisfied by construction. If the DRV2605L init later shares the bus, it also runs in TK_PRI 3 context (CLAUDE.md §2) — same client, no conflict.
- **Static buffers only** (`USE_IMALLOC=0`): one file-static `xfer_buf[8]`, 32-byte aligned and padded per the design §4.1 rule already applied to `gate_buf` (app_i2c.c:64-67), covers all Word/DWord bounces.
- **Cache:** D-cache is OFF in this build (app_i2c.c:66, 349-353), and `i2c_xfer_once` already invalidates after reads — inert today. **Future D-cache enable hazard:** `ReadMulti` DMA-writes into ULD-owned buffers that are not cache-line aligned/padded; `SCB_InvalidateDCache_by_Addr` on such a buffer can corrupt adjacent data sharing the line. Options for that day, decision deferred:
  - **Option A** — bounce all `ReadMulti` through a static aligned buffer + `memcpy` out (cost: one copy of ≤ tens of bytes per frame; simplest, matches the existing alignment rule).
  - **Option B** — audit/align the ULD's result buffers (touches ST-licensed core or wraps it; more fragile).
  Record as a Red-Zone-3-adjacent note in CLAUDE.md when D-cache work starts.
- **Timing sanity:** the bus must not exceed the sensor's 400 kHz maximum `{DS}`. `I2C_BUS_HZ` lives in `i2c_timing.h`, not uploaded `[UNVERIFIED → V-4]`.
- Worst-case per-call latency through the shim is the primitive's bounded ~145 ms failure path (app_i2c.c:357-362); SensorInit's WrByte loop multiplies that only under sustained bus failure, which the caller already treats as init failure, not a hang.

---

## 6. FIRST-LIGHT TEST PLAN — LOCKED (with corrected target)

**★ CORRECTION: the expected `GetSensorId` value is `0xEACC`, not `0xEEAC`.** UM2510: "This function returns the sensor ID which must be 0xEACC" `{UM2510}`. `0xEEAC` (the value previously recorded) appears in *later*-generation ULD code comments and has caused documented confusion — an ST community thread (Jan 2025) reports a genuine VL53L1X returning 0xEEAA against a code comment claiming 0xEEAC, with ST staff acknowledging the family's docs are inconsistent across L1X/L1CB/L3/L4CD silicon iterations `{ID-VAR}`. Consequence for the plan: **log the word, don't hard-fail on it** — see step L3. In-repo tiebreaker: the doc comment on `VL53L1X_GetSensorId` in our v3.5.5 `API/core` copy `[→ V-6]`.

Sequence (each step names its pass criterion and what a failure isolates):

- **L0 — pre-flight (no code changes):** VL53L1X wired to I2C1 (PH9/PC1), XSHUT tied to the 3.3 V rail — datasheet requires XSHUT always driven `{DS}`; tie-high satisfies this and deliberately forfeits hardware-standby/multi-sensor address assignment (accepted: single sensor, default address). GPIO1 → PD0 per CLAUDE.md §2, unused at first light.
- **L1 — raw-primitive probe (bypasses the shim entirely):** `i2c_rd(0x29, 0x010F, I2C_REG16, buf, 1)` from the existing gate-test slot. Expect `buf[0] == 0xEA` (MODEL_ID at 0x010F) `{ID}`. Passing proves: address shift constant, 16-bit index emission, DMA/IRQ/semaphore chain against a real ACKing slave. Failing with NACK isolates address/wiring; completing with wrong data isolates index byte order → go straight to L5's capture.
- **L2 — boot gate:** through the shim, poll `VL53L1X_BootState` until 1 (UM2510: 1 = booted) `{UM2510}`. Do not encode an absolute boot-time number — poll with `VL53L1_WaitMs` between attempts and a bounded attempt count.
- **L3 — identity:** `VL53L1X_GetSensorId`. **Pass = 0xEACC** `{UM2510}`. If a stable other value appears (e.g. 0xEEAA-class per `{ID-VAR}`): the *bus and shim are proven* by stability + the L1 result; log the word, record the module marking, and resolve against the in-repo API comment (V-6) before amending the pass value. 0x0000/0xFFFF = bus-level failure, not an ID variant.
- **L4 — function:** `SensorInit` → `StartRanging` → poll `CheckForDataReady` → read → `ClearInterrupt` (mandatory before the next datum per UM2510's flow: "a clear interrupt is required after getting ranging data") `{UM2510}` → `StopRanging`. Pass = plausible mm values that track a hand moved in front of the sensor.
- **L5 — H1 hardware closure (V-5):** logic-analyzer capture (24 MHz sigrok clone, PulseView I2C decoder) of one `WrByte`. Expected wire bytes: `[0x52+W] [idx MSB] [idx LSB] [data]`, e.g. any 0x01xx-register write shows `0x52 0x01 xx dd`. This single capture closes H1 with hardware evidence and doubles as contest documentation.
- **L6 — D-cache enablement gate (added 2026-08-26):** Once VL53L1X is streaming correctly with D-cache OFF and readings are validated, set `USE_DCACHE` in `app_config.h:21`, rebuild, reflash, and confirm identical sensor readings. Any divergence is a cache-coherency defect in the I2C DMA path (see `PROJECT_DEFENSE.md` §2.2, L1-10). Resolve before adding a second device to the bus. Rationale: one sensor, known-good baseline, single variable — the cheapest environment in the project to find this class of bug. Deferring to Phase 6 means debugging coherency, NPU deployment, and integration at once.

**Read API for the 50 Hz pipeline: `VL53L1X_GetResult` over `VL53L1X_GetDistance` — confirmed as design intent, existence pending V-7.** Rationale: the per-frame cost is transaction count, and each transaction costs one full semaphore round-trip through `i2c_xfer` plus I2C framing overhead (per transaction ≈ (addr + 2 index + repeated-start + addr + N data) × 9 bits ÷ f_SCL). `GetResult` returns status + distance (+ signal metrics) from one register block read; the `GetDistance` route needs separate `GetRangeStatus` + `GetDistance` transactions for the same decision inputs. At 50 Hz (20 ms budget) both fit comfortably — the win is margin and fewer preemption windows, not feasibility. `GetResult` is not listed in the UM2510 revision consulted; it was added to the ULD after the manual's initial release `[UNVERIFIED for v3.5.5 specifically → V-7]`. Fallback if absent: `GetRangeStatus` + `GetDistance`, or a direct `ReadMulti` of the result block modeled on the API source — decide only after V-7.

Hardware-gated items H2 (XSHUT/INT pin behavior) and H3 (polling timing at 50 Hz) remain open and are exercised by L0/L4 respectively; H2's INT half (PD0 edge behavior, future EXTI migration) is out of first-light scope by design — first light polls.

---

## 7. CORRECTIONS REGISTER — Gate-1 item 9 closed, plus new findings

1. **Sibling design doc (Gate-1 item 9): delete the "88 KB firmware upload" claim from every VL53L1X context.** The VL53L1X has **no firmware upload at all** — its ULD `SensorInit` writes a short default-configuration table via a `WrByte` loop `{UM2510-scope}`. The firmware-upload behavior belongs exclusively to the VL53L5CX family, whose sensor stores firmware in volatile RAM and requires host upload at every power-on `{L5-FW}`.
2. **The transplanted number is also wrong for the VL53L5CX.** UM2884: `vl53l5cx_init` "copies the firmware (~84 kbytes)" over I2C `{UM2884}`; the ULD source uploads three chunks of 0x8000 + 0x8000 + 0x5000 = 0x15000 = 86,016 bytes = exactly 84 KiB `{L5-CODE}`. **Action: CLAUDE.md §2 (onboard upgrade path line) — change "~88 KB firmware upload" → "~84 KB (86,016 B) firmware upload".**
3. **CLAUDE.md "Key learnings" / first-light target: `0xEEAC` → `0xEACC`** `{UM2510}`, with the log-don't-hard-fail caveat and `{ID-VAR}` note from §6-L3.
4. **The "~135 bytes" SensorInit figure is `[UNVERIFIED → V-8]` and probably wrong or mis-scoped.** Recollection (explicitly labeled as memory, not fact): the loop spans registers 0x2D–0x87 = 91 configuration bytes; at 3 wire bytes per WrByte (2 index + 1 data) that is ~273 bytes on the wire — neither number is 135. V-8 (one grep) settles it; until then, record the claim as "a short WrByte loop over the default configuration table (exact count per in-repo `VL53L1X_api.c`)" and drop any specific byte figure from prose.

---

## 8. VERIFICATION LEDGER

| ID | Claim gated | Verification step (named, concrete) | Type |
|---|---|---|---|
| V-1 | N6 HAL emits 16-bit memaddr MSB-first (cited today from same-IP H7 source) | `grep -n "I2C_MEM_ADD_MSB" Drivers/STM32N6xx_HAL_Driver/Src/stm32n6xx_hal_i2c.c` in the project tree; confirm MSB precedes LSB in both `I2C_RequestMemory*` and the `Mem…DMA` prefetch | desk, pre-flash |
| V-2 | `I2C_REG16` numeric value (moot if shim uses the symbol — §1.3 rule) | read `app_i2c.h` | desk |
| V-3 | Uploaded platform templates ≡ in-repo `API/platform/` contract | `diff` uploaded files vs `API/platform/` in STSW-IMG009 v3.5.5 (commit 2dcd060) | desk |
| V-4 | `I2C_BUS_HZ` ≤ 400 kHz `{DS}` | **CLOSED 2026-07-12:** `app_i2c.h:15` `#define I2C_BUS_HZ 400000u` (constant lives in `app_i2c.h`, not `i2c_timing.h`; wire-level capture remains — PROJECT_DEFENSE.md BUS-2) | done |
| V-5 | **H1 on hardware** — index MSB-first on the wire | §6-L5 logic-analyzer capture of one WrByte | hardware |
| V-6 | v3.5.5's own documented sensor-ID value | **EXECUTED 2026-07-12:** in-repo `API/core/VL53L1X_api.h:197` says 0xEEAC — conflicting with UM2510's 0xEACC, confirming ST doc drift ({ID-VAR}). Resolution = this doc's §6-L3 log-don't-hard-fail rule, now harmonized into v1 §6 step 3 (PROJECT_DEFENSE.md A-1) | done |
| V-7 | `VL53L1X_GetResult` exists in v3.5.5 | `grep -n "VL53L1X_GetResult" API/core/VL53L1X_api.h` | desk |
| V-8 | SensorInit config-write loop bounds / byte count | read `SensorInit` + `VL51L1X_DEFAULT_CONFIGURATION` in in-repo `VL53L1X_api.c` | desk |
| H2 | XSHUT tie-high behavior; PD0/INT edge | §6-L0 + later EXTI phase | hardware |
| H3 | 50 Hz polling timing budget | §6-L4 with `tk_get_otm()` instrumentation | hardware |

Desk items V-1…V-4 and V-6…V-8 are all pre-wiring; **complete them before soldering anything** — they are eight greps/diffs and they de-risk the only two silent-failure modes left (index byte order, symbol-vs-literal regsz).

## 9. SOURCES

| Key | Source |
|---|---|
| {DS} | VL53L1X datasheet, st.com/resource/en/datasheet/vl53l1x.pdf — 400 kHz max, default address 0x52, 16-bit index, XSHUT always driven |
| {UM2510} | UM2510 "A guide to using the VL53L1X ultra lite driver", st.com — sensor ID 0xEACC, BootState semantics, clear-interrupt-after-data flow |
| {UM2510-scope} | UM2510 — ULD scope ("only four files", turnkey init; no firmware image) |
| {HAL-DMA} | ST community (STM32H7A3 I2C thread) quoting `HAL_I2C_Mem_*_DMA`: 16-bit case prefetches `I2C_MEM_ADD_MSB`, stages LSB via `Memaddress` for `I2C_Mem_ISR_DMA` |
| {HAL-BLK} | ST community threads quoting `I2C_RequestMemoryWrite/Read`: "Send MSB of Memory Address" then "Send LSB" |
| {ST-PLAT} | ST community "VL53L1X problem with write value to register" — ST reference `vl53l1_platform.c` WrByte: `index>>8` then `index&0xFF`; error convention |
| {ST-L5-PLAT} | ST community "I2C multi-byte read and write functions" (ST staff) — 16-bit register, MSB-first serialization for HAL |
| {ADDR} | ST community "VL53L1X address changing" — 0x52 write / 0x53 read / 0x29 7-bit equivalence; SetI2CAddress flow |
| {ID} | ST community "vl53l1" thread — MODEL_ID 0x010F = 0xEA, 0x0110 = 0xCC |
| {ID-VAR} | ST community "VL53L1X and VL53L4CD GetSensorId" (Jan 2025) — 0xEEAA observed on real L1X, 0xEEAC in code comments, ST staff: docs inconsistent across family |
| {L5-FW} | Adafruit_VL53L5 README + ST community — VL53L5CX firmware in volatile RAM, host uploads ~84 KB at every power-on |
| {UM2884} | UM2884 (VL53L5CX ULD guide) — `vl53l5cx_init` "copies the firmware (~84 kbytes)" over I2C |
| {L5-CODE} | VL53L5CX ULD init source (community-reproduced) — chunks 0x8000+0x8000+0x5000 = 0x15000 = 86,016 B |
