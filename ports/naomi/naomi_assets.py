#!/usr/bin/env python3
"""The NAOMI's assets: the game's pack (ORAP, runtime/src/assets_pack.c) made smaller for its 32 MB of RAM.

Usage: naomi_assets.py PACK.assets OUT.naomi-assets OUT.pcm

- the tiles' pens two to a byte (128 bytes a tile instead of 256; the low nibble the even pixel), which
  runtime/src/video.c reads with ORUNNERS_PACKED_PENS: 4 MB less;
- the MultiPCM's samples out of the pack, into OUT.pcm: the chip's 4 MB as the sound board sees them (headers,
  then each block at its place, 0xff where there is none), for the AICA's RAM. The pack keeps the headers and
  no blocks.
The magic becomes "ORAN" so that a computer's build never takes it for its own. Like the pack, the output is
made from the player's ROM set on the player's machine and is never shared.
"""
import struct
import sys

TILE_COUNT = 0x8000
PCM_HEADERS_SIZE = 0x200 * 12
PCM_SIZE = 0x400000


class Reader:
    def __init__(self, data):
        self.data, self.at = data, 0

    def u32(self):
        value, = struct.unpack_from("<I", self.data, self.at)
        self.at += 4
        return value

    def raw(self, size):
        out = self.data[self.at:self.at + size]
        if len(out) != size:
            raise ValueError("the pack ends early")
        self.at += size + (-size) % 4
        return out


class Writer:
    def __init__(self):
        self.parts = []

    def u32(self, value):
        self.parts.append(struct.pack("<I", value))

    def raw(self, data):
        self.parts.append(bytes(data) + b"\0" * ((-len(data)) % 4))

    def bytes(self):
        return b"".join(self.parts)


def main():
    pack, out_pack, out_pcm = sys.argv[1:4]
    r = Reader(open(pack, "rb").read())
    if r.raw(4) != b"ORAP":
        sys.exit(f"{pack}: not an assets pack")
    w = Writer()
    w.raw(b"ORAN")
    w.u32(r.u32())    # the pack's version
    w.u32(r.u32())    # the tables' version
    for _region in range(2):    # the V70's pieces, then the sound program's
        count = r.u32()
        w.u32(count)
        for _ in range(count):
            address, size = r.u32(), r.u32()
            w.u32(address)
            w.u32(size)
            w.raw(r.raw(size))
    present = r.raw(TILE_COUNT // 8)
    w.raw(present)
    tiles = 0
    for tile in range(TILE_COUNT):
        if (present[tile // 8] >> (tile % 8)) & 1:
            pens = r.raw(256)
            w.raw(bytes((pens[i] & 15) | (pens[i + 1] & 15) << 4 for i in range(0, 256, 2)))
            tiles += 1
    blocks = r.u32()
    w.u32(blocks)
    sprite_bytes = 0
    for _ in range(blocks):
        first, count = r.u32(), r.u32()
        w.u32(first)
        w.u32(count)
        w.raw(r.raw(count * 4))
        sprite_bytes += count * 4
    headers = r.raw(PCM_HEADERS_SIZE)
    w.raw(headers)
    w.u32(0)    # no sample blocks: they go to the AICA
    pcm = bytearray(b"\xff" * PCM_SIZE)
    pcm[:PCM_HEADERS_SIZE] = headers
    for _ in range(r.u32()):
        offset, length = r.u32(), r.u32()
        pcm[offset:offset + length] = r.raw(length)
    out = w.bytes()
    open(out_pack, "wb").write(out)
    open(out_pcm, "wb").write(pcm)
    print(f"{out_pack}: {len(out) / 2**20:.1f} MB ({tiles} tiles, {sprite_bytes / 2**20:.1f} MB of sprites); "
          f"{out_pcm}: {len(pcm) / 2**20:.0f} MB")


if __name__ == "__main__":
    main()
