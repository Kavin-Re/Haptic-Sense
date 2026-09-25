#!/usr/bin/env python3
"""
Haptic-Sense -- self-serve live-capture analyzer. No Claude session needed
to run this; it's pure stdlib. Written 2026-09-19 as a backup so threshold
validation isn't blocked on Claude being available.

Parses [NPUDIAG] lines from a live serial capture (see
docs/FINAL_STRETCH_RUNBOOK_20260919.md Phase B for how to make one) and
reports, for the CURRENT v10 PROVISIONAL thresholds, how many frames each
path would flag as hazard -- using the RAW pre/postq bytes already in the
log, so this works even though the log was captured with the OLD firmware
thresholds baked into its own cpu=/npu= fields.

Line format (from app_tasks.c's tm_printf calls, HAZARD_NPU_DIAG block):
  [NPUDIAG] seq=<N> match|MISMATCH cpu=<0/1> npu=<0/1> pre=<int> postq=<int> poste=<int>
  pre    = CPU path's raw pre-sigmoid logit byte (tensor 9 space)
           -> compare against HAZ_LOGIT_THRESH_Q
  postq  = NPU path's raw post-sigmoid probability byte (tensor 10 space)
           -> compare against HAZ_PROB_THRESH_Q
  cpu/npu = the OLD firmware's own decisions at capture time -- NOT what
           v10's thresholds would decide; ignore these for v10 evaluation,
           they're only useful to compare old-vs-new behavior if you want.

CAVEAT (same as this project's own history in app_hazard_classifier.h):
match lines are throttled to 1-per-50 frames, MISMATCH lines are always
printed. This script's frame counts reflect ONLY what's in the log file,
not literally every frame the board processed.

There is NO ground truth in this file -- a live capture has no per-frame
hand-labeled "was this actually a hazard" column. Use this script to see
WHEN and HOW OFTEN each candidate threshold fires, then judge for yourself
whether those moments line up with what you actually did in front of the
sensor during the capture (this is exactly the same manual cross-check
this project's threshold history has always used -- see the ATTEMPT 1/2/
CURRENT comments above HAZ_PROB_THRESH_Q in app_hazard_classifier.h).

Usage:
    python3 analyze_live_capture.py path/to/capture.log
    python3 analyze_live_capture.py path/to/capture.log --logit-thresh 35 --prob-thresh -51
"""
import argparse, re, sys

LINE_RE = re.compile(
    r"\[NPUDIAG\] seq=(?P<seq>\d+) (?P<status>match|MISMATCH) "
    r"cpu=(?P<cpu>\d) npu=(?P<npu>\d) "
    r"pre=(?P<pre>-?\d+) postq=(?P<postq>-?\d+) poste=(?P<poste>-?\d+)"
)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("logfile")
    ap.add_argument("--logit-thresh", type=int, default=35,
                     help="HAZ_LOGIT_THRESH_Q candidate (v10 default: 35)")
    ap.add_argument("--prob-thresh", type=int, default=-51,
                     help="HAZ_PROB_THRESH_Q candidate (v10 default: -51)")
    args = ap.parse_args()

    rows = []
    with open(args.logfile, errors="replace") as f:
        for line in f:
            m = LINE_RE.search(line)
            if m:
                rows.append({k: int(v) if k != "status" else v
                             for k, v in m.groupdict().items()})

    if not rows:
        print(f"No [NPUDIAG] lines found in {args.logfile}.")
        print("Check: was HAZARD_NPU_DIAG defined in this build? Is this the right log file?")
        sys.exit(1)

    n = len(rows)
    n_mismatch = sum(1 for r in rows if r["status"] == "MISMATCH")
    cpu_v10 = [1 if r["pre"] >= args.logit_thresh else 0 for r in rows]
    npu_v10 = [1 if r["postq"] >= args.prob_thresh else 0 for r in rows]
    agree = sum(1 for a, b in zip(cpu_v10, npu_v10) if a == b)

    print(f"log: {args.logfile}")
    print(f"[NPUDIAG] lines parsed: {n}  (of which {n_mismatch} were logged MISMATCH; "
          f"remember matches are throttled to 1-per-50, this is not literally every frame)")
    print()
    print(f"v10 CANDIDATE thresholds: HAZ_LOGIT_THRESH_Q={args.logit_thresh}  "
          f"HAZ_PROB_THRESH_Q={args.prob_thresh}")
    print(f"  CPU-path-would-fire (pre>={args.logit_thresh}):   {sum(cpu_v10)}/{n} frames "
          f"({100*sum(cpu_v10)/n:.1f}%)")
    print(f"  NPU-path-would-fire (postq>={args.prob_thresh}):  {sum(npu_v10)}/{n} frames "
          f"({100*sum(npu_v10)/n:.1f}%)")
    print(f"  CPU/NPU agreement at these v10 thresholds: {agree}/{n} ({100*agree/n:.1f}%)")
    print()
    print("Frames where the v10 CPU-path candidate would fire (seq, pre, postq) --")
    print("cross-check these moments against what you actually did in front of the sensor:")
    fired = [r for r, c in zip(rows, cpu_v10) if c == 1]
    if not fired:
        print("  (none -- if you deliberately approached the sensor during this capture,")
        print("   that's a bad sign for this threshold, same failure mode as the v9/near-field")
        print("   issues already documented. Don't just accept it silently.)")
    for r in fired[:200]:
        print(f"  seq={r['seq']:<6} pre={r['pre']:<5} postq={r['postq']:<5} "
              f"(old-firmware decision at capture time: cpu={r['cpu']} npu={r['npu']})")
    if len(fired) > 200:
        print(f"  ... and {len(fired)-200} more")

if __name__ == "__main__":
    main()
