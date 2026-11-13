/**
 *    Name:        aica_model.c
 *    Description: a rough AICA in software, to hear on a computer what runtime/src/multipcm_aica.c tells the
 *                 NAOMI's: ORUNNERS_AICA_WAV=FILE (with --sound-wav OTHER for the MultiPCM's own, to compare;
 *                 ORUNNERS_AICA_OUTPUT=0 or 1 one output only, as the NAOMI plays the left seat's: 1)
 *    Created:     13-11-2026
 *
 *    license:BSD-3-Clause
 *    Included by main.c. Channels play 8-bit samples looping between LSA and LEA at 44100 x 2^OCT x
 *    (1024 + FNS) / 1024, linearly interpolated; the envelope is the MultiPCM's own arithmetic (multipcm.c) on the
 *    AICA's effective rates (2 x R plus the key scale), as the two chips share the scheme; TL 0.375 dB a step,
 *    DISDL 3 dB a step, DIPAN 3 dB a step. No LFO, no filter. Enough to check the registers, not a model of
 *    the chip.
 */
#include "s32/multipcm_aica.h"

#include <math.h>

#define MODEL_CHANNELS 64

static struct
{
	uint16_t regs[MODEL_CHANNELS][0x30 / 4];
	bool playing[MODEL_CHANNELS];
	uint32_t position[MODEL_CHANNELS];    /* 16.16 samples */
	int32_t volume[MODEL_CHANNELS];       /* 10.16, 0x3ff loudest */
	int state[MODEL_CHANNELS];            /* 0 attack, 1 decay 1, 2 decay 2, 3 release */
	uint32_t attack_step[64], decay_step[64];
	int32_t exp_volume[0x400];
	FILE *wav;
	uint32_t wav_bytes;
	multipcm_aica aica;
	uint32_t fraction;                     /* samples owed, 16.16 */
} model;

static uint16_t model_reg(int channel, int reg)
{
	return model.regs[channel][reg / 4];
}

static void model_write(void *context, int channel, int reg, uint16_t value)
{
	(void)context;
	model.regs[channel][reg / 4] = value;
	if (reg == AICA_PLAY_CONTROL && (value & 0x8000))
		for (int at = 0; at < MODEL_CHANNELS; at++)
		{
			const bool on = (model.regs[at][0] & 0x4000) != 0;

			if (on && !model.playing[at])
			{
				model.playing[at] = true;
				model.position[at] = 0;
				model.volume[at] = 0;
				model.state[at] = 0;
			}
			else if (!on && model.playing[at])
				model.state[at] = 3;
		}
}

static uint8_t model_bank(void)
{
	return s32_board.pcm_bank;
}

static int model_rate(int channel, int rate)
{
	const uint16_t envelope = model_reg(channel, AICA_ENVELOPE_2), pitch = model_reg(channel, AICA_PITCH);
	const int scale = (envelope >> 10) & 0xf;
	int octave = (pitch >> 11) & 0xf, effective;

	if (rate == 0)
		return 0;
	if (octave & 8)
		octave -= 16;
	effective = 2 * rate + (scale == 0xf ? 0 : (octave + scale) * 2 + ((pitch >> 9) & 1));
	return effective < 0 ? 0 : (effective > 63 ? 63 : effective);
}

static void model_start(void)
{
	static const double base_times[64] = {
		0, 0, 0, 0, 6222.95, 4978.37, 4148.66, 3556.01, 3111.47, 2489.21, 2074.33, 1778.00, 1555.74, 1244.63,
		1037.19, 889.02, 777.87, 622.31, 518.59, 444.54, 388.93, 311.16, 259.32, 222.27, 194.47, 155.60, 129.66,
		111.16, 97.23, 77.82, 64.85, 55.60, 48.62, 38.91, 32.43, 27.80, 24.31, 19.46, 16.24, 13.92, 12.15, 9.75,
		8.12, 6.98, 6.08, 4.90, 4.08, 3.49, 3.04, 2.49, 2.13, 1.90, 1.72, 1.41, 1.18, 1.04, 0.91, 0.73, 0.59,
		0.50, 0.45, 0.45, 0.45, 0.45
	};
	const multipcm_aica_output out = { NULL, model_write };

	for (int rate = 4; rate < 64; rate++)
	{
		model.attack_step[rate] = (uint32_t)((float)(0x400 << 16) / (float)(base_times[rate] * 44100.0 / 1000.0));
		model.decay_step[rate] = (uint32_t)((float)(0x400 << 16) / (float)(base_times[rate] * 14.32833 * 44100.0 / 1000.0));
	}
	model.attack_step[63] = 0x400 << 16;
	for (int index = 0; index < 0x400; index++)
		model.exp_volume[index] = (int32_t)(4096.0 * pow(10.0, -(96.0 - 96.0 * index / 1024.0) / 20.0));
	multipcm_aica_init(&model.aica, &out, 0, S32_MULTIPCM_CLOCK, model_bank,
		getenv("ORUNNERS_AICA_OUTPUT") ? atoi(getenv("ORUNNERS_AICA_OUTPUT")) : -1);
}

static void model_written(const struct multipcm *chip, int voice, int reg)
{
	multipcm_aica_voice_written(&model.aica, chip, voice, reg);
}

