# NPU saturation (raw=127) — root cause found

**Date:** 2026-09-18
**Status:** **ROOT CAUSE IDENTIFIED AND CONFIRMED** by static analysis of the generated
model, the linked ELF, and the flash artifact. **Not yet hardware-verified** — the board
was unplugged for this session (see §6 for the one remaining hardware step).
**Scope:** optional NPU path only. `HAZARD_CLASSIFIER_USE_NPU` remains undefined; the CPU
path (`app_hazard_classifier.c`) is untouched and ships regardless.

> **Update 2026-09-25 (status as shipped).** The fix was applied and confirmed on hardware:
> `network_data.hex` itself now carries the `0x71000000` address (the separate
> `network_data_0x71000000.hex` was byte-identical and has been removed), and in the
> 2026-09-19 30-minute soak the NPU output varies with the scene
> (`docs/evidence/phase6/`). `HAZARD_CLASSIFIER_USE_NPU` is now defined in the build, so the
> NPU path is the one that shipped. Sections below are the original 2026-09-18 record.

---

## 1. Root cause — the weights are flashed 12.5 MB away from where the NPU reads them

**The NPU reads its weights from `0x71000000`. `network_data.hex` is flashed at
`0x70380000`. Nothing was ever programmed at `0x71000000`.**

The generated model states its own weight location three times, unambiguously
(`firmware/Model/STM32N6570-DK/network.c`):

```
network.c:66   /* index=8 file postfix=xSPI2 name=octoFlash offset=0x71000000
                  absolute_mode size=117440504 READ_ONLY ... use4initializers=YES */

network.c:240  .name = "Gemm_2_weights_transposed_3",     .addr_base = 0x71000000UL,
               .offset_start = 0,    .offset_end = 544,   .is_param = 1   /* 17->32 */
network.c:264  .name = "Gemm_6_weights_transposed_9",     .addr_base = 0x71000000UL,
               .offset_start = 544,  .offset_end = 1057,  .is_param = 1   /* 32->16 */
network.c:288  .name = "Gemm_10_weights_transposed_15",   .addr_base = 0x71000000UL,
               .offset_start = 1072, .offset_end = 1088,  .is_param = 1   /* 16->1  */
```

The flash artifact says something else. `network_data.hex`'s single type-04 Extended
Linear Address record is `:02000004703852`, i.e. upper 16 bits = `0x7038`:

```
payload bytes: 1088
addr range:    0x70380000 .. 0x70380440
```

Origin of the mismatch — `model/generate-n6-model.sh:19`, inherited verbatim from the
YOLOX person-detection scaffold this project was cloned from:

```sh
arm-none-eabi-objcopy -I binary network_data.xSPI2.bin --change-addresses 0x70380000 -O ihex network_data.hex
```

`0x70380000` was the *person-detection* model's pool-8 offset. The regenerated
hazard-classifier model places pool 8 at `0x71000000`. The script's hardcoded address was
never updated; `CLAUDE.md` §4 and `docs/PROJECT_DEFENSE.md` §633 both still document the
stale `0x70380000`.

### Corroboration from the epoch-controller blob

`network_ecblobs.h` embeds the stream-engine source addresses directly in its
micro-instruction words. They match the `network.c` table exactly, and they are absolute:

| blob word (observed)   | decodes to | matches |
|------------------------|-----------|---------|
| `0x7100022024020043`   | `0x71000220` | `offset_start = 544` of layer 2 |
| `0x7100043038020043`   | `0x71000430` | `offset_start = 1072` of layer 3 |
| `0x7100046700000001`   | `0x71000467` | inside layer-3 region |
| `0x7100047f00000001`   | `0x7100047f` | end of param region |

`absolute_mode` in the pool declaration, plus `LL_ATON_RT_RELOC` being undefined project-wide,
means **no runtime relocation ever rewrites these addresses**. What is compiled in is what
the hardware fetches.

### Why this produces a *constant* output, not a noisy one

Unprogrammed NOR flash reads `0xFF` = `-1` in int8. With every weight in layer 1 equal to
the same constant, all 32 layer-1 accumulators compute the *identical* value; for realistic
inputs that value is negative and ReLU clamps all 32 to zero. From that point the network is
input-independent by construction: layer 2 sees 32 identical constants, layer 3 sees 16
identical constants, and the final byte is a fixed value — saturated to `INT8_MAX` because
the requant multipliers (which come from the blob in firmware, and *are* correct) are scaled
for real trained weights, not for `-1` everywhere.

