/**
 *    Name:        multipcm_aica.c
 *    Description: the MultiPCM's voices on a Yamaha AICA (s32/multipcm_aica.h)
 *    Created:     13-11-2026
 *
 *    license:BSD-3-Clause
 *    The MultiPCM's side follows runtime/src/multipcm.c (MAME's multipcm.cpp); the AICA's registers, the
 *    Dreamcast's documentation as KallistiOS's sound driver (kernel/arch/dreamcast/sound/arm/aica.c) and MAME's
 *    sound/aica.cpp use them.
 */
#include "s32/multipcm_aica.h"

#include <string.h>

#define AICA_RATE        44100u
#define DIRECT_LEVEL     0xc      /* -9 dB: about the MultiPCM's own (its pan table's / 4, -12 dB, against
                                     the reference mix's level: ports/headless/aica_model.c) */

/* The AICA's LFO frequency (LFOF) nearest the MultiPCM's 0.168, 2.019, 3.196, 4.206, 5.215, 5.888, 6.224 and
   7.066 Hz (the AICA's: 0.17 ... 1.87, 2.27, 2.87, 3.31, 3.92, 4.79, 6.15, 7.18 Hz at 14-21) */
static const uint8_t lfo_frequencies[8] = { 0, 14, 17, 18, 19, 20, 20, 21 };

uint16_t multipcm_aica_pitch(uint32_t rate, uint8_t octave, uint16_t pitch, int *aica_octave)
{
	/* the MultiPCM plays rate x (1024 + pitch) / 1024 x 2^e samples a second, e the octave less one as 4 signed
	   bits (multipcm.c's update_step); the AICA 44100 x (1024 + FNS) / 1024 x 2^OCT */
	int exponent = (int)((octave - 1) & 0xf);
	uint32_t value = (uint32_t)(1024 + (pitch & 0x3ff)) * rate / (16u * AICA_RATE);

	if (exponent & 8)
		exponent -= 16;
	while (value >= 2048)
	{
		value >>= 1;
		exponent++;
	}
	while (value < 1024)
	{
		value <<= 1;
		exponent--;
	}
	if (exponent > 7)
	{
		exponent = 7;
		value = 2047;
	}
	if (exponent < -8)
	{
		exponent = -8;
		value = 1024;
	}
	if (aica_octave)
		*aica_octave = exponent;
	return (uint16_t)((uint32_t)(exponent & 0xf) << 11 | (value - 1024));
}

/* A 4-bit rate as the AICA's 5 bits: 0 never, 15 at once, twice the bits between (the effective rates, 4 x the
   bits plus the key's scaling, are the same) */
static uint16_t rate5(uint8_t rate)
{
	return rate == 0 ? 0 : (rate >= 0xf ? 0x1f : (uint16_t)(rate * 2));
}

static void write(multipcm_aica *aica, int channel, int reg, uint16_t value)
{
	aica->out.write(aica->out.context, channel, reg, value);
}

static void write_pitch(multipcm_aica *aica, const multipcm_voice *voice, int channel)
{
	int octave;

	write(aica, channel, AICA_PITCH, multipcm_aica_pitch(aica->rate, voice->octave, voice->pitch, &octave));
	aica->voices[channel].octave = (int8_t)octave;
}

static void write_envelope(multipcm_aica *aica, const multipcm_voice *voice, int channel)
{
	uint16_t scale = voice->key_rate_scale;

	if (scale != 0xf)
	{
		/* the MultiPCM scales by its own octave register, the AICA by OCT: the difference into the scale */
		int octave = voice->octave & 0xf;
		const int aica_octave = aica->voices[channel].octave;
		int scaled;

		if (octave & 8)
			octave -= 16;
		scaled = (int)scale + octave - aica_octave;
		scale = (uint16_t)(scaled < 0 ? 0 : (scaled > 0xe ? 0xe : scaled));
	}
	write(aica, channel, AICA_ENVELOPE_1, (uint16_t)(rate5(voice->decay2_reg) << 11 | rate5(voice->decay1_reg) << 6
		| rate5(voice->attack_reg)));
	write(aica, channel, AICA_ENVELOPE_2, (uint16_t)(scale << 10 | (voice->decay_level_reg & 0xf) * 2 << 5
		| rate5(voice->release_reg)));
}

static void write_lfo(multipcm_aica *aica, const multipcm_voice *voice, int channel)
{
	/* triangles both (the MultiPCM's tables), the same depths' steps */
	write(aica, channel, AICA_LFO, (uint16_t)(lfo_frequencies[voice->lfo_frequency & 7] << 10 | 2 << 8
		| (voice->vibrato & 7) << 5 | 2 << 3 | (voice->tremolo & 7)));
}

static void write_pan(multipcm_aica *aica, const multipcm_voice *voice, int channel)
{
	/* the MultiPCM's pan: 0 both outputs, 1-7 the first down 3 dB a step (7: off), 9-15 the second down by
	   16 - pan, 8 neither. The AICA's DIPAN: 0-15 its left down, 16-31 its right down by its low bits; DISDL
	   3 dB a step. One output only (a side of the cabinet): in the middle, at that output's level */
	const int pan = (int)(voice->pan & 0xf);
	int first = 0, second = 0;    /* each output's attenuation in 3 dB steps, 15 off */
	uint16_t direct;

	if (pan == 8)
		first = second = 15;
	else if (pan & 8)
		second = (16 - pan) == 7 ? 15 : 16 - pan;
	else if (pan != 0)
		first = pan == 7 ? 15 : pan;
	if (aica->output >= 0)
	{
		const int down = aica->output == 0 ? first : second;

		direct = (uint16_t)(down >= DIRECT_LEVEL ? 0 : (DIRECT_LEVEL - down) << 8);
	}
	else if (first == 15 && second == 15)
		direct = 0;
	else if (first)
		direct = (uint16_t)(DIRECT_LEVEL << 8 | first);
	else
		direct = (uint16_t)(DIRECT_LEVEL << 8 | (second ? 0x10 | second : 0));
	write(aica, channel, AICA_DIRECT, direct);
}

