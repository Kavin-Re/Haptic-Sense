#!/usr/bin/env python3
"""
Haptic-Sense Block 6 -> Block 7 aggregation script.
Reads all real collection log files in logs/, keeps the correct final
version of each of the 60 surface/angle/speed combos (skipping known-bad
or superseded attempts), filters to FRESH rows only (ivs==165 and
tvs==165, per DATA_COLLECTION_PROTOCOL_20260916.md Sec 8), and writes
one flat training CSV with provenance columns plus the 17 features + label.
"""
import re, csv, glob, os, sys
from collections import defaultdict, Counter

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
LOGDIR = os.path.join(SCRIPT_DIR, "logs")
OUTFILE = os.path.join(SCRIPT_DIR, "training_data_20260918.csv")

# Known-bad / superseded files -- excluded in favor of their _REDO or later attempt.
EXCLUDE = {
    "csv_hand_high_normal_200654.log",   # 100% IMU/ToF dead -- replaced by 010742_REDO
    "csv_hand_high_slow_200602.log",     # partial IMU/ToF dropout -- replaced by 010519_REDO
    "csv_hand_left_slow_195503.log",     # pacing varied (1st attempt) -- replaced eventually
    "csv_hand_left_slow_010203_REDO.log",# pacing still varied (2nd attempt) -- replaced by 010344_REDO
    "csv_phone_high_slow_005235.log",    # pacing varied -- replaced by 010841_REDO
}

CSV_LINE_RE = re.compile(
    r"\[CSV\]\s+"
    r"d0=(-?\d+)\s+d1=(-?\d+)\s+d2=(-?\d+)\s+d3=(-?\d+)\s+d4=(-?\d+)\s+"
    r"d5=(-?\d+)\s+d6=(-?\d+)\s+d7=(-?\d+)\s+d8=(-?\d+)\s+d9=(-?\d+)\s+"
    r"v=(-?\d+)\s+a=(-?\d+)\s+ax=(-?\d+)\s+ay=(-?\d+)\s+az=(-?\d+)\s+"
    r"amb=(-?\d+)\s+spad=(-?\d+)\s+"
    r"ivs=(\d+)\s+iva=(\d+)\s+tvs=(\d+)\s+tva=(\d+)\s+label=(\d+)"
)

FEATURE_COLS = ["d0","d1","d2","d3","d4","d5","d6","d7","d8","d9",
                "v","a","ax","ay","az","amb","spad"]

FNAME_RE = re.compile(r"^csv_([a-z]+)_([a-z]+)_([a-z]+)_")

def parse_filename(fname):
    m = FNAME_RE.match(fname)
    if not m:
        return None, None, None
    return m.group(1), m.group(2), m.group(3)

def main():
    all_files = sorted(glob.glob(os.path.join(LOGDIR, "csv_*.log")))
    used_files = [f for f in all_files if os.path.basename(f) not in EXCLUDE]
    excluded_present = [f for f in all_files if os.path.basename(f) in EXCLUDE]

    per_file_stats = []
    combo_seen = defaultdict(list)
    total_lines_seen = 0
    total_fresh_kept = 0
    hazard_count = 0
    nohazard_count = 0

    rows_out = []

    for fpath in used_files:
        fname = os.path.basename(fpath)
        surface, angle, speed = parse_filename(fname)
        if surface is None:
            print(f"WARNING: could not parse surface/angle/speed from {fname}, skipping", file=sys.stderr)
            continue
        combo_seen[(surface, angle, speed)].append(fname)

        file_csv_lines = 0
        file_fresh = 0
        with open(fpath, "r", errors="replace") as fh:
            for line in fh:
                if "[CSV]" not in line:
                    continue
                m = CSV_LINE_RE.search(line)
                if not m:
                    continue
                file_csv_lines += 1
                vals = m.groups()
                d = vals[0:10]
                v, a, ax, ay, az, amb, spad = vals[10:17]
                ivs, iva, tvs, tva, label = vals[17:22]
                if ivs != "165" or tvs != "165":
                    continue
                file_fresh += 1
                rows_out.append([fname, surface, angle, speed] + list(d) + [v,a,ax,ay,az,amb,spad] + [label])
                if label == "1":
                    hazard_count += 1
                else:
                    nohazard_count += 1

        total_lines_seen += file_csv_lines
        total_fresh_kept += file_fresh
        per_file_stats.append((fname, surface, angle, speed, file_csv_lines, file_fresh))

    # Write output CSV
    header = ["source_file","surface","angle","speed"] + FEATURE_COLS + ["label"]
    with open(OUTFILE, "w", newline="") as out:
        w = csv.writer(out)
        w.writerow(header)
        w.writerows(rows_out)

    # ---- Report ----
    print("=" * 70)
    print("AGGREGATION REPORT")
    print("=" * 70)
    print(f"Files found in logs/:        {len(all_files)}")
    print(f"Files excluded (bad/superseded): {len(excluded_present)}")
    for f in excluded_present:
        print(f"  - {f}")
    print(f"Files used:                  {len(used_files)}")
    print()

    # combo coverage check -- expect exactly 60 unique combos, each with exactly 1 file used
    expected_surfaces = ["hand","sleeve","wall","phone"]
    expected_angles = ["straight","left","right","high","low"]
    expected_speeds = ["slow","normal","fast"]
    missing = []
    duplicate = []
    for s in expected_surfaces:
        for a in expected_angles:
            for sp in expected_speeds:
                key = (s,a,sp)
                n = len(combo_seen.get(key, []))
                if n == 0:
                    missing.append(key)
                elif n > 1:
                    duplicate.append((key, combo_seen[key]))
    print(f"Expected combos: {len(expected_surfaces)*len(expected_angles)*len(expected_speeds)}")
    print(f"Combos with exactly 1 used file: {60 - len(missing) - len(duplicate)}")
    if missing:
        print(f"MISSING combos ({len(missing)}):")
        for k in missing:
            print(f"  - {k}")
    if duplicate:
        print(f"DUPLICATE combos (more than one file used, needs manual check) ({len(duplicate)}):")
        for k, files in duplicate:
            print(f"  - {k}: {files}")
    print()

    print(f"Total raw [CSV] rows scanned:  {total_lines_seen}")
    print(f"Total FRESH rows kept:         {total_fresh_kept}  ({100.0*total_fresh_kept/total_lines_seen:.1f}%)")
    print(f"  hazard (label=1):            {hazard_count}  ({100.0*hazard_count/total_fresh_kept:.1f}%)")
    print(f"  no-hazard (label=0):         {nohazard_count}  ({100.0*nohazard_count/total_fresh_kept:.1f}%)")
    print()

    print("Per-file FRESH retention (flag anything under 50%):")
    for fname, surface, angle, speed, csvn, fresh in per_file_stats:
        pct = 100.0*fresh/csvn if csvn else 0
        flag = "  <-- LOW" if pct < 50 else ""
        print(f"  {fname:45s} {surface:7s} {angle:9s} {speed:7s} csv={csvn:5d} fresh={fresh:5d} ({pct:5.1f}%){flag}")

    print()
    print(f"Output written: {OUTFILE}")
    print(f"Output rows: {len(rows_out)}")

if __name__ == "__main__":
    main()
