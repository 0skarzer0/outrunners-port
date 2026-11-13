# OutRunners on a SEGA NAOMI / NAOMI 2 (NetDIMM netboot)

Work in progress for a Crazy Taxi upright with a NAOMI 2 and a 512 MB NetDIMM loaded by a Raspberry Pi
(WiPi Netbooter). The study behind it (what each part costs, what the game uses of the System 32 video chip):
[docs/outrunners.md of 0skarzer0/naomi-wheel-games](https://github.com/0skarzer0/naomi-wheel-games/blob/main/docs/outrunners.md).

## Stages

1. **Boot and measure** (this directory today): the game's C, the board and the sound program on the SH-4; the
   cabinet's controls through the JVS; `runtime/src/video.c` drawn every few frames into a framebuffer, only to
   see something; the time each part takes, read live from the Raspberry (`live.py`). Not yet run on a board.
2. **PowerVR2 renderer** in place of `video.c`: tilemap pages as textures, NBG0 as a scrolled quad, NBG2 a strip a
   line (row scroll and row select), sprites as quads from a texture cache, priorities as depth, fades.
3. **Sound on the AICA:** the MultiPCM's 4 MB of samples (`orunners.pcm`) in the AICA's RAM, its 28 voices on AICA
   channels. The YM3438 only gives the sound program its timers: OutRunners plays no FM note.
4. **A converter for Windows**: drop `orunners.zip`, get the `.bin.gz`.

## Build

```sh
git submodule update --init --depth 1
ports/naomi/build_naomi.sh /path/to/orunners.zip build/naomi
```

Needs the sh-elf toolchain and KallistiOS built for `KOS_SUBARCH=naomi` (`KOS_NAOMI`, default
`/opt/toolchains/dc/kos-naomi`; recipe: `outrunners/build_toolchain.sh` of naomi-wheel-games), CMake, a C compiler
and Python 3. Out: `build/naomi/orunners_naomi.bin.gz` for the netbooter. It is made from the player's ROM set:
never share it.

What it does: the computers' build takes the assets out of the ROM set and writes them as one pack;
`naomi_assets.py` makes the NAOMI's pack from it (tiles two pens to a byte, `ORUNNERS_PACKED_PENS`, 21.6 MB, read
into main RAM at boot; the samples apart); `make` builds the ELF; `naomibintool` and `naomi_pack.py` make the
netboot image with the pack in the NetDIMM after the program (`/dimm`).

Checked on a computer (2026-11-13): a build with `ORUNNERS_PACKED_PENS` on the NAOMI's pack draws the same frames as
the normal build on the normal pack (video hashes of 3,501 frames of attract and race, identical). Every C file
compiles with sh-elf-gcc 15.2.0 (`-m4-single -ml -O2`): 490 KB of code, 5.8 MB of static data.

## On the board

The controls (values of the Crazy Taxi upright, widening by themselves): wheel analog 0, gas 1, brake 2; the
two-position shifter forward = SHIFT UP (HIGH), back = SHIFT DOWN (LOW); START; coins by the meters' difference;
SERVICE and TEST as the board's.

Live figures, on the Raspberry of the cabinet (DragonMinded's netdimm.py), with the address `build_naomi.sh`
prints:

```sh
python3 live.py 10.0.0.4 0x8c...... fps 60     # game fps, frames drawn, us of the game, sound and video
python3 live.py 10.0.0.4 0x8c...... draw 8     # a frame drawn every 8
python3 live.py 10.0.0.4 0x8c...... log        # the console
```

A failure, or no game frame for 10 s, puts the console's last lines on the screen (with the main thread's pc and
pr for `sh-elf-addr2line` on `orunners_naomi.elf`).

## Credits

The NetDIMM files, the peeks, the console ring and `naomi_pack.py` come from the NAOMI build of Daytona USA's
native port ([0skarzer0/daytona-arcade-recomp](https://github.com/0skarzer0/daytona-arcade-recomp), branch
`naomi-native`); the peek protocol and the cartridge port's reads follow DragonMinded's
[libnaomi](https://github.com/DragonMinded/libnaomi) and [netboot](https://github.com/DragonMinded/netboot). Built
on [KallistiOS](https://github.com/KallistiOS/KallistiOS). The game's C, its runtime and its tools are cheviman's
(Sebastian Toyos). OutRunners is a trademark of SEGA; this project is not affiliated with or endorsed by SEGA.