static void write_level(multipcm_aica *aica, int channel)
{
	const uint8_t level = (uint8_t)(aica->voices[channel].level >> 8);

	aica->voices[channel].level_written = level;
	/* (LPOFF: no filter) */
	write(aica, channel, AICA_LEVEL, (uint16_t)(level << 8 | 1 << 5));
}

static void key(multipcm_aica *aica, int channel, bool on, uint32_t start)
{
	/* 8-bit samples (PCMS 1), looping (LPCTL) */
	const uint16_t control = (uint16_t)(1 << 9 | 1 << 7 | ((start >> 16) & 0x7f));

	write(aica, channel, AICA_PLAY_CONTROL, (uint16_t)(0x8000 | (on ? 0x4000 : 0) | control));
	aica->voices[channel].keyed = on;
}

void multipcm_aica_init(multipcm_aica *aica, const multipcm_aica_output *out, uint32_t base, uint32_t clock,
	uint8_t (*bank)(void), int output)
{
	memset(aica, 0, sizeof(*aica));
	aica->output = output;
	aica->out = *out;
	aica->base = base;
	aica->rate = clock / 14;    /* (the samples a second, clock / 224, times 16) */
	aica->bank = bank;
	for (int channel = 0; channel < MULTIPCM_VOICES; channel++)
	{
		write(aica, channel, AICA_DIRECT, 0);
		write(aica, channel, AICA_DSP_SEND, 0);
		write(aica, channel, AICA_LEVEL, 0xff00 | 1 << 5);
		key(aica, channel, false, 0);
	}
}

/* The sample's place in the AICA's RAM: the MultiPCM's 2 MB, the top MB in two windows on the banks' pages */
static uint32_t sample_place(const multipcm_aica *aica, uint32_t start)
{
	uint32_t address = start & 0x1fffff;

	if (address >= 0x100000)
	{
		const uint8_t banks = aica->bank ? aica->bank() : 0;
		const uint32_t page = address < 0x180000 ? (banks & 7u) : ((banks >> 3) & 7u);

		address = (page * 0x80000 + (address & 0x7ffff)) & 0x3fffff;
	}
	return aica->base + address;
}

static void key_on(multipcm_aica *aica, const multipcm_voice *voice, int channel)
{
	const uint32_t place = sample_place(aica, voice->start);

	if (aica->voices[channel].keyed)
		key(aica, channel, false, place);    /* (again from the start: the MultiPCM's retrigger) */
	write(aica, channel, AICA_SAMPLE_LOW, (uint16_t)(place & 0xffff));
	write(aica, channel, AICA_LOOP_START, (uint16_t)voice->loop);
	write(aica, channel, AICA_LOOP_END, (uint16_t)voice->end);
	write_pitch(aica, voice, channel);
	write_envelope(aica, voice, channel);
	write_lfo(aica, voice, channel);
	write_pan(aica, voice, channel);
	aica->voices[channel].level = (uint16_t)(voice->dest_total_level << 8);
	write_level(aica, channel);
	key(aica, channel, true, place);
}

void multipcm_aica_voice_written(multipcm_aica *aica, const multipcm *chip, int voice_index, int reg)
{
	const multipcm_voice *voice = &chip->voices[voice_index];
	const int channel = voice_index;

	switch (reg)
	{
		case 0:
			write_pan(aica, voice, channel);
			break;
		case 1:    /* a new sample: playing, it starts again (multipcm.c's retrigger) */
			if (voice->playing)
				key_on(aica, voice, channel);
			break;
		case 2:
		case 3:
			write_pitch(aica, voice, channel);
			if (aica->voices[channel].keyed)
				write_envelope(aica, voice, channel);
			break;
		case 4:
			if (voice->regs[4] & 0x80)
				key_on(aica, voice, channel);
			else if (aica->voices[channel].keyed)
				key(aica, channel, false, sample_place(aica, voice->start));
			break;
		case 5:    /* the level: at once, or glided to (multipcm_aica_frame) */
			if (voice->regs[5] & 1)
			{
				aica->voices[channel].level = (uint16_t)(voice->dest_total_level << 8);
				write_level(aica, channel);
			}
			break;
		case 6:
		case 10:
			write_lfo(aica, voice, channel);
			break;
		case 7:
		case 8:
		case 9:
			write_envelope(aica, voice, channel);
			break;
		default:
			break;
	}
}

void multipcm_aica_frame(multipcm_aica *aica, const multipcm *chip, uint32_t microseconds)
{
	/* the MultiPCM glides 128 levels in 78.2 ms down (louder), in 156.4 ms up: in 7.8 levels a frame */
	const uint32_t down = (128u << 8) * (microseconds > 100000u ? 100000u : microseconds) / 78200u;
	const uint32_t up = down / 2;

	for (int channel = 0; channel < MULTIPCM_VOICES; channel++)
	{
		const uint16_t target = (uint16_t)(chip->voices[channel].dest_total_level << 8);
		uint16_t *level = &aica->voices[channel].level;

		if (*level == target)
			continue;
		if (*level > target)
			*level = (uint16_t)((uint32_t)(*level - target) > down ? *level - down : target);
		else
			*level = (uint16_t)((uint32_t)(target - *level) > up ? *level + up : target);
		if ((uint8_t)(*level >> 8) != aica->voices[channel].level_written)
			write_level(aica, channel);
	}
}
