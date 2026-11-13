/**
 *    Name:        main.c
 *    Description: OutRunners on a SEGA NAOMI / NAOMI 2 (KallistiOS, KOS_SUBARCH=naomi), first stage: the game, its
 *                 board and its sound program on the SH-4, the cabinet's controls through the JVS, the computers'
 *                 video (runtime/src/video.c) drawn every few frames, and the time each part takes, for the
 *                 netbooter's Raspberry to read (naomi.h, live.py). See ports/naomi/README.md
 *    Created:     13-11-2026
 *
 *    license:BSD-3-Clause
 *    What it is for: measuring on the board what docs/outrunners.md of 0skarzer0/naomi-wheel-games estimated from
 *    a computer (the game's C and the Z80's well inside a frame, video.c about ten times over it), before the
 *    PowerVR2 renderer replaces video.c.
 *
 *    The assets: ports/naomi/naomi_assets.py makes the NAOMI's pack from the player's (tiles two pens to a byte,
 *    ORUNNERS_PACKED_PENS; the MultiPCM's samples apart), naomi_pack.py puts it after the program in the netboot
 *    image, and it is read from the NetDIMM into main RAM here (about 22 MB). The samples stay out of RAM at this
 *    stage: the MultiPCM plays silence (they go to the AICA's RAM in the third stage).
 */
#include <kos.h>
#include <arch/timer.h>
#include <dc/video.h>

#include "s32/machine.h"
#include "s32/program.h"
#include "s32/video.h"
#include "sound/chips.h"
#include "game/assets.h"
#include "host/tasks.h"

#include "naomi.h"
#include "pvr_video.h"
#include "aica_sound.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

KOS_INIT_FLAGS(INIT_DEFAULT);

#ifdef ORUNNERS_NAOMI_STAGE
/* test builds: where main() stops, a variable so that everything after it is linked all the same (pokeable) */
static volatile int naomi_stop_at = ORUNNERS_NAOMI_STAGE;
#endif

#define ASSETS_FILE     "/dimm/orunners_naomi.assets"

/* The board's wheel as MAME has it (ports/sdl2/main.c) */
#define WHEEL_LEFT      0x10
#define WHEEL_CENTRE    0x80
#define WHEEL_RIGHT     0xf0

/* The live figures (naomi.h): peeked at by the Raspberry */
volatile orunners_naomi_live_t orunners_naomi_live;

/* ---- the cabinet's controls (naomi-kit's nk_cabinet_read: a Crazy Taxi upright's JVS) ---- */

static int coin_frames;

/* the wheel (-128..127) to the board's 0x10-0xf0 */
static uint8_t board_wheel(int8_t steer)
{
	return (uint8_t)(steer < 0 ? WHEEL_CENTRE + steer * (WHEEL_CENTRE - WHEEL_LEFT) / 128
		: WHEEL_CENTRE + steer * (WHEEL_RIGHT - WHEEL_CENTRE) / 127);
}

/* At the start of VBLANK, when the board reads its controls (MAME polls them then too). The shifter's two
   positions with their spring: forward is shift up (HIGH), back shift down (LOW); the game holds the gear. START
   is the coin button too (nk_cabinet_start_coin, set in main): a press inserts a coin, the release is START */
static void on_controls_update(void)
{
	s32_inputs *input = &s32_board.input;
	nk_cabinet cabinet;
	bool coin = false, start;

	nk_cabinet_read(&cabinet);
	if (cabinet.coins > 0)
		coin_frames = 4;    /* (the board counts a coin from a pulse of a few frames) */
	if (coin_frames > 0)
	{
		coin = true;
		coin_frames--;
	}
	start = cabinet.start && !coin;
	/* the 315-5296s' ports are active low */
	input->port_a[0] = (uint8_t)(0xff & ~(cabinet.forward ? 0x01 : 0) & ~(cabinet.back ? 0x02 : 0));
	input->port_b[0] = 0xff;
	input->port_e[0] = (uint8_t)(0xff & ~(cabinet.service ? 0x01 : 0) & ~(cabinet.test ? 0x02 : 0) & ~(coin ? 0x04 : 0)
		& ~(start ? 0x10 : 0));
	input->port_a[1] = input->port_b[1] = input->port_e[1] = 0xff;
	input->analog[0] = board_wheel(cabinet.steer);
	input->analog[1] = cabinet.gas;
	input->analog[2] = cabinet.brake;
	input->analog[3] = WHEEL_CENTRE;
	input->analog[6] = input->analog[7] = 0;
}

/* ---- the time each part takes ---- */

static uint64_t frame_started, video_time, sound_time, frame_worst;
static uint64_t second_game, second_sound, second_video, second_texels, second_build, second_wait, second_submit;
static uint32_t missing_seen;
static uint32_t second_frames, second_drawn;

