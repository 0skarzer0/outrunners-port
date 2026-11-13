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
#include <dc/maple.h>
#include <dc/maple/mie.h>
#include <dc/video.h>

#include "s32/machine.h"
#include "s32/program.h"
#include "s32/video.h"
#include "sound/chips.h"
#include "game/assets.h"
#include "host/tasks.h"

#include "naomi.h"
#include "naomi_dimmfs.h"
#include "pvr_video.h"

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

/* ---- the cabinet's controls (docs/hardware.md of naomi-wheel-games: a Crazy Taxi upright) ---- */

static struct
{
	int wheel_min, wheel_centre, wheel_max;
	int gas_min, gas_max;
	int brake_min, brake_max;
} calibration = { 8448, 33792, 60160, 11776, 47104, 14080, 47360 };

static int coin_counts_known;
static uint16_t coin_counts[MIE_JVS_COIN_SLOTS];
static int coin_frames;

static int raw_analog(uint16_t raw)
{
	return (int)(raw & MIE_JVS_ANALOG_RAW_MASK);
}

/* the wheel to the board's 0x10-0xf0, the calibration widening when the wheel goes further */
static uint8_t steer(int raw)
{
	int value;

	if (raw < calibration.wheel_min)
		calibration.wheel_min = raw;
	if (raw > calibration.wheel_max)
		calibration.wheel_max = raw;
	if (raw < calibration.wheel_centre)
		value = WHEEL_CENTRE - (calibration.wheel_centre - raw) * (WHEEL_CENTRE - WHEEL_LEFT)
			/ (calibration.wheel_centre - calibration.wheel_min);
	else
		value = WHEEL_CENTRE + (raw - calibration.wheel_centre) * (WHEEL_RIGHT - WHEEL_CENTRE)
			/ (calibration.wheel_max - calibration.wheel_centre);
	return (uint8_t)(value < WHEEL_LEFT ? WHEEL_LEFT : (value > WHEEL_RIGHT ? WHEEL_RIGHT : value));
}

/* a pedal to 0-255, with 6 % at each end so that rest reads 0 and a full press 255 */
static uint8_t pedal(int raw, int *low, int *high)
{
	int margin, from, to;

	if (raw < *low)
		*low = raw;
	if (raw > *high)
		*high = raw;
	margin = (*high - *low) * 6 / 100;
	from = *low + margin;
	to = *high - margin;
	if (raw <= from)
		return 0;
	if (raw >= to)
		return 255;
	return (uint8_t)((raw - from) * 255 / (to - from));
}

/* At the start of VBLANK, when the board reads its controls (MAME polls them then too). The shifter's two
   positions with their spring: forward is shift up (HIGH), back shift down (LOW); the game holds the gear */
static void on_controls_update(void)
{
	s32_inputs *input = &s32_board.input;
	maple_device_t *device = maple_enum_type(0, MAPLE_FUNC_MIE);
	const mie_state_t *state = device && device->valid ? (const mie_state_t *)maple_dev_status(device) : NULL;
	bool shift_up = false, shift_down = false, start = false, coin = false, service = false, test = false;
	uint8_t wheel = WHEEL_CENTRE, gas = 0, brake = 0;

	if (state)
	{
		const mie_jvs_inputs_t *jvs = &state->jvs;

		wheel = steer(raw_analog(jvs->wheel));
		gas = pedal(raw_analog(jvs->accel), &calibration.gas_min, &calibration.gas_max);
		brake = pedal(raw_analog(jvs->brake), &calibration.brake_min, &calibration.brake_max);
		shift_up = jvs->p1.up;
		shift_down = jvs->p1.down;
		start = jvs->p1.start;
		service = jvs->p1.service || jvs->panel.psw.service;
		test = jvs->system.test || jvs->panel.psw.test;
		/* the coins by the meters' difference: the driver's pulse arrives already cleared (docs/hardware.md) */
		for (int slot = 0; slot < MIE_JVS_COIN_SLOTS; slot++)
		{
			if (coin_counts_known && jvs->coin[slot].count != coin_counts[slot])
				coin_frames = 4;
			coin_counts[slot] = jvs->coin[slot].count;
		}
		coin_counts_known = 1;
	}
	if (orunners_naomi_live.test_inputs & 0x80000000u)
	{
		const uint32_t test = orunners_naomi_live.test_inputs;

		if (test & 1)
		{
			coin_frames = 4;
			orunners_naomi_live.test_inputs &= ~1u;
		}
		start = start || (test & 2);
		shift_up = shift_up || (test & 4);
		shift_down = shift_down || (test & 8);
		if (test >> 8 & 0xff)
			gas = (uint8_t)(test >> 8);
		if (test >> 16 & 0xff)
			wheel = (uint8_t)(test >> 16);
	}
	if (coin_frames > 0)
	{
		coin = true;
		coin_frames--;
	}
	/* the 315-5296s' ports are active low */
	input->port_a[0] = (uint8_t)(0xff & ~(shift_up ? 0x01 : 0) & ~(shift_down ? 0x02 : 0));
	input->port_b[0] = 0xff;
	input->port_e[0] = (uint8_t)(0xff & ~(service ? 0x01 : 0) & ~(test ? 0x02 : 0) & ~(coin ? 0x04 : 0)
		& ~(start ? 0x10 : 0));
	input->port_a[1] = input->port_b[1] = input->port_e[1] = 0xff;
	input->analog[0] = wheel;
	input->analog[1] = gas;
	input->analog[2] = brake;
	input->analog[3] = WHEEL_CENTRE;
	input->analog[6] = input->analog[7] = 0;
	orunners_naomi_live.inputs = (uint32_t)wheel | (uint32_t)gas << 8 | (uint32_t)brake << 16
		| (uint32_t)input->port_e[0] << 24;
}

