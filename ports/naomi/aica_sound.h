/**
 *    Name:        aica_sound.h
 *    Description: the MultiPCM's voices on the NAOMI's AICA (aica_sound.c)
 *    Created:     13-11-2026
 *
 *    license:BSD-3-Clause
 */
#ifndef AICA_SOUND_H
#define AICA_SOUND_H

#include <stdbool.h>
#include <stdint.h>

/* The samples into the AICA's RAM and the MultiPCM's voices on its channels (after sound_chips_attach): false
   when there are no samples (then no sound) */
bool aica_sound_start(void);

/* The channels' register writes so far (the live block) */
uint32_t aica_sound_register_writes(void);

#endif
