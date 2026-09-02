#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Decode I2C out of a sigrok .sr capture and print a census, without PulseView.

    python3 decode_sr_i2c.py sensorinit_fail_20260901.sr [SCL_BIT] [SDA_BIT]

Defaults SCL=bit2 (probe3/D2), SDA=bit3 (probe4/D3) -- the mapping this
project uses (the clone silkscreens CH1-CH8; the driver names them D0-D7,
so CH3 = D2). Samplerate is read from the capture metadata.

Prints: transaction census by address, address-NACK vs data-NACK, the
register indices written and which are MISSING, NACK bursts with their
spans, real STOP->START gaps, and SCL low-pulse widths (clock stretching).
Needs numpy.
"""
import zipfile, sys, re
import numpy as np

def load(path, scl_bit=2, sda_bit=3):
    z = zipfile.ZipFile(path)
    meta = z.read('metadata').decode()
    m = re.search(r'samplerate=(\d+)\s*([kM]?)Hz', meta)
    rate = int(m.group(1)) * {'':1, 'k':1000, 'M':1000000}[m.group(2)]
    names = sorted((n for n in z.namelist() if n.startswith('logic-1-')),
                   key=lambda n: int(n.rsplit('-', 1)[1]))
    raw = np.frombuffer(b"".join(z.read(n) for n in names), dtype=np.uint8)
    return raw, rate, meta

def frames_of(raw, rate, scl_bit, sda_bit):
    scl = ((raw >> scl_bit) & 1).astype(np.uint8)
    sda = ((raw >> sda_bit) & 1).astype(np.uint8)
    comb = (scl << 1) | sda
    idx = np.concatenate(([0], np.flatnonzero(np.diff(comb)) + 1))
    s_scl, s_sda = scl[idx], sda[idx]
    ev = [(int(idx[i]), 'S' if s_sda[i] == 0 else 'P', None)
          for i in range(1, len(idx))
          if s_scl[i] and s_scl[i-1] and s_sda[i] != s_sda[i-1]]
    rise = idx[np.flatnonzero((s_scl[1:] == 1) & (s_scl[:-1] == 0)) + 1]
    ev += [(int(r), 'b', int(b)) for r, b in zip(rise, sda[rise])]
    ev.sort()
    out, cur, acc = [], None, []
    for pos, kind, val in ev:
        if kind == 'S':
            if cur: cur['end'] = pos; cur['term'] = 'RESTART'; out.append(cur)
            cur = {'start': pos, 'bytes': [], 'acks': [], 'end': None, 'term': '?'}; acc = []
        elif kind == 'P':
            if cur: cur['end'] = pos; cur['term'] = 'STOP'; out.append(cur); cur = None
            acc = []
        elif cur is not None:
            acc.append(val)
            if len(acc) == 9:
                cur['bytes'].append(int(np.packbits(np.array(acc[:8], dtype=np.uint8))[0]))
                cur['acks'].append(acc[8]); acc = []
    if cur: cur['end'] = len(raw); cur['term'] = 'EOF'; out.append(cur)
    return [f for f in out if f['bytes']], scl, sda, idx

def main():
    path = sys.argv[1]
    scl_bit = int(sys.argv[2]) if len(sys.argv) > 2 else 2
    sda_bit = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    raw, rate, meta = load(path, scl_bit, sda_bit)
    print("%s\n  %d samples = %.3f s at %d Hz" % (path, len(raw), len(raw)/rate, rate))
    fr, scl, sda, idx = frames_of(raw, rate, scl_bit, sda_bit)
    T = lambda f: f['start'] / rate
    print("  frames (START..STOP/RESTART): %d" % len(fr))

    from collections import Counter
    tot, ack, nak = Counter(), Counter(), Counter()
    for f in fr:
        a = f['bytes'][0]
        k = "0x%02X%s" % (a >> 1, 'R' if a & 1 else 'W')
        tot[k] += 1
        (nak if f['acks'][0] else ack)[k] += 1
    print("\n=== by address ===")
    for k in sorted(tot):
        print("  %-8s total %6d   addr-ACK %6d   addr-NACK %6d"
              % (k, tot[k], ack.get(k, 0), nak.get(k, 0)))

    # register indices successfully written (2-byte index + data, write frames)
    wr_ok = {}
    for f in fr:
        if f['bytes'][0] & 1 or f['acks'][0] or f['term'] != 'STOP':
            continue
        if len(f['bytes']) >= 4 and not any(f['acks'][1:]):
            wr_ok.setdefault(f['bytes'][0] >> 1, set()).add((f['bytes'][1] << 8) | f['bytes'][2])
    for a7, s in sorted(wr_ok.items()):
        lo, hi = min(s), max(s)
        miss = [v for v in range(lo, hi + 1) if v not in s]
        print("\n=== 0x%02X: %d distinct 16-bit indices written and ACKed, 0x%04X..0x%04X"
              % (a7, len(s), lo, hi))
        print("    gaps in that span: %d %s" % (len(miss),
              ("(" + ", ".join("0x%04X" % v for v in miss[:12])
               + (", ..." if len(miss) > 12 else "") + ")") if miss else ""))

    naks = [f for f in fr if f['acks'][0]]
    if naks:
        ts = [T(f) for f in naks]
        bursts, cur_b = [], [ts[0]]
        for a, b in zip(ts, ts[1:]):
            if b - a > 0.05: bursts.append(cur_b); cur_b = [b]
            else: cur_b.append(b)
        bursts.append(cur_b)
        print("\n=== address-NACK bursts ===")
        for b in bursts:
            print("   t=%.4f -> %.4f   %d NACKs   span %.2f ms"
                  % (b[0], b[-1], len(b), (b[-1] - b[0]) * 1000))

    gaps = np.array([(g['start'] - f['end']) / rate * 1e6
                     for f, g in zip(fr, fr[1:]) if f['term'] == 'STOP'])
    if len(gaps):
        print("\n=== real STOP -> next START gaps, us (n=%d) ===" % len(gaps))
        print("   min=%.2f median=%.2f p99=%.1f max=%.1f   below 1.3us: %d"
              % (gaps.min(), np.median(gaps), np.percentile(gaps, 99),
                 gaps.max(), int((gaps < 1.3).sum())))

    s_scl = scl[idx]
    lo = idx[np.flatnonzero((s_scl[1:] == 0) & (s_scl[:-1] == 1)) + 1]
    hi = idx[np.flatnonzero((s_scl[1:] == 1) & (s_scl[:-1] == 0)) + 1]
    if len(hi) and len(lo):
        if hi[0] < lo[0]: hi = hi[1:]
        n = min(len(lo), len(hi))
        d = (hi[:n] - lo[:n]) / rate * 1e6
        d = d[(d > 0) & (d < 10000)]
        print("\n=== SCL LOW pulse width, us (stretching = outliers) ===")
        print("   n=%d median=%.3f p99=%.3f max=%.1f   >10us: %d  >100us: %d"
              % (len(d), np.median(d), np.percentile(d, 99), d.max(),
                 int((d > 10).sum()), int((d > 100).sum())))

if __name__ == '__main__':
    main()