/* The last second's figures, from naomi-kit's peek thread once a second */
static void naomi_live_second(void)
{
	const irq_mask_t old = irq_disable();
	const uint32_t frames = second_frames, drawn = second_drawn;
	const uint64_t game = second_game, sound = second_sound, video = second_video, worst = frame_worst;
	const uint64_t texels = second_texels, build = second_build, wait = second_wait, submit = second_submit;

	second_frames = second_drawn = 0;
	second_game = second_sound = second_video = frame_worst = second_texels = 0;
	second_build = second_wait = second_submit = 0;
	irq_restore(old);
	orunners_naomi_live.game_frames = frames;
	orunners_naomi_live.drawn_frames = drawn;
	orunners_naomi_live.game_us = frames ? (uint32_t)(game / frames) : 0;
	orunners_naomi_live.sound_us = frames ? (uint32_t)(sound / frames) : 0;
	orunners_naomi_live.video_us = drawn ? (uint32_t)(video / drawn) : 0;
	orunners_naomi_live.worst_frame_us = (uint32_t)worst;
	orunners_naomi_live.texels_drawn = (uint32_t)texels;
	orunners_naomi_live.build_us = drawn ? (uint32_t)(build / drawn) : 0;
	orunners_naomi_live.wait_us = drawn ? (uint32_t)(wait / drawn) : 0;
	orunners_naomi_live.submit_us = drawn ? (uint32_t)(submit / drawn) : 0;
}

/* ---- the picture: the scene on the PowerVR2 (pvr_video.c) ---- */


/* When the board's frame is shown (VBLANK): its scene, to the PowerVR2 */
static void on_frame_shown(void)
{
	const uint64_t start = timer_us_gettime64();
	pvr_video_stats stats;

	pvr_video_frame(&stats);
	video_time += timer_us_gettime64() - start;
	second_drawn++;
	second_texels += (uint64_t)stats.texels_drawn;
	second_build += stats.build_us;
	second_wait += stats.wait_us;
	second_submit += stats.submit_us;
	if (stats.missing & ~missing_seen)
	{
		printf("NAOMI: frame %lu needs what the scene does not draw: %04lx\n", (unsigned long)orunners_naomi_live.frames_total,
			(unsigned long)stats.missing);
		missing_seen |= stats.missing;
	}
	orunners_naomi_live.quads = (uint32_t)stats.quads;
	if (orunners_naomi_live.sound_writes != 0xffffffffu)
		orunners_naomi_live.sound_writes = aica_sound_register_writes();
	orunners_naomi_live.missing = missing_seen;
	orunners_naomi_live.video_memory_free = stats.video_memory_free;
	orunners_naomi_live.textures = (uint32_t)stats.images | (uint32_t)stats.failed << 16;
}

static void on_frame_done(void)
{
	uint64_t start = timer_us_gettime64(), now;
	size_t count;

	/* the sound board up to the frame's end: the Z80's C, the YM3438's timers and the chips' samples */
	if (s32_board.time_passes)
		s32_board.time_passes(s32_board.cycles);
	sound_chips_end_frame(s32_board.cycles);
	(void)sound_chips_frame_mix(&count);
	now = timer_us_gettime64();
	sound_time += now - start;

	{
		const irq_mask_t old = irq_disable();
		const uint64_t total = now - frame_started;

		second_frames++;
		second_sound += sound_time;
		second_video += video_time;
		second_game += total - sound_time - video_time;
		if (total > frame_worst)
			frame_worst = total;
		irq_restore(old);
	}
	orunners_naomi_live.head.frames_alive++;
	orunners_naomi_live.frames_total++;
	if (orunners_naomi_live.frames_total <= 600 && orunners_naomi_live.frames_total % 30 == 1)
		printf("NAOMI: frame %lu: %lu us (sound %lu, video %lu)\n", (unsigned long)orunners_naomi_live.frames_total,
			(unsigned long)(now - frame_started), (unsigned long)sound_time, (unsigned long)video_time);
	video_time = sound_time = 0;

	/* (no faster than the board: the PowerVR2 takes a new scene once a vblank, pvr_video_frame waits for it) */
	frame_started = timer_us_gettime64();
}

static void on_irq_changed(bool asserted)
{
	program_irq_line(asserted);
}

static void on_log(const char *message)
{
	printf("board: %s\n", message);
}

/* ---- the assets: the NAOMI's pack from the NetDIMM into main RAM ----

   (Found on the board, 2026-11-13: an image whose program and .bss end beyond about 0x8c530000 never reaches
   main(), while the same addresses taken from the heap later are fine; so video.c's 4 MB of state comes from the
   heap here, ORUNNERS_VIDEO_ON_HEAP) */

