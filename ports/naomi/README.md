# OutRunners on a SEGA NAOMI / NAOMI 2 (NetDIMM netboot)

Work in progress for a Crazy Taxi upright with a NAOMI 2 and a 512 MB NetDIMM loaded by a Raspberry Pi
(WiPi Netbooter). The study behind it (what each part costs, what the game uses of the System 32 video chip):
[docs/outrunners.md of 0skarzer0/naomi-wheel-games](https://github.com/0skarzer0/naomi-wheel-games/blob/main/docs/outrunners.md).

## Stages

1. **Boot and measure**: the game's C, the board and the sound program on the SH-4; the cabinet's controls through
   the JVS; the time each part takes, read live from the Raspberry (`live.py`). Done (2026-11-13).
2. **PowerVR2 renderer** in place of `video.c` (`ORUNNERS_VIDEO_SCENE`: runtime/src/video_scene.c makes the frame a
   scene of textured quads, `pvr_video.c` draws it): tilemap pages and the text map as textures, NBG0 scrolled and
   zoomed, NBG2 a strip a line (row scroll and row select), the bitmap layer (the road) by rows, sprites as quads
   from a texture cache, priorities as depth, colour offsets as vertex colours, the sprites' shadow pen, the text
   layer's blending over the sprites, fades taken away exactly. **Runs on the board at 60 fps** in attract mode and
   most of a race (2026-11-13); the shadows, blending and fades are checked on a computer only so far.
3. **Sound on the AICA** (`aica_sound.c`, runtime/src/multipcm_aica.c): the MultiPCM's 4 MB of samples
   (`/dimm/orunners.pcm`) in the AICA's RAM, its 28 voices on AICA channels whose registers the SH-4 works out from
   the MultiPCM's; the left seat's sound on both speakers. The YM3438 only gives the sound program its timers:
   OutRunners plays no FM note. Checked on a computer (`ORUNNERS_AICA_WAV`, below); **not yet heard on the board**.
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

Stage 1 (the software `video.c`, a frame drawn every 60): the game's C, the board and the interrupts ~8 ms a frame,
the sound program and the chips ~2.9 ms (the MultiPCM in software), `video.c`'s frame **~360 ms**: hence stage 2.
Stage 2, a race (`race.py`): the game ~8 ms, the sound ~1.1 ms (no MultiPCM synthesis), the scene built ~4-5 ms and
handed to the PowerVR2 ~1.2 ms: 60 fps, with dips where many colours change at once (palette cycling), which tables
of colours by cell and the bitmap's rows redrawn only where seen should cut (not yet measured on the board).

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
- **Misaligned reads**: the V70 reads 16-bit words at odd addresses (src/objects/scenery.c's flashing scenery, the
  game froze there); the SH-4 faults. `naomi_log.c` does them a byte at a time in the exception handler, in a
  branch's delay slot too (counted in the live block, +0x78).
- **The PowerVR2's wait is the vblank's**: `vid_waitvbl` on top of `pvr_wait_ready` halved the frame rate.
- **newlib's `memcmp` goes a byte at a time** on the SH-4: the scene compares by words (`same()`).
- **The MultiPCM in software** made a third of a race's frame (for silence: no samples in RAM): off
  (`ORUNNERS_NO_PCM_SYNTHESIS`), now the AICA plays the voices.
Checked on a computer: with `ORUNNERS_NAOMI ORUNNERS_NO_FM_SYNTHESIS ORUNNERS_PACKED_PENS`, the game state and video
hashes of 3,501 frames of attract and race are the same as the normal build's.

Testing traps: an unanswered peek keeps the NetDIMM busy for up to a minute (sends then time out: wait for
`info()` to answer); and the Claude workspace's copy to the user's computer can deliver an older file under the same
staged name (give each image a new name, check its md5).

## Checked on a computer

```sh
# the scene drawn in software against video.c, a line every 30 frames (pixels that differ, quads, blending)
cmake -B build-scene -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS=-DORUNNERS_VIDEO_SCENE
make -C build-scene orunners_game_headless
ORUNNERS_SCENE_COMPARE=/tmp/scene build/bin/orunners_game_headless --script race_arcade.txt --hashes /dev/null \
    --video-hashes /dev/null --screens 1 --assets orunners.assets
# the AICA's registers heard through a rough model of the chip, against the MultiPCM's own sound
ORUNNERS_AICA_OUTPUT=1 ORUNNERS_AICA_WAV=aica.wav build/bin/orunners_game_headless --script race_arcade.txt \
    --hashes /dev/null --screens 1 --assets orunners.assets --sound-wav /dev/null
build/bin/orunners_game_headless --script race_arcade.txt --hashes /dev/null --screens 1 \
    --assets orunners.assets --sound-wav reference.wav
```

Results (2026-11-13, the arcade race script, 9,000 frames): the scene's pixels differing from `video.c`'s, 0.68 %
over all frames (the edges of zoomed sprites; shadows that overlap darken twice; a fade's shadows a little darker).
The AICA model's sound against the MultiPCM's (its left speaker, the left seat): loudness a frame correlated 0.89,
spectra over 4 s 0.98-0.997, the same level. (`race_arcade.txt` is in naomi-wheel-games, `outrunners/`.)

## On the board

The controls (values of the Crazy Taxi upright, widening by themselves): wheel analog 0, gas 1, brake 2; the
two-position shifter forward = SHIFT UP (HIGH), back = SHIFT DOWN (LOW); START, which is also the coin button (pressing it inserts a coin, letting it go presses START: one tap starts a race); the coin slots too, by the meters' difference;
SERVICE and TEST as the board's.

Live figures, on the Raspberry of the cabinet (DragonMinded's netdimm.py), with the address `build_naomi.sh`
prints:

```sh
python3 live.py 10.0.0.4 0x8c...... fps 60     # game fps, us of the game, sound and video, misaligned, AICA writes
python3 live.py 10.0.0.4 0x8c...... log        # the console
```

A failure, or no game frame for 10 s, puts the console's last lines on the screen (with the game thread's pc and
pr for `sh-elf-addr2line` on `orunners_naomi.elf`); an exception nothing handles writes its code, pc, pr and address
to the console and keeps answering peeks (the live block's version becomes 0xdeadXXXX).

`race.py LIVE SECONDS` plays without anyone at the cabinet (a coin, START, the gas down and the wheel swinging,
through the live block's `test_inputs`) and prints the same figures. More tools for the Raspberry: `watch.py LIVE [ADDR VALUE]` (a poke, then the console's lines as they come, until the
peeks stop: the last thing said before a crash) and `profile.py LIVE [DRAW_EVERY]` (the game thread's pc sampled by
a thread of the program, `sh-elf-addr2line -f` names them). Test builds: `make EXTRA=-DORUNNERS_NAOMI_STAGE=N` stops
`main()` at stage 0 (KallistiOS and the peeks), 1 (the assets read) or 2 (everything ready; waits for a poke of
`naomi_stop_at`).

## Credits

The NetDIMM files, the peeks, the console ring and `naomi_pack.py` come from the NAOMI build of Daytona USA's
native port ([0skarzer0/daytona-arcade-recomp](https://github.com/0skarzer0/daytona-arcade-recomp), branch
`naomi-native`); the peek protocol and the cartridge port's reads follow DragonMinded's
[libnaomi](https://github.com/DragonMinded/libnaomi) and [netboot](https://github.com/DragonMinded/netboot). Built
on [KallistiOS](https://github.com/KallistiOS/KallistiOS) (its PowerVR2 and AICA support; the AICA's registers as
its sound driver and [MAME](https://github.com/mamedev/mame)'s `sound/aica.cpp` use them). The MultiPCM's
behaviour is MAME's `sound/multipcm.cpp` (Miguel Angel Horna), as runtime/src/multipcm.c follows it; the
System 32 video chip's, MAME's `sega/segas32_v.cpp` (Aaron Giles), as runtime/src/video.c follows it. The game's C, its runtime and its tools are cheviman's
(Sebastian Toyos). OutRunners is a trademark of SEGA; this project is not affiliated with or endorsed by SEGA.
