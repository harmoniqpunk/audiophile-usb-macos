#!/usr/bin/env python3
"""Analyse an `apusb-diag hal-test --out FILE` dump (input+output per IO cycle).

Reconstructs the sent and received signals on the HAL sample timeline and
reports: per-channel level, cross-correlation delay (round trip) and gain,
residual after delay/gain alignment (SNR), and discontinuities.
SPDX-License-Identifier: MIT
"""
import struct, sys
import numpy as np

def load(path):
    b = open(path, 'rb').read(); off = 0
    ins, outs = {}, {}
    while off + 24 <= len(b):
        ti, to, nbytes, ch = struct.unpack_from('<ddII', b, off); off += 24
        x = np.frombuffer(b, dtype='<f4', count=nbytes // 4, offset=off); off += nbytes
        y = np.frombuffer(b, dtype='<f4', count=nbytes // 4, offset=off); off += nbytes
        ins[int(ti)] = x.reshape(-1, ch); outs[int(to)] = y.reshape(-1, ch)
    def assemble(d):
        t0 = min(d); t1 = max(k + len(v) for k, v in d.items())
        a = np.zeros((t1 - t0, 2), np.float64)
        for k, v in d.items(): a[k - t0:k - t0 + len(v), :v.shape[1]] = v[:, :2]
        return t0, a
    return assemble(ins), assemble(outs)

def db(x): return 20 * np.log10(max(x, 1e-12))

(ti0, xin), (to0, xout) = load(sys.argv[1])
rate = float(sys.argv[2]) if len(sys.argv) > 2 else 48000.0
print(f"input  frames {len(xin)} from t={ti0}; output frames {len(xout)} from t={to0}")
for c in range(2):
    print(f"ch{c}: out RMS {db(np.sqrt(np.mean(xout[:,c]**2))):.1f} dBFS, in RMS {db(np.sqrt(np.mean(xin[:,c]**2))):.1f} dBFS")
# align on the HAL timeline: in[t] vs out[t - d]
ref = 0 if np.sum(xout[:,0]**2) >= np.sum(xout[:,1]**2) else 1
lo, hi = max(ti0, to0), min(ti0 + len(xin), to0 + len(xout))
o = xout[:, ref]; maxd = int(0.05 * rate)
seg_in = xin[lo - ti0 + maxd: hi - ti0, :]
best = None
for c in range(2):
    i = seg_in[:, c]
    n = len(i)
    for d in range(0, maxd):
        s = o[lo - to0 + maxd - d: lo - to0 + maxd - d + n]
        if len(s) < n: break
        num = float(np.dot(i, s)); den = float(np.dot(s, s)) + 1e-20
        g = num / den; r = i - g * s
        snr = 10 * np.log10((g * g * den) / (float(np.dot(r, r)) + 1e-20))
        if best is None or snr > best[0]: best = (snr, d, g, c)
snr, d, g, c = best
print(f"best match: output ch{ref} -> input ch{c}, round trip {d} frames = {1000*d/rate:.2f} ms, gain {db(abs(g)):.1f} dB "
      f"(polarity {'inverted' if g < 0 else 'normal'}), aligned SNR {snr:.1f} dB")
# other channel leakage (interleave check)
oc = 1 - c
lk = db(np.sqrt(np.mean(seg_in[:, oc]**2))) - db(np.sqrt(np.mean(seg_in[:, c]**2)))
print(f"input ch{oc} level relative to ch{c}: {lk:.1f} dB (should be very low if only ch{ref} was driven)")
# local SNR in 10 ms blocks to find glitches
s = o[lo - to0 + maxd - d: lo - to0 + maxd - d + len(seg_in)]
r = seg_in[:, c] - g * s; blk = int(0.01 * rate); bad = 0
for k in range(0, len(r) - blk, blk):
    e = np.sqrt(np.mean(r[k:k+blk]**2)); sg = np.sqrt(np.mean((g*s[k:k+blk])**2)) + 1e-12
    if e > 0.1 * sg and sg > 1e-5: bad += 1
print(f"10-ms blocks with residual > -20 dB of signal: {bad} of {len(r)//blk}")
