# data/ — published datasets

Feature rows captured from the board over UART on 2026-09-18 with the firmware's `[CSV]`
logger, already normalised by the firmware to integer milli-units in [-1000, 1000]
(`norm1000()` in `firmware/Appli/Core/Src/app_tasks.c`).

| File | Rows | Columns |
|---|---|---|
| `training_data_20260918.csv` | 37,216 | `source_file, surface, angle, speed`, 17 features, `label` — every FRESH row from 60 recordings |
| `ei_upload_training_20260918.csv` | 33,495 | 17 features, `label` — the 90 % uploaded to Edge Impulse |
| `held_out_test_vectors_20260918.csv` | 3,721 | same columns as the first file — the 10 % held out |

Features, in order: `d0..d9` (distance history, newest first), `v` (closing velocity), `a`
(closing acceleration), `ax, ay, az` (accelerometer), `amb`, `spad` (ToF ambient and signal
rate). `label` is the hazard rule (distance < 80 cm and closing velocity > 20 cm/s)
evaluated 12 frames later.

**Split caveat.** The 90/10 split is by row, not by recording: all 60 recordings contribute
rows to both sides, and consecutive rows share 9 of their 10 distance samples. Held-out
scores from this split are therefore optimistic compared with a split by recording or
session. `tests/tools/test_tools.py` asserts this property so it stays visible.

Only one raw recording is published, `logs/csv_hand_straight_normal_195036.log`; the other
59 are not in the repository.

Produced by [`tools/collect_data.sh`](../tools/collect_data.sh) and
[`tools/aggregate_training_data.py`](../tools/aggregate_training_data.py); see
[`model/README.md`](../model/README.md) for how the model was trained from them.
