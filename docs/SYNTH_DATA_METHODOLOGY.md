# Synthetic off-axis augmentation — methodology and sources (2026-09-19)

Generated from the real 37,216-row baseline (`training_data_20260918.csv`, 60 real
hardware-collected surface x angle x speed combos). No physical re-collection was
done tonight — all three files below are derived from real captures already on disk.

## Files produced
- `training_data_light_v2_20260919.csv`      — 39,720 rows (2,504 synthetic)
- `training_data_aggressive_v2_20260919.csv` — 41,120 rows (3,904 synthetic)
- `training_data_broad_20260919.csv`         — 52,132 rows (14,916 synthetic)

All three tag every row `synthetic` (0/1) and `synth_method`
(real / jitter / mixup / mixup_crossspeed). `held_out_test_vectors_20260918.csv`
was NOT touched and stays 100% real — that must remain the only eval set.

## What was researched, and what it changed

1. **VL53L1X angle-of-incidence response: no published spec exists.**
   [ST VL53L1X datasheet](https://www.st.com/resource/en/datasheet/vl53l1x.pdf) gives
   FOV (27° typical) and reflectance-vs-accuracy, but nothing on off-axis/angle
   response — because the sensor is single-zone with no directional sensitivity.
   The "angle" effect this project measures is emergent (hand geometry sweeping
   through a narrow FOV from different directions), specific to this rig, not a
   documented sensor characteristic. **Conclusion: off-axis synthetic rows stay
   strictly anchored to this project's own real off-axis captures — there is
   nothing to source a broader angle model from.**

2. **Jitter magnitude was wrong in the first pass — corrected here.**
   Datasheet repeatability spec: ±0.15%–1% of reading (32-sample std, static
   target). The original jitter (0.4× each combo's full-sweep std) came out to
   9–21% of reading when checked against that spec — 10-20x too large, because
   sweep std mixes real hand motion with sensor noise. Frame-to-frame diffs are
   still motion-contaminated (~9-19%, hand covers real distance in 20ms even
   during a slow sweep) — isolating a true static noise floor would need a new
   bench session, which was explicitly declined tonight. **Fix: jitter factor
   cut 0.4 → 0.10, a conservative approximation, not a lab-measured value.**

3. **Reflectance vs. accuracy: datasheet claims dark-condition ranging accuracy
   is ~reflectance-invariant (±20mm across white/gray/dark targets)** — but our
   own 4 real materials show real mean-distance differences (hand/phone/sleeve/
   wall: 405/390/326/216mm). No reliable reflectance% is known for our actual
   materials, so **no 5th/6th synthetic material was invented** — extrapolating
   without a real reflectance measurement would be exactly the kind of
   assumption this project has been burned by before (`PROJECT_AUDIT_20260903.md`
   §1.3 — a noiseless synthetic ramp hid a real bug until real noisy data
   arrived). Stayed with the 4 real materials only.

4. **[ST community: ambient rate vs. reflectance](https://community.st.com/imaging-sensors-49/vl53l1x-ambient-rate-drops-with-better-target-reflectance-28116)**
   — confirms `amb`/`spad` shift with reflectance because the sensor throttles
   active SPADs at high signal strength (qualitative mechanism, not a formula).
   Not used to generate new rows — informational only, explains why `amb`/`spad`
   already vary by surface in the real data.

5. **[MPU6050 datasheet](https://www.cdiweb.com/datasheets/invensense/mpu-6050_datasheet_v3%204.pdf)**
   — accel noise PSD 400 µg/√Hz, zero-G offset ±50mg (X/Y) / ±80mg (Z). Used only
   as a plausibility bound on IMU jitter, not as a replacement data source — IMU
   jitter is still drawn from this project's own per-combo real std.

## Honest disclosure for the write-up

- These are **synthetically augmented**, not additional real hardware captures.
  Straight-down data is 100% real throughout; off-axis hazard-positive rows are
  a mix of real + synthetic (see exact ratios per file above).
- The **BROAD file leans heavily synthetic on the `high` angle specifically**:
  of its ~4,400 total hazard-positive rows, only 200 are real — the rest are
  jitter/mixup derivatives of those 200. That ratio should be stated plainly if
  BROAD is the version that ships, not left for a judge to discover.
- Whichever file gets trained on: **re-derive class_weight and the operating
  threshold fresh** (do not reuse v4's `class_weight=3.35`/`p=0.20`, tuned for a
  different class balance) — likely cause of v5/v6's across-the-board
  regression. **Evaluate only against `held_out_test_vectors_20260918.csv`**
  (100% real, untouched) — never against synthetic rows.

## 2026-09-19 addendum: unclamped-noise bug found and fixed

`gen_synth_augment_v4_mvn.py` and `gen_nearfield_boost.py` (the
correlation-preserving/mvn method superseding everything above) sampled
multivariate-normal noise and added it to a real anchor row **without
clamping to the real, physically-achievable range**. Result: ~24% of `mvn`
rows and ~16% of `mvn_nearfield` rows held physically-impossible values
(e.g. `d0=-1766`, `amb=-769` — outside the firmware's own `norm1000()`
clamp and outside anything the real sensor ever produced). The v9 EI Studio
training run on this data collapsed completely: ROC AUC 0.5, 0% hazard
recall/precision, loss 26.8 — the model learned nothing.

Fix: both scripts now clip every synthetic feature value to
`[real_min, real_max]` per column (computed from
`training_data_TRAINONLY_20260919.csv`, i.e. the exact range real captures
ever exhibited) before rounding. Re-run, verified 0 out-of-range values,
identical row counts/class balance/correlation-preservation as before the
fix. Outputs renamed with a `_clipped` suffix so the buggy v4/v5 files stay
on disk for the audit trail but are not the ones to upload:
`training_data_comprehensive_v5_clipped_20260919.csv` /
`ei_upload_comprehensive_v5_clipped_20260919.csv` are the correct files to
train on going forward. **Do not use v9's export or any file without
`_clipped` in the name.**