static void load_assets(void)
{
	FILE *input = fopen(ASSETS_FILE, "rb");
	long size;
	uint8_t *bytes;
	const char *problem;
	const uint64_t start = timer_ms_gettime64();

	if (!input || fseek(input, 0, SEEK_END) != 0 || (size = ftell(input)) <= 0 || fseek(input, 0, SEEK_SET) != 0)
		nk_show_log("NO " ASSETS_FILE " (ports/naomi/naomi_assets.py)");
	printf("NAOMI: %s, %ld bytes; %lu bytes of RAM above the heap\n", ASSETS_FILE, size,
		(unsigned long)nk_heap_left());
	if (!(bytes = malloc((size_t)size)))
		nk_show_log("NO ROOM FOR THE ASSETS");
	/* in pieces: the NetDIMM's peeks share the cartridge port and wait for each piece (naomi-kit) */
	for (long done = 0; done < size;)
	{
		const size_t piece = size - done > (256 << 10) ? (256 << 10) : (size_t)(size - done);

		if (fread(bytes + done, 1, piece, input) != piece)
			nk_show_log("THE ASSETS CANNOT BE READ");
		done += (long)piece;
		if ((done & ((4 << 20) - 1)) < (256 << 10) || done == size)
			printf("NAOMI: %ld of %ld bytes read, %lu ms\n", done, size,
				(unsigned long)(timer_ms_gettime64() - start));
	}
	fclose(input);
	printf("NAOMI: assets read in %lu ms\n", (unsigned long)(timer_ms_gettime64() - start));
	if ((problem = assets_load_pack_bytes(bytes, (size_t)size)) != NULL)
	{
		printf("NAOMI: %s\n", problem);
		nk_show_log("THE ASSETS ARE NOT THIS BUILD'S");
	}
}

/* The game runs in a thread of its own whose stack holds the tasks' stacks too (runtime/src/tasks.c): KallistiOS
   stops a thread whose stack pointer goes below its stack's base, and the main thread's is at the top of RAM */
static void *game_thread(void *unused)
{
	(void)unused;
	program_run();
	return NULL;
}

static void run_game_thread(void)
{
	const kthread_attr_t attributes = { .stack_size = host_task_area_size, .stack_ptr = host_task_area,
		.label = "outrunners" };
	kthread_t *thread = thd_create_ex(&attributes, game_thread, NULL);

	if (!thread)
		nk_show_log("NO THREAD FOR THE GAME");
	nk_watch_thread(thread);
	thd_join(thread, NULL);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	{
		/* the console, /dimm, the peeks (first: the console can be read through the NetDIMM while the assets load),
		   the watchdog (the boot reads 26 MB through the cartridge port), misaligned accesses (the V60's code) */
		static const nk_config config = {
			.title = "OUTRUNNERS (NAOMI)", .live = &orunners_naomi_live.head, .game = "ORUN",
			.names = ORUNNERS_LIVE_NAMES, .count = ORUNNERS_LIVE_COUNT, .need_dimm = true,
			.hang_first_s = 120, .hang_s = 10, .misaligned = true, .every_second = naomi_live_second,
		};

		nk_boot(&config);
		nk_cabinet_start_coin(6);
	}
#if defined(ORUNNERS_NAOMI_STAGE)
	if (naomi_stop_at == 0)    /* does KallistiOS run, and do the peeks answer? */
		for (;;)
		{
			thd_sleep(1000);
			orunners_naomi_live.head.frames_alive++;
			orunners_naomi_live.frames_total++;
			printf("NAOMI: stage 0 alive, %lu s, heap left %lu\n", (unsigned long)orunners_naomi_live.frames_total,
				(unsigned long)nk_heap_left());
		}
#endif
	pvr_video_start();
	load_assets();
	printf("NAOMI: %lu bytes of RAM left\n", (unsigned long)nk_heap_left());
#if defined(ORUNNERS_NAOMI_STAGE)
	if (naomi_stop_at == 1)
	for (;;)
	{
		thd_sleep(1000);
		orunners_naomi_live.head.frames_alive++;
		orunners_naomi_live.frames_total++;
	}
#endif

	if (!s32_video_allocate())
		nk_show_log("NO ROOM FOR THE VIDEO");
	s32_board.irq_changed = on_irq_changed;
	s32_board.controls_update = on_controls_update;
	s32_board.log = on_log;
	s32_board.frame_timing = true;
	s32_board.program_at = data_byte_at;
	printf("NAOMI: %s\n", "s32_init(NULL);");
	s32_init(NULL);
	s32_video_init_graphics(assets_graphics());
	s32_video_show_screens(1);
	s32_board.frame_shown = on_frame_shown;
	s32_board.sound_at = assets_sound_data;
	s32_board.sound_byte_at = assets_sound_byte;
	s32_board.pcm_at = assets_pcm_byte;
	printf("NAOMI: %s\n", "sound_chips_attach();");
	sound_chips_attach();
	sound_cpu_attach();
	if (!aica_sound_start())
		orunners_naomi_live.sound_writes = 0xffffffffu;
	printf("NAOMI: %s\n", "program_reset();");
	program_reset();
	program_frame_done = on_frame_done;
	frame_started = timer_us_gettime64();
	printf("NAOMI: the game starts\n");
#if defined(ORUNNERS_NAOMI_STAGE)
	if (naomi_stop_at == 2)    /* waits here until poked to anything else (live.py's ADDR of naomi_stop_at) */
	{
		printf("NAOMI: stage 2, waiting for a poke at %p\n", (void *)&naomi_stop_at);
		while (naomi_stop_at == 2)
		{
			thd_sleep(100);
			orunners_naomi_live.head.frames_alive++;
		}
		printf("NAOMI: poked, the game runs\n");
	}
#endif
	run_game_thread();
	nk_show_log("THE GAME STOPPED");
}