/* ---- the time each part takes ---- */

static uint64_t frame_started, video_time, sound_time, frame_worst;
static uint64_t second_game, second_sound, second_video, second_texels, second_build, second_wait, second_submit;
static uint32_t missing_seen;
static uint32_t second_frames, second_drawn;

/* The last second's figures, from the peek thread once a second (naomi_dimmcomms.c) */
void naomi_live_second(void)
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
	++orunners_naomi_live.seconds;
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
	naomi_frames_alive++;
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
		naomi_show_log("NO " ASSETS_FILE " (ports/naomi/naomi_assets.py)");
	printf("NAOMI: %s, %ld bytes; %lu bytes of RAM above the heap\n", ASSETS_FILE, size,
		(unsigned long)naomi_heap_left());
	if (!(bytes = malloc((size_t)size)))
		naomi_show_log("NO ROOM FOR THE ASSETS");
	/* in pieces: the NetDIMM's peeks share the cartridge port and wait for each piece (naomi_dimmcomms.c) */
	for (long done = 0; done < size;)
	{
		const size_t piece = size - done > (256 << 10) ? (256 << 10) : (size_t)(size - done);

		if (fread(bytes + done, 1, piece, input) != piece)
			naomi_show_log("THE ASSETS CANNOT BE READ");
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
		naomi_show_log("THE ASSETS ARE NOT THIS BUILD'S");
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
		naomi_show_log("NO THREAD FOR THE GAME");
	naomi_watch_thread(thread);
	thd_join(thread, NULL);
}

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	naomi_boot();
	naomi_dimmcomms_start();    /* (first: the console can be read through the NetDIMM while the assets load) */
#if defined(ORUNNERS_NAOMI_STAGE)
	if (naomi_stop_at == 0)    /* does KallistiOS run, and do the peeks answer? */
		for (;;)
		{
			thd_sleep(1000);
			naomi_frames_alive++;
			orunners_naomi_live.frames_total++;
			printf("NAOMI: stage 0 alive, %lu s, heap left %lu\n", (unsigned long)orunners_naomi_live.frames_total,
				(unsigned long)naomi_heap_left());
		}
#endif
	pvr_video_start();
	load_assets();
	orunners_naomi_live.heap_left = naomi_heap_left();
	printf("NAOMI: %lu bytes of RAM left\n", (unsigned long)orunners_naomi_live.heap_left);
#if defined(ORUNNERS_NAOMI_STAGE)
	if (naomi_stop_at == 1)
	for (;;)
	{
		thd_sleep(1000);
		naomi_frames_alive++;
		orunners_naomi_live.frames_total++;
	}
#endif

	if (!s32_video_allocate())
		naomi_show_log("NO ROOM FOR THE VIDEO");
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
			naomi_frames_alive++;
		}
		printf("NAOMI: poked, the game runs\n");
	}
#endif
	run_game_thread();
	naomi_show_log("THE GAME STOPPED");
}
