/**
 *    Name:        multipcm_aica.h
 *    Description: the MultiPCM's voices on a Yamaha AICA (the NAOMI's and Dreamcast's sound chip): its registers
 *                 worked out from the voices' (s32/multipcm.h), for a port that has the chip
 *    Created:     13-11-2026
 *
 *    license:BSD-3-Clause
 *    The MultiPCM (YMW-258-F) is the AICA's ancestor: the same 8-bit samples (signed), the same kind of
 *    envelope (attack, decay 1, decay level, decay 2, release, key rate scale) with rates of twice the bits,
 *    the same level steps (0.375 dB), pan steps (3 dB) and LFO depths (MAME's multipcm.cpp and the AICA's
 *    tables agree). Voice n plays on channel n. The samples are the 4 MB ROM in the AICA's RAM at 'base'; a
 *    voice's start in the MultiPCM's 2 MB of address space is put on the ROM by the banks when it is keyed on
 *    (s32_board.pcm_bank, sound.c's s32_pcm_sample_read).
 *
 *    What differs: the pitch's rate (the MultiPCM plays at its clock / 224, the AICA at 44100 Hz: the octave
 *    and the fraction are worked out again for the same speed, and the key rate scale for the new octave), the
 *    attack's start (the MultiPCM's starts part way up), the level's glides (here once a frame), the LFOs'
 *    frequencies (the AICA's nearest), no effect send (the Multi 32 has no DSP).
 */
#ifndef S32_MULTIPCM_AICA_H
#define S32_MULTIPCM_AICA_H

#include "s32/multipcm.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The AICA's channel registers, as offsets in a channel's 0x80 bytes (16 bits each in 32-bit words) */
#define AICA_PLAY_CONTROL   0x00    /* KYONEX 15, KYONB 14, SSCTL 10, LPCTL 9, PCMS 8-7, SA 22-16 */
#define AICA_SAMPLE_LOW     0x04    /* SA 15-0 */
#define AICA_LOOP_START     0x08
#define AICA_LOOP_END       0x0c
#define AICA_ENVELOPE_1     0x10    /* D2R 15-11, D1R 10-6, AR 4-0 */
#define AICA_ENVELOPE_2     0x14    /* LPSLNK 14, KRS 13-10, DL 9-5, RR 4-0 */
#define AICA_PITCH          0x18    /* OCT 14-11, FNS 9-0 */
#define AICA_LFO            0x1c    /* LFORE 15, LFOF 14-10, PLFOWS 9-8, PLFOS 7-5, ALFOWS 4-3, ALFOS 2-0 */
#define AICA_DSP_SEND       0x20
#define AICA_DIRECT         0x24    /* DISDL 11-8, DIPAN 4-0 */
#define AICA_LEVEL          0x28    /* TL 15-8, VOFF 6, LPOFF 5, Q 4-0 */

typedef struct multipcm_aica_output
{
	void *context;
	/* a channel's register (16 bits); a write of AICA_PLAY_CONTROL with bit 15 keys every channel on or off as
	   its bit 14 says */
	void (*write)(void *context, int channel, int reg, uint16_t value);
} multipcm_aica_output;

typedef struct multipcm_aica
{
	multipcm_aica_output out;
	uint32_t base;                         /* where the ROM is in the AICA's RAM */
	uint32_t rate;                         /* the MultiPCM's samples a second, times 16 (its clock / 14) */
	uint8_t (*bank)(void);                 /* the board's bank register (s32_board.pcm_bank) */
	int output;                            /* -1 both outputs, as the AICA's left and right; 0 or 1 one only, on both */
	struct
	{
		bool keyed;                        /* on, as the AICA has it */
		uint16_t level;                    /* the level now, 7.8 (glides once a frame to the voice's) */
		uint8_t level_written;
		int8_t octave;                     /* the AICA's octave, for the key rate scale */
	} voices[MULTIPCM_VOICES];
} multipcm_aica;

/* Channels 0-27 off and quiet; the ROM is at 'base' in the AICA's RAM; 'clock' the MultiPCM's. 'output': -1
   the MultiPCM's two outputs as the AICA's left and right; 0 or 1 that output only, on both. The Multi 32's
   two outputs are the cabinet's two sides (each seat its sound: sound_chips.c's mix puts the second on the
   left speaker): the left seat, the left screen, is output 1 */
void multipcm_aica_init(multipcm_aica *aica, const multipcm_aica_output *out, uint32_t base, uint32_t clock,
	uint8_t (*bank)(void), int output);

/* A voice register the MultiPCM has just taken (sound_chips_pcm_player's voice_written) */
void multipcm_aica_voice_written(multipcm_aica *aica, const multipcm *chip, int voice, int reg);

/* The end of a frame of 'microseconds': the levels glide */
void multipcm_aica_frame(multipcm_aica *aica, const multipcm *chip, uint32_t microseconds);

/* What the AICA's pitch registers are for a MultiPCM voice's octave (4 bits, signed) and pitch (10 bits), at
   'rate' (its samples a second times 16): OCT << 11 | FNS, and the AICA's octave as a signed number */
uint16_t multipcm_aica_pitch(uint32_t rate, uint8_t octave, uint16_t pitch, int *aica_octave);

#ifdef __cplusplus
}
#endif

#endif
