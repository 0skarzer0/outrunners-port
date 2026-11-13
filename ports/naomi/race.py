#!/usr/bin/env python3
"""race.py LIVE_ADDR [SECONDS]: a race with no one at the cabinet (the live block's test_inputs): a coin, START
pressed until the race starts, then full gas, the wheel swinging a little; the live figures once a second.
At the end the cabinet's own controls again (test_inputs 0). Runs on the netbooter's Raspberry, as live.py."""
import sys, time
sys.path.append('/sbin/piforce/dm_netboot')
from netdimm import NetDimm, PeekPokeTypeEnum as T

d = NetDimm('10.0.0.4')
off = lambda a: a & 0x01ffffff
base = int(sys.argv[1], 0)
seconds = int(sys.argv[2]) if len(sys.argv) > 2 else 60
peek = lambda o: d.peek(off(base + o), T.TYPE_LONG)
poke = lambda o, v: d.poke(off(base + o), T.TYPE_LONG, v)
if peek(0) != 0x4e55524f:
    sys.exit('no live block')
ON = 0x80000000
poke(0x74, ON | 1)                      # a coin
time.sleep(1.5)
for _ in range(12):                     # START through the screens before the race
    poke(0x74, ON | 2); time.sleep(0.3)
    poke(0x74, ON); time.sleep(0.7)
start, last = time.time(), None
while time.time() - start < seconds:
    t = time.time() - start
    wheel = 0x80 + int(40 * ((t // 3) % 3 - 1))
    poke(0x74, ON | 0xff << 8 | wheel << 16)
    s = peek(0x08)
    if s != last:
        last = s
        f = [peek(o) for o in (0x0c, 0x14, 0x18, 0x1c, 0x68, 0x6c, 0x70, 0x20, 0x54, 0x58, 0x5c)]
        print('%4ds  game %2d fps  game %5d  sound %5d  video %5d (build %5d wait %5d submit %4d)  worst %6d  '
              'quads %3d  texels %6d/s  missing %04x' % ((s,) + tuple(f)), flush=True)
    time.sleep(0.25)
poke(0x74, 0)