static int32_t model_envelope(int channel)
{
	const uint16_t first = model_reg(channel, AICA_ENVELOPE_1), second = model_reg(channel, AICA_ENVELOPE_2);
	int32_t *volume = &model.volume[channel];

	switch (model.state[channel])
	{
		case 0:
			*volume += (int32_t)(((int64_t)((0x817 << 15) - *volume) * model.attack_step[model_rate(channel, first & 0x1f)]) >> 24);
			if (*volume >= (0x3ff << 16))
			{
				*volume = 0x3ff << 16;
				model.state[channel] = 1;
			}
			break;
		case 1:
			*volume -= (int32_t)model.decay_step[model_rate(channel, (first >> 6) & 0x1f)];
			if (*volume < 0)
				*volume = 0;
			if ((*volume >> (16 + 5)) <= 31 - ((second >> 5) & 0x1f))
				model.state[channel] = 2;
			break;
		case 2:
			*volume -= (int32_t)model.decay_step[model_rate(channel, (first >> 11) & 0x1f)];
			if (*volume < 0)
				*volume = 0;
			break;
		default:
			*volume -= (int32_t)model.decay_step[model_rate(channel, second & 0x1f)];
			if ((second & 0x1f) == 0x1f || *volume <= 0)
			{
				*volume = 0;
				model.playing[channel] = false;
			}
			break;
	}
	return model.exp_volume[*volume >> 16];
}

static void model_render(int16_t *out, int count)
{
	for (int index = 0; index < count; index++)
	{
		double left = 0, right = 0;

		for (int channel = 0; channel < MODEL_CHANNELS; channel++)
		{
			const uint16_t control = model_reg(channel, AICA_PLAY_CONTROL), pitch = model_reg(channel, AICA_PITCH);
			const uint16_t direct = model_reg(channel, AICA_DIRECT), level = model_reg(channel, AICA_LEVEL);
			const uint32_t start = (uint32_t)(control & 0x7f) << 16 | model_reg(channel, AICA_SAMPLE_LOW);
			const uint32_t loop = model_reg(channel, AICA_LOOP_START), end = model_reg(channel, AICA_LOOP_END);
			int octave = (pitch >> 11) & 0xf;
			uint32_t at, next;
			double sample, gain, pan_left = 1, pan_right = 1;

			if (!model.playing[channel])
				continue;
			if (octave & 8)
				octave -= 16;
			at = model.position[channel] >> 16;
			next = at + 1 >= end ? loop : at + 1;
			sample = (int8_t)assets_pcm_byte(start + at) * (1.0 - (model.position[channel] & 0xffff) / 65536.0)
				+ (int8_t)assets_pcm_byte(start + next) * ((model.position[channel] & 0xffff) / 65536.0);
			model.position[channel] += (uint32_t)(65536.0 * pow(2.0, octave) * (1024 + (pitch & 0x3ff)) / 1024.0);
			if (end > loop && (model.position[channel] >> 16) >= end)
				model.position[channel] -= (end - loop) << 16;
			gain = model_envelope(channel) / 4096.0 * pow(10.0, -0.375 * (level >> 8) / 20.0);
			gain *= ((direct >> 8) & 0xf) ? pow(10.0, -3.0 * (15 - ((direct >> 8) & 0xf)) / 20.0) : 0;
			if (direct & 0x10)
				pan_right = (direct & 0xf) == 0xf ? 0 : pow(10.0, -3.0 * (direct & 0xf) / 20.0);
			else
				pan_left = (direct & 0xf) == 0xf ? 0 : pow(10.0, -3.0 * (direct & 0xf) / 20.0);
			left += sample * 256 * gain * pan_left;
			right += sample * 256 * gain * pan_right;
		}
		out[2 * index] = (int16_t)(left > 32767 ? 32767 : (left < -32768 ? -32768 : left));
		out[2 * index + 1] = (int16_t)(right > 32767 ? 32767 : (right < -32768 ? -32768 : right));
	}
}

static void model_frame(const struct multipcm *chip)
{
	int16_t samples[2 * 1024];
	int count;

	multipcm_aica_frame(&model.aica, chip, 16683);
	/* a frame of the board's (59.94 Hz) at 44100 Hz */
	model.fraction += (uint32_t)(65536.0 * 44100.0 / 59.94);
	count = (int)(model.fraction >> 16);
	model.fraction &= 0xffff;
	model_render(samples, count);
	model.wav_bytes += (uint32_t)fwrite(samples, 4, (size_t)count, model.wav) * 4u;
}

static void model_finish(void)
{
	const uint32_t header[11] = { 0x46464952u, 36 + model.wav_bytes, 0x45564157u, 0x20746d66u, 16, 0x00020001u,
		44100, 44100 * 4, 0x00100004u, 0x61746164u, model.wav_bytes };

	fseek(model.wav, 0, SEEK_SET);
	fwrite(header, sizeof(header), 1, model.wav);
	fclose(model.wav);
}

/* ORUNNERS_AICA_WAV=FILE: the MultiPCM's voices through multipcm_aica.c and this model, into FILE */
static void aica_model_attach(void)
{
	static const sound_chips_pcm_player player = { model_written, model_frame };
	const char *path = getenv("ORUNNERS_AICA_WAV");

	if (!path || !(model.wav = fopen(path, "wb")))
		return;
	fseek(model.wav, 44, SEEK_SET);
	model_start();
	sound_chips_pcm_play_on(&player);
	atexit(model_finish);
}
