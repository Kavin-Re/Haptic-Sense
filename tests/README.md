# tests/ — host-side tests

None of this needs the board, the ARM toolchain or the ST packages. CI runs both suites on
every push (`.github/workflows/ci.yml`).

## `host/` — C unit tests of the firmware's decision logic

```
make -C tests/host          # gcc + AddressSanitizer + UBSan
make -C tests/host SAN=0    # without sanitizers
```

The firmware sources are compiled **unmodified** with the host gcc. Where a file needs the
kernel or the HAL, it gets the minimal stand-in headers in `host/stubs/`; the test file
supplies the functions behind them.

| Test | Firmware source under test | What it checks |
|---|---|---|
| `test_velocity_lsq` | `velocity_lsq.c` | Sign convention, units, rounding, real-timestamp handling, degenerate windows (no divide by zero), noise averaging, large uptime |
| `test_hazard_classifier` | `app_hazard_classifier.c` + weights | Requantisation and clamping, input saturation, and a regression over the 3,721 held-out rows in `data/` |
| `test_app_tasks` | `app_tasks.c` (built as shipped, `HAZARD_CLASSIFIER_USE_NPU`) | Urgency → pulse interval (400 ms at 20 cm/s, −4 ms per cm/s, 75 ms floor), hazard = model OR rule with strict boundaries, canary stamping and detection, feature-frame assembly, FRESH/HELD/INVALID ageing, the `[CSV]` label shift, task priorities and start-up order |

`test_app_tasks` runs the real task bodies one iteration at a time: the stubbed blocking
calls `longjmp` back to the test. There is no scheduler, so these tests say nothing about
preemption or timing — that evidence is on hardware, in `docs/evidence/`. The NPU itself
is replaced by a stub that returns whatever the test asks for.

## `tools/` — Python tests of the data tools (stdlib `unittest`)

```
python3 -m unittest discover -s tests/tools -v
```

Checks that `tools/aggregate_training_data.py` reproduces the published rows for the one
published raw recording, and that the three CSVs in `data/` are mutually consistent.
