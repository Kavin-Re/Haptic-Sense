# Phase 3 Design — Minimal LED+UART Application ("Heartbeat")

**Project:** Haptic-Sense · TRON Forum Contest 2026
**Prerequisite state:** Phase 2.6 complete — signed `-Trusted.bin` flashed @ `0x70100000`, FSBL chain verified executing on hardware.
**Status:** DESIGN — not yet implemented.

---

## 1. Objective

Replace the reference repo's camera→YOLO application with the smallest possible µT-Kernel app that proves, end-to-end, that **our own application code** (not the prebuilt reference binaries) survives the full pipeline:

```
edit → build → sign → flash → boot → observable behavior
```

Observable behavior = LD1 blinking + periodic `tm_printf` heartbeat on the STLINK VCP.

**Why this phase exists:** Phase 2.6 proved the *pipeline* with reference binaries. Phase 3 proves the pipeline with *code we control*, and — critically — measures the **iteration loop time** (edit-to-running-on-board). Every later phase pays this loop cost dozens of times per day; it must be known and minimized now.

## 2. Scope

**IN:**
- Strip camera/YOLO/display pipeline from the app
- One heartbeat task: toggle LD1 + `tm_printf` counter
- Keep the full boot chain (FSBL → signed app @ `0x70100000`) unchanged
- Time and document the iteration loop (Dev Boot workflow)

**OUT (explicitly deferred):**
- All I2C. `DEVCNF_USE_HAL_IIC` **stays 0** in `config_bsp/stm32_cube/config_bsp.h:40`. The flip to `1` is a Phase 5 (sensor bring-up) action — recorded here so it isn't lost.
- Sensors, DRV2605L, NPU/model, semaphores, the Priority 1–3 task architecture (Phase 4).
- `network_data.hex` — leave the flashed weights in place; the app simply won't reference them. Do not erase that region.

## 3. Hardware used this phase

| Item | Value | Source |
|---|---|---|
| User LED | **LD1, green, PO1, active HIGH** | UM3300 Table 8 (verified via live lookup, 2026-07-04); Zephyr board def concurs |
| LED **not** used | LD2 (red, PG10, active LOW) — UM3300 notes it may indicate BOOTFAILEDN; leave untouched | UM3300 Table 8 |
| Debug UART | USART1, PE5 (TX) / PE6 (RX), STLINK VCP, 115200 8N1 | CLAUDE.md §1 |
| Boot mode switch | BOOT1 (PA6/SW1) HIGH + NRST = Dev Boot; both LOW = Flash Boot | CLAUDE.md §4 |

