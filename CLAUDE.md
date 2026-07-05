# CLAUDE.md — Haptic-Sense (TRON Forum Contest 2026)

AI-driven predictive spatial-awareness wearable for visually impaired users.
Hard deadline: **September 25, 2026**. Benchtop prototype is the deliverable — NOT a wearable enclosure.
Developer: solo BTech student, **zero prior experience** in RTOS, ML training, FSBL/TrustZone boot, NPU deployment. Explain new µT-Kernel concepts with a FreeRTOS/real-world analogy first, then the exact API, then where the same pattern appears in the reference repo.

**PHASE STATUS (2026-07-05):** Phase 3 COMPLETE, verified on hardware — minimal LED+UART heartbeat (commit `7a87671`) runs from Flash Boot. UART shows `[HB]` counter at steady ~503 ms period (500 ms `tk_slp_tsk` + overhead) over 27+ samples; `tk_get_otm()` uptime monotonic and sane; no `[CAMERA_INIT]` in the boot trace (camera-pipeline strip confirmed; binary 682 KB → 63 KB). `DEVCNF_USE_HAL_IIC` still `0` — the flip to `1` is the FIRST action of Phase 5.

---

## 1. TARGET & TOOLCHAIN (verify before first build)

- Board: **STM32N6570-DK** (MB1939) — STM32N657X0HXQ, Cortex-M55 @ 600 MHz + Ethos-U55/Neural-ART NPU
- App executes from **XSPI RAM** (no internal flash execution) — linker script `STM32N657X0HXQ_LRUN.ld`
- RTOS: **µT-Kernel 3.0** (T-Kernel family). Lower integer = higher priority.
- Host: Linux Mint. IDE: STM32CubeIDE **≥ 1.19.0**. Programmer: STM32CubeProgrammer **≥ 2.20.0** (older = wrong Cortex-M55 GCC flags — RED ZONE)
- Compile flags (required): `-mcpu=cortex-m55 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -mcmse`
- NeuralART define: `#define LL_ATON_PLATFORM LL_ATON_PLAT_STM32N6`
- Debug UART: USART1, PE5 (TX) / PE6 (RX), STLINK VCP, 115200 baud
- POWER: with camera module attached, Type-A→C USB cannot power the board (~550 mA limit, boot failure). Use C-to-C cable or powered source.

## 2. HARDWARE MAP (LOCKED — schematic-verified MB1939 rev, Nov 2024)

### I2C1 — the ONLY sensor bus. PH9 = SCL, PC1 = SDA. Onboard 1.5 kΩ pull-ups (R153/R149).
**NEVER suggest external pull-up resistors. NEVER put sensors on I2C2** (PD4/PD14 — occupied by LCD touch 0x5D, audio codec, STLINK).

| Device | 7-bit addr | Breakout | Extra pins |
|---|---|---|---|
| VL53L1X ToF (primary) | 0x29 | 7SEMI | XSHUT → 3.3 V rail; GPIO1 (INT) → **PD0** (ARD_D2) |
| MPU6050 IMU (primary) | 0x68 (verify AD0 by bus scan; 0x69 if high) | GY-521 | INT → **PE9** (ARD_D3) |
| DRV2605L haptic driver | 0x5A | Adafruit ×1 + SmartElex ×2 | EN → **PE7** (ARD_D8) |

### Arduino header pin map (subset in use)
D15/PH9=SCL · D14/PC1=SDA · D8/PE7=DRV_EN · D7/PD6=**TIMING_D1** · D4/PH5=**TIMING_D0** · D3/PE9=IMU_INT · D2/PD0=TOF_INT

### Onboard LED (HARDWARE-CONFIRMED 2026-07-05, Phase 3)
LD1 = **PO1, active HIGH**. Port O carries XSPI1 (PSRAM) on PO0/PO2/PO3/PO4 — the memory the app executes from. **Configure ONLY PO1, pin-masked calls only**: `HAL_GPIO_Init` (masked RMW) + `HAL_GPIO_TogglePin` (atomic BSRR) confirmed working on hardware with XSPI1 untouched. Never `GPIO_PIN_All` / port-wide writes on port O. Verified against source: `__HAL_RCC_GPIOO_CLK_ENABLE()` exists (`stm32n6xx_hal_rcc.h:981`); `GPIOO` resolves to secure alias `GPIOO_S` (correct for this TrustZone build). LD2 (red, PG10, active LOW) may indicate BOOTFAILEDN — leave untouched.

### Actuator
ERM coin 10 mm × 3.4 mm (3 V class) driven ONLY through DRV2605L. **NEVER connect any motor to GPIO directly** — GPIO abs max ~20 mA, ERM draws 60–90 mA. Priority 1 task touches EN/GPIO only; DRV2605L I2C configuration happens at init from the Priority 3 task context.

