# docs/ — what to read, and what is history

Start with the top-level `README.md` (what the device does, how to operate it, how to build
and flash it). The documents below go deeper.

## Reference (describes the shipped state)

| File | What it covers |
|---|---|
| `PROVENANCE.md` | Origin of every non-original file; files removed or replaced |
| `NPU_SATURATION_DEBUG_FINDINGS.md` | NPU weight-address bug, fix, and hardware confirmation |
| `SYNTH_DATA_METHODOLOGY.md` | How the synthetic training data used in later model experiments was made |
| `evidence/phase4/` | Logic-analyzer captures and analysis behind the 3.375 µs latency figure |
| `evidence/phase5/` | Sensor bring-up and I2C soak logs, with per-finding write-ups |
| `evidence/phase6/README.md` | Full-pipeline 30-minute soak behind the 4.19 µs figure |
| `../model/README.md` | Which model the firmware carries and how it was checked |
| `../CLAUDE.md` | Engineering rulebook: pin map, task rules, red zones |

## Build history (dated snapshots)

Everything else in `docs/` (`PROJECT_DEFENSE.md`, the `PHASE*`, `BLOCK*`, `HANDOFF*` and
plan documents, `audits/`, `design/`) is a point-in-time record written during the build. They are kept unedited as the
project's working history. Later documents supersede earlier ones where they disagree, and
the firmware source is the final word.

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
- `logs/*.log`: raw serial captures. The ones that back published figures are in
  `evidence/`.
