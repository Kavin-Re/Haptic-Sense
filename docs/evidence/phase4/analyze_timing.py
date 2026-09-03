#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
analyze_timing.py — Phase 4 preemption-latency analysis (Red Zone #3)
Haptic-Sense / TRON Forum Contest 2026

Measures hazard-path preemption + context-switch latency from sigrok logic-
analyzer captures (runbook docs/PHASE4_RUNBOOK_timing_campaign.md §6).

Input:  docs/evidence/phase4/timing_chatter*.sr  (Run A, chatter ON)
        Channels: D0 = PH5/TIMING_D0 (inference_task signals result_ready_sem)
                  D1 = PD6/TIMING_D1 (hazard_task first action after wake)
        Sample rate 8 MHz -> 125 ns/sample.

Method: export each capture to CSV via `sigrok-cli -i <file> -O csv`
        (streamed, not written to disk). For every RISING edge on D0, find
        the NEXT rising edge on D1. dt = (D1_sample - D0_sample) * 125 ns.

Rules:  events pooled across all input files. A D0 rising edge with no D1
        rising edge before the next D0 edge is a MISSED hazard event —
        counted and reported, never silently discarded (expected: zero).
        A trailing D0 with neither a D1 nor a following D0 before end of
        capture is a capture-boundary truncation, reported separately.

Pass:   max dt < 1000 us over the pooled event set (>= 100 events).
"""

import glob
import math
import os
import subprocess
import sys

SAMPLE_NS = 125.0          # 8 MHz sample rate
CPU_MHZ = 800              # DWT cross-check: cycles = us * 800
# CORRECTED 2026-09-03. Was 600. G-8 closed 2026-08-30: CPUCLK is 800 MHz and
# DWT->CYCCNT counts the processor clock (CLAUDE.md sections 1 and 3, evidence
# docs/evidence/phase5/PHASE5_T1_CYCCNT_CLOCK_20260830.md). The stale constant
# made both archived analysis_run*.txt print "2,025 cycles @ 600 MHz" for the
# 3.375 us worst case -- and those two files are the submission evidence for
# the project's headline number. 3.375 x 800 = 2,700 cycles.
# REGENERATE BOTH .txt FILES WHILE THE .sr CAPTURES STILL EXIST.
PASS_LIMIT_US = 1000.0     # < 1 ms, hardware-verified
HIST_BINS = 20
HIST_BAR_WIDTH = 56

EVIDENCE_DIR = os.path.dirname(os.path.abspath(__file__))


def scan_capture(path):
    """Stream one .sr file through sigrok-cli CSV export; return
    (d0_high, d1_high, n_samples) where *_high are ascending lists of
    sample indices at which the channel reads '1'."""
    proc = subprocess.Popen(
        ["sigrok-cli", "-i", path, "-O", "csv"],
        stdout=subprocess.PIPE,
    )
    out = proc.stdout

    # Header: ';' comment lines, then exactly one 'logic,logic' line.
    while True:
        line = out.readline()
        if not line:
            raise RuntimeError(f"{path}: EOF before CSV header")
        if line.startswith(b";"):
            continue
        if line.strip() == b"logic,logic":
            break
        raise RuntimeError(f"{path}: unexpected CSV header line {line!r}")

    # Body: fixed 4-byte records b"<d0>,<d1>\n". '1' bytes are rare
    # (pulses are microseconds wide at 125 ns/sample), so scan with
    # bytes.find — memchr speed — instead of iterating 100M lines.
    d0_high = []
    d1_high = []
    rec_base = 0
    carry = b""
    first_record_checked = False
    CHUNK = 4 << 20  # 4 MiB, multiple of record size

    while True:
        chunk = out.read(CHUNK)
        if not chunk:
            break
        if carry:
            chunk = carry + chunk
        usable = len(chunk) - (len(chunk) % 4)
        carry = chunk[usable:]
        body = chunk[:usable]

        if not first_record_checked and len(body) >= 4:
            if body[0:1] not in (b"0", b"1") or body[1:2] != b"," or \
               body[3:4] != b"\n":
                raise RuntimeError(
                    f"{path}: CSV record format mismatch: {body[:4]!r} "
                    "(expected fixed 4-byte '<d0>,<d1>\\n' records)")
            first_record_checked = True

        pos = body.find(b"1")
        while pos != -1:
            col = pos % 4
            if col == 0:
                d0_high.append(rec_base + pos // 4)
            elif col == 2:
                d1_high.append(rec_base + pos // 4)
            else:  # '1' in a separator/newline slot = corrupt export
                raise RuntimeError(f"{path}: '1' at record column {col}")
            pos = body.find(b"1", pos + 1)
        rec_base += usable // 4

    if carry:
        raise RuntimeError(f"{path}: {len(carry)} trailing bytes "
                           "(not a whole record)")
    rc = proc.wait()
    if rc != 0:
        raise RuntimeError(f"sigrok-cli exited {rc} on {path}")
    return d0_high, d1_high, rec_base


def rising_edges(high_samples):
    """Ascending high-sample indices -> rising-edge sample indices
    (start of each contiguous high run)."""
    edges = []
    prev = -2
    for s in high_samples:
        if s != prev + 1:
            edges.append(s)
        prev = s
    return edges


def pair_events(d0_edges, d1_edges):
    """For each D0 rising edge, take the NEXT D1 rising edge.
    Returns (deltas_in_samples, missed_count, truncated_count)."""
    deltas = []
    missed = 0
    truncated = 0
    j = 0  # cursor into d1_edges (both lists ascending)
    for i, t0 in enumerate(d0_edges):
        while j < len(d1_edges) and d1_edges[j] <= t0:
            j += 1
        next_d0 = d0_edges[i + 1] if i + 1 < len(d0_edges) else None
        if j < len(d1_edges) and (next_d0 is None or d1_edges[j] < next_d0):
            deltas.append(d1_edges[j] - t0)
        elif next_d0 is not None:
            missed += 1        # a real hazard event with no hazard wake
        else:
            truncated += 1     # capture ended between D0 and its D1
    return deltas, missed, truncated


def histogram(values_us, bins=HIST_BINS, width=HIST_BAR_WIDTH):
    lo, hi = min(values_us), max(values_us)
    if hi == lo:
        hi = lo + 1e-9
    span = (hi - lo) / bins
    counts = [0] * bins
    for v in values_us:
        idx = min(int((v - lo) / span), bins - 1)
        counts[idx] += 1
    peak = max(counts)
    lines = []
    for b, c in enumerate(counts):
        b_lo = lo + b * span
        b_hi = b_lo + span
        bar = "#" * (round(c * width / peak) if c else 0)
        lines.append(f"  {b_lo:8.2f} - {b_hi:8.2f} us | {c:6d} | {bar}")
    return "\n".join(lines)


def main():
    # Input glob as argv[1]; default = Run A chatter captures. Both runs use
    # this identical analysis path — do not fork the logic.
    if len(sys.argv) > 1:
        pattern = sys.argv[1]
        files = sorted(glob.glob(pattern))
        if not files:  # retry relative to the evidence dir
            pattern = os.path.join(EVIDENCE_DIR, sys.argv[1])
            files = sorted(glob.glob(pattern))
    else:
        pattern = os.path.join(EVIDENCE_DIR, "timing_chatter*.sr")
        files = sorted(glob.glob(pattern))
    if not files:
        sys.exit(f"no captures match {pattern}")

    # Run label derives from the input pattern so a baseline run never
    # mislabels itself as a load run (and vice versa).
    base = os.path.basename(pattern)
    load_label = ("under chatter load" if "chatter" in base
                  else "at baseline (chatter off)" if "baseline" in base
                  else f"for input {base}")

    print(f"Phase 4 preemption-latency analysis — input: {pattern} ({load_label})")
    print(f"sample rate 8 MHz -> {SAMPLE_NS} ns/sample; "
          f"pass line: max dt < {PASS_LIMIT_US:.0f} us\n")

    all_deltas = []
    total_missed = 0
    total_truncated = 0

    for path in files:
        d0_high, d1_high, n_samples = scan_capture(path)
        d0_edges = rising_edges(d0_high)
        d1_edges = rising_edges(d1_high)
        deltas, missed, truncated = pair_events(d0_edges, d1_edges)
        us = [d * SAMPLE_NS / 1000.0 for d in deltas]
        all_deltas.extend(us)
        total_missed += missed
        total_truncated += truncated
        dur_s = n_samples / 8_000_000
        fmax = f"{max(us):.2f}" if us else "-"
        print(f"  {os.path.basename(path):24s} {dur_s:6.2f} s  "
              f"D0 edges={len(d0_edges):4d}  paired={len(deltas):4d}  "
              f"missed={missed}  truncated@EOF={truncated}  max={fmax} us")

    n = len(all_deltas)
    print(f"\npooled events N = {n}"
          f"   missed = {total_missed}   truncated@EOF = {total_truncated}")
    if total_missed:
        print("  *** MISSED HAZARD EVENTS DETECTED — a D0 fired with no D1 "
              "before the next D0. This is a FINDING, not noise. ***")
    if n == 0:
        sys.exit("no paired events — nothing to analyze")
    if n < 100:
        print(f"  WARNING: N < 100 — below the campaign minimum")

    mn = min(all_deltas)
    mx = max(all_deltas)
    mean = sum(all_deltas) / n
    sigma = math.sqrt(sum((v - mean) ** 2 for v in all_deltas) / n)

    print(f"\n  min   = {mn:10.3f} us")
    print(f"  mean  = {mean:10.3f} us")
    print(f"  sigma = {sigma:10.3f} us")
    print(f"  max   = {mx:10.3f} us   "
          f"({mx * CPU_MHZ:,.0f} cycles @ {CPU_MHZ} MHz — DWT cross-check)")

    print("\nhistogram (pooled dt):")
    print(histogram(all_deltas))

    verdict = "PASS" if mx < PASS_LIMIT_US else "FAIL"
    print(f"\n{verdict}: worst-case dt = {mx:.3f} us "
          f"{'<' if verdict == 'PASS' else '>='} {PASS_LIMIT_US:.0f} us "
          f"over {n} events {load_label}"
          + (f" ({total_missed} missed)" if total_missed else ""))
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
