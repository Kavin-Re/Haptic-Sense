# PHASE 5 — I2C1 FIRST LIGHT
**2026-08-29, ~23:37 IST. First sensor ever addressed on this project.**

Supersedes the wiring/bring-up assumptions in `HAPTIC_SENSE_HANDOFF_20260829_COWORK.md`.
Every hardware specific below traces to a file in this repo, the local datasheet copy, or the
serial capture quoted verbatim.

---

## 1. RESULT

```
[I2C] init=0 gate=0 whoami=0x140e0 addr=0x5a ok=4 err=0 tmo=0 recov=0 pclk1=200000000 sysclk=400000000
```

`whoami` packs three single-byte register reads from the SmartElex DRV2605L at 7-bit 0x5A,
little-endian, each against its documented reset value in TI SLOS854D Rev D Table 3
(`docs/datasheets/drv2605l_datasheet.pdf`):

| Register | Read | Expected | Source |
|---|---|---|---|
| 0x00 STATUS | 0xE0 | 0xE0 (DEVICE_ID bits 7:5 = 7) | SLOS854D §8.6.1 Table 4 |
| 0x01 MODE | 0x40 | 0x40 | SLOS854D §8.6 Table 3 |
| 0x03 LIBRARY_SEL | 0x01 | 0x01 | SLOS854D §8.6 Table 3 |

Four transfers, zero errors, zero timeouts, zero bus recoveries.

**Closes handoff §5.4 pass criteria 1 and 2**, and with them, in one event: the GND/VCC/SDA/SCL
solder joints, the breadboard rails, the CN8/CN12 pin mapping, the HAL I2C1 configuration, the
semaphore-wrapped GPDMA primitives, the µT-Kernel IRQ registration, and the Priority-3 task
ownership model.

Bench configuration for this result: EN tied to the 3V3 rail by a bench jumper (**not** PE7),
IN tied low, no motor, VL53L1X and MPU6050 not yet soldered, camera FFC unplugged from CN14,
CN4 empty.

---

## 2. ROOT CAUSE — GPDMA channels transferred nothing, silently

The first three runs of the night produced `init=0 gate=0 whoami=0x0 addr=0x5a ok=1 err=0
recov=0`. The device ACKed at 0x5A but every register read returned 0x00.

**0x00 was never a reading.** It was the pre-fill. `app_i2c_gate_test()` did `gate_buf[0] = 0`
immediately before each read, so an untransferred buffer and a device driving zeros were
indistinguishable. The physical tell was missed for an hour: **on a bus with pull-ups an absent
talker reads 0xFF, not 0x00**, so 0x00 could only ever have been the prefill or a device actively
driving eight zero bits.

### The proof
Pre-fill changed from `0x00` to the sentinel `0xA5` and three registers read instead of one:

```
[I2C] init=0 gate=0 whoami=0xa5a5a5 addr=0x5a ok=4 err=0 tmo=0 recov=0
```

Four transfers reported success. **Not one moved a byte.**

### Why
This build is TrustZone-secure. `gate_buf` is at `0x34013160` — the **secure** AXISRAM1_2_S alias
(`Debug/*.map`) — and I2C1 is a secure peripheral. `i2c1_dma_init()` called `HAL_DMA_Init()` and
`__HAL_LINKDMA()` but never set the channel's security/privilege/CID attributes, so GPDMA1
channels 0 and 1 ran at reset defaults and performed **non-secure** accesses. They could reach
neither end of the transfer.

The failure is invisible because the I2C peripheral does not depend on the DMA to finish: with
NBYTES set and AUTOEND, the I2C hardware clocks the byte into RXDR and generates STOP on its own.
`I2C_Mem_ISR_DMA` sees STOPF, calls `I2C_ITMasterCplt`, which calls `HAL_I2C_MemRxCpltCallback`.
Our `i2c_complete()` records `E_OK`. **HAL never reports an error, and the byte stays in RXDR.**

