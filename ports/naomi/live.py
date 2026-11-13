#!/usr/bin/env python3
"""live.py NETDIMM_IP ADDR [fps [SECONDS] | log | draw N]: OutRunners' live figures on the NAOMI through the NetDIMM.

Runs ON the netbooter Raspberry (WiPi), with DragonMinded's netdimm.py in /sbin/piforce/dm_netboot, while the game
runs (ports/naomi/naomi_dimmcomms.c answers the peeks). ADDR is the address of `orunners_naomi_live`: the console's
line "NAOMI: NetDIMM peek/poke on; live figures at 0x8c......", or what build_naomi.sh prints.
After live.py of Daytona USA's native port on the NAOMI (0skarzer0/daytona-arcade-recomp, branch naomi-native).

  fps [N]   one line a second for N seconds (default 30): the game's frames, the frames drawn, and the time of
            the game's C, of the sound board and of video.c (microseconds)
  log       the console's ring
  draw N    draw a frame every N (1-600): video.c's cost against the game's speed

Example: python3 live.py 10.0.0.4 0x8c2a1b40 fps 60
"""
import struct
import sys
import time
sys.path.append('/sbin/piforce/dm_netboot')
from netdimm import NetDimm, PeekPokeTypeEnum as T

FIELDS = ('magic version seconds game_frames drawn_frames game_us sound_us video_us worst_frame_us frames_total '
          'dimm_requests log_lines log_count log_width log_next heap_left draw_every inputs profile_pcs profile_count '
          'profile_size quads texels_drawn missing video_memory_free textures build_us wait_us submit_us test_inputs '
          'misaligned sound_writes').split()


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    dimm, base = NetDimm(sys.argv[1]), int(sys.argv[2], 0)
    what = sys.argv[3] if len(sys.argv) > 3 else 'fps'
    off = lambda a: a & 0x01ffffff  # firmware 3.17: an offset into the 32 MB of RAM
    peek = lambda a: dimm.peek(off(a), T.TYPE_LONG)
    read = lambda: dict(zip(FIELDS, (peek(base + 4 * i) for i in range(len(FIELDS)))))
    live = read()
    if live['magic'] != 0x4e55524f:
        sys.exit('no live block at 0x%08x (magic %08x): wrong address, or the game is not running' % (base, live['magic']))
    if what == 'draw':
        dimm.poke(off(base + 0x40), T.TYPE_LONG, int(sys.argv[4]))
        print('a frame drawn every', int(sys.argv[4]))
    elif what == 'log':
        n, width, written = live['log_count'], live['log_width'], peek(live['log_next'])
        for i in range(max(0, written - n), written + 1):
            at = live['log_lines'] + (i % n) * width
            raw = b''.join(struct.pack('<I', peek(at + 4 * k)) for k in range(width // 4))
            line = raw.split(b'\0', 1)[0].decode('ascii', 'replace')
            if line:
                print(line)
    else:
        seconds = int(sys.argv[4]) if len(sys.argv) > 4 else 30
        last = None
        for _ in range(seconds * 4):
            live = read()
            if live['seconds'] != last:
                last = live['seconds']
                i = live['inputs']
                print('%5ds  game %2d fps  drawn %2d  game %5d us  sound %5d us  video %5d us (build %d wait %d submit %d)  worst %6d us  '
                      'quads %3d  texels %6d/s  missing %04x  textures %d (%d failed)  vram %d KB  '
                      'wheel %02x gas %02x brake %02x E %02x  misaligned %d  aica writes %s' % (
                          live['seconds'], live['game_frames'], live['drawn_frames'], live['game_us'],
                          live['sound_us'], live['video_us'], live['build_us'], live['wait_us'], live['submit_us'],
                          live['worst_frame_us'], live['quads'],
                          live['texels_drawn'], live['missing'], live['textures'] & 0xffff, live['textures'] >> 16,
                          live['video_memory_free'] // 1024, i & 255, i >> 8 & 255, i >> 16 & 255, i >> 24, live['misaligned'],
                          'NO SAMPLES' if live['sound_writes'] == 0xffffffff else live['sound_writes']), flush=True)
            time.sleep(0.25)


if __name__ == '__main__':
    main()