**Caution — GPIO port O is shared territory.** PO0/PO2/PO3/PO4 carry XSPI1 (PSRAM) signals per the Zephyr pin map — the very memory the app executes from. PO1 itself is free (it's the LED), but the GPIO init code must configure **only PO1** on that port: no port-wide writes, no `GPIO_PIN_All`, use pin-masked HAL calls only.

**Unverified item (verify before coding):** the exact clock-enable macro for port O (expected `__HAL_RCC_GPIOO_CLK_ENABLE()`) — confirm it exists in `STM32Cube_FW_N6/.../stm32n6xx_hal_rcc.h` in the bundled firmware package before use. Do not assume from other STM32 families.

## 4. Design

### 4.1 What to strip / keep from the reference app

| Reference repo element | Action |
|---|---|
| `app.c` camera init, ISP, YOLO invocation, display/LCD output | **Remove** (their pipeline does not apply — CLAUDE.md §5) |
| Kernel init + task creation skeleton (`app.c` ~1040–1132) | **Keep as pattern**, reduce to one task |
| `mtk3_bsp2/`, linker script `STM32N657X0HXQ_LRUN.ld`, signing post-build step | **Unchanged** |
| `Lib/AI_Runtime/` link references | Keep linked if removing breaks the build; otherwise prune later. Priority is a green build, not a minimal binary. |

### 4.2 Task design

One task. Not priority 1–3 (reserved for Phase 4 architecture per CLAUDE.md §3).

| Property | Value |
|---|---|
| Task | `heartbeat_task` |
| TK_PRI | **10** (mid-range; verify free in `sysdef.h` alongside the 1–3 check — Red Zone #7 pre-work done early) |
| Body | loop: toggle LD1 → `tm_printf("[HB] %d uptime_ms=%lu\n", n, ms)` → `tk_slp_tsk(500)` |
| Uptime source | `tk_get_otm()` — doubles as first exercise of the timing API needed in Phase 4/6 |

µT-Kernel APIs exercised (deliberately minimal): `tk_cre_tsk`, `tk_sta_tsk`, `tk_slp_tsk`, `tk_get_otm`, `tm_printf`. This is the "hello world" of every API class Phase 4 depends on except semaphores.

Note: `tm_printf` in a task body is acceptable **only in this phase and only at TK_PRI 10**. The Phase 4 rule (no printf in priority 1–2 tasks) is not violated because those tasks don't exist yet.

### 4.3 Files & licensing

- New file: `Appli/app_heartbeat.c` (or edit `app.c` in place — Option A: edit in place, faster; Option B: new file, cleaner diff against reference. Developer decides; recommend **B** so the reference `app.c` stays as readable pattern source).
- Own code: Apache 2.0 SPDX header. Never on `mtk3_bsp2/` (T-License 2.2) or ST-SLA files (CLAUDE.md §7).

## 5. Iteration loop to establish (the real deliverable)

1. Edit code in CubeIDE.
2. Build → post-build signing runs → **verify `*-Trusted.bin` exists and is non-zero** (every build, per CLAUDE.md §4).
3. BOOT1 HIGH + NRST → Dev Boot (debug port open).
4. CubeProgrammer: flash app binary @ `0x70100000` via external loader `MX66UW1G45G_STM32N6570-DK.stldr`. FSBL and weights are already resident — do **not** re-flash them each cycle.
5. BOOT1 LOW + NRST → Flash Boot → observe LD1 + VCP output (`minicom`/`picocom` @ 115200).
6. **Record the wall-clock time of steps 1–5.** Target: < 3 min/cycle. If worse, investigate before Phase 4 (options: keep programmer connected in Dev Boot and only NRST-cycle; script the CLI flash with `STM32_Programmer_CLI`).

## 6. Exit criteria (all must pass)

- [ ] Build: 0 errors, signed binary present and non-zero
- [ ] LD1 blinks at ~1 Hz (500 ms toggle) — visually confirmed
- [ ] `tm_printf` heartbeat visible on VCP with monotonically increasing counter and sane `tk_get_otm()` values
- [ ] Board runs from **Flash Boot** (cold power-cycle test: unplug → replug → runs unattended)
- [ ] Iteration loop timed and documented in CLAUDE.md
- [ ] `sysdef.h` checked: priorities 1, 2, 3, 10 free — finding documented in a comment block (closes Red Zone #7 pre-work)

## 7. Failure triage (ranked by likelihood)

1. **Builds but no LED, no UART** — most likely app never reached `main`/kernel start: wrong load address, signing step skipped, or stale binary flashed. Test: re-flash the known-good Phase 2.6 reference app binary; if it runs, the pipeline is fine and the fault is in our app. *(likely)*
2. **LED works, no UART** — USART1 init/muxing lost when stripping camera code, or VCP terminal settings. Test: loopback of terminal settings + confirm `tm_printf` path enabled in BSP config. *(likely)*
3. **Runs in Dev Boot, dead in Flash Boot** — signature/header issue. Test: byte-compare flashed vs. built `-Trusted.bin` via CubeProgrammer read-back. *(speculative until observed)*
4. **Hard fault after seconds/minutes** — port-O GPIO misconfiguration disturbing XSPI (see §3 caution). Test: comment out LED code, run UART-only. *(speculative)*

---
*After exit criteria pass: update CLAUDE.md (iteration loop time, sysdef.h finding, LD1 pin added to hardware map) and proceed to Phase 4.*
