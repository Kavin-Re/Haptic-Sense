#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Turn a Haptic-Sense serial soak log into a verdict.

    python3 check_soak.py soak_10min_20260830.log

Every check below is a thing that has actually gone wrong on this project at least once,
or is a canary that exists to catch a class that has. A soak is only evidence if somebody
reads every line of it -- so this reads them.
"""
import re, sys, statistics

PAT = {
 "HB":  re.compile(r"\[HB\] up_ms=(\d+) frames=(\d+) inf=(\d+) hazard=(\d+) canary_err=(\d+) q=(-?\d+) qovr=(\d+)"),
 "I2C": re.compile(r"\[I2C\] init=(-?\d+) .*? ok=(\d+) err=(\d+) tmo=(\d+) recov=(\d+)"),
 "DRV": re.compile(r"\[DRV\] init=(-?\d+) id=(\d+) mode=0x(\w+) lib=0x(\w+) seq=0x(\w+) odc=0x(\w+) sts=0x(\w+) armed=(\d+)"),
 "TRG": re.compile(r"\[TRG\] pulses=(\d+) suppressed=(\d+) cyc=(\d+) rst=(\d+) rstmode=0x(\w+)"),
 "HLT": re.compile(r"\[HLT\] polls=(\d+) faults=0x(\w+) cfglost=(\d+)"),
 "EFF": re.compile(r"\[EFF\] n=(\d+) last=(\d+) min=(\d+) max=(\d+) late=(\d+) stuck=(\d+)"),
 "DWT": re.compile(r"\[DWT\] ok=(\d+) cyc=(\d+) cyc_per_ms=(\d+)"),
 "NAK": re.compile(r"\[NAK\] nacks=(\d+)"),
 "TOF": re.compile(r"\[TOF\] res=(-?\d+) step=(\d+) id=0x(\w+) blk=0x(\w+) ctl=0x(\w+)"),
}

def main():
    if len(sys.argv) < 2: sys.exit("usage: check_soak.py <logfile>")
    rows = {k: [] for k in PAT}
    for line in open(sys.argv[1], errors="replace"):
        for k, p in PAT.items():
            m = p.search(line)
            if m: rows[k].append(m.groups())

    hb = rows["HB"]
    if not hb: sys.exit("no [HB] lines found -- wrong file, or the board was not running")

    t0, t1 = int(hb[0][0]), int(hb[-1][0])
    dur = (t1 - t0) / 1000.0
    fr  = (int(hb[-1][1]) - int(hb[0][1])) / dur if dur else 0

    print("=" * 68)
    print("  %d heartbeats over %.1f s (%.1f min), up_ms %d -> %d"
          % (len(hb), dur, dur / 60.0, t0, t1))
    print("=" * 68)

    fails, notes = [], []
    def check(ok, label, detail=""):
        (notes if ok else fails).append((label, detail))

    # --- the sticky fault latch: the whole reason drv2605l_poll() exists ---
    faults = {g[1] for g in rows["HLT"]}
    check(faults <= {"0"}, "faults (OC_DETECT | OVER_TEMP), sticky",
          "values seen: " + ", ".join("0x" + f for f in sorted(faults)))
    cfg = {int(g[2]) for g in rows["HLT"]}
    check(cfg <= {0}, "cfglost -- MODE still 0x01", "max %d" % max(cfg or {0}))

    # --- bus health ---
    errs = [int(g[2]) for g in rows["I2C"]]
    tmo = {int(g[3]) for g in rows["I2C"]}
    rec = {int(g[4]) for g in rows["I2C"]}
    # nacks is a SUBSET of err (app_i2c.c HAL_I2C_ErrorCallback, 2026-08-30):
    # transfers that failed because nothing ACKed, as opposed to a bus fault.
    # Older logs have no [NAK] line; treat those as nacks=0, which reproduces
    # the original single "I2C err" check exactly.
    naks = [int(g[0]) for g in rows["NAK"]]
    if len(naks) != len(errs):
        naks = [0] * len(errs)
    busfault = {e - n for e, n in zip(errs, naks)}
    check(busfault <= {0}, "I2C bus faults (err - nacks)",
          "max %d" % max(busfault or {0}))
    check(tmo <= {0}, "I2C tmo", "max %d" % max(tmo or {0}))
    check(rec <= {0}, "I2C recov", "max %d" % max(rec or {0}))
    # A NACK is not a bus fault, but it is not nothing either: it means a
    # device this firmware probes did not answer. Expected to be exactly 1
    # while the VL53L1X is unsoldered (the Block 2 L1 probe), and 0 after.
    # --- Block 2 L1 probe: VL53L1X reference registers, DS12385 Table 8 ---
    # Parsed BEFORE the nack check, because it is what makes the expected
    # nack count derivable rather than a magic number.
    tof_ok = None
    if rows["TOF"]:
        tof = rows["TOF"][-1]
        res, step, tid, blk, ctl = int(tof[0]), int(tof[1]), tof[2], tof[3], tof[4]
        tof_ok = (res == 0 and step == 0 and tid.lower() == "eacc"
                  and blk.lower() == "eacc10" and ctl.lower() != "ea")
        check(tof_ok,
              "VL53L1X L1 probe (0x010F/0x0110 -> EA CC 10, REG8 control)",
              "res=%d step=%d id=0x%s blk=0x%s ctl=0x%s "
              "(res=-42 step=1 = no ACK: not soldered, XSHUT, or joints; "
              "id/blk 0xa5.. = DMA never wrote; 0xff.. = nothing driving; "
              "res=-57 = ACKed then failed, a different problem)"
              % (res, step, tid, blk, ctl))
    else:
        # Never skip a check silently -- say so on the report.
        check(True, "VL53L1X L1 probe",
              "no [TOF] line in this log: pre-Block-2 firmware, check N/A")

    # A NACK is not a bus fault, but it is not nothing either: it means a
    # device this firmware probes did not answer. The expected count is
    # DERIVED, not a magic number: the L1 probe stops at its first failing
    # step, so a VL53L1X that is not on the bus yet accounts for exactly one
    # -- and one only. Anything above that is an unexplained NACK.
    exp = 0 if (tof_ok is None or tof_ok) else 1
    nakmax = max(naks or [0])
    check(nakmax == exp,
          "I2C nacks -- every probed device answered (expected %d)" % exp,
          "max %d; expected %d %s" % (nakmax, exp,
              "(VL53L1X not yet on the bus -- see the L1 probe above)"
              if exp else "(all probed devices should answer)"))

    # --- RZ4 canary and the lockstep it guards ---
    can = {int(g[4]) for g in hb}
    check(can <= {0}, "canary_err (RZ4 torn read)", "max %d" % max(can or {0}))
    lock = [g for g in hb if g[1] != g[2]]
    check(not lock, "frames == inf lockstep", "%d lines diverged" % len(lock))
    qo = {int(g[6]) for g in hb}
    check(qo <= {0}, "qovr -- inference never fell behind", "max %d" % max(qo or {0}))

    # --- driver config integrity ---
    if rows["DRV"]:
        bad = [g for g in rows["DRV"] if (g[0], g[1], g[2], g[3], g[5], g[7]) !=
               ("0", "7", "1", "2", "8b", "1")]
        check(not bad, "DRV init=0 id=7 mode=0x1 lib=0x2 odc=0x8b armed=1",
              "%d lines off-nominal" % len(bad))
    if rows["TRG"]:
        rm = {g[4] for g in rows["TRG"]}
        check(rm <= {"40"}, "rstmode -- DEV_RESET restored defaults",
              "values: " + ", ".join(sorted(rm)))

    # --- HAP-T9 discarded-sample counters: the OC fingerprint ---
    if rows["EFF"]:
        late = max(int(g[4]) for g in rows["EFF"]); stk = max(int(g[5]) for g in rows["EFF"])
        check(late == 0 and stk == 0, "EFF late/stuck (playback aborted)",
              "late=%d stuck=%d" % (late, stk))
        mn = min(int(g[2]) for g in rows["EFF"]); mx = max(int(g[3]) for g in rows["EFF"])
        floor_ratio = 75000.0 / mx if mx else 0
        print("  effect duration   %d - %d us   R-3 floor 75 ms = %.3fx the max %s"
              % (mn, mx, floor_ratio, "(rule needs >= 1.2)" if floor_ratio >= 1.2 else "*** BELOW 1.2 ***"))

    # --- clock ---
    if rows["DWT"]:
        dok = {int(g[0]) for g in rows["DWT"]}
        check(dok <= {1}, "DWT ok -- CYCCNT counting", "saw %s" % sorted(dok))
        cps = [int(g[2]) for g in rows["DWT"] if int(g[2]) > 0]
        if cps:
            md = statistics.median(cps)
            ppm = (max(cps) - min(cps)) / md * 1e6
            check(abs(md - 800000) < 1000, "CPUCLK 800 MHz",
                  "median %d cyc/ms, spread %.0f ppm" % (md, ppm))
            print("  cyc_per_ms        %d - %d  (median %d)" % (min(cps), max(cps), md))

    # --- trigger accounting: calls should equal hazard, modulo print skew ---
    if rows["TRG"] and hb:
        n = min(len(rows["TRG"]), len(hb))
        skew = [abs((int(rows["TRG"][i][0]) + int(rows["TRG"][i][1])) - int(hb[i][3]))
                for i in range(n)]
        check(max(skew) <= 4, "pulses + suppressed == hazard",
              "worst skew %d (a few frames of print-ordering skew is normal)" % max(skew))
        print("  pulses            %s -> %s     frame rate %.1f Hz"
              % (rows["TRG"][0][0], rows["TRG"][-1][0], fr))

    print()
    for label, detail in notes:
        print("  PASS  %-46s %s" % (label, detail))
    for label, detail in fails:
        print("  FAIL  %-46s %s" % (label, detail))
    print()
    if fails:
        # ONE special case, and it exists so this tool never cries wolf.
        # Before the VL53L1X is soldered the L1 probe MUST fail -- that is the
        # point of running the soak first. If it is the ONLY failure and it
        # failed by not being answered at all (E_NOEXS, step 1), say so
        # plainly instead of telling the operator not to solder, which is
        # exactly what they are about to and should do.
        only_absent = (len(fails) == 1
                       and fails[0][0].startswith("VL53L1X L1 probe")
                       and "res=-42 step=1" in fails[0][1])
        if only_absent:
            print("RESULT: 1 CHECK FAILED, and it is the EXPECTED one -- the "
                  "VL53L1X is not on the bus yet")
            print("        (res=-42 step=1 = no ACK). Everything else over "
                  "%.1f min is clean, so the" % (dur / 60.0))
            print("        Block 1 chain is intact and the bench is ready for "
                  "the 7SEMI to go on.")
            if dur < 540:
                print("        NOTE: only %.1f min of data -- the re-soak "
                      "target is 10 min." % (dur / 60.0))
            sys.exit(3)
        print("RESULT: %d CHECK(S) FAILED. Do not solder anything onto this bench." % len(fails))
        sys.exit(1)
    if dur < 540:
        print("RESULT: all checks pass, but only %.1f min of data -- the target is 10 min." % (dur/60))
        sys.exit(2)
    print("RESULT: PASS over %.1f min. Bench is clean." % (dur / 60.0))

if __name__ == "__main__":
    main()
