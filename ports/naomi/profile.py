#!/usr/bin/env python3
"""or_profile.py LIVE_ADDR [DRAW_EVERY]: (set draw_every), wait for a full ring, print its pcs as hex, one a line."""
import sys, time
sys.path.append('/sbin/piforce/dm_netboot')
from netdimm import NetDimm, PeekPokeTypeEnum as T
d = NetDimm('10.0.0.4')
off = lambda a: a & 0x01ffffff
peek = lambda a: d.peek(off(a), T.TYPE_LONG)
base = int(sys.argv[1], 0)
if peek(base) != 0x4e55524f:
    sys.exit('no live block')
if len(sys.argv) > 2:
    d.poke(off(base + 0x40), T.TYPE_LONG, int(sys.argv[2]))
pcs, size = peek(base + 0x48), peek(base + 0x50)
start = peek(base + 0x4c)
while peek(base + 0x4c) - start < size:
    time.sleep(1)
print('# samples', peek(base + 0x4c), file=sys.stderr)
for i in range(size):
    print('%08x' % peek(pcs + 4 * i))
