/**
 *    Name:        pvr_video.h
 *    Description: the frame on the NAOMI's PowerVR2 (pvr_video.c)
 *    Created:     13-11-2026
 *
 *    license:BSD-3-Clause
 */
#ifndef ORUNNERS_PVR_VIDEO_H
#define ORUNNERS_PVR_VIDEO_H

#include <stdint.h>

typedef struct
{
	int quads;
	int images_drawn;          /* this frame: tiles, bitmap rows and sprites (re)made */
	int texels_drawn;
	uint32_t missing;          /* S32_SCENE_MISSING_ bits */
	int images;                /* textures held */
	int failed;                /* times there was no room for one (all along) */
	uint32_t video_memory_free;
	uint32_t build_us;         /* the scene worked out (s32_scene_build) */
	uint32_t wait_us;          /* waiting for the PowerVR2 to take a new scene */
	uint32_t submit_us;        /* the quads handed to it */
	uint32_t render_us;        /* the PowerVR2's render of the frame before */
	int pt_quads, tr_quads;
	uint32_t pt_pixels, tr_pixels;    /* their area on the 320 x 224 screen */
} pvr_video_stats;

/* The PowerVR2 on, the scene on it (video.c then draws no sprites of its own) */
void pvr_video_start(void);

/* The board's frame now, drawn (waits for the PowerVR2 to take a new scene) */
void pvr_video_frame(pvr_video_stats *stats);

#endif
