# Model files

| File | What it is |
|---|---|
| `haptic_sense_hazard_v4.tflite` | The model the committed firmware is generated from. INT8, 17 → 32 → 16 → 1 (ReLU, ReLU, sigmoid). Edge Impulse project 1115874, 2026-09-17. |
| `generate-n6-model.sh` | ST Edge AI / Neural-ART conversion to `firmware/Model/STM32N6570-DK/` (weights placed at `0x71000000`). |
| `user_neuralart.json` | Neural-ART compiler profile used by the script. |

**Regenerating the NPU artifacts needs one file this repo does not include.**
`user_neuralart.json` points at `./my_mpools/stm32n6-app2.mpool`, the Neural-ART memory-pool
description from ST's STM32N6 sample application
([STM32N6-GettingStarted-ObjectDetection](https://github.com/STMicroelectronics/STM32N6-GettingStarted-ObjectDetection),
where it is currently `Model/my_mpools/stm32n6-app2_STM32N6570-DK.mpool`). It is ST's file
and is not redistributed here. The firmware does not need it: the generated artifacts in
`firmware/Model/STM32N6570-DK/` are committed and build as they are.

## How v4 was matched to the firmware (checked 2026-09-25)

- All six weight/bias tensors equal `firmware/Appli/Core/Inc/app_hazard_classifier_weights.h`
  (the CPU path) exactly.
- `firmware/Model/STM32N6570-DK/network_data.hex` (the NPU weights, 1,088 bytes at
  `0x71000000`) contains the layer-1 (544 B) and layer-3 (16 B) weights byte-for-byte; the
  layer-2 weights (512 B) are present as a permutation, which is the NPU's tiled layout.

## Training pipeline for v4

`collect_data.sh` (serial capture per surface × angle × speed) → `aggregate_training_data.py`
(FRESH rows only, 37,216 rows from 60 recordings) → 90/10 split into
`ei_upload_training_20260918.csv` / `held_out_test_vectors_20260918.csv` → Edge Impulse,
raw features with no DSP block, class weight 3.35, 50 epochs. Held-out result at p = 0.20:
recall 81.3 %, precision 64.1 %, F1 0.717.

## Later experiments (not in the firmware's weights)

Models v5–v10 were trained on 2026-09-18/19 with added off-axis and near-field data, part
of it synthetic (`docs/SYNTH_DATA_METHODOLOGY.md`; the scripts that made the final
augmented set are `gen_synth_augment_v4_mvn.py` and `gen_nearfield_boost.py`). None of
them was converted into firmware artifacts in this repository: v5 was byte-identical to v4,
and v6–v10 are not what the weights above contain.

**Known inconsistency.** The decision thresholds in
`firmware/Appli/Core/Inc/app_hazard_classifier.h` (`HAZ_PROB_THRESH_Q = -51`,
`HAZ_LOGIT_THRESH_Q = 35`) were derived from v10 with `eval_v10.py`, while the weights are
v4's. The v4-validated NPU threshold is kept in the same header as
`HAZ_PROB_THRESH_Q_V4_LOCKED (-87)`. Because the firmware ORs the model's decision with the
rule `d < 800 mm AND v > 20 cm/s` (search "SAFETY-NET FALLBACK" in `app_tasks.c`), hazard
alerts do not depend on the model alone.
