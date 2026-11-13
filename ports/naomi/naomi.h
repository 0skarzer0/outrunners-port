/**
 *    Name:        naomi.h
 *    Description: OutRunners on a SEGA NAOMI / NAOMI 2 with a NetDIMM (KallistiOS, KOS_SUBARCH=naomi): the board's
 *                 pieces the computers' ports get from SDL (ports/naomi/README.md)
 *    Created:     13-11-2026
 *
 *    license:BSD-3-Clause
 *    The NetDIMM files (naomi_dimmfs.c), the peeks (naomi_dimmcomms.c) and the console ring (naomi_log.c) come from
 *    the NAOMI build of Daytona USA's native port (0skarzer0/daytona-arcade-recomp, branch naomi-native).
 */
#ifndef ORUNNERS_NAOMI_H
#define ORUNNERS_NAOMI_H

#include <stdint.h>

/* Live figures for the netbooter's Raspberry to peek at (live.py): 32-bit words, little endian. Its address is
   the console's line "NAOMI: live figures at ..." or `sh-elf-nm` on the ELF (orunners_naomi_live). */
typedef struct
{
	uint32_t magic;            /* +0x00 "ORUN" (0x4e55524f) */
	uint32_t version;          /* +0x04 1 */
	uint32_t seconds;          /* +0x08 seconds counted; the figures below are of the last one */
	uint32_t game_frames;      /* +0x0c the game's frames run in the last second (60 = full speed) */
	uint32_t drawn_frames;     /* +0x10 frames drawn on the screen in the last second */
	uint32_t game_us;          /* +0x14 the V70's C and the board a frame, average (microseconds) */
	uint32_t sound_us;         /* +0x18 the Z80's C and the sound chips a frame, average */
	uint32_t video_us;         /* +0x1c video.c's frame plus the copy to the screen, average of the frames drawn */
	uint32_t worst_frame_us;   /* +0x20 the longest game frame of the last second, video and sound included */
	uint32_t frames_total;     /* +0x24 the game's frames since boot */
	uint32_t dimm_requests;    /* +0x28 peeks and pokes answered */
	uint32_t log_lines;        /* +0x2c address of the console ring: log_count lines of log_width bytes */
	uint32_t log_count;        /* +0x30 */
	uint32_t log_width;        /* +0x34 */
	uint32_t log_next;         /* +0x38 address of the number of lines written so far */
	uint32_t heap_left;        /* +0x3c bytes above the heap's top after loading */
	uint32_t draw_every;       /* +0x40 a frame drawn every this many (pokeable: 1-60) */
	uint32_t inputs;           /* +0x44 wheel | gas << 8 | brake << 16 | port E bits << 24, as the game reads them */
} orunners_naomi_live_t;
extern volatile orunners_naomi_live_t orunners_naomi_live;

/* main(), first: the console into the ring, the archive in the NetDIMM mounted on /dimm, the watchdog */
int naomi_boot(void);

/* The kept console lines on the screen, for good */
void naomi_show_log(const char *why) __attribute__((noreturn));

/* Main RAM above the heap's top */
uint32_t naomi_heap_left(void);

/* The thread that answers the NetDIMM's peeks and pokes */
void naomi_dimmcomms_start(void);

/* The game's frame counter, for the watchdog */
extern volatile uint32_t naomi_frames_alive;

#endif
