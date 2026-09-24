# CLAUDE.md — Haptic-Sense (TRON Forum Contest 2026)

AI-driven predictive spatial-awareness wearable for visually impaired users.
Hard deadline: **September 30, 2026** (TRON Forum Contest 2026 submission); internal target **September 18, 2026** for hardware freeze/dispatch. Benchtop prototype is the deliverable — NOT a wearable enclosure.
Developer: solo BTech student, **zero prior experience** in RTOS, ML training, FSBL/TrustZone boot, NPU deployment. Explain new µT-Kernel concepts with a FreeRTOS/real-world analogy first, then the exact API, then where the same pattern appears in the reference repo.

**PHASE STATUS (2026-07-06):** Phase 3 COMPLETE (commit `7a87671`, hardware-verified: heartbeat ~503 ms, camera strip confirmed, binary 682 KB → 63 KB). **Phase 4 FULLY COMPLETE** — (a) correctness gate: four-task architecture (hazard=1 / inference=2 / sensor=3 / heartbeat=10, commit `04994a5`), ~32 min soak, ~91,000 frames in `frames==inf` lockstep, `canary_err=0` (zero torn reads), `q=0`/`qovr=0`, ~47.6 Hz (`docs/evidence/phase4/phase4_soak.log`); (b) preemption campaign (RZ3) PASSED: worst case **3.375 µs** over 2,229 events under chatter load, baseline statistically identical (see §8 RZ3 for full numbers; evidence in `docs/evidence/phase4/`). **Phase 5 IN PROGRESS** (Option A decided 2026-07-06): `DEVCNF_USE_HAL_IIC` stays `0` PERMANENTLY — app owns HAL I2C directly (see §3 "I2C driver decision"); next: own L0/L1 driver.

