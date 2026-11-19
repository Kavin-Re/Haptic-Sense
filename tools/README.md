# tools/ — host-side data and evaluation scripts

Everything here runs on a PC, not on the board. Install the optional dependencies with
`pip install -r tools/requirements.txt`; the first three scripts need only the standard
library (or bash).

| Script | Needs | What it does |
|---|---|---|
| `collect_data.sh` | bash, a serial port | Guided capture of `[CSV]` feature rows from the board, one recording per surface × angle × speed, into `logs/`. |
| `aggregate_training_data.py` | stdlib | Parses `logs/csv_*.log`, keeps FRESH rows only, writes the flat training CSV. Refuses to overwrite an existing output without `--force`. |
| `analyze_live_capture.py` | stdlib | Summarises `[NPUDIAG]` lines from a live capture against candidate thresholds. |
| `eval_v10.py` | numpy, pandas, ai-edge-litert | Scores a held-out CSV with a `.tflite` model. |
| `gen_synth_augment_v4_mvn.py`, `gen_nearfield_boost.py` | numpy, pandas | Generators for the synthetic augmentation used in the v5–v10 experiments ([method](../docs/SYNTH_DATA_METHODOLOGY.md)). |

## What can and cannot be re-run from this repository

- `aggregate_training_data.py` **can** be checked: the repository publishes one of the 60
  raw recordings (`logs/csv_hand_straight_normal_195036.log`), and
  `tests/tools/test_tools.py` verifies that aggregating it reproduces that recording's 996
  rows of `data/training_data_20260918.csv` exactly.
- `eval_v10.py` and the two `gen_*` scripts are kept as the record of how the later
  experiments were run. They read intermediate files from the current directory
  (`training_data_TRAINONLY_20260919.csv`, `held_out_test_vectors_WITH_ANGLE_20260918.csv`,
  …) that are **not** published here, and the v6–v10 models they relate to are not in the
  firmware (see [`model/README.md`](../model/README.md)). They are lint-clean but were not
  re-executed after being moved into this directory.

Lint: `ruff check .` from the repository root (configuration in `ruff.toml`).