That is exactly the observed signature: **zero variance across the whole run.**

---

## 2. The leading hypothesis (OSAL / NPU IRQ never armed) is DISPROVEN

Worth recording, because it was plausible and it is now closed with evidence.

**Claim under test:** `LL_ATON_OSAL_BARE_METAL` leaves `LL_ATON_OSAL_INSTALL_IRQ` and
`LL_ATON_OSAL_ENABLE_IRQ` as empty no-ops, so `triggered_events` is never set.

**Finding 1 — `ENABLE_IRQ` is not a no-op.** Only `INSTALL_IRQ` and `REMOVE_IRQ` are empty.
The `#ifndef LL_ATON_OSAL_ENABLE_IRQ` fallback at `ll_aton_osal.h:199-222` is a real
`switch` that calls `NVIC_EnableIRQ(CDNN0_IRQn)`. `INSTALL_IRQ` is *supposed* to be empty on
bare-metal Cortex-M — installation is static, via the vector table.

**Finding 2 — the handler resolves, and it is in the vector table of the binary that
produced your captured data** (`Debug/…​.elf`, timestamp 09:42, matching
`npu_diag_run4_20260918_0942.log`). Full chain:

```
ATON_STD_IRQ_LINE   = 0                                  (ll_aton_platform.h:383)
ATON_STD_IRQHandler = CDNN0_IRQHandler = NPU0_IRQHandler  (ll_aton_platform.h:194, 396)
ATON_STD_IRQn       = CDNN0_IRQn       = NPU0_IRQn = 53   (ll_aton_platform.h:199; stm32n657xx.h:109)
```

```
nm ll_aton_runtime.o   →  00000000 T NPU0_IRQHandler        (strong definition)
nm  …Appli.elf         →  34004af0 T NPU0_IRQHandler        (NOT Default_Handler @ 34003d0c)
startup_stm32n657x0hxq.s:201  .word NPU0_IRQHandler
                        :517  .weak NPU0_IRQHandler  → overridden by the strong def above
```

Vector table read straight out of the ELF — `.isr_vector` base `0x34000400`, IRQ53 is word
index 16+53 = 69 → byte offset `0x114` → address `0x34000514`:

```
 34000510  0d3d0034 f14a0034 0d3d0034 0d3d0034
                    ^^^^^^^^
           0x34000514 = 0x34004af1 = NPU0_IRQHandler + thumb bit
```

**The NPU interrupt is correctly installed and correctly enabled.** The epoch loop
completes normally — consistent with the log showing a `poste` value on every one of 325
frames rather than hanging.

### Answer to the specific open question (`_wait_mask`)

Determined statically from `network.c:167-190` — no debugger needed. The network has exactly
one real epoch block plus a terminator:

```c
{ .blob_address = _ec_blob_network_1_address,
  .wait_mask = 0,
  .flags = epoch_start | epoch_end | blob | pure_hw },
{ .flags = last_eb },
```

`__LL_ATON_RT_GetWaitMask()` (`ll_aton_runtime.c:307`) special-cases blob blocks:

```c
if (EpochBlock_IsEpochBlob(eb))
    return (1 << EpochBlock_EpochControllerUnit(eb));   // = 1 << eb->wait_mask = 1 << 0 = 1
```

So **`_wait_mask` = 1, non-zero** — the struct's literal `.wait_mask = 0` is reinterpreted as
*epoch-controller unit number 0*, not as a mask. The ASYNC completion path therefore genuinely
depends on the interrupt, and it works, because the interrupt is armed (above).

Also eliminated: `ec_inference_init` → `LL_ATON_EC_Inference_Init_network()` is
`{ return true; }` (`network_ecblobs.h:151-154`). It cannot fail.

---

## 3. What the captured log already told us, re-read

From `npu_diag_run4_20260918_0942.log`:

| field | observation | meaning |
|---|---|---|
| `postq` | **67 distinct values** (−31 … −128) | our quantized write lands; input genuinely varies; aliasing claim is true |
| `poste` | `127` on **325/325** frames | output constant, zero variance |
| `npu` | `1` on 326/326 | decision pinned |
| `cpu`  | `0` on 324, `1` on 2 | CPU path varies normally in the same run |

The plumbing was never the problem — the input reaches the NPU correctly. The NPU computes
correctly. It computes on **blank flash**.

---

## 4. The fix

One address. `network_data.hex` must be flashed at **`0x71000000`**, not `0x70380000`.

