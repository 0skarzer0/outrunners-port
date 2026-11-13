#!/bin/bash
# The NAOMI netboot image of OutRunners from the player's ROM set (ports/naomi/README.md):
#
#   ports/naomi/build_naomi.sh orunners.zip [OUTDIR]
#
# Needs the sh-elf toolchain and KallistiOS built for KOS_SUBARCH=naomi (KOS_NAOMI, default
# /opt/toolchains/dc/kos-naomi, with naomibintool), a computer's build of the game for its assets (cmake, a C
# compiler, python3). Output: OUTDIR/orunners_naomi.bin.gz for the WiPi netbooter,
# OUTDIR/dimm/orunners.pcm (the MultiPCM's samples, for the AICA: aica_sound.c). What it makes comes from the player's ROM set:
# never share it.
set -e
ROM=$(realpath "${1:?usage: build_naomi.sh orunners.zip [OUTDIR]}")
OUT=$(realpath -m "${2:-build/naomi}")
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
KOS_NAOMI=${KOS_NAOMI:-/opt/toolchains/dc/kos-naomi}
mkdir -p "$OUT/dimm"

# 1. the game's assets from the ROM set, as the computers' first run takes them, in one pack
cmake -S "$REPO" -B "$OUT/host" -DCMAKE_BUILD_TYPE=Release -DORUNNERS_ROM="$ROM" > "$OUT/host.log"
make -C "$OUT/host" -j"$(nproc)" orunners_game_headless orunners_assets >> "$OUT/host.log"
"$REPO/build/bin/orunners_game_headless" --frames 1 --hashes /dev/null --assets "$REPO/assets" \
	--write-pack "$OUT/orunners.assets" > /dev/null
# 2. the NAOMI's pack: tiles two pens to a byte, the samples apart
python3 "$HERE/naomi_assets.py" "$OUT/orunners.assets" "$OUT/dimm/orunners_naomi.assets" "$OUT/dimm/orunners.pcm"
# 3. the program
(source "$KOS_NAOMI/environ.sh" && make -C "$HERE" -j"$(nproc)")
# 4. the netboot image: the program in a NAOMI header, the pack after it in the NetDIMM (/dimm)
"$KOS_NAOMI/utils/naomibintool/naomibintool" build "$OUT/orunners_naomi-prog.bin" -t "OUTRUNNERS" -d "HOMEBREW" \
	-e "$HERE/orunners_naomi.elf"
python3 "$HERE/naomi_pack.py" "$OUT/orunners_naomi-prog.bin" "$OUT/dimm" "$OUT/orunners_naomi.bin"
gzip -9 -f "$OUT/orunners_naomi.bin"
cp "$HERE/orunners_naomi.elf" "$OUT/"
echo "Netboot image: $OUT/orunners_naomi.bin.gz"
echo "Live figures: $(/opt/toolchains/dc/sh-elf/bin/sh-elf-nm "$OUT/orunners_naomi.elf" | grep -i 'orunners_naomi_live$')"