### The fix (`app_i2c.c`, `i2c1_dma_init()`)
```c
HAL_DMA_ConfigChannelAttributes(&hdma_i2c1_rx, DMA_CHANNEL_PRIV | DMA_CHANNEL_SEC |
                                               DMA_CHANNEL_SRC_SEC | DMA_CHANNEL_DEST_SEC);
HAL_DMA_ConfigChannelAttributes(&hdma_i2c1_tx, /* same */);
isolation.CidFiltering = DMA_ISOLATION_ON;
isolation.StaticCid    = DMA_CHANNEL_STATIC_CID_1;   /* = main.c:334 RIF_CID_1 */
HAL_DMA_SetIsolationAttributes(&hdma_i2c1_rx, &isolation);
HAL_DMA_SetIsolationAttributes(&hdma_i2c1_tx, &isolation);
```

Pattern and CID copied from two precedents already in the tree:
- `Lib/screenl/Src/scrl_spi.c:488-495` — this project's own SPI5 display path
- `STM32Cube_FW_N6/Drivers/BSP/STM32N6570-DK/stm32n6570_discovery_audio.c:3230, 3349, 3514` — ST's BSP

Distinct return codes so a future failure is identifiable from the printed `init=` alone,
without a debugger: `E_ID` (−18) = `ConfigChannelAttributes` failed, `E_NOSPT` (−9) =
`SetIsolationAttributes` failed (`mtkernel/include/tk/errno.h`).

### Why this mattered more than it looks
With `DEVCNF_USE_HAL_IIC = 0` and Option A, **every** sensor read on this project goes through
`i2c_rd`. Unfixed, the VL53L1X and the MPU6050 would both have "worked" — ACKing, no errors,
clean counters — while feeding the Edge Impulse training set nothing but stale buffer contents.
It would have surfaced weeks later as an inexplicable ML problem. **Promoted to RED ZONE #9 in
CLAUDE.md §3, with the sentinel rule as its corollary.**

---

## 3. SPEC CORRECTIONS MADE THIS SESSION

All against the local copy `docs/datasheets/drv2605l_datasheet.pdf` (TI SLOS854D Rev D, March 2018),
replacing web-fetch citations with repo-local ones.

| Item | Was | Is | Source |
|---|---|---|---|
| DRV2605L DEVICE_ID | "0b011 = 3" (handoff §5.4, `[UNVERIFIED]`) | **7 = DRV2605L**; 3 is the non-L DRV2605. STATUS reset = **0xE0** | §8.6.1 Table 4 |
| IN/TRIG max | "1.8 V, requires divider" (CLAUDE.md §2) | **abs max VDD+0.3 = 3.6 V**; VIH min 1.3 V. 3.3 V drive is legal, **no divider anywhere** | §6.1, §6.3 |
| The "1.8 V" | treated as a rating | `VI(ANA)` — analog-input-mode full-scale reference, for a mode this project does not use | §6.3 |
| SCL max (H-D8) | open | **400 kHz** with no wait states — `I2C_BUS_HZ 400000` is legal. **H-D8 CLOSED** | §6.7 |
| Min load (H-D6) | "8 Ω" | 8 Ω **at VDD = 5.2 V**, "ensured by design, not production tested" — quote the condition | §6.3 |
| EN low behaviour | — | device still **ACKs its address** but no register read/write is possible ⇒ an ACK proves the bus, only a register read proves EN | §8.4.1.3 |
| 7-bit address | inferred | **0x5A**, and silkscreened `I2C ADDR 0x5A` on the SmartElex board (photo 2026-08-29) | §8.5.1.1 |

Third cross-sensor contamination event caught. The DEVICE_ID error would have made a correct
board (reading 0xE0) look like a failure.

---

## 4. FIRMWARE STATE FOUND AT SESSION START

The handoff assumed a fresh write. It was wrong on all four §5.1 questions:

