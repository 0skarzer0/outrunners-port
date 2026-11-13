/**
 *    Name:        aica_sound.c
 *    Description: the MultiPCM's voices on the NAOMI's AICA (runtime/src/multipcm_aica.c): the sample ROM in the
 *                 AICA's RAM, the channels' registers from the SH-4 over the G2 bus
 *    Created:     13-11-2026
 *
 *    license:BSD-3-Clause
 *    KallistiOS's spu_init (at start) leaves the AICA's ARM in a loop of its own and the RAM clear: the channels
 *    play without it. /dimm/orunners.pcm (naomi_assets.py: the MultiPCM's 4 MB as the board sees them) goes to
 *    the AICA's RAM at 1 MB; the left seat's output plays on both speakers.
 */
#include <kos.h>
#include <dc/g2bus.h>
#include <dc/spu.h>

#include "s32/machine.h"
#include "s32/multipcm_aica.h"
#include "sound/chips.h"
#include "aica_sound.h"

#include <stdio.h>
#include <stdlib.h>

#define PCM_FILE   "/dimm/orunners.pcm"
#define PCM_BASE   0x100000u
#define CHANNEL_REGISTER(channel, reg) (0xa0700000u + 0x80u * (uint32_t)(channel) + (uint32_t)(reg))

static multipcm_aica aica;
static volatile uint32_t register_writes;

static void aica_write(void *context, int channel, int reg, uint16_t value)
{
	(void)context;
	g2_fifo_wait();
	g2_write_32(CHANNEL_REGISTER(channel, reg), value);
	register_writes++;
}

static uint8_t board_banks(void)
{
	return s32_board.pcm_bank;
}

static void voice_written(const struct multipcm *chip, int voice, int reg)
{
	multipcm_aica_voice_written(&aica, chip, voice, reg);
}

static void frame_ended(const struct multipcm *chip)
{
	multipcm_aica_frame(&aica, chip, 16683);    /* (the board's 59.94 Hz) */
}

uint32_t aica_sound_register_writes(void)
{
	return register_writes;
}

bool aica_sound_start(void)
{
	static const multipcm_aica_output out = { NULL, aica_write };
	static const sound_chips_pcm_player player = { voice_written, frame_ended };
	static uint8_t piece[64 << 10] __attribute__((aligned(32)));
	FILE *input = fopen(PCM_FILE, "rb");
	const uint64_t start = timer_ms_gettime64();
	uint32_t done = 0;

	if (!input)
	{
		printf("NAOMI: no %s, no sound (ports/naomi/naomi_assets.py)\n", PCM_FILE);
		return false;
	}
	/* the samples into the AICA's RAM, a piece at a time (store queues) */
	for (;;)
	{
		const size_t got = fread(piece, 1, sizeof(piece), input);

		if (got == 0)
			break;
		spu_memload_sq(PCM_BASE + done, piece, (got + 31) & ~(size_t)31);
		done += (uint32_t)got;
	}
	fclose(input);
	printf("NAOMI: %lu bytes of samples in the AICA's RAM at 0x%x, %lu ms\n", (unsigned long)done, PCM_BASE,
		(unsigned long)(timer_ms_gettime64() - start));
	if (done < S32_PCM_ROM_SIZE)
		return false;
	spu_master_mixer(15, 1);
	multipcm_aica_init(&aica, &out, PCM_BASE, S32_MULTIPCM_CLOCK, board_banks, 1);
	sound_chips_pcm_play_on(&player);
	return true;
}
