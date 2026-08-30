#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
BUS-2 — measure the delivered I2C1 SCL period from a sigrok capture.

Usage:
    sigrok-cli -i bus2_scl_20260830.sr -O csv > bus2_scl.csv
    python3 analyze_scl.py bus2_scl.csv --rate 8e6 --channel D2

Why this exists: ST's timing algorithm targets tSCL = tSCL_L + tSCL_H + trise + tfall
and hits 2500 ns exactly, but only 2150 ns of that is hardware -- the other 350 ns is an
ASSUMED worst-case edge time. On this board CLAUDE.md computes tr ~= 76 ns, which would put
the real period near 2246 ns (~445 kHz) against a 400 kHz limit on both the DRV2605L
(SLOS854D 6.6) and the VL53L1X (datasheet Table 7). See docs/BUS2_SCL_FREQUENCY_20260830.md.

PASS is period >= 2500 ns. Only periods INSIDE a byte are counted -- the gaps between
bytes, the ACK slot boundaries and the idle time between transactions are excluded by an
outlier filter, because they are not clock periods.
"""
import argparse, statistics, sys

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("csv")
    ap.add_argument("--rate", type=float, default=8e6, help="sample rate in Hz (default 8e6)")
    ap.add_argument("--channel", default="D2", help="SCL channel name as it appears in the CSV header")
    a = ap.parse_args()

    ts = 1e9 / a.rate                       # ns per sample
    col, rows = None, []
    with open(a.csv) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith(";"):
                continue
            parts = [p.strip() for p in line.split(",")]
            if col is None:
                if a.channel in parts:
                    col = parts.index(a.channel)
                    continue
                # header without our channel, or headerless: try to guess
                if not parts[0].lstrip("-").isdigit():
                    sys.exit("channel %s not in header: %s" % (a.channel, parts))
                col = 0
            try:
                rows.append(int(parts[col]))
            except (ValueError, IndexError):
                continue

    if not rows:
        sys.exit("no samples parsed -- check --channel and the CSV")

    # rising-edge indices
    edges = [i for i in range(1, len(rows)) if rows[i - 1] == 0 and rows[i] == 1]
    if len(edges) < 20:
        sys.exit("only %d rising edges -- capture more, or wrong channel" % len(edges))

    periods = [(edges[i] - edges[i - 1]) * ts for i in range(1, len(edges))]

    # Keep only in-byte periods. Clock periods cluster tightly; gaps between bytes and
    # between transactions are far larger. Median-anchored window, generous either side.
    med = statistics.median(periods)
    inb = [p for p in periods if 0.55 * med <= p <= 1.45 * med]
    if len(inb) < 20:
        sys.exit("only %d in-byte periods after filtering -- capture more" % len(inb))

    mean = statistics.mean(inb)
    print("samples        : %d at %.0f Hz (%.2f ns/sample)" % (len(rows), a.rate, ts))
    print("rising edges   : %d   in-byte periods kept: %d of %d"
          % (len(edges), len(inb), len(periods)))
    print()
    print("SCL period  min %8.1f ns   median %8.1f ns   mean %8.1f ns   max %8.1f ns"
          % (min(inb), statistics.median(inb), mean, max(inb)))
    print("SCL freq    max %8.1f kHz  median %8.1f kHz  mean %8.1f kHz"
          % (1e6 / min(inb), 1e6 / statistics.median(inb), 1e6 / mean))
    if len(inb) > 1:
        print("stdev       %.1f ns (quantisation floor is %.1f ns)"
              % (statistics.pstdev(inb), ts))
    print()
    print("LIMIT: 400 kHz  =>  period must be >= 2500.0 ns")
    print("       DRV2605L SLOS854D 6.6 f(SCL) max 400 kHz")
    print("       VL53L1X  datasheet Table 7 FI2C max 400 kHz")
    print()
    if mean >= 2500.0:
        print("RESULT: PASS  -- delivered clock is within spec. Close H-D8 with this number.")
    else:
        over = (2500.0 / mean - 1.0) * 100.0
        print("RESULT: FAIL  -- %.1f kHz, %.1f%% over the limit." % (1e6 / mean, over))
        print("        Fix: lower I2C_Charac[I2C_SPEED_FREQ_FAST].freq in i2c_timing.c.")
        print("        Do NOT lower I2C_BUS_HZ -- I2C_GetTiming() discards it and targets")
        print("        the hardcoded .freq. See docs/BUS2_SCL_FREQUENCY_20260830.md section 5.")

if __name__ == "__main__":
    main()
