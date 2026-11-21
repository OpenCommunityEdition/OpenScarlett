#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare two captured frames (32-bit BMP): fraction of differing pixels and their size.
usage: bmpdiff.py a.bmp b.bmp"""
import struct, sys
def load(p):
    d = open(p, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    return d[off:], w, abs(h)
try:
    a, w, h = load(sys.argv[1]); b, w2, h2 = load(sys.argv[2])
except (OSError, struct.error) as e:
    print("no-frame (%s)" % e.__class__.__name__); sys.exit(1)
if (w, h) != (w2, h2):
    print("size-mismatch %dx%d vs %dx%d" % (w, h, w2, h2)); sys.exit(1)
n = 0; big = 0; mx = 0
for i in range(0, min(len(a), len(b)), 4):
    if a[i:i + 3] != b[i:i + 3]:
        n += 1
        d = max(abs(a[i + k] - b[i + k]) for k in range(3))
        mx = max(mx, d)
        big += d > 8
print("differing=%.4f%% (>8: %.4f%%) max=%d" % (100.0 * n / (w * h), 100.0 * big / (w * h), mx))
