# docs/dev-log/ — dated working documents

Hand-offs, plans, pre-flight reviews and runbooks written during the build (August –
September 2026). They are kept as the project's working history and are **not** edited to
match the shipped state: later documents supersede earlier ones, and the firmware source is
the final word. For the current state start at the top-level [`README.md`](../../README.md)
and [`docs/README.md`](../README.md).

These files lived directly under `docs/` until 2026-11-19. Text inside them, in other dated
documents, and in firmware source comments still cites them by the old path
(`docs/<name>.md`); the file names did not change, so `docs/<name>.md` now means
`docs/dev-log/<name>.md`. Some also cite files that were never published — see "Files
referenced here but not published" in [`docs/README.md`](../README.md).

| Date | File | What it is |
|---|---|---|
| 2026-08-30 | [`HANDOFF_20260830_BLOCK1B.md`](HANDOFF_20260830_BLOCK1B.md) | Hand-off after the Block 0 / 1a session (DRV2605L first light) |
| 2026-08-30 | [`PHASE5_T0_T1_REVIEW_20260830.md`](PHASE5_T0_T1_REVIEW_20260830.md) | Block 1a review and the CPU-clock (800 MHz) analysis |
| 2026-08-30 | [`HANDOFF_20260830_BLOCK2.md`](HANDOFF_20260830_BLOCK2.md) | Hand-off into Block 2 (superseded the same day) |
| 2026-08-30 | [`HANDOFF_20260831_BLOCK2.md`](HANDOFF_20260831_BLOCK2.md) | Hand-off into Block 2, replacing the one above |
| 2026-08-30 | [`PLAN_TO_SUBMISSION_20260830.md`](PLAN_TO_SUBMISSION_20260830.md) | Block-by-block plan to the submission date |
| 2026-08-30 | [`RISK_ANALYSIS_20260830.md`](RISK_ANALYSIS_20260830.md) | Gap and risk analysis (the G-n items cited in the source) |
| 2026-08-30 | [`BLOCK2_PREFLIGHT_20260830.md`](BLOCK2_PREFLIGHT_20260830.md) | Review pass before the VL53L1X was soldered |
| 2026-08-30 | [`BLOCK2_FOUR_DAY_PLAN.md`](BLOCK2_FOUR_DAY_PLAN.md) | Four-day bench plan, 30 Aug – 3 Sep |
| 2026-09-01 | [`WHILE_YOU_WAIT_20260901.md`](WHILE_YOU_WAIT_20260901.md) | Task list for 1 – 3 Sep |
| 2026-09-03 | [`HANDOFF_20260903.md`](HANDOFF_20260903.md) | Hand-off (superseded 2026-09-06) |
| 2026-09-05 | [`BLOCK5_MECHANICAL_FREEZE_RUNBOOK_20260905.md`](BLOCK5_MECHANICAL_FREEZE_RUNBOOK_20260905.md) | Mechanical-freeze runbook before shipping the unit |
| 2026-09-15 | [`BLOCK6_TO_SUBMISSION_TECHNICAL_PLAN_20260915.md`](BLOCK6_TO_SUBMISSION_TECHNICAL_PLAN_20260915.md) | Technical plan from data collection to submission |
| 2026-09-16 | [`DATA_COLLECTION_PROTOCOL_20260916.md`](DATA_COLLECTION_PROTOCOL_20260916.md) | Data-collection protocol: label shift k, normalisation constants, surfaces × angles × speeds |
| 2026-09 | [`DATA_COLLECTION_HANDOFF_GUIDE.md`](DATA_COLLECTION_HANDOFF_GUIDE.md) | Step-by-step instructions for a helper running `tools/collect_data.sh` |