### Onboard upgrade path (documented, OFF critical path)
MB1854B camera module (present in kit) carries VL53L5CX @ 0x29 + ISM330DLC (0x6A/0x6B) on I2C1 via FFC CN14. GPIOs: TOF_INT=PQ0, TOF_LPn=PQ5, IMU_INT1=PQ1, IMU_INT2=PQ2, NRST_CAM=PC8, EN_MODULE=PD2. Do not integrate before core pipeline works end-to-end. VL53L5CX limits: 15 Hz @ 8×8 / 60 Hz @ 4×4, ~88 KB firmware upload over I2C at init.

## 3. TASK ARCHITECTURE (µT-Kernel TK_PRI — lower = higher priority)

| TK_PRI | Task | Hard constraints |
|---|---|---|
| 1 | Hazard Alert | GPIO only. NO I/O, NO I2C, NO printf. < 1 ms preemption guaranteed. |
| 2 | TinyML Inference | Reads buffer via semaphore, calls NeuralART. NO I/O, NO I2C. |
| 3 | Sensor Acquisition | ALL I2C. DMA mode ONLY. Never blocking. |
| 15+ | Idle | WFI only. NEVER Stop mode (wake latency breaks < 1 ms guarantee). |

**ABSOLUTE RULES:**
- Any `HAL_I2C_Master_Transmit()` / `HAL_I2C_Master_Receive()` (blocking) = red-zone violation. Only `_DMA()` (preferred) or `_IT()` variants.
- Any I2C call outside the Priority 3 task = red-zone violation. Comment every I2C function: `// ONLY CALL FROM PRIORITY 3 SENSOR TASK`.
- µT-Kernel API only. NEVER FreeRTOS (`xTaskCreate`, `vTaskDelay`, `xSemaphoreGive` are all wrong here). Use `tk_cre_tsk`, `tk_sta_tsk`, `tk_slp_tsk`, `tk_wup_tsk`, `tk_cre_sem`, `tk_wai_sem`, `tk_sig_sem`, `tk_loc_mtx`/`tk_unl_mtx`, `tk_get_otm`, `tm_printf`.
- Before the first `tk_cre_tsk()`: verify priorities 1–3 are free in `Appli/mtk3_bsp2/config/config.h:27` (`CNF_MAX_TSKPRI 32`) and document the finding in a comment block.
- When writing any RTOS code, always state which task it runs in and its TK_PRI. For semaphore code, always name producer and consumer.

### Race-condition pattern (implement BEFORE any task body — paired semaphores)
```c
ID result_free_sem;   // init count = 1
ID result_ready_sem;  // init count = 0
// Inference (producer): tk_wai_sem(result_free_sem,1,TMO_FEVR) → write result → tk_sig_sem(result_ready_sem,1)
// Hazard   (consumer): tk_wai_sem(result_ready_sem,1,TMO_FEVR) → read result → tk_sig_sem(result_free_sem,1)
```

### Cache maintenance (Cortex-M55 + XSPI RAM — mandatory)
After every I2C DMA completion, before signalling the inference semaphore:
```c
SCB_InvalidateDCache_by_Addr((uint32_t*)sensor_buf, sizeof(sensor_buf));
```
NPU-managed buffers are covered by the `--cache-maintenance` flag in `user_neuralart.json`.

### Preemption instrumentation (from Day 1 of task code, guard with `#ifdef DEBUG_TIMING`)
- GPIO method for logic analyzer (contest evidence): PH5 (D0) set at hazard-signal, PD6 (D1) set at haptic-EN. Δt(D0→D1) on PulseView = preemption latency. LA: 24 MHz sigrok clone (verify `sigrok-cli --scan`).
- DWT method in firmware: `DWT->CYCCNT`; cycles / 600000 = ms at 600 MHz.
- Terminology: never write "zero-latency" anywhere. Use "deterministic sub-millisecond latency (< 1 ms, hardware-verified)".

### I2C driver status (CRITICAL — verify before first I2C code, verified 2026-07-04)
The µT-Kernel STM32 I2C driver (`sysdepend/stm32_cube/device/hal_i2c/hal_i2c.c`) is currently **GATED OFF** by `DEVCNF_USE_HAL_IIC=0` in `config_bsp/stm32_cube/config_bsp.h:40` — it is inert / not in the built image. Haptic-sense's Priority 3 sensor task **REQUIRES** I2C1 (VL53L1X 0x29, MPU6050 0x68, DRV2605L 0x5A), so `DEVCNF_USE_HAL_IIC` must be flipped to `1` when bringing up the I2C peripheral. `hal_i2c.c` already has a correct static-fallback `#else` branch for `TK_SUPPORT_MEMLIB=0` (`dev_i2c_cb` static array), so enabling it should need NO Kmalloc patch — but re-verify the `#else` path compiles clean under `USE_IMALLOC=0` after flipping the flag. Re-verified still `0` through Phase 3 (2026-07-05); the flip is the FIRST action of Phase 5.

