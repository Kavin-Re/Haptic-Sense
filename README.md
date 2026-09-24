# Haptic-Sense

**An open-source, AI-driven predictive spatial-awareness wearable for visually impaired users.**

TRON Forum Programming Contest 2026 — Entry ID **34476** — RTOS Application category (Students,
individual entry) — [tron.org](https://www.tron.org/programming_contest-2026/)

Author: Kavin K K, B.Tech Electronics Engineering (VLSI), Rashtriya Raksha University.

---

## What this is

Haptic-Sense is a benchtop prototype (not a finished wearable enclosure) built around a single
forward-facing time-of-flight sensor, a wrist-mounted IMU, and a haptic motor. A lightweight
on-device model fuses distance, closing velocity, and motion to decide when an obstacle is
approaching fast enough to be a hazard, and drives a haptic pulse whose rate scales with
urgency. The whole sensing-to-actuation loop runs under a hard real-time task architecture on
**µT-Kernel 3.0**, with a measured, hardware-verified worst-case preemption latency of
**3.375 µs** — well under the sub-millisecond target the project set for itself.

The single-zone ToF sensor cannot measure direction, and this project does not claim it does:
feedback here is strictly "how urgent," never "which way."

## Hardware

| Component | Part | Role |
|---|---|---|
| MCU board | STM32N6570-DK (STM32N657X0HXQ) | Cortex-M55 @ 800 MHz + Ethos-U55/Neural-ART NPU |
| Time-of-flight sensor | ST VL53L1X (7SEMI breakout) | Forward-facing distance, I2C1 @ 0x29 |
| IMU | InvenSense MPU6050 (GY-521 breakout) | Motion / closing-velocity signal, I2C1 @ 0x68 |
| Haptic driver + motor | TI DRV2605L (SmartElex breakout) + ERM motor | Urgency-graded haptic pulses, I2C1 @ 0x5A |

All three peripherals share a single I2C1 bus (PH9 = SCL, PC1 = SDA) — see `CLAUDE.md` §2 for
the full pin map, pull-up budget, and per-device wiring notes. The board is USB-powered with no
battery installed.

## Repository layout

```
firmware/           STM32CubeIDE project (µT-Kernel 3.0 + the sensor/actuator/inference code)
  Appli/             Application sources, task code, µT-Kernel BSP port (mtk3_bsp2)
  Model/             Generated NPU model artifacts (network.c/.h, network_data.hex) — the
                     inference code the board actually runs
  Lib/, STM32Cube_FW_N6/   Vendored ST/NPU runtime libraries (not tracked in git — see below)
model/               Model-generation entry point (generate-n6-model.sh, NeuralART config)
docs/                Design docs, verification evidence, block-by-block build history, and the
                     honest plan-vs-build divergence writeups (see docs/PROJECT_DEFENSE.md)
docs/evidence/       Logic-analyzer captures, soak-test logs, and timing-campaign data that back
                     every hard performance claim in this README and in docs/PROJECT_DEFENSE.md
CLAUDE.md            The project's own engineering rulebook — hardware map, RTOS task rules,
                     red-zone tracking, code standards. The most detailed technical reference
                     in this repo; read it before modifying anything under firmware/
```

Two large third-party dependencies (`firmware/Lib/AI_Runtime/` — ST's NPU runtime, ~68 MB —
and `firmware/STM32Cube_FW_N6/` — the ST HAL/CMSIS/BSP package, ~94 MB) are **not** tracked in
this repository under their own ST Software License Agreement terms. Obtain them from
[STM32CubeN6](https://github.com/STMicroelectronics/STM32CubeN6) and the X-CUBE-AI package per
ST's own distribution terms, and place them at the paths above before building.

## Build & flash

Toolchain: STM32CubeIDE ≥ 1.19.0, STM32CubeProgrammer ≥ 2.20.0 (older versions default to the
wrong Cortex-M55 flags), on Linux or Windows.

1. Import `firmware/Appli` into STM32CubeIDE.
2. Place the two vendored dependencies noted above at `firmware/Lib/AI_Runtime/` and
   `firmware/STM32Cube_FW_N6/`.
3. Build (Debug configuration — this project has no separate Release build).
4. Sign the output (required — an unsigned or unaligned binary boot-fails silently on this
   part):
   ```
   STM32_SigningTool_CLI -bin <ProjName>.bin -nk -of 0x80000000 -t fsbl -o <ProjName>-Trusted.bin -hv 2.3 -dump <ProjName>-Trusted.bin -align
   ```
   `-align` is mandatory for STM32CubeProgrammer ≥ 2.21. After every build, confirm
   `*-Trusted.bin` exists and is non-zero before flashing.
5. Flash, in this order, with STM32CubeProgrammer and the `MX66UW1G45G_STM32N6570-DK.stldr`
   external loader:
   - `binaries/ai_fsbl.hex` → `0x70000000`
   - `firmware/Model/STM32N6570-DK/network_data.hex` (model weights) → `0x71000000`
   - `<ProjName>-Trusted.bin` (the signed application) → `0x70100000`
6. Boot-pin configuration: Dev Boot = BOOT0 low + BOOT1 (PA6/SW1) high; Flash Boot = both low.
7. Debug UART is USART1 (PE5 TX / PE6 RX) over the ST-LINK VCP at 115200 baud.

See `CLAUDE.md` §1 and §4 for the full toolchain/boot detail, including a known documentation
drift in the upstream reference repo this project's µT-Kernel BSP port was built from.

## RTOS architecture

Four µT-Kernel 3.0 tasks, priority = lower integer = higher priority:

| Priority | Task | Constraint |
|---|---|---|
| 1 | Hazard Alert | GPIO only — no I/O, no I2C, no `tm_printf`. This is the < 1 ms path. |
| 2 | TinyML Inference | Reads the sensor buffer via a paired semaphore, runs the on-device model. |
| 3 | Sensor Acquisition | Owns all I2C traffic, DMA-mode only, never blocking. |
| 10 | Heartbeat | Liveness indicator, lowest-frequency task. |
| 15+ | Idle | Spin/WFI only — this port never enters a sleep state that would add wake latency. |

Producer/consumer handoffs between tasks use paired counting semaphores
(`tk_cre_sem` / `tk_wai_sem` / `tk_sig_sem`), never raw task wakeups, so no result buffer is
ever read half-written. The µT-Kernel API is used throughout — no FreeRTOS calls appear
anywhere in this codebase.

**Measured, hardware-verified (see `docs/evidence/phase4/`):** worst-case preemption latency
3.375 µs over 2,229 events under synthetic CPU load; baseline (no load) statistically
identical. This is the number behind every "sub-millisecond, deterministic" claim in this
project's contest submission.

## The on-device model

Pipeline: hardware data collection → Edge Impulse (feature engineering + training) → INT8
`.tflite` export → `generate-n6-model.sh` (ST NeuralART compiler) → `network.c` /
`network_data.hex`, compiled into the firmware image above.

- 15-feature input vector at 50 Hz: 10 frames of distance history, closing velocity, closing
  acceleration, and 3-axis accelerometer readings (12-feature fallback if the IMU is dropped).
- A hazard label is distance < 80 cm **and** closing velocity > 20 cm/s.
- A small fully-connected network (15→32→16→1, sigmoid output) using only NPU-safe op types
  (Conv/DepthwiseConv/FullyConnected/ReLU/Sigmoid/BatchNorm) — no recurrent or attention layers,
  which fall back to the CPU silently and 10-30× slower on this part.
- **Honesty note on NPU status:** an addressing issue between where the NPU reads its weights
  and where they were flashed was root-caused via static analysis late in the build
  (`docs/NPU_SATURATION_DEBUG_FINDINGS.md`) but was not re-verified on hardware before the unit
  shipped for judging. The CPU inference path is untouched by this issue and is what the
  shipped unit actually runs classification on.

## Known limitations

- This is a benchtop prototype, not a wearable enclosure — see `docs/PROJECT_DEFENSE.md` for
  the honest plan-vs-build divergence record kept throughout the build.
- A single-zone ToF sensor has no directional resolution. Feedback here communicates urgency
  only, never direction — this is a deliberate scope decision, not a missing feature.
- See `docs/NPU_SATURATION_DEBUG_FINDINGS.md` for the current, not-yet-hardware-verified status
  of the NPU inference path.

## License

This project is released under the **GNU General Public License v3.0** — see `LICENSE`.

Individual source files may carry their own SPDX license header where that reflects their
actual provenance (for example, code written to match a vendor's own permissively-licensed
API surface, or files ported from the µT-Kernel BSP reference implementation under T-License
2.1/2.2 — see `CLAUDE.md` §7 and §5). Those headers govern the individual file; `LICENSE`
governs the combined work. Vendored third-party libraries (`firmware/Lib/AI_Runtime/`,
`firmware/STM32Cube_FW_N6/`) are **not** included in this repository and remain under ST's own
license terms.

## Acknowledgements

Built on the µT-Kernel 3.0 BSP2 port for STM32N6570-DK
([tron-forum/mtk3_bsp2](https://github.com/tron-forum/mtk3_bsp2)) and ST's NeuralART /
X-CUBE-AI tooling for on-device NPU inference.
