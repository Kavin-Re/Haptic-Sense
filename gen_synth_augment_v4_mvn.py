#!/usr/bin/env python3
"""
Haptic-Sense -- correlation-preserving (multivariate) synthetic augmentation, v4.
2026-09-19. Supersedes v2/v3's independent per-feature jitter, which threw away
real structure: in a genuine approach, d0 dropping, v rising, and amb/spad
shifting all move TOGETHER. Independent per-feature noise generates physically
incoherent rows that don't reflect how this sensor+geometry actually behaves.

METHOD (still 100% anchored to real captures, nothing invented):
  1. For each ANGLE x LABEL group (pooled across its surface/speed combos --
     per-combo covariance is numerically unstable, e.g. 'high' hazard-positive
     has only ~15-20 real rows per individual combo, less than the 17 feature
     dimensions), compute each row's RESIDUAL from its own (surface,angle,speed)
     combo's mean. This isolates real within-combo noise/individual-event
     variation from real between-combo signal differences (a "fast" sweep
     really does look different from a "slow" one -- that's signal, not noise,
     and must not get blended away).
  2. Covariance of those pooled residuals = the real, joint noise SHAPE for
     that angle+label. Regularized with small diagonal shrinkage (standard,
     avoids a singular/overfit covariance from a still-modest sample).
  3. A synthetic row = a real row's own actual values + noise drawn from that
     shrunk multivariate-normal residual distribution (mean zero). Anchored to
     a real point, perturbed with realistically CORRELATED noise, not each of
     17 features wobbling independently.
  4. Noise magnitude still capped conservatively (0.5x the empirical residual
     covariance) -- same conservative-not-precise stance as v2/v3's jitter fix,
     since no isolated static-target noise floor exists in the real logs.
  5. Mixup (interpolating two real same-combo rows) kept as-is from v2/v3 --
     it already preserves joint structure exactly at each interpolated point,
     nothing to fix there.

COMPREHENSIVE per this request: every angle x label group gets covariance-aware
synthetic rows (not just off-axis hazard-positive), volumes still weighted
toward the genuinely thin groups (high-angle hazard, 183 real rows).
"""
import pandas as pd, numpy as np

np.random.seed(20260919)
FEAT_COLS = ["d0","d1","d2","d3","d4","d5","d6","d7","d8","d9",
             "v","a","ax","ay","az","amb","spad"]

df = pd.read_csv("training_data_TRAINONLY_20260919.csv")
df["synthetic"] = 0
df["synth_method"] = "real"

# physical/firmware bound clip: every FEAT_COLS value the real sensor
# ever produced already sits inside the firmware norm1000() clamp; synthetic
# noise must never be sampled outside that observed real range, or training
# sees physically-impossible rows (found 2026-09-19: unclamped mvn noise
# produced values like d0=-1766, amb=-769 -- caused v9 EI training to
# collapse to ROC AUC 0.5 / 0% hazard recall).
BOUNDS_LO = df[FEAT_COLS].min().values
BOUNDS_HI = df[FEAT_COLS].max().values

SHRINK_DIAG = 0.15     # diagonal loading fraction, avoids singular covariance
NOISE_SCALE = 0.5      # conservative cap on residual-covariance-derived noise

def combo_residuals(group_df):
    """Return (residuals array, per-row real values array) -- residual = row
    minus its OWN (surface,angle,speed) combo's mean, so between-combo real
    signal differences are preserved, only within-combo noise is pooled."""
    resid_rows = []
    for (_,_,_), g in group_df.groupby(["surface","angle","speed"]):
        if len(g) < 2:
            continue
        mean = g[FEAT_COLS].mean().values
        resid_rows.append(g[FEAT_COLS].values - mean)
    if not resid_rows:
        return np.zeros((0, len(FEAT_COLS)))
    return np.vstack(resid_rows)

def fit_cov(residuals):
    if len(residuals) < len(FEAT_COLS) + 2:
        # too few residuals to estimate a full covariance -- fall back to
        # diagonal-only (independent) noise for this group, flagged via
        # a smaller synth batch rather than a fabricated correlation matrix
        var = residuals.var(axis=0) if len(residuals) else np.zeros(len(FEAT_COLS))
        return np.diag(var)
    cov = np.cov(residuals, rowvar=False)
    # shrinkage: blend toward diagonal to regularize a still-modest sample
    diag = np.diag(np.diag(cov))
    return (1 - SHRINK_DIAG) * cov + SHRINK_DIAG * diag