## 4. BOOT & SIGNING (RED ZONE #1 — silent failure mode)

Flash order (strict): `ai_fsbl.hex @ 0x70000000` → `network_data.hex @ 0x70380000` (model weights) → `*-Trusted.bin @ 0x70100000` (application). External loader: `MX66UW1G45G_STM32N6570-DK.stldr` (verified from `Appli/STM32N6_MTK_Person_Detection_Appli.launch` + ST docs 2026-07-03).
- Post-build signing command (verbatim from `Appli/.cproject:17`, author's hardcoded path replaced with placeholder):
  ```
  cd "${ProjDirPath}/Debug" && echo y | "<SIGNING_TOOL_PATH>/STM32_SigningTool_CLI" -bin "${ProjName}.bin" -nk -of 0x80000000 -t fsbl -o "${ProjName}-Trusted.bin" -hv 2.3 -dump "${ProjName}-Trusted.bin" -align
  ```
  `-align` is **MANDATORY for CubeProgrammer ≥ 2.21** (we run 2.23) — the repo's command predates this and omits it. Omitting `-align` = signed-looking but non-booting binary, silent failure (RZ1). After EVERY build verify `*-Trusted.bin` exists and is non-zero.
- Boot pins: Dev Boot = BOOT0 LOW + BOOT1 (PA6/SW1) HIGH. Flash Boot = both LOW. Source: repo `README.md:297–376`.
- First bring-up: flash the reference repo's PREBUILT binaries to validate board+cable+programmer before building anything.
- **Reference repo README has drift** — it names `Binaries/network_weights.hex` (doesn't exist; actual file is `network_data.hex`) and references a `STM32N6_MTK_Person_Detection.ioc` and root `.project` not present in the repo copy. Trust `.cproject`/`.launch`/actual files over README prose.

## 5. REFERENCE REPO — `~/Github_projects/Reference Projects/STM32N6_Survivor_Detection`

Copy wholesale (do NOT reinvent): `Appli/mtk3_bsp2/` (µT-Kernel BSP2 port) · `Appli/STM32N657X0HXQ_LRUN.ld` · `Binaries/ai_fsbl.hex` · `Lib/AI_Runtime/` (LL_ATON) · `Model/STM32N6570-DK/user_neuralart.json` · `generate-n6-model.sh` · `serial_protocol.h`.
- **PATCHED**: `mtk3_bsp2/mtkernel/device/common/drvif/msdrvif.c` has a local static-pool patch (commit `7a91f51`) replacing `Kmalloc`/`Kfree` with a fixed `T_MSDI[CNF_MAX_REGDEV]` pool, to preserve `USE_IMALLOC=0`. Do NOT revert or re-copy this file from the reference repo. All 16 other `Kmalloc`/`Kfree` call sites in the tree (`ser.c`, `i2c.c`, `adc.c`, `hal_i2c.c`, `hal_adc.c`, vendor `hal_*` variants) are verified dead code in this build — gated off by `USE_SDEV_DRV=0`, `DEVCNF_USE_HAL_IIC=0`, `DEVCNF_USE_HAL_ADC=0`, or undefined `MTKBSP_*` macros — so no other patch is needed for this build.
- `user_neuralart.json` options string (verbatim from repo file — **copy verbatim, never hand-edit**): `--enable-epoch-controller -O3 --all-buffers-info --mvei --cache-maintenance --Oalt-sched --native-float --enable-virtual-mem-pools --Omax-ca-pipe 4 --Ocache-opt --Os`
- `-O3` + `--Os` coexistence is ST's own default N6 profile (verified against ST Edge AI docs 2026-07-03, both appear in ST's official stedgeai output) — not an error.
- Version note: recent ST Edge AI Core releases removed `--mvei` (now auto-derived from `--target`); if the installed ST Edge AI Core rejects it, delete only `--mvei` and keep all other flags.
Read-and-adapt only: `app.c` lines ~280–330 (buffer semaphore pattern) and ~1040–1132 (task creation) · `bsp2_stm32_cube_en.md` · `sysdef.h`.
Their camera→YOLO pipeline does NOT apply — sensor drivers are written from scratch for I2C ToF/IMU.
Fallback BSP: official tron-forum/mtk3_bsp2 **v1.00.04 (May 2026) officially supports N6570-DK** — switch only if the repo copy fails.

## 6. TinyML (RED ZONES #5, #6)

- Pipeline: Edge Impulse (browser) → `.tflite` INT8 → `generate-n6-model.sh` → `network.c` + `network_data.hex`
- Feature vector (13, @ 50 Hz): [d(t)…d(t−9) mm, v cm/s, a cm/s², ax, ay, az mg]. Fallback if IMU dropped: 12 features.
- Label: Hazard = distance < 80 cm AND closing velocity > 20 cm/s
- Model: 3-layer FC 13→32→16→1 sigmoid. NPU-safe ops ONLY: Conv1D/2D, DepthwiseConv, FullyConnected, ReLU, Sigmoid, BatchNorm. **NEVER LSTM/GRU/attention** — unsupported ops fall back to CPU silently, 10–30× slower, no error.
- Verify order: CPU inference → validate test vectors → enable NPU → compare NPU vs CPU outputs → measure `inference_ms` via `tk_get_otm()`.
- Haptic semantics: intensity/pattern-graded urgency (pulse rate ∝ closing velocity). Single-zone ToF gives NO direction — never claim directional feedback in code comments or docs.

## 7. CODE STANDARDS

- No dynamic allocation, no malloc, minimal footprint. Static buffers only.
- Every code block: language tag, target file path, task context + TK_PRI.
- `#ifdef DEBUG_TIMING` around all DWT/GPIO instrumentation.
- `tm_printf` format support is LIMITED to `%d`/`%u`/`%x`/`%s` (`libtm_printf.c`) — **no `%lu`**; print 32-bit values with `%u` and a `UW` cast. Verified against source 2026-07-04.
- Licensing: own `.c/.h` = Apache 2.0 with SPDX header on each file. `mtk3_bsp2/` files are T-License 2.1 OR 2.2 depending on the individual file header — check each file's header, do not assume. Verified: `msdrvif.c` is T-License 2.2 (and was modified per §5, header preserved); `discovery_stm32n657` sysdef files are 2.1. `AI_Runtime/`, `STM32Cube_FW_N6/` = ST SLA. Never mix headers.

## 8. RED ZONES — current status

| # | Zone | Status |
|---|---|---|
| 1 | FSBL signing pipeline | RESOLVED 2026-07-05: full chain verified on hardware — FSBL → signed app @ 0x70100000, Flash Boot, Phase 3 heartbeat running (`-align` present, correct tool path) |
| 2 | Toolchain versions | VERIFIED: STM32CubeIDE 2.2.0, STM32CubeProgrammer 2.23.0, both N6-capable, build succeeds |
| 3 | <1 ms preemption measurement | instrument from Day 1; LA verified via sigrok scan |
| 4 | Inference-buffer race | pattern defined (§3) — implement before task bodies |
| 5 | ML scope creep | Edge Impulse only; feature vector locked (§6) |
| 6 | NPU silent CPU fallback | FC/ReLU/Sigmoid only |
| 7 | Task priorities free | PRE-WORK DONE 2026-07-05: priorities 1/2/3/10 verified free (`config.h:27`, range 1–32; heartbeat=10, main_thread=15). Wrinkle: kernel inittask is created at TK_PRI 1 (`inittask.h:26`), runs `usermain()`, then parks forever on `tk_slp_tsk(TMO_FEVR)` (`main.c:107`) — permanently dormant, never preempts; µT-Kernel allows multiple tasks per priority, so the Phase 4 Hazard task at TK_PRI 1 coexists safely |
| 8 | ERM GPIO damage | RESOLVED by DRV2605L (ordered ×3); rule stands permanently |
| — | I2C pull-up conflict | RESOLVED: 1.5 kΩ onboard on I2C1 & I2C2, add nothing |
| — | µT-Kernel M55 port | RESOLVED: copy mtk3_bsp2 from repo (fallback: official v1.00.04) |

## 9. HOW TO BEHAVE (Claude Code)

- Direct, concise, highly technical. No filler. Tradeoffs as Option A/B — the developer decides.
- Hardware calculations: formula → substitution → result. Never skip steps. Never approximate specs — if unsure, say so and name the verification step.
- Bold the critical constraint in every hardware/RTOS answer.
- Diagnostics: list ALL plausible causes ranked by likelihood; label evidence vs inference vs speculation; state the single test that eliminates each; give confidence (certain/likely/speculative). Never generalize from partially tested cases.
- Do not change a recommendation because of pushback alone. Re-evaluate honestly: hold if right (and ask for the developer's reasoning), correct if wrong. Never switch to avoid friction.
- Before ANY firmware/hardware answer, check: I2C? → DMA, Priority 3 only. Task creation? → TK_PRI stated + priorities confirmed free (verified 2026-07-03, config.h:27). Shared memory? → semaphore pattern present. ML? → NPU-safe ops only.
- After ANY Claude Code commit: rebuild in CubeIDE and confirm the `-Trusted.bin` timestamp is NEWER than the commit before flashing. "Committed" ≠ "built" ≠ "flashed" — flashing a stale pre-strip binary cost three flash cycles on 2026-07-05.
