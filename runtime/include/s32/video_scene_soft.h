/**
 *    Name:        video_scene_soft.h
 *    Description: the scene (s32/video_scene.h) drawn in software, to set against video.c's frames
 *    Created:     13-11-2026
 *
 *    license:BSD-3-Clause
 */
#ifndef S32_VIDEO_SCENE_SOFT_H
#define S32_VIDEO_SCENE_SOFT_H

#include "s32/video_scene.h"

#ifdef __cplusplus
extern "C" {
#endif

/* A backend in RAM with room for 'texels' texels (a video memory's budget) */
const s32_scene_backend *s32_scene_soft_backend(long texels);

/* The frame's quads into 320 x 224 pixels as video.c's frames have them (5-bit channels << 3, 0x00RRGGBB),
   'stride' pixels a row */
void s32_scene_soft_draw(const s32_scene_frame *frame, uint32_t *rgb, int stride);

/* An image's texels (for looking at them), or NULL */
const uint16_t *s32_scene_soft_image(int image, int *width, int *height);

#ifdef __cplusplus
}
#endif

#endif
