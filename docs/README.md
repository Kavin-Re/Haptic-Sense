# docs/ — what to read, and what is history

Start with the top-level [`README.md`](../README.md) (what the device does, how to operate
it, how to build and flash it). The documents below go deeper.

## Reference (describes the shipped state)

| File | What it covers |
|---|---|
| [`PROVENANCE.md`](PROVENANCE.md) | Origin of every non-original file; files removed or replaced |
| [`../THIRD_PARTY_NOTICES.md`](../THIRD_PARTY_NOTICES.md) | Licence of each bundled third-party component, by path |
| [`NPU_SATURATION_DEBUG_FINDINGS.md`](NPU_SATURATION_DEBUG_FINDINGS.md) | NPU weight-address bug, fix, and hardware confirmation |
| [`SYNTH_DATA_METHODOLOGY.md`](SYNTH_DATA_METHODOLOGY.md) | How the synthetic training data used in later model experiments was made |
| [`evidence/phase4/`](evidence/phase4/) | Logic-analyzer captures and analysis behind the 3.375 µs latency figure |
| [`evidence/phase5/`](evidence/phase5/) | Sensor bring-up and I2C soak logs, with per-finding write-ups |
| [`evidence/phase6/README.md`](evidence/phase6/README.md) | Full-pipeline 30-minute soak behind the 4.19 µs figure |
| [`../model/README.md`](../model/README.md) | Which model the firmware carries and how it was checked |
| [`../data/README.md`](../data/README.md) | The published datasets and their split |
| [`../tests/README.md`](../tests/README.md) | Host-side unit tests and what they do and do not cover |
| [`../CLAUDE.md`](../CLAUDE.md) | Engineering rulebook: pin map, task rules, red zones |

## Design and review record (point-in-time, kept unedited)

| Where | What |
|---|---|
| [`PROJECT_DEFENSE.md`](PROJECT_DEFENSE.md) | The 2026-07-12 silent-failure catalogue and verification ledger |
| `PHASE3_DESIGN_*`, `PHASE4_DESIGN_*`, `PHASE5_DESIGN_*`, `PHASE5_L1_*` | Per-phase design documents (LED/UART bring-up, task architecture, sensor bring-up, I2C transfer layer) |
| [`PHASE4_RUNBOOK_timing_campaign.md`](PHASE4_RUNBOOK_timing_campaign.md) | How the logic-analyzer preemption campaign was run |
| [`BUS2_SCL_FREQUENCY_20260830.md`](BUS2_SCL_FREQUENCY_20260830.md) | I2C SCL frequency measurement write-up |
| [`design/`](design/) | Driver port designs (VL53L1X, MPU6050, DRV2605L) and the feature-frame validity design |
| [`audits/`](audits/) | The 2026-07-11 red-zone audits and the 2026-09-03 full-project audit |
| [`dev-log/`](dev-log/README.md) | Dated hand-offs, plans, pre-flight reviews and runbooks, with an index |

These were written during the build and are kept as the project's working history. Later
documents supersede earlier ones where they disagree, and the firmware source is the final
word. The dated hand-off and plan documents moved from `docs/` to `docs/dev-log/` on
2026-11-19 without a change of file name; older text and source comments that say
`docs/<name>.md` for one of them mean `docs/dev-log/<name>.md`.

## Files referenced here but not published

Some documents cite files that are not in this repository:

- `CONTEST_LOGISTICS.md`, `PACK_SHIP_SUBMIT_CHECKLIST_20260915.md`,
  `FINAL_STRETCH_RUNBOOK_20260919.md`: internal contest and shipping logistics, withheld.
- `BLOCK2_LOG.md`, `ML_LABEL_DECISION.md`, `ADVERSARIAL_REVIEW_I2C_DECISION_20260905.md`,
  `evidence/phase5/PHASE5_I2C_TWISTED_JOINT_20260905.md`, and paths under `claude/`: working
  notes kept outside the repository.
- `datasheets/*_datasheet.pdf`, `mpu6050_*.pdf`: vendor datasheets (TI, ST, InvenSense),
  not redistributed. Get them from the vendors.
- The STM32N6570-DK board schematic (MB1939-N6570-C02), cited as "schematic-verified" in
  `CLAUDE.md`: download it from ST's
  [STM32N6570-DK product page](https://www.st.com/en/evaluation-tools/stm32n6570-dk.html)
  (CAD Resources → Schematic Pack).
- `logs/*.log`: raw serial captures. One is published
  (`logs/csv_hand_straight_normal_195036.log`); the ones that back published figures are in
  `evidence/`.
