#!/usr/bin/env python3
"""Fail if two data/transfer-segment variables share an address (SCC bug guard)."""
import sys, collections
seen = collections.defaultdict(list)
for l in open(sys.argv[1]):
    p = l.split()
    if len(p) == 3 and p[1] in ('D', 'T') and not p[2].startswith('__'):
        seen[int(p[0], 16)].append(p[2])
bad = {a: n for a, n in seen.items() if len(n) > 1}
for a, n in sorted(bad.items()):
    print('OVERLAP %04x: %s' % (a, ' '.join(n)))
sys.exit(1 if bad else 0)