**PHASE 5 FIRST LIGHT (2026-08-29):** First sensor ever addressed on this project. SmartElex DRV2605L ACKs at 7-bit **0x5A** on I2C1 and reads back **STATUS 0x00 = 0xE0, MODE 0x01 = 0x40, LIBRARY_SEL 0x03 = 0x01** — all three SLOS854D Table 3 reset values, `ok=4 err=0 tmo=0 recov=0`. Closes handoff §5.4 pass criteria 1 and 2, and with them the solder joints, the breadboard rails, the CN8/CN12 mapping, the HAL I2C1 config, the GPDMA primitives, the kernel IRQ registration and the Priority-3 ownership model. **Required a real fix first — see §3 "GPDMA channel attributes" (RED ZONE #9).** Evidence: `docs/evidence/phase5/PHASE5_I2C_FIRST_LIGHT_20260829.md`. Bench state: EN on a 3V3 jumper, not yet PE7; no motor; VL53L1X and MPU6050 not yet soldered.

---

## 1. TARGET & TOOLCHAIN (verify before first build)

- Board: **STM32N6570-DK** (MB1939) — STM32N657X0HXQ, **Cortex-M55 @ 800 MHz** + Ethos-U55/Neural-ART NPU
  - **800 MHz, not 600 — corrected 2026-08-30, hardware-confirmed (G-8 CLOSED).** `main.c:206-212` PLL1 = HSI 64 MHz / M 2 × N 25 = 800 MHz; `main.c:249-255` CPUCLK = IC1 ÷ 1 = **800 MHz**, sysb_ck = IC2 ÷ 2 = **400 MHz**, HCLK/PCLK1 = 200 MHz. The board prints `cpu=800000000 sysb=400000000 pclk1=200000000` and an on-target CYCCNT measurement gives **800000 cycles/ms** over five heartbeats including a 32-bit wrap. The long-standing `sysclk=400000000` reading was always correct — it is IC2, a different clock tree from the CPU, read by `HAL_RCC_GetSysClockFreq()` (`stm32n6xx_hal_rcc.c:1440`) versus `HAL_RCC_GetCpuClockFreq()` (`:1351`). Evidence: `docs/evidence/phase5/PHASE5_T1_CYCCNT_CLOCK_20260830.md`
- App executes from **XSPI RAM** (no internal flash execution) — linker script `STM32N657X0HXQ_LRUN.ld`
- RTOS: **µT-Kernel 3.0** (T-Kernel family). Lower integer = higher priority.
- Host: Linux Mint. IDE: STM32CubeIDE **≥ 1.19.0**. Programmer: STM32CubeProgrammer **≥ 2.20.0** (older = wrong Cortex-M55 GCC flags — RED ZONE)
- Compile flags (required): `-mcpu=cortex-m55 -mthumb -mfpu=fpv5-d16 -mfloat-abi=hard -mcmse`
- NeuralART define: `#define LL_ATON_PLATFORM LL_ATON_PLAT_STM32N6`
- Debug UART: USART1, PE5 (TX) / PE6 (RX), STLINK VCP, 115200 baud
- POWER: with camera module attached, Type-A→C USB cannot power the board (~550 mA limit, boot failure). Use C-to-C cable or powered source.
  - **MEASURED 2026-08-30: a Type-A-to-C cable DOES boot and run this board in the current configuration** — camera FFC unplugged, one DRV2605L, ERM pulsing, no NPU. The blanket claim "an A-to-C will not boot it" is FALSE for this build; the UM3300 §6.1 ~550 mA limit is real but the camera module is what exceeds it. **Still ship a C-to-C** for margin — the VL53L1X, the MPU6050 and the NPU are not on the rail yet. **The A-to-C case under NPU load is untested and must not be assumed.**

## 2. HARDWARE MAP (LOCKED — schematic-verified MB1939 rev, Nov 2024)

### I2C1 — the ONLY sensor bus. PH9 = SCL, PC1 = SDA. Onboard 1.5 kΩ pull-ups (R153/R149).
**NEVER suggest external pull-up resistors. NEVER put sensors on I2C2** (PD4/PD14 — occupied by LCD touch 0x5D, audio codec, STLINK).

| Device | 7-bit addr | Breakout | Extra pins |
|---|---|---|---|
| VL53L1X ToF (primary) | 0x29 | 7SEMI | **XSHUT → leave UNCONNECTED** (on-board pull-up to the sensor's own VDD); if a HW-reset path is ever wanted, drive it **open-drain, no internal pull-up** — low = reset, Hi-Z = run. Never hard-tie to 3V3: most VL53L1X carriers run the die from a 2.8 V LDO and XSHUT abs max is VDD+0.3 = 3.1 V. `[pending V-W-3]`. GPIO1 (INT) → **PD0** (ARD_D2 = CN11 pin 3), input, **no internal pull** (breakout already pulls it up; may be 2.8 V logic), EXTI **falling** — GPIO1 is active-low open-drain data-ready |
| MPU6050 IMU (primary) | 0x68 (verify AD0 by bus scan; 0x69 if high) | GY-521 | INT → **PE9** (ARD_D3) |
| DRV2605L haptic driver | 0x5A (silkscreened on the SmartElex board; SLOS854D §8.5.1.1) | **SmartElex ×2 = PRIMARY** · Adafruit ×1 = spare/bench only | EN → **PE7** (ARD_D8 = CN12 pin 1); IN/TRIG → **PE13** (ARD_D6 = CN11 pin 7) |

**IN/TRIG at 3.3 V is LEGAL. There is no voltage divider anywhere in this design.** Corrected 2026-08-29 against the local datasheet copy `docs/datasheets/drv2605l_datasheet.pdf` (TI SLOS854D Rev D, March 2018): §6.1 Absolute Maximum Ratings gives EN / SDA / SCL / IN-TRIG as `−0.3 V … VDD + 0.3 V` — 3.6 V at VDD = 3.3 V; §6.3 gives `VIH min 1.3 V`. The "1.8 V" that blocked this item for a month is §6.3 `VI(ANA) — Input voltage (analog mode), IN/TRIG: 0 … 1.8 V`, the full-scale reference for **analog input mode, which this project does not use**. **Derived rule, permanent:** the ceiling tracks VDD, so **PE7 (EN) and PE13 (TRIG) must be initialised LOW at reset and may only be driven high after the breakout's 3V3 rail is up.**

**The Adafruit board does NOT break out EN** — its pin set is VIN, GND, SCL, SDA, STEMMA QT, Motor±, INT, power-LED jumper [learn.adafruit.com DRV2605L pinouts, fetched 2026-08-29]. TI requires EN high for register access (§8.4.1.3), so the Adafruit unit must have EN tied high on-board and is therefore **permanently enabled** — it cannot take part in EN-arbitrated sequential init and will collide at 0x5A. **SmartElex is primary.** SmartElex silkscreen order, mounting-hole end first: **GND · VCC · SDA · SCL · IN · EN**, motor pads O−/O+ on the opposite edge [board photo 2026-08-29]. **IN and EN are adjacent — trivially swapped; label the wires.** Silkscreen is on the top face and the header pins on the bottom, so flipping the board reverses left-to-right; use the mounting hole as the landmark from either side.

SmartElex I2C pull-ups are **2.2 kΩ ("222") with an `I2C-PU` jumper**. Budget against the 1.5 kΩ onboard pair: board alone 1500 Ω / 2.2 mA; +1 SmartElex 892 Ω / 3.7 mA (over the 3 mA I2C budget but tolerable — VOL only has to stay under VIL = 0.99 V); +2 SmartElex 634 Ω / 5.2 mA — **not acceptable**. **Open the I2C-PU jumper on at least one SmartElex before the full bus is assembled.** Rise time is never the problem here: 892 Ω × ~100 pF ⇒ tr ≈ 76 ns, well inside the 300 ns fast-mode limit — **confirmed by measurement 2026-08-30**, real edge time ~100 ns (H-D8).

**BUDGET REDONE 2026-08-30 WITH MEASURED VALUES. The mitigation this file used to prescribe — "open the I2C-PU jumper on at least one SmartElex" — IS NOT SUFFICIENT.**

Pull-ups measured with a DMM, each board disconnected (SDA↔SCL reads 2R; SDA↔supply reads R):

| board | SDA↔SCL | SDA↔supply | ⇒ pull-up | tied to |
|---|---|---|---|---|
| 7SEMI VL53L1X | 19.8 kΩ | 9.9 kΩ (VIN) | **2 × 9.9 kΩ** | **VIN directly** |
| GY-521 MPU6050 | 4.3 kΩ | **72.3 kΩ** (VCC) | **2 × 2.15 kΩ** | **the onboard LDO output, NOT VCC** |

The GY-521's 72.3 kΩ to VCC is the reverse-leakage path through its regulator: its pull-ups sit on the regulated rail, so **they are live whenever the board is powered and there is no jumper to lift them.** Removing them means desoldering two resistors.

**The binding constraint is the weakest DRIVER on the bus, and a device counts whether or not its own pull-ups are fitted:** MPU6050 **3 mA** (VOL 0–0.4 V at 3 mA sink, PS-MPU-6000A §6.5) vs DRV2605L **4 mA** (VOL 0.4 V at IOL 4 mA, SLOS854D §6.5) and VL53L1X **4 mA** (VOL 0.4 V at IOUT 4 mA, DS12385 Table 16). Required sink = (3.3 − 0.4)/R_pu.

| configuration | R_pu | sink needed | limit | |
|---|---|---|---|---|
| today: board + SmartElex | 892 Ω | 3.25 mA | 4 mA | OK |
| Block 2: + 7SEMI | 818 Ω | 3.54 mA | 4 mA | OK |
| Block 3: all pull-ups fitted | 593 Ω | 4.89 mA | **3 mA** | **FAIL** |
| Block 3: SmartElex jumper open only | 811 Ω | 3.58 mA | **3 mA** | **FAIL** |
| Block 3: GY-521 resistors off only | 818 Ω | 3.54 mA | **3 mA** | **FAIL** |
| Block 3: **both 2.2 kΩ pairs removed** | 1303 Ω | 2.23 mA | 3 mA | **OK** |
| **IMU dropped, nothing modified** | 818 Ω | 3.54 mA | 4 mA | **OK** |

**SUPERSEDED, see the 2026-09-03/05 update a few paragraphs below (after the VIL CORRECTION note): re-derived with both datasheet IOL/VOL points, either single mitigation alone is sufficient — not both. What was actually done: SmartElex jumper opened, GY-521 left untouched.** **So: if the MPU6050 ships, BOTH 2.2 kΩ pairs must come off — the SmartElex jumper AND two desoldered resistors on the GY-521. Removing either alone leaves the MPU6050 sinking ~3.55 mA against a 3 mA spec.** If the IMU is dropped (risk register schedule valve #2), no pull-up modification is needed at all. **This is a new, independent argument for that valve and it was not available when the valve was written.** If the IMU is dropped (risk register schedule valve #2), no pull-up modification is needed at all.

**VIL CORRECTION.** This file previously justified 892 Ω with "VOL only has to stay under VIL = 0.99 V". Wrong — 0.99 V is the generic 0.3 × VDD. **SLOS854D §6.3 gives the DRV2605L an absolute `VIL` max of 0.5 V on EN / IN-TRIG / SDA / SCL**, which is the tightest receiver threshold on this bus (VL53L1X 0.6 V, MPU6050 0.3 × VLOGIC = 0.99 V). Budget against 0.5 V, not 0.99 V.

**UPDATE 2026-09-03/05 — the "both pull-up pairs must come off" conclusion above was over-derived, and here is what was actually done about it.** `PROJECT_AUDIT_20260903.md` §6.2 re-derived the same table using BOTH of the MPU6050 datasheet's IOL/VOL points (this file's table above used only the "Typical" 3 mA column, silently dropping the second, `VOL = 0.6 V → 5 mA` point). Solving with both points against the 0.5 V VIL threshold: the **818 Ω / 811 Ω single-mitigation rows both settle ~0.45 V — inside 0.5 V, i.e. they PASS.** Only the **593 Ω all-pull-ups-fitted row genuinely fails** (~0.562 V, ~12% over). So the table's own "either single mitigation only → FAIL" rows were wrong; **removing either the SmartElex jumper OR the GY-521's resistors is sufficient on its own** — not both.

**Decided and done, 2026-09-05: the SmartElex `I2C-PU` jumper was desoldered** (verified by SDA-to-SCL resistance jumping to 174.4 kΩ post-desolder — SDA-to-SCL, not SDA-to-GND, is the correct measurement here; SDA-to-GND is misleading because it depends on the VCC net's LDO-reverse-leakage path, the same phenomenon behind the GY-521's own 72.3 kΩ reading below). **The GY-521's onboard pull-up resistors were deliberately left untouched** — desoldering 0402-class resistors on an irreplaceable board with no spare, for a mitigation the corrected analysis above shows isn't required once the SmartElex side is open.

**Net result, from paired 30-minute soaks before/after (full data:
`docs/evidence/phase5/PHASE5_I2C_TWISTED_JOINT_20260905.md` §4–§5):** a small, MPU6050-isolated
I2C fault survives in both configurations — one dropped accel read out of ~81,000 per 30-minute
run, unchanged before and after the jumper was opened. Bus-recovery/NACK *counts* actually rose
after the change (small-sample noise on an already-bursty fault, or the rework itself disturbing
a marginal contact — not resolved either way), but the number that measures real data loss did
not move. **Accepted as residual risk 2026-09-05**, on the grounds that the hazard classifier
does not consume IMU data yet (`PH6-1_feature_frame_validity.md` §6) and the contest's actual
disqualification bar is untouched by it. Revisit if the fault rate ever visibly worsens beyond
this baseline, or once Block 6 wires real accel data into the classifier — `imu_valid` MUST be
honored (skip/hold-last on an invalid frame) by whatever consumes it then.

Good news from the same measurements: the 7SEMI's I2C pull-ups go to **VIN, not to its 2.8 V LDO**, and its `VIH` range is 1.12–3.5 V (DS12385 Table 16), so **3.3 V logic needs no level shifter** — a latent worry now closed. Rise time is confirmed by measurement, not just computed: real edge time ~100 ns (H-D8).

**EN low ≠ absent.** §8.4.1.3: with EN low the device still ACKs its address but no register read or write is possible. So an address ACK proves the bus and the joints; only a successful **register read** proves EN. STATUS (0x00) reset value is **0xE0**, DEVICE_ID bits 7:5 = **7** for the DRV2605L (§8.6.1 Table 4). **7, not 3 — 3 is the non-L DRV2605.**

All vendor breakouts wire the same DRV2605L IC pins — one firmware, interchangeable boards [TI SLOS854D pin functions].

### Arduino header pin map (subset in use)
**Always quote the connector pin, not just the Arduino name — "D15" is not actionable at the bench at 1 a.m.; "CN12 pin 10" is.** ARDUINO connectors are **CN7 (analog) · CN8 (power) · CN11 (D0–D7) · CN12 (D8–D15)** [UM3300 Rev 1 §8.7, Table 16]. **CN10 is the MIPI20 debug connector — never plug anything into it** [§8.8].

| Signal | Arduino | MCU | Connector pin |
|---|---|---|---|
| **+3V3 supply** | — | — | **CN8 pin 4** |
| **GND** | — | — | **CN8 pin 7** (pin 6 also GND; CN12 pin 7 = GND on the digital header) |
| SCL | D15 | PH9 | CN12 pin 10 |
| SDA | D14 | PC1 | CN12 pin 9 |
| DRV_EN (ch A) | D8 | PE7 | CN12 pin 1 |
| DRV_TRIG (ch A) | D6 | PE13 | CN11 pin 7 |
| IMU_INT | D3 | PE9 | CN11 pin 4 |
| TOF_INT | D2 | PD0 | CN11 pin 3 |
| **TIMING_D0** | D4 | PH5 | CN11 pin 5 — LA probe, **do not reuse** |
| **TIMING_D1** | D7 | PD6 | CN11 pin 8 — LA probe, **do not reuse** |

> **CN8 pin 5 is +5V and physically adjacent to pin 4.** A one-pin slip destroys every breakout on the rail; pin 3 on the other side is NRST. Count twice from the connector end and meter pin 4 → pin 7 (expect 3.25–3.35 V) before connecting anything.

Expansion channels, free and verified: ch B **EN D5/PE10, TRIG D9/PE14**; ch C **EN D10/PA3, TRIG D11/PG2**. Leaves D0/PF6, D1/PD5, D12/PH8, D13/PE15 free (**D13 also drives LD6 — avoid**). All three DRV2605L parts answer at the fixed 0x5A, so channels are distinguished by EN-arbitrated sequential init plus a per-channel TRIG.

### PERMANENT BUS-HYGIENE RULES (not one-time checks — these bite again during integration)
- **The MB1854 camera FFC must stay unplugged from CN14.** UM3300 Table 19: CN14 pin 20 = I2C1_SCL (PH9), pin 21 = I2C1_SDA (PC1) — the same nets as D14/D15 — and its VL53L5CX also answers at **0x29**, colliding with the VL53L1X. VDD_CAM is gated by SB53 (OFF by default) but do not rely on that.
- **Nothing in CN4 (STMod+), ever.** UM3300 Table 13: CN4 pin 7 = I2C1_SCL, pin 10 = I2C1_SDA; §8.4 warns explicitly that STMod+ signals are shared with the ARDUINO connectors. The MB1280 fan-out board also brings its own regulator and level shifters.
- LCD touch is on **I2C2** (PD14/PD4) — not a conflict, leave it alone.
- **Power with a C-to-C cable.** UM3300 §6.1 note 1: Type-A-to-C limits supply to ~550 mA.
- A4/A5 have an alternate I2C1 route via solder bridges SB31–SB34; **D14/D15 are hard-wired to PC1/PH9 and are not affected.** Leave all four bridges alone.

**DRV_TRIG resolved (HAP-T5 CLOSED, 2026-08-26):** D6 = **PE13** — verified against the MB1939 Rev C-02 schematic, sheet 9 (Arduino/ST Zio header, CN11 pin 7). Free, GPIO-output capable, not shared with any other onboard peripheral (checked against camera FFC CN14 and every other schematic sheet — no aliasing). **D6 (PE13) and D7 (PD6, TIMING_D1) are different pins** — do not conflate. Fallback: D5 = PE10, equally verified free, recorded as the alternate if D6 is ever needed for something else.

### Onboard LED (HARDWARE-CONFIRMED 2026-07-05, Phase 3)
LD1 = **PO1, active HIGH**. Port O carries XSPI1 (PSRAM) on PO0/PO2/PO3/PO4 — the memory the app executes from. **Configure ONLY PO1, pin-masked calls only**: `HAL_GPIO_Init` (masked RMW) + `HAL_GPIO_TogglePin` (atomic BSRR) confirmed working on hardware with XSPI1 untouched. Never `GPIO_PIN_All` / port-wide writes on port O. Verified against source: `__HAL_RCC_GPIOO_CLK_ENABLE()` exists (`stm32n6xx_hal_rcc.h:981`); `GPIOO` resolves to secure alias `GPIOO_S` (correct for this TrustZone build). LD2 (red, PG10, active LOW) may indicate BOOTFAILEDN — leave untouched.

### Actuator
ERM coin 10 mm × 3.4 mm (3 V class) driven ONLY through DRV2605L. **NEVER connect any motor to GPIO directly** — GPIO abs max ~20 mA, ERM draws 60–90 mA. Priority 1 task touches EN/GPIO only; DRV2605L I2C configuration happens at init from the Priority 3 task context.

### Onboard upgrade path (documented, OFF critical path)
MB1854B camera module (present in kit) carries VL53L5CX @ 0x29 + ISM330DLC (0x6A/0x6B) on I2C1 via FFC CN14. GPIOs: TOF_INT=PQ0, TOF_LPn=PQ5, IMU_INT1=PQ1, IMU_INT2=PQ2, NRST_CAM=PC8, EN_MODULE=PD2. Do not integrate before core pipeline works end-to-end. VL53L5CX limits: 15 Hz @ 8×8 / 60 Hz @ 4×4, ~84 KB firmware upload over I2C at init (86,016 B = 0x8000+0x8000+0x5000).

### LOCKED DRIVER DECISIONS (July 10 2026)
- MPU6050 DLPF_CFG = 4 (~21 Hz bandwidth, under 25 Hz Nyquist for the 50 Hz pipeline) [source: RM-MPU-6000A register map]
- MPU6050 accel full-scale range ±4g, AFS_SEL=1, 8192 LSB/g [corrected 2026-07-11 — RM-MPU-6000A §4.18; 4096 LSB/g is the ±8g row]
- Both MPU6050 settings locked BEFORE Edge Impulse data collection — changing either afterward rescales all collected training samples.
- DRV2605L = open-loop baseline (ERM + ROM library); closed-loop deferred as a clean add-on per drv2605l_port_design_v1.md §4.2/§5 [source: TI SLOS854D §9.3.1].

## 3. TASK ARCHITECTURE (µT-Kernel TK_PRI — lower = higher priority)

| TK_PRI | Task | Hard constraints |
|---|---|---|
| 1 | Hazard Alert | GPIO only. NO I/O, NO I2C, NO printf. < 1 ms preemption guaranteed. |
| 2 | TinyML Inference | Reads buffer via semaphore, calls NeuralART. NO I/O, NO I2C. |
| 3 | Sensor Acquisition | ALL I2C. DMA mode ONLY. Never blocking. |
| 15+ | Idle | WFI only. NEVER Stop mode (wake latency breaks < 1 ms guarantee). |

Idle reality check (verified 2026-07-05): the STM32 port's `low_pow()` is an EMPTY function (`mtk3_bsp2/sysdepend/stm32_cube/power_save.c:28-30`) — idle spins, never enters WFI or Stop mode, so the "NEVER Stop mode" rule holds by construction; there is no deep-sleep wake latency on the hazard path.

**ABSOLUTE RULES:**
- Any `HAL_I2C_Master_Transmit()` / `HAL_I2C_Master_Receive()` (blocking) = red-zone violation. Only `_DMA()` (preferred) or `_IT()` variants.
- Any I2C call outside the Priority 3 task = red-zone violation. Comment every I2C function: `// ONLY CALL FROM PRIORITY 3 SENSOR TASK`.
- µT-Kernel API only. NEVER FreeRTOS (`xTaskCreate`, `vTaskDelay`, `xSemaphoreGive` are all wrong here). Use `tk_cre_tsk`, `tk_sta_tsk`, `tk_slp_tsk`, `tk_wup_tsk` (reserved — not used in this project; see rule below), `tk_cre_sem`, `tk_wai_sem`, `tk_sig_sem`, `tk_loc_mtx`/`tk_unl_mtx`, `tk_get_otm`, `tm_printf`.
- Before the first `tk_cre_tsk()`: verify priorities 1–3 are free in `Appli/mtk3_bsp2/config/config.h:27` (`CNF_MAX_TSKPRI 32`) and document the finding in a comment block.
- When writing any RTOS code, always state which task it runs in and its TK_PRI. For semaphore code, always name producer and consumer.
- All sensor data-ready ISR→P3 wakes use `tk_sig_sem` (count-carrying), never `tk_wup_tsk` — decided 2026-07-11, audit M-3. Producer/consumer must be named at every signal/wait site.

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
NPU-managed buffers are covered by the `--cache-maintenance` flag in `user_neuralart.json` — that flag covers ONLY NPU buffers, not the I2C DMA buffers.
**D-cache enable is GATED on closing F-6a (write-side clean in the primitive), F-6b (ULD bounce buffer in the shim), F-6c (DRV2605L buffer rule) — see `docs/PROJECT_DEFENSE.md` §2.2.** D-cache is OFF today (`app_config.h:21`); flipping it without those three fixes arms three latent corruption defects at once.

### GPDMA channel attributes (RED ZONE #9 — silent failure mode, cost a full bench session 2026-08-29)

**Every GPDMA/HPDMA channel used on this target MUST be given explicit security, privilege and CID attributes
immediately after `HAL_DMA_Init()` + `__HAL_LINKDMA()`, or it will silently transfer NOTHING.**

```c
/* app_i2c.c i2c1_dma_init(), TK_PRI 3 */
HAL_DMA_ConfigChannelAttributes(&hdma, DMA_CHANNEL_PRIV | DMA_CHANNEL_SEC |
                                       DMA_CHANNEL_SRC_SEC | DMA_CHANNEL_DEST_SEC);
DMA_IsolationConfigTypeDef iso = { .CidFiltering = DMA_ISOLATION_ON,
                                   .StaticCid    = DMA_CHANNEL_STATIC_CID_1 };
HAL_DMA_SetIsolationAttributes(&hdma, &iso);
```

Why: this build is TrustZone-secure (GPIOO resolves to `GPIOO_S`), the DMA buffers live in the **secure** AXISRAM
alias (`gate_buf` @ `0x34013160`, see the `.map`), and I2C1 is a secure peripheral. A channel left at reset
attributes performs **non-secure** accesses and can reach neither end of the transfer. It arms, moves zero bytes,
and because the I2C generates its own STOP after NBYTES under AUTOEND, HAL still reaches
`HAL_I2C_MemRxCpltCallback` and returns success. **There is no error anywhere.** Measured on hardware 2026-08-29:
`init=0 gate=0 ok=4 err=0 tmo=0 recov=0` with all four destination buffers still holding their pre-fill byte.
`StaticCid = CID1` matches `main.c:334` (`RIMC_master.MasterCID = RIF_CID_1`).
Precedents already in this tree: `Lib/screenl/Src/scrl_spi.c:488-495` (this project's own SPI5 path) and
`STM32Cube_FW_N6/Drivers/BSP/STM32N6570-DK/stm32n6570_discovery_audio.c:3230, 3349, 3514` (ST's own BSP).
Attributes **latch** — `HAL_DMA_ConfigChannelAttributes` has no effect if called twice
(`stm32n6xx_hal_dma.c:175`), so it must run once and correctly; `i2c1_bus_recover()` deliberately does not
re-run `i2c1_dma_init()`.

### NEVER pre-fill a DMA destination buffer with a value you would accept as a reading

Corollary of #9, and the only reason it was caught. `gate_buf[0] = 0` before a register read made an untransferred
buffer indistinguishable from a device genuinely driving zeros — and on a pulled-up I2C bus an absent talker reads
**0xFF**, never 0x00, so 0x00 should have been suspicious from the first look. Pre-fill with a sentinel
(`0xA5`) that is neither a plausible register value nor the idle-bus value, and **verify against a known non-zero
reset value** (DRV2605L STATUS = 0xE0, MODE = 0x40, LIBRARY_SEL = 0x01) rather than against "the call returned OK".

### Preemption instrumentation (from Day 1 of task code, guard with `#ifdef DEBUG_TIMING`)
- GPIO method for logic analyzer (contest evidence): PH5 (D0) set at hazard-signal, PD6 (D1) set at haptic-EN. Δt(D0→D1) on PulseView = preemption latency. LA: 24 MHz sigrok clone (verify `sigrok-cli --scan`).
- DWT method in firmware: `DWT->CYCCNT`; **cycles / 800000 = ms at 800 MHz** (CPUCLK — CYCCNT counts the processor clock, never sysb_ck). **Corrected 2026-08-30 from /600000, which was wrong by 800/600 = 1.333×; every DWT-derived figure predating that date reads 1.333× too large.** DWT is enabled UNCONDITIONALLY (`app_tasks.c` `app_gpio_init()`, T1) — it was previously inside `#ifdef DEBUG_TIMING`, which this build does not define, so the counter was not running outside the Phase 4 campaign. Liveness is checked at enable (`DWT_CTRL_NOCYCCNT_Msk`, plus two spaced reads) and printed as `[DWT] ok=`; **a spin-delay built on CYCCNT must carry an iteration bound as well as a cycle bound, or a stopped counter hangs whatever task it runs in.**
- Terminology: never write "zero-latency" anywhere. Use "deterministic sub-millisecond latency (< 1 ms, hardware-verified)".

### I2C driver decision (Phase 5, Option A — decided 2026-07-06)
**`DEVCNF_USE_HAL_IIC` stays `0` PERMANENTLY** (`config_bsp/stm32_cube/config_bsp.h:40`). Phase 5 sensor I2C is **Option A**: the app owns ST HAL directly (own L0/L1, semaphore-wrapped DMA/IT, Priority 3 task only). The BSP wrapper must NOT compile in — it defines the strong `HAL_I2C_*CpltCallback` symbols (`hal_i2c.c:134-172`) that our driver must own; flag=1 + Option A = duplicate-symbol link errors.
Corrections to earlier claims (verified against source 2026-07-06):
- The flag gates ONLY the BSP wrapper (`hal_i2c.c`) + its devinit registration (`devinit.c` `knl_start_device()` also IMPORTs an application-defined `hi2c1` — flag=1 without one is a link error). It does NOT gate HAL I2C compilation: `stm32n6xx_hal_i2c.c` compiles regardless via `HAL_I2C_MODULE_ENABLED` (`stm32n6xx_hal_conf.h:53`). The Phase 5 design doc §4.1 "flip it anyway" premise is wrong.
- The earlier "correct static-fallback, no Kmalloc patch needed" claim was WRONG: `hal_i2c.c:60`'s `#else` branch has a one-char typo (`dev_I2C_cb` vs `dev_i2c_cb`) — compile error under `TK_SUPPORT_MEMLIB=0`; it would need an msdrvif-style typo patch if ever enabled. Its Kmalloc/Kfree calls ARE correctly guarded — no heap issue.
- The BSP IIC driver is IT-mode internally (six `HAL_I2C_*_IT` calls; zero blocking, zero DMA) — kept as REFERENCE ONLY for the flag-wait pattern (`tk_wai_flg` + HAL callbacks → `tk_set_flg`), which our L1 reimplements as semaphore + DMA.

## 4. BOOT & SIGNING (RED ZONE #1 — silent failure mode)

Flash order (strict): `ai_fsbl.hex @ 0x70000000` → `network_data.hex @ 0x71000000` (model weights) → `*-Trusted.bin @ 0x70100000` (application). External loader: `MX66UW1G45G_STM32N6570-DK.stldr` (verified from `Appli/STM32N6_MTK_Person_Detection_Appli.launch` + ST docs 2026-07-03).
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
- Feature vector (**15**, @ 50 Hz): [d(t)…d(t−9) mm (10) · v cm/s · a cm/s² · ax, ay, az mg (3)]. Fallback if IMU dropped: 12 features (10+2). (Corrected 2026-07-05: headline said "13" but the enumeration counts 15; the enumeration is authoritative.)
- **LOCKED CONTRACT**: firmware `FEAT_COUNT` (`Core/Src/app_tasks.c`) and the Edge Impulse feature spec are ONE contract — neither changes without the other. A mismatch silently degrades the NPU model (Red Zone #6).
- Label: Hazard = distance < 80 cm AND closing velocity > 20 cm/s
- Model: 3-layer FC 15→32→16→1 sigmoid. NPU-safe ops ONLY: Conv1D/2D, DepthwiseConv, FullyConnected, ReLU, Sigmoid, BatchNorm. **NEVER LSTM/GRU/attention** — unsupported ops fall back to CPU silently, 10–30× slower, no error.
- Verify order: CPU inference → validate test vectors → enable NPU → compare NPU vs CPU outputs → measure `inference_ms` via `tk_get_otm()`.
- Haptic semantics: urgency graded by pulse rate (∝ closing velocity); intensity banding deferred (drv2605l_port_design_v1.md §3). Single-zone ToF gives NO direction — never claim directional feedback in code comments or docs.

## 7. CODE STANDARDS

- No dynamic allocation, no malloc, minimal footprint. Static buffers only.
- Every code block: language tag, target file path, task context + TK_PRI.
- `#ifdef DEBUG_TIMING` around all DWT/GPIO instrumentation.
- `tm_printf` format support is LIMITED to `%d`/`%u`/`%x`/`%s` (`libtm_printf.c`) — **no `%lu`**; print 32-bit values with `%u` and a `UW` cast. Verified against source 2026-07-04.
- Licensing: own `.c/.h` = Apache 2.0 with SPDX header on each file. `mtk3_bsp2/` files are T-License 2.1 OR 2.2 depending on the individual file header — check each file's header, do not assume. Verified: `msdrvif.c` is T-License 2.2 (and was modified per §5, header preserved); `discovery_stm32n657` sysdef files are 2.1. `AI_Runtime/`, `STM32Cube_FW_N6/` = ST SLA. Never mix headers.

## 8. RED ZONES — current status

**Milestone (2026-07-10):** Three sensor/actuator driver port designs complete (VL53L1X, MPU6050, DRV2605L). Design docs at `~/haptic-sense/docs/design/`: `vl53l1x_port_design.md`, `mpu6050_port_design_v1.md`, `drv2605l_port_design_v1.md`.

| # | Zone | Status |
|---|---|---|
| 1 | FSBL signing pipeline | RESOLVED 2026-07-05: full chain verified on hardware — FSBL → signed app @ 0x70100000, Flash Boot, Phase 3 heartbeat running (`-align` present, correct tool path) |
| 2 | Toolchain versions | VERIFIED: STM32CubeIDE 2.2.0, STM32CubeProgrammer 2.23.0, both N6-capable, build succeeds |
| 3 | <1 ms preemption measurement | RESOLVED 2026-07-06: worst-case preemption latency **3.375 µs** (**2,700 cycles at 800 MHz** — was recorded as 2,025 using the wrong 600 MHz constant; the 3.375 µs itself is a logic-analyzer figure and is UNAFFECTED) over 2,229 events under CPU load; baseline identical at 3.375 µs over 1,776 events; σ ≈ 41–45 ns (below LA resolution — deterministic); missed=0 both runs; DWT cross-check consistent (~4 µs integer-truncated firmware reading). Locked claim wording: "deterministic sub-millisecond latency (< 1 ms, hardware-verified), worst case 3.375 µs over 2,229 events under CPU load; baseline identical." Evidence: `docs/evidence/phase4/` (7 `.sr` captures, `analyze_timing.py`, `analysis_runA_chatter.txt`, `analysis_runB_baseline.txt`, `sigrok_scan.txt`) |
| 4 | Inference-buffer race | RESOLVED 2026-07-05: paired-semaphore handshake (seq/seq_check canary torture test) proven correct on hardware — ~91k events, zero torn reads over ~32 min soak (`docs/evidence/phase4/phase4_soak.log`, commit `04994a5`) |
| 5 | ML scope creep | Edge Impulse only; feature vector locked (§6) |
| 6 | NPU silent CPU fallback | FC/ReLU/Sigmoid only |
| 7 | Task priorities free | PRE-WORK DONE 2026-07-05: priorities 1/2/3/10 verified free (`config.h:27`, range 1–32; heartbeat=10, main_thread=15). Wrinkle: kernel inittask is created at TK_PRI 1 (`inittask.h:26`), runs `usermain()`, then parks forever on `tk_slp_tsk(TMO_FEVR)` (`main.c:107`) — permanently dormant, never preempts; µT-Kernel allows multiple tasks per priority, so the Phase 4 Hazard task at TK_PRI 1 coexists safely |
| 8 | ERM GPIO damage | RESOLVED by DRV2605L (ordered ×3); rule stands permanently |
| **9** | **GPDMA channel security attributes** | RESOLVED 2026-08-29 in `app_i2c.c` `i2c1_dma_init()` — rule is PERMANENT and applies to every future DMA channel (see §3). Failure mode is silent success with an untouched buffer; it would have fed the Edge Impulse training set pure garbage and surfaced weeks later as an ML problem |
| — | I2C pull-up conflict | RESOLVED: 1.5 kΩ onboard on I2C1 & I2C2, add nothing |
| — | µT-Kernel M55 port | RESOLVED: copy mtk3_bsp2 from repo (fallback: official v1.00.04) |

### DRV2605L Verification Ledger (H-D series, from drv2605l_port_design_v1.md §9)
- H-D1: IN/TRIG wiring — (a) breakouts expose the pin: CONFIRMED (Adafruit `INT`, SmartElex `IN`); (b) GPIO allocation: OPEN.
- H-D2: EN-rise state ambiguity — bench test pending.
- **H-D3: CLOSED 2026-08-30 — effect-1 playback measured 58605–58708 µs** (n=5, late=0, stuck=0), by timing the GO bit (0x0C bit 0) against `DWT->CYCCNT` at the hardware-confirmed 800 MHz CPUCLK. Spread 103 µs = **0.18%** — a ROM effect replaying deterministically. Start is exact (stamped at the TRIG rising edge); end is up to one I2C read (~98 µs) late, so 58.708 ms is an **upper bound**, the safe direction for a floor. **R-3 floor set to 75 ms** (= 58.708 × 1.278, above the ×1.2 rule), replacing the interim 125 ms. Method needs no oscilloscope; a differential OUT+/OUT− capture through the SLOS854D Fig. 11 filter is still worth taking in lab to separate rise from brake, but nothing is blocked on it.
- **H-D4: CLOSED 2026-08-30 by bounding — Library B confirmed.** Total playback is 58.708 ms, and total = rise + brake, so **rise alone cannot exceed 80 ms** — the threshold that would have forced a move to Library C or D. 58.7 ms also sits mid-band in the Table 1 Library B prediction (rise 40–60 ms + brake 5–15 ms = 45–75 ms). GO timing cannot separate rise from brake; only a scope can, and it is not needed for this decision.
- H-D5: breakout VDD rail — measurement attempted July 10, inconclusive (unstable meter reading, likely breadboard contact); assume 3.3V, remeasure pending.
- **H-D6 / V-W-6: CLOSED 2026-08-30 — ERM coil measured 32.5 Ω** by multimeter, motor disconnected from everything. Clears the SLOS854D §6.3 `ZL` min 8 Ω (at VDD = 5.2 V, "ensured by design, not production tested") with 4× margin, and sits inside the 25–37.5 Ω predicted from the vendor rated V/I (3 V / 80–120 mA). The motor is legal on this driver.
- H-D7: DEV_RESET self-clear time — instrument on first hardware run.
- **H-D8: CLOSED 2026-08-30 — delivered SCL measured, found 11% out of spec, fixed, re-measured in spec.** Two logic-analyzer captures on PH9 (`docs/evidence/phase5/`): **before, median 2250 ns = 444.4 kHz, every one of 360 periods below the limit; after, median 2625 ns = 381.0 kHz, not one of 288 periods below 2500 ns.** The fix is `I2C_Charac[I2C_SPEED_FREQ_FAST].freq` 400000 → 350000 in `i2c_timing.c` — see that file's comment, and do not undo it. `min = 2500 ns` in the report is 20 samples at 125 ns quantisation, **not** a real excursion to 400 kHz; quote the median. Real edge time measured ~100 ns, confirming §2's computed `tr ≈ 76 ns`. Background follows: `f(SCL)` max **400 kHz** is correct but sits in SLOS854D **§6.6 Timing Requirements**, not §6.7 (which is Switching Characteristics). The VL53L1X limit is the same (datasheet Table 7, `FI2C` 0–400 kHz). ST's timing algorithm computes `tSCL = tSCL_L + tSCL_H + trise + tfall` and hits 2500 ns exactly — but only 2150 ns of that is hardware (`tSCL_L=1320`, `tSCL_H=830`, `TIMINGR=0x60911523` at pclk1 = 200 MHz); the other 350 ns is an ASSUMED worst-case edge time. CLAUDE.md §2 computes this bus at **tr ≈ 76 ns**, which puts the real period near 2246 ns ⇒ **~445 kHz**. Corroborated independently by the HAP-T9 poll at ≈79 µs per 1-byte read. **Lowering `I2C_BUS_HZ` does NOT fix this** — `I2C_GetTiming()` uses the request only to pick a speed bucket and then targets the hardcoded `I2C_Charac[FAST].freq = 400000`, so any value in [320000, 480000] yields a byte-identical TIMINGR. **MEASURED 2026-08-30 (BUS-2, LA on PH9, `docs/evidence/phase5/bus2_scl_20260830.sr`): median period 2250 ns = 444.4 kHz, 11% over the limit. The prediction was 2246 ns.** Real edge time is therefore ~100 ns, confirming §2's computed tr ≈ 76 ns. **FIXED** by lowering `I2C_Charac[I2C_SPEED_FREQ_FAST].freq` 400000 → 350000 in `i2c_timing.c`, which yields `tSCL_L + tSCL_H = 2505 ns` — past the 2500 ns line before any edge time, so the result does not depend on the ±125 ns capture resolution. Predicted delivered clock ~384 kHz. **H-D8 stays open until the confirming re-capture.** Analysis: `docs/BUS2_SCL_FREQUENCY_20260830.md`.
- Note also: `tSCL_L = 1320 ns` against SLOS854D §6.6 `tw(L)` min **1.3 µs** — legal by 20 ns. Re-check if `pclk1` or the PLL configuration ever changes.
- H-D6 addendum: SLOS854D §6.3 gives `ZL` min **8 Ω at VDD = 5.2 V**, footnoted "ensured by design, not production tested" — quote the condition, not a bare 8 Ω. Direct measurement (V-W-6) still owed before any motor is connected.
- **H-D9 (new, 2026-08-29): DRV2605L register access CONFIRMED on hardware.** STATUS 0xE0 / MODE 0x40 / LIBRARY_SEL 0x01 read back over I2C1 DMA from the SmartElex board. **Write direction CLOSED 2026-08-30.** `drv2605l_write_probe()` (`app_i2c.c`) reads 0x02 RTP_INPUT, writes 0x27, reads it back, restores the original and verifies the restore — two independent writes, `wr=0 wrseen=0x27 ok=9 err=0 recov=0` on hardware. `hdma_i2c1_tx` has now moved bytes. The full L1 primitive (`i2c_rd` AND `i2c_wr`, DMA both directions) is proven.
- **H-D10 CLOSED 2026-08-30. EN is driven by PE7 (D8, CN12 pin 1).** Bench jumper removed. Register access unchanged across the move: `whoami=0x140e0 wr=0 wrseen=0x27 ok=9 err=0 recov=0`. PE7 is init-only (`drv2605l_power_up()`, TK_PRI 3) per R-1; the hazard pattern moved to PE13. **Block 0 COMPLETE.**
- **H-D2 (EN-rise state ambiguity) — MECHANISM RESOLVED 2026-08-30.** The DRV2605L **retains its register configuration across an MCU reset but loses it across a true power cycle.** Evidence, both from `app_i2c_gate_test()`, which runs BEFORE any config write: after a reflash (VDD maintained) it packed `whoami=0x0201E0` — MODE 0x01, LIBRARY 0x02, the *configured* values; after USB power was removed to do the T3 wiring it packed `whoami=0x0140E0` — 0x40 / 0x01, the Table 3 reset values. **Retention is because VDD never dropped, not because of anything EN did.** Consequence: a readback cannot by itself prove that *this* boot's writes landed, which is why `drv2605l_init()` issues **DEV_RESET first and confirms MODE reads 0x40** before configuring.
- **H-D7: CLOSED 2026-08-30 — DEV_RESET self-clears in under one kernel tick (< 2 ms).** Measured as `rst=1`: the bit had already cleared at the first poll, taken after a single `tk_dly_tsk(1)`. `rstmode=0x40` confirms defaults were restored.
- **FIRST REAL FAULT CAUGHT BY THE RUNTIME POLL, 2026-08-30.** `[HLT] faults=0x1` = `OC_DETECT`: two bare twisted joints between the motor leads and the jumper wires, a centimetre apart in free air, shorted **only while the motor was running — its own vibration closed them.** Every other indicator was clean (`init=0 armed=1 cfglost=0 err=0 canary_err=0`), so without the poll the symptom was "the buzz feels weaker sometimes". `[EFF] late=3 stuck=1` localised it as the shutdown-and-restart cycle rather than a hard short. Fixed by separating and offsetting the joints; 22 heartbeats clean afterwards, coil undamaged (effect duration unchanged across three boots, 58505–58760 µs, 0.44% spread). **Block 5 rule, earned the hard way: every motor connection soldered, sleeved, offset so no two conductors can meet, and strain-relieved.** Evidence: `docs/evidence/phase5/PHASE5_OC_DETECT_20260830.md`.
- **HAPTIC CHAIN LIVE 2026-08-30 — first physical haptic output in the project.** Synthetic hazard → `drv2605l_trig_fire()` 2 µs edge on PE13 (CN11 pin 7) → DRV2605L → ERM buzz. Two pulses per 6.6 s hazard burst at the interim 125 ms R-3 floor. Over 280 hazard events: `pulses + suppressed == hazard` exactly, `faults=0x0`, `cfglost=0`, `err=0 tmo=0 recov=0`, `canary_err=0`, 47.6 Hz — identical to the Phase 4 frame rate. **H-D3 (effect duration) is the last open item before the R-3 floor can leave its interim value.**

### VL53L1X Verification Ledger (Block 2, 2026-08-31 / 09-01)

- **V-W-3 CLOSED 2026-08-31 — 7SEMI pull-ups measured, board disconnected.**
  XSHUT↔VIN **4.13 kΩ** (gradual rise: the meter was charging board capacitance,
  so treat it as "a path exists", not as a resistor value); XSHUT↔GND 1.79 MΩ
  (no pull-down); SDA↔SCL 19.8 kΩ (= 2 × 9.9 kΩ, board identity confirmed).
  **Powered, XSHUT idles at 2.62 V** — note this is NOT the 3.32 V system rail,
  so XSHUT is pulled to something else on the carrier. Relevant because
  DS12385 gives AVDD a **2.6 V minimum** and there is no bulk capacitance.
- **GPIO1 HAS NO PULL-UP ON THE 7SEMI. §2's premise for PD0 was wrong.**
  GPIO1↔VIN measured **2.46 MΩ**. §2 specifies PD0 as "input, no internal pull
  (breakout already pulls it up)" — it does not. GPIO1 is an open-drain output
  (DS12385 pin table), so with no pull-up it can only pull low and floats
  otherwise. **Confirmed on the wire 2026-09-01:** an 8 s logic-analyzer capture
  of GPIO1 shows **7,202 low episodes, all short and clustered in a 1.4 ms
  window** — that is a floating input picking up noise, not an interrupt.
  Block 2 is unaffected (the frame loop polls `CheckForDataReady` over I2C and
  nothing reads PD0). **When EXTI is wanted, either fit 10 kΩ GPIO1→VIN (ST's
  recommended value) or enable the STM32 internal pull-up on PD0** — the latter
  is now safe, because DS12385 gives AVDD 2.6/2.8/**3.5 V** so 3.3 V logic is in
  range and the "may be 2.8 V logic" caveat in §2 does not apply.
- **V-W-1 CLOSED 2026-08-31 — rail metered at last.** CN8 **3.32 V**, DRV2605L
  breakout **3.31 V**. **H-D5 CLOSED with it: 10 mV of wiring drop.**
  Still owed: VIN metered at the 7SEMI pin itself, which has never been done.
- **Pull-up budget confirmed by measurement, no modification needed.**
  Powered-off SDA↔SCL with all three devices on the bus read **1.612 kΩ**
  against a predicted 2 × 818 = 1636 Ω (1.5%, inside meter tolerance).
  Sink = (3.32 − 0.4)/818 = **3.57 mA** against a 4 mA limit. §2's "Block 2:
  + 7SEMI → 818 Ω, OK" row is now measured, not computed.
- **V-5 / H1 CLOSED 2026-09-01 — 16-bit addressing PROVEN ON THE WIRE.**
  This is the L5 deliverable and it is done. From
  `docs/evidence/phase5/sensorinit_fail_20260901.sr`, decoded:
  `52 01 0F` → `53 EA`, `52 01 10` → `53 CC`, and `52 01 0F` → `53 EA CC 10`
  (three-byte block read, device auto-increment). Wire order is exactly
  `0x52 idxMSB idxLSB data`, as HAL_I2C_Mem_Read_DMA predicts. **The REG8
  negative control is on the same capture:** `52 0F` → `53 00` — one index byte,
  not two, returning a different value, so the 8-bit and 16-bit branches are
  provably distinct on this bus. **Archive the `.sr`; it is submission
  evidence and cannot be retaken after 18 Sep.**
- **Bus timing verified across 4,609 measured gaps.** Real STOP→next-START:
  min **10.75 µs**, median 80.75 µs, and **zero** below the 1.3 µs fast-mode
  `t(BUF)` minimum. SCL low pulse: median 1.500 µs, p99 2.500 µs over 207,130
  pulses. The bus is clean and the master's timing is not a suspect.
- **Adding the 7SEMI did not harm the DRV2605L.** Same capture: 41 reads and
  33 writes to 0x5A, **zero NACKs**. Block 1 is intact and the fault is
  specific to 0x29.
- **OPEN — THE VL53L1X REFUSES ITS OWN ADDRESS FOR 12.8 ms MID-CONFIGURATION.**
  After 30–31 of `VL53L1X_SensorInit`'s 91 writes ACK (~8.7 ms of sustained
  traffic), the part stops answering its **address** — not a data NACK, the
  address byte itself — for **12.8 ms ± 0.1 ms, identical across three boots**,
  then recovers and serves 4,236 reads with no failures. Registers
  **0x004C..0x0087, 60 of 91, are never written**, and the SensorInit data-ready
  timeout is a downstream symptom (the poll then reads `0x0030 → 0x11` and
  `0x0031 → 0x03`, so `IntPol = 0` while `Temp & 1 = 1` and `isDataReady` can
  never be set). `calls=3093` matches the full path exactly.
  **NOT back-to-back transfer spacing** — the gap before the first NACK is
  161 µs and no gap in the whole capture is under 1.3 µs. **NOT XSHUT dropping**
  — a dedicated 8 s capture with the analyzer on XSHUT shows it **never once
  low**. Live candidates: an AVDD sag on the 7SEMI's own rail (see the 2.62 V
  note above; no bulk capacitance fitted), or an internal busy state in the
  part. Mitigated, visibly, by a bounded write retry in the platform shim
  (`VL53L1_PORT_WRITE_RETRY`, counted on the `[RTY]` heartbeat line — a
  mitigation that hides itself would be the same defect class this project
  keeps finding). Full record:
  `docs/evidence/phase5/PHASE5_SENSORINIT_NACK_20260901.md`.
- **Tooling:** `docs/evidence/phase5/decode_sr_i2c.py` decodes I2C straight out
  of a sigrok `.sr` without PulseView — address/NACK census, which register
  indices were written and which are missing, NACK bursts and their spans,
  real STOP→START gaps, SCL low-pulse widths. Needs numpy.

## 9. HOW TO BEHAVE (Claude Code)

- Direct, concise, highly technical. No filler. Tradeoffs as Option A/B — the developer decides.
- Hardware calculations: formula → substitution → result. Never skip steps. Never approximate specs — if unsure, say so and name the verification step.
- Bold the critical constraint in every hardware/RTOS answer.
- Diagnostics: list ALL plausible causes ranked by likelihood; label evidence vs inference vs speculation; state the single test that eliminates each; give confidence (certain/likely/speculative). Never generalize from partially tested cases.
- Do not change a recommendation because of pushback alone. Re-evaluate honestly: hold if right (and ask for the developer's reasoning), correct if wrong. Never switch to avoid friction.
- Before ANY firmware/hardware answer, check: I2C? → DMA, Priority 3 only. Task creation? → TK_PRI stated + priorities confirmed free (verified 2026-07-03, config.h:27). Shared memory? → semaphore pattern present. ML? → NPU-safe ops only.
- After ANY Claude Code commit: rebuild in CubeIDE and confirm the `-Trusted.bin` timestamp is NEWER than the commit before flashing. "Committed" ≠ "built" ≠ "flashed" — flashing a stale pre-strip binary cost three flash cycles on 2026-07-05.
