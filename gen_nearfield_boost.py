#!/usr/bin/env python3
"""
Near-field (<200mm) hazard-positive boost, correlation-preserving, same method
as the angle boost (residual-from-combo-mean covariance + shrinkage, anchored
to real rows). Adds to comprehensive_v4 rather than rebuilding from scratch --
the angle fix and the distance fix are orthogonal and both real.
"""
import pandas as pd, numpy as np

np.random.seed(20260919)
FEAT_COLS = ["d0","d1","d2","d3","d4","d5","d6","d7","d8","d9",
             "v","a","ax","ay","az","amb","spad"]
SHRINK_DIAG = 0.15
NOISE_SCALE = 0.5

real = pd.read_csv("training_data_TRAINONLY_20260919.csv")
real["d0_mm"] = real["d0"]*625/1000 + 675

# same physical bound clip as gen_synth_augment_v4_mvn.py -- see that file
# for why (2026-09-19 v9 training collapse root cause).
BOUNDS_LO = real[FEAT_COLS].min().values
BOUNDS_HI = real[FEAT_COLS].max().values

def combo_residuals(group_df):
    resid_rows = []
    for _, g in group_df.groupby(["surface","angle","speed"]):
        if len(g) < 2:
            continue
        mean = g[FEAT_COLS].mean().values
        resid_rows.append(g[FEAT_COLS].values - mean)
    if not resid_rows:
        return np.zeros((0, len(FEAT_COLS)))
    return np.vstack(resid_rows)

def fit_cov(residuals):
    if len(residuals) < len(FEAT_COLS) + 2:
        var = residuals.var(axis=0) if len(residuals) else np.zeros(len(FEAT_COLS))
        return np.diag(var)
    cov = np.cov(residuals, rowvar=False)
    diag = np.diag(np.diag(cov))
    return (1 - SHRINK_DIAG) * cov + SHRINK_DIAG * diag

def mvn_oversample(group_df, n_new, rng, method_tag):
    if len(group_df) == 0 or n_new <= 0:
        return pd.DataFrame(columns=list(group_df.columns)+["synthetic","synth_method"])
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

def mixup_interp(group_df, n_new, rng, method_tag, alpha_range=(0.3,0.7)):
    if len(group_df) < 2 or n_new <= 0:
        return pd.DataFrame(columns=list(group_df.columns)+["synthetic","synth_method"])
    i_idx = rng.choice(group_df.index, size=n_new, replace=True)
    j_idx = rng.choice(group_df.index, size=n_new, replace=True)
    rows_i = group_df.loc[i_idx, FEAT_COLS].reset_index(drop=True)
    rows_j = group_df.loc[j_idx, FEAT_COLS].reset_index(drop=True)
    alphas = rng.uniform(*alpha_range, size=n_new).reshape(-1,1)
    mixed = (alphas*rows_i.values + (1-alphas)*rows_j.values).round().astype(int)
    out = pd.DataFrame(mixed, columns=FEAT_COLS)
    meta = group_df.iloc[0]
    out["source_file"]="SYNTHETIC_MIXUP_NEARFIELD"; out["surface"]=meta["surface"]
    out["angle"]=meta["angle"]; out["speed"]="mixed"; out["label"]=meta["label"]
    out["synthetic"]=1; out["synth_method"]=method_tag
    return out

rng = np.random.default_rng(20260923)
band0 = real[(real.d0_mm < 100) & (real.label==1)]
band1 = real[(real.d0_mm >= 100) & (real.d0_mm < 200) & (real.label==1)]
print(f"real near-field hazard rows: 0-100mm={len(band0)}  100-200mm={len(band1)}")

TARGET_0_100   = 1200
TARGET_100_200 = 2000
MIXUP_0_100    = 500
MIXUP_100_200  = 800

parts = []
parts.append(mvn_oversample(band0, TARGET_0_100 - len(band0), rng, "mvn_nearfield"))
parts.append(mixup_interp(band0, MIXUP_0_100, rng, "mixup_nearfield"))
parts.append(mvn_oversample(band1, TARGET_100_200 - len(band1), rng, "mvn_nearfield"))
parts.append(mixup_interp(band1, MIXUP_100_200, rng, "mixup_nearfield"))

nearfield_synth = pd.concat(parts, ignore_index=True)
# drop the helper d0_mm column before merging (comprehensive_v4 doesn't have it)
nearfield_synth = nearfield_synth.drop(columns=["d0_mm"], errors="ignore")

comp_v4 = pd.read_csv("training_data_comprehensive_v4_clipped_20260919.csv")
comp_v5 = pd.concat([comp_v4, nearfield_synth], ignore_index=True)
comp_v5.to_csv("training_data_comprehensive_v5_clipped_20260919.csv", index=False)

comp_v5["d0_mm"] = comp_v5["d0"]*625/1000 + 675
bins=[0,100,200,400,800,1300,5000]; labels=["0-100mm","100-200mm","200-400mm","400-800mm","800-1300mm",">1300mm"]
comp_v5["band"] = pd.cut(comp_v5["d0_mm"], bins=bins, labels=labels)
print("\n=== comprehensive_v5: rows by distance band x label ===")
print(comp_v5.groupby(["band","label"], observed=True).size().unstack(fill_value=0))

n=len(comp_v5); n0=(comp_v5.label==0).sum(); n1=(comp_v5.label==1).sum()
print(f"\ntotal={n}  label0={n0} ({100*n0/n:.1f}%)  label1={n1} ({100*n1/n:.1f}%)")
print(f"balanced class_weight -> {{0: {n/(2*n0):.3f}, 1: {n/(2*n1):.3f}}}")

# sanity check correlation preservation in the new near-field synthetic rows
real_b1 = band1
synth_b1 = comp_v5[(comp_v5.synth_method=="mvn_nearfield") & (comp_v5.d0_mm>=100) & (comp_v5.d0_mm<200)]
print(f"\n--- 100-200mm band sanity check ---")
print("real   d0~v corr:", round(real_b1['d0'].corr(real_b1['v']),3), " n=", len(real_b1))
print("synth  d0~v corr:", round(synth_b1['d0'].corr(synth_b1['v']),3), " n=", len(synth_b1))

FEAT = FEAT_COLS + ["label"]
comp_v5[FEAT].to_csv("ei_upload_comprehensive_v5_clipped_20260919.csv", index=False)
print("\nEI-upload-ready file written:", len(comp_v5), "rows")
