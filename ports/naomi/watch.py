#!/usr/bin/env python3
"""or_watch.py LIVE_ADDR [POKE_ADDR VALUE]: (optionally poke a word), then print the console ring's new lines as
they come, until the peeks stop answering (the line before is the last thing the program said)."""
import struct, sys, time
sys.path.append('/sbin/piforce/dm_netboot')
from netdimm import NetDimm, PeekPokeTypeEnum as T
d = NetDimm('10.0.0.4')
off = lambda a: a & 0x01ffffff
peek = lambda a: d.peek(off(a), T.TYPE_LONG)
base = int(sys.argv[1], 0)
live = [peek(base + 4 * i) for i in range(18)]
if live[0] != 0x4e55524f:
    sys.exit('no live block (%08x)' % live[0])
lines_at, n, w, next_at = live[11], live[12], live[13], live[14]
seen = peek(next_at) - 1
def show():
    global seen
    written = peek(next_at)
    for i in range(max(seen + 1, written - n + 1), written + 1):
        raw = b''.join(struct.pack('<I', peek(lines_at + (i % n) * w + 4 * k)) for k in range(w // 4))
        line = raw.split(b'\0', 1)[0].decode('ascii', 'replace')
        if line and i < written:
            print('%6.1f | %s' % (time.time() - t0, line), flush=True)
            seen = i
t0 = time.time()
show()
if len(sys.argv) > 3:
    d.poke(off(int(sys.argv[2], 0)), T.TYPE_LONG, int(sys.argv[3], 0))
    print('poked', flush=True)
while time.time() - t0 < 60:
    show()
    time.sleep(0.2)