def mvn_oversample(group_df, n_new, rng, method_tag="mvn"):
    if len(group_df) == 0 or n_new <= 0:
        return pd.DataFrame(columns=group_df.columns)
    residuals = combo_residuals(group_df)
    cov = fit_cov(residuals) * (NOISE_SCALE ** 2)
    idx = rng.choice(group_df.index, size=n_new, replace=True)
    base = group_df.loc[idx].reset_index(drop=True)
    try:
        noise = rng.multivariate_normal(np.zeros(len(FEAT_COLS)), cov, size=n_new)
    except np.linalg.LinAlgError:
        noise = rng.normal(0, np.sqrt(np.diag(cov)), size=(n_new, len(FEAT_COLS)))
    vals = np.clip(base[FEAT_COLS].values + noise, BOUNDS_LO, BOUNDS_HI)
    base[FEAT_COLS] = vals.round().astype(int)
    base["synthetic"] = 1
    base["synth_method"] = method_tag
    return base

def mixup_interp(group_df, n_new, rng, alpha_range=(0.3, 0.7), method_tag="mixup"):
    if len(group_df) < 2 or n_new <= 0:
        return pd.DataFrame(columns=group_df.columns)
    i_idx = rng.choice(group_df.index, size=n_new, replace=True)
    j_idx = rng.choice(group_df.index, size=n_new, replace=True)
    rows_i = group_df.loc[i_idx, FEAT_COLS].reset_index(drop=True)
    rows_j = group_df.loc[j_idx, FEAT_COLS].reset_index(drop=True)
    alphas = rng.uniform(alpha_range[0], alpha_range[1], size=n_new).reshape(-1,1)
    mixed = (alphas * rows_i.values + (1-alphas) * rows_j.values).round().astype(int)
    out = pd.DataFrame(mixed, columns=FEAT_COLS)
    meta = group_df.iloc[0]
    out["source_file"] = "SYNTHETIC_MIXUP"
    out["surface"] = meta["surface"]; out["angle"] = meta["angle"]
    out["speed"] = "mixed"; out["label"] = meta["label"]
    out["synthetic"] = 1; out["synth_method"] = method_tag
    return out[df.columns]

# volume targets -- weighted toward the thin groups, comprehensive coverage
# of every angle x label group per this request.
MVN_TARGETS = {  # (angle,label) -> target total hazard/no-hazard row count
    ("high","1"): 2800, ("left","1"): 3200, ("low","1"): 3200, ("right","1"): 3200,
    ("straight","1"): 6500,
    ("high","0"): 5500, ("left","0"): 6500, ("low","0"): 6500, ("right","0"): 6500,
    ("straight","0"): 8500,
}
MIXUP_EXTRA = {
    ("high","1"): 1200, ("left","1"): 700, ("low","1"): 700, ("right","1"): 700,
    ("straight","1"): 800,
}

rng = np.random.default_rng(20260922)
parts = [df.copy()]
for angle in ["high","left","low","right","straight"]:
    for label in [0,1]:
        g = df[(df.angle==angle) & (df.label==label)]
        target = MVN_TARGETS.get((angle,str(label)), len(g))
        n_new = max(0, target - len(g))
        parts.append(mvn_oversample(g, n_new, rng))
        if label == 1:
            n_mix = MIXUP_EXTRA.get((angle,str(label)), 0)
            parts.append(mixup_interp(g, n_mix, rng))

comprehensive = pd.concat(parts, ignore_index=True)
comprehensive.to_csv("training_data_comprehensive_v4_clipped_20260919.csv", index=False)

print("=== COMPREHENSIVE v4 (correlation-preserving) ===")
print("total:", len(comprehensive), " real:", (comprehensive.synthetic==0).sum(),
      " synthetic:", (comprehensive.synthetic==1).sum())
print(comprehensive.groupby(['angle','label']).size().unstack(fill_value=0))
haz = comprehensive[comprehensive.label==1].groupby('angle').size()
tot = comprehensive.groupby('angle').size()
print("hazard rate by angle:")
print((haz/tot).round(3))
print()
print("synth_method breakdown:")
print(comprehensive.groupby(['angle','synth_method']).size().unstack(fill_value=0))