A corrected artifact is already generated and verified byte-identical:

```
firmware/Model/STM32N6570-DK/network_data_0x71000000.hex
```

Produced by re-basing the payload decoded out of the existing `network_data.hex` (not
re-derived from a separate source, so the data cannot drift), then independently decoded
back with `arm-none-eabi-objcopy -I ihex -O binary`:

```
existing hex : base=0x70380000 len=1088 md5=4d981086e460620a6a3d87b6e83ab873
raw.forverify:                 len=1088 md5=4d981086e460620a6a3d87b6e83ab873
NEW hex      : base=0x71000000 len=1088 md5=4d981086e460620a6a3d87b6e83ab873
```

The original `network_data.hex` is left untouched, and `generate-n6-model.sh` is **not**
edited yet — see §6.

No address collision: FSBL `0x70000000`, app `0x70100000` (~100 KB), stale weights
`0x70380000`. `0x71000000` is 16 MB into the 128 MB MX66UW1G45G and is free. Same
`MX66UW1G45G_STM32N6570-DK.stldr` external loader applies.

---

## 5. Second, separate bug — still open, unchanged by the above

The threshold-space mismatch you already identified stands and is **not** fixed by this.
`hazard_classify_npu()` compares the NPU's raw byte against `HAZ_LOGIT_THRESH_Q020 = 21`,
which was calibrated in **tensor 9** space (pre-sigmoid logit, scale 0.1126, zp 33). The
NPU emits **tensor 10** (post-sigmoid probability, scale 0.00390625, zp −128). Different
units. This must be re-derived against `held_out_test_vectors_20260918.csv` once the
saturation is cleared — do not fix it blind, and do not trust the ≈−77 mental estimate
without checking it against real data.

Fixing the threshold *before* fixing the weights would be actively misleading: with blank
weights the output is constant, so any threshold looks "stable".

---

## 6. What is left to do (needs the board — it was unplugged this session)

`dmesg` showed `usb 1-4: USB disconnect`; no `/dev/ttyACM*`, no STMicro device in `lsusb`.
A GDB session turned out not to be necessary — the bug is fully determined statically — but
the confirmation and the fix both need hardware.

1. **Confirm the diagnosis (10 seconds, no debug session needed).** Read flash at the two
   addresses and compare:
   ```
   STM32_Programmer_CLI -c port=SWD mode=HOTPLUG -el <path>/MX66UW1G45G_STM32N6570-DK.stldr \
       -r 0x71000000 0x40 blank_check.bin
   ```
   **Prediction: all `0xFF`.** The same read at `0x70380000` should show the real weight
   bytes (first bytes `04 E8 F4 1A FC 05 04 FF …`, per `network_data.hex` line 2).
   If `0x71000000` is `0xFF`, the diagnosis is confirmed outright.

2. **Flash the corrected weights** at `0x71000000` using
   `network_data_0x71000000.hex`. No rebuild or reflash of the application is needed — the
   firmware is unchanged; only flash contents change.

3. **Recapture** `[NPUDIAG]`:
   ```
   stty -F /dev/ttyACM0 115200 cs8 -cstopb -parenb raw -echo -echoe -echok -crtscts
   cat /dev/ttyACM0 | tee npu_diag_postfix_$(date +%Y%m%d_%H%M).log
   ```
   **Success criterion:** `poste` varies with the scene. It will NOT agree with `cpu=` yet —
   that needs §5's threshold fix — but `poste` moving at all is the proof.

4. **Only then**, make the fix permanent: update
   `model/generate-n6-model.sh:19` to `--change-addresses 0x71000000`, replace
   `network_data.hex`, and correct the stale `0x70380000` in `CLAUDE.md` §4 and
   `docs/PROJECT_DEFENSE.md` §633. Sequenced last so the documented flash address is only
   changed once it is proven on hardware.

---

## 7. Note for the project's own red-zone ledger

This is the same failure class as RED ZONE #9 (GPDMA channel attributes): **a silent success
with an untouched buffer.** The NPU reported completion, the runtime returned `DONE`, no
error surfaced anywhere, and the only symptom was a plausible-looking constant. It also
explains why ST's cloud validation was bit-exact — their flow flashes the weight blob at the
address the generated code declares. The local flash procedure used a hardcoded address
inherited from a different model.

Generalized rule, same shape as the existing "never pre-fill a DMA destination with a value
you would accept as a reading": **a model's weight-load address is generated output, not a
project constant. Re-read it from `network.c`'s pool table on every regenerate.**
