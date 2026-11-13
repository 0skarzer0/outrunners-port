/**
 *    Name:        naomi.h
 *    Description: OutRunners on a SEGA NAOMI / NAOMI 2 with a NetDIMM (KallistiOS, KOS_SUBARCH=naomi): its live
 *                 figures for the netbooter's Raspberry (ports/naomi/README.md)
 *    Created:     13-11-2026
 *
 *    license:BSD-3-Clause
 *    The NetDIMM's files, the peeks, the console ring, the watchdog and the controls are naomi-kit's
 *    (ports/naomi/naomi-kit, 0skarzer0/naomi-kit). The live block is its head, then the game's words below, which
 *    naomi-kit's tools/pi/live.py shows by these names. Its address: the console's line "naomi-kit: ... live
 *    figures at ..." or `sh-elf-nm` on the ELF (orunners_naomi_live).
 */
#ifndef ORUNNERS_NAOMI_H
#define ORUNNERS_NAOMI_H

#include <stdint.h>

#include "nk/naomi_kit.h"

typedef struct
{
	nk_live_head head;         /* +0x00 naomi-kit's (0x60 bytes): seconds, the console, test_inputs, inputs... */
	uint32_t game_frames;      /* the game's frames run in the last second (60 = full speed) */
	uint32_t drawn_frames;     /* frames handed to the PowerVR2 in the last second */
	uint32_t game_us;          /* the V70's C and the board a frame, average (microseconds) */
	uint32_t sound_us;         /* the Z80's C and the sound chips a frame, average */
	uint32_t video_us;         /* the scene built and handed to the PowerVR2 (pvr_video.c), average a frame */
	uint32_t worst_frame_us;   /* the longest game frame of the last second, video and sound included */
	uint32_t frames_total;     /* the game's frames since boot */
	uint32_t quads;            /* the last frame's quads (pvr_video.c) */
	uint32_t texels_drawn;     /* texels made in the last second (tiles, bitmap rows, sprites) */
	uint32_t missing;          /* what the frames needed that the scene does not draw (S32_SCENE_MISSING_ bits, or'ed) */
	uint32_t video_memory_free;/* bytes of video memory left */
	uint32_t textures;         /* textures held; failed allocations << 16 */
	uint32_t build_us;         /* of video_us, the last second's average: the scene worked out */
	uint32_t wait_us;          /* ... waiting for the PowerVR2 */
	uint32_t submit_us;        /* ... the quads handed to it */
	uint32_t sound_writes;     /* the AICA's channel registers written (aica_sound.c); 0xffffffff no samples */
} orunners_naomi_live_t;

#define ORUNNERS_LIVE_NAMES "game_frames drawn_frames game_us sound_us video_us worst_frame_us frames_total quads " \
	"texels_drawn missing video_memory_free textures build_us wait_us submit_us sound_writes"
#define ORUNNERS_LIVE_COUNT 16

extern volatile orunners_naomi_live_t orunners_naomi_live;

#endif
