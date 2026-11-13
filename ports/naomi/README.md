# OutRunners on a SEGA NAOMI / NAOMI 2 (NetDIMM netboot)

Work in progress for a Crazy Taxi upright with a NAOMI 2 and a 512 MB NetDIMM loaded by a Raspberry Pi
(WiPi Netbooter). The study behind it (what each part costs, what the game uses of the System 32 video chip):
[docs/outrunners.md of 0skarzer0/naomi-wheel-games](https://github.com/0skarzer0/naomi-wheel-games/blob/main/docs/outrunners.md).

## Stages

1. **Boot and measure** (this directory today): the game's C, the board and the sound program on the SH-4; the
   cabinet's controls through the JVS; `runtime/src/video.c` drawn every few frames into a framebuffer, only to
   see something; the time each part takes, read live from the Raspberry (`live.py`). **Runs on the board**
   (2026-11-13, NAOMI 2 of a Crazy Taxi upright): see "Measured on the board".
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

## Measured on the board (2026-11-13)

Attract mode, a frame drawn every 60 (`live.py draw 60`), after the fixes below:

| Part | Time a frame |
|---|---:|
| The game's C, the board, the interrupts | ~8 ms |
| The sound program and the chips (MultiPCM in software, FM off) | ~2.9 ms |
| `video.c`'s frame + the copy to the screen | **~360 ms** (about 22 frames) |

So 57-60 game frames a second without the picture, and the software video at about 3 frames a second: the PowerVR2
renderer (stage 2) is what the port needs, as the computer's measurements said. A profile of the drawn frames:
`page_pixel`, `scrolled_pixel`, `zoomed_pixel`, `mix_rows`, the sprites' rows, and 32-bit divisions inside them.

What it took to run (each found with the peeks):
- **KallistiOS stops a thread whose stack pointer is below its stack's base** ("Thread stack underrun", checked at
  every switch). The game's tasks run on stacks of their own; malloc'd, they were below the main thread's stack (at
  the top of RAM), and the game died at its first switch with nothing on the screen. Now the game runs in a thread
  of its own whose stack (`host_task_area`, runtime/src/tasks.c) holds the tasks' stacks too.
- **64-bit divisions** (libgcc's, there is no division instruction for them) in the sound board's time conversions
  took about a fifth of the CPU: replaced with exact 32-bit ones (`ORUNNERS_NAOMI`, sound_chips.c and sound_cpu.c).
- **The FM synthesis**, for a chip that plays no note in OutRunners: off (`ORUNNERS_NO_FM_SYNTHESIS`; the timers
  stay). **The sound tests' hashes** and **the sprites' erase for the screen not shown**: off on the NAOMI.
- `video.c`'s 4 MB of state from the heap (`ORUNNERS_VIDEO_ON_HEAP`).
Checked on a computer: with `ORUNNERS_NAOMI ORUNNERS_NO_FM_SYNTHESIS ORUNNERS_PACKED_PENS`, the game state and video
hashes of 3,501 frames of attract and race are the same as the normal build's.

Testing traps: an unanswered peek keeps the NetDIMM busy for up to a minute (sends then time out: wait for
`info()` to answer); and the Claude workspace's copy to the user's computer can deliver an older file under the same
staged name (give each image a new name, check its md5).

## On the board

The controls (values of the Crazy Taxi upright, widening by themselves): wheel analog 0, gas 1, brake 2; the
two-position shifter forward = SHIFT UP (HIGH), back = SHIFT DOWN (LOW); START, which is also the coin button (pressing it inserts a coin, letting it go presses START: one tap starts a race); the coin slots too, by the meters' difference;
SERVICE and TEST as the board's.

Live figures, on the Raspberry of the cabinet (DragonMinded's netdimm.py), with the address `build_naomi.sh`
prints:

```sh
python3 live.py 10.0.0.4 0x8c...... fps 60     # game fps, frames drawn, us of the game, sound and video
python3 live.py 10.0.0.4 0x8c...... draw 8     # a frame drawn every 8
python3 live.py 10.0.0.4 0x8c...... log        # the console
```

A failure, or no game frame for 10 s, puts the console's last lines on the screen (with the game thread's pc and
pr for `sh-elf-addr2line` on `orunners_naomi.elf`); an exception nothing handles writes its code, pc, pr and address
to the console and keeps answering peeks (the live block's version becomes 0xdeadXXXX).

More tools for the Raspberry: `watch.py LIVE [ADDR VALUE]` (a poke, then the console's lines as they come, until the
peeks stop: the last thing said before a crash) and `profile.py LIVE [DRAW_EVERY]` (the game thread's pc sampled by
a thread of the program, `sh-elf-addr2line -f` names them). Test builds: `make EXTRA=-DORUNNERS_NAOMI_STAGE=N` stops
`main()` at stage 0 (KallistiOS and the peeks), 1 (the assets read) or 2 (everything ready; waits for a poke of
`naomi_stop_at`).

## Credits

The NetDIMM files, the peeks, the console ring and `naomi_pack.py` come from the NAOMI build of Daytona USA's
native port ([0skarzer0/daytona-arcade-recomp](https://github.com/0skarzer0/daytona-arcade-recomp), branch
`naomi-native`); the peek protocol and the cartridge port's reads follow DragonMinded's
[libnaomi](https://github.com/DragonMinded/libnaomi) and [netboot](https://github.com/DragonMinded/netboot). Built
on [KallistiOS](https://github.com/KallistiOS/KallistiOS). The game's C, its runtime and its tools are cheviman's
(Sebastian Toyos). OutRunners is a trademark of SEGA; this project is not affiliated with or endorsed by SEGA.