| Question | Answer |
|---|---|
| Does `app_i2c.c` exist? | Yes — complete L0/L1, commit `fc6ff88`, in the build (`Debug/Core/Src/subdir.mk:9`) |
| Has `MX_I2C1_Init` been called? | Never, and never needed — `app_i2c_init()` calls `HAL_I2C_Init()` directly (`app_i2c.c:212`) |
| GPIO AF for PH9/PC1? | Present — `i2c1_msp_init()` (`app_i2c.c:112-139`), `GPIO_AF4_I2C1`, AF_OD, NOPULL, VddIO4 first |
| Any blocking `HAL_I2C_Master_*`? | **None.** Only `_DMA` variants (`app_i2c.c:327,331`). Red zone clean |

Independently confirmed against ST's BSP header: `BUS_I2C1_SCL_PIN GPIO_PIN_9 / GPIOH`,
`BUS_I2C1_SDA_PIN GPIO_PIN_1 / GPIOC`, `AF4` (`stm32n6570_discovery_bus.h:74-79`).
`DEVCNF_USE_HAL_IIC 0` confirmed at `config_bsp.h:40`.

---

## 5. OPEN ITEMS CARRIED FORWARD

| ID | Item | How to close |
|---|---|---|
| **H-D9** | **Write direction UNPROVEN.** All four transfers were reads; a Mem_Read sends the register index via TXIS, not TX DMA, so `hdma_i2c1_tx` has never moved a byte | Handoff §5.4 step 3 — write a scratch value to a writable register, read it back, confirm match |
| **H-D10** | EN is on a 3V3 bench jumper, not PE7 | §5.4 step 4 — move EN to CN12 pin 1 (D8/PE7), re-read registers. Pass on rail / fail on PE7 ⇒ GPIO config |
| V-W-7 | VOL on SDA/SCL during traffic, target < 0.6 V | Baseline it now with one device, before the bus gets crowded and the I2C-PU jumpers have to come off |
| V-W-1 | CN8 pin 4 → pin 7 = 3.25–3.35 V | Still not metered. CubeProgrammer's "Target voltage 3.29 V" is the ST-LINK's VDD sense, **not** this |
| V-W-3 | 7SEMI XSHUT idle level | Deferred until the ToF is soldered |
| V-W-6 | ERM coil DC resistance ≥ 8 Ω | Before any motor is connected |
| — | `sysclk=400000000` vs CLAUDE.md §1 "600 MHz" and the DWT constant `cycles / 600000` | `[UNVERIFIED]` — CPU clock and system bus clock are separate on the N6, so 400 MHz for the bus may be correct. Verify which clock feeds CYCCNT before trusting any DWT-derived figure. RZ3's 3.375 µs came from the logic analyzer and is unaffected |
| — | `firmware/Lib/` and `firmware/Appli/Lib/` are two real duplicate trees, not symlinks | The build uses `firmware/Lib` (`-I../../Lib/...`); the VL53L1X ULD was unpacked into `firmware/Appli/Lib`. Cause of the two "Invalid project path" warnings. Fix the include paths to `../Lib/...` and drop the absolute ones from `.cproject` |

---

## 6. WORKFLOW NOTES EARNED THE HARD WAY

- **Flashing this board needs the external loader.** `0x70100000` is external XSPI flash;
  without `MX66UW1G45G_STM32N6570-DK.stldr` selected, CubeProgrammer fails at
  `Erasing memory corresponding to segment 0` → `failed to download Sector[0]` and drops the
  SWD link. The lost connection is the symptom, not the cause. Board in **Dev Boot** (BOOT1 HIGH
  + NRST) for programming, **Flash Boot** (both LOW) to run.
- **Eclipse caches file state.** An edit made outside the IDE is invisible until the project is
  refreshed — select the project and press **F5** before Ctrl+B, or the build is a no-op and the
  binary never changes.
- `picocom` holds `/dev/ttyACM0` exclusively; `fuser -v /dev/ttyACM0` names the holder.
- Verified by byte-pattern search in `Debug/*.bin` that a rebuild actually contains the intended
  change before flashing — cheaper than a flash cycle and a puzzled serial log.
