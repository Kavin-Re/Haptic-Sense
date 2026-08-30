#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
BUS-2 — measure the delivered I2C1 SCL period from a sigrok capture.

Usage:
    sigrok-cli --driver fx2lafw --config samplerate=8m --time 5s -o bus2_scl.sr
    sigrok-cli -i bus2_scl.sr -O csv > bus2_scl.csv
    python3 analyze_scl.py bus2_scl.csv --rate 8e6

Channel naming: these 24 MHz 8-channel clones silkscreen their pins CH1..CH8, while the
fx2lafw driver calls the same lines D0..D7 -- CH1 = D0, so CH3 = D2. The script does not
rely on that: it prints edge counts for every channel and picks SCL as the busiest, because
the clock toggles on every bit while SDA only toggles when the data changes.

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
    ap.add_argument("--channel", default="auto",
                    help="SCL channel name in the CSV header, or 'auto' to detect it (default: auto)")
    a = ap.parse_args()

    ts = 1e9 / a.rate                       # ns per sample

    # ---- read every channel column; we may not know which one SCL is on ----
    names, cols = None, []
    with open(a.csv) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith(";"):
                continue
            parts = [p.strip() for p in line.split(",")]
            if names is None and not parts[0].lstrip("-").isdigit():
                names = parts
                cols = [[] for _ in names]
                continue
            if names is None:                      # headerless capture
                names = ["D%d" % i for i in range(len(parts))]
                cols = [[] for _ in names]
            for i, v in enumerate(parts[:len(cols)]):
                try:
                    cols[i].append(int(v))
                except ValueError:
                    pass

    if not cols or not cols[0]:
        sys.exit("no samples parsed -- is this a sigrok CSV export?")

    def rising(seq):
        return [i for i in range(1, len(seq)) if seq[i - 1] == 0 and seq[i] == 1]

    counts = [(names[i], len(rising(c)), i) for i, c in enumerate(cols) if c]

    # ---- channel selection ----
    # THE PROBE SILKSCREEN AND THE DRIVER DISAGREE. These 24 MHz 8-channel clones
    # label their pins CH1..CH8; the fx2lafw driver names the same lines D0..D7.
    # CH1 = D0, so CH3 = D2 and CH4 = D3. Rather than trust that mapping, detect:
    # SCL toggles on EVERY bit, SDA only when the data changes, so on an I2C pair
    # the clock is reliably the channel with the most rising edges.
    print("channel activity (rising edges):")
    for n, c, _ in counts:
        print("    %-4s %8d %s" % (n, c, "<-- busiest" if c == max(x[1] for x in counts) and c > 0 else ""))
    print()

    if a.channel == "auto":
        name, cnt, idx = max(counts, key=lambda x: x[1])
        if cnt < 20:
            sys.exit("no channel has more than 20 edges -- check the probe, GND, and that "
                     "the board is powered and running")
        print("auto-selected %s as SCL (most rising edges).\n"
              "If that is wrong, re-run with --channel <name>.\n" % name)
    else:
        match = [c for c in counts if c[0] == a.channel]
        if not match:
            sys.exit("channel %s not present. Available: %s"
                     % (a.channel, ", ".join(n for n, _, _ in counts)))
        name, cnt, idx = match[0]

    rows = cols[idx]
    edges = rising(rows)
    if len(edges) < 20:
        sys.exit("only %d rising edges on %s -- wrong channel, or capture more"
                 % (len(edges), name))

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
