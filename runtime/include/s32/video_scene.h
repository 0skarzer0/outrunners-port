/**
 *    Name:        video_scene.h
 *    Description: the frame as textured quads instead of pixels (ORUNNERS_VIDEO_SCENE): for a GPU that draws
 *                 them, the NAOMI's PowerVR2 first (ports/naomi/pvr_video.c)
 *    Created:     13-11-2026
 *
 *    license:BSD-3-Clause
 *    What it covers is what OutRunners uses of the chips (docs/outrunners.md of 0skarzer0/naomi-wheel-games): the
 *    text layer, NBG0 (scroll and zoom), NBG2 (scroll, row scroll, row select), the backdrop, the sprites (zoom,
 *    flips, 8 bits, indirect colours, the list's clip rectangle) and the mixer's priorities and colour offsets.
 *    Blending: the text layer's over the sprites, by 8 x 8 cells over the sprites' rectangles. Shadows: the
 *    sprites' shadow pen (an image of its own, drawn black at half over what is under it).
 *    Not covered (yet): NBG1, NBG3, clip windows, other layers' blending, shadow sprites, the list's outside clip, the
 *    screens' flips, WIDE. Left screen only.
 *
 *    The scene keeps images of 16-bit pixels (ARGB1555: bit 15 opaque, red in 14-10) that a backend holds
 *    (in video memory, say): the tilemap pages and the text map, redrawn where their cells or colours change,
 *    and the sprites' images, one per graphics and colours, kept while they are used. Quads say where an image's
 *    texels go on the 320 x 224 screen, at what depth (higher in front) and with what colour.
 */
#ifndef S32_VIDEO_SCENE_H
#define S32_VIDEO_SCENE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Where the images live. Sizes are powers of two, at most 1024 x 512 */
typedef struct
{
	void *context;
	/* an image of width x height texels, its handle (1 or more), or 0 when there is no room */
	int (*image_create)(void *context, int width, int height);
	void (*image_free)(void *context, int image);
	/* width x height texels at (x, y) of an image, 'stride' texels a row in 'pixels'; width is even */
	void (*image_write)(void *context, int image, int x, int y, int width, int height, const uint16_t *pixels,
		int stride);
} s32_scene_backend;

/* One quad: its screen rectangle (pixels, the left and top edges in, the right and bottom out), the image's
   texels at its corners (u0, v0 at x0, y0; u1, v1 at x1, y1: a flip has them the other way), its depth, and the
   colour it takes: each channel times modulate's (0-255 each, 0x00RRGGBB: 255 keeps it), plus add's (0-255 each,
   0x00RRGGBB). Image 0 is no image: a plain 'colour' (ARGB1555, opaque). Texels whose bit 15 is clear are not
   drawn. 'weight' below 8 blends: the quad's colour times weight / 8 plus what is under it times (8 - weight) / 8
   (the mixer's blending: the text layer over the sprites) */
typedef struct
{
	float x0, y0, x1, y1;
	float u0, v0, u1, v1;
	float z;
	int image;
	uint16_t colour;
	uint32_t modulate;
	uint32_t add;
	uint8_t weight;
} s32_scene_quad;

typedef struct
{
	const s32_scene_quad *quads;
	int count;
	/* taken from every channel of the whole picture at the end, 0-255 each (0x00RRGGBB), floored at zero: the
	   mixer's negative colour offsets when all the layers take the same (fades) */
	uint32_t subtract;
	/* what the frame needed that the scene does not cover, as bits (S32_SCENE_MISSING_), for the log */
	uint32_t missing;
	/* the work done: images (re)drawn, their texels */
	int images_drawn;
	int texels_drawn;
} s32_scene_frame;

#define S32_SCENE_MISSING_NBG1      0x0001u
#define S32_SCENE_MISSING_NBG3      0x0002u
#define S32_SCENE_MISSING_BITMAP    0x0004u
#define S32_SCENE_MISSING_WINDOWS   0x0008u
#define S32_SCENE_MISSING_BLEND     0x0010u
#define S32_SCENE_MISSING_SHADOW    0x0020u
#define S32_SCENE_MISSING_OUTSIDE   0x0040u
#define S32_SCENE_MISSING_FLIP      0x0080u
#define S32_SCENE_MISSING_SHIFT     0x0100u
#define S32_SCENE_MISSING_IMAGES    0x0200u    /* the backend had no room for an image */
#define S32_SCENE_MISSING_FROM_RAM  0x0400u

/* Starts the scene on 'backend' (kept by the caller). With 'only', the video draws no sprites into its own
   buffers any more (s32_video_draw_frame then misses them: the scene alone shows the frame) */
void s32_scene_start(const s32_scene_backend *backend, bool only);

/* The frame the board shows now, as quads (valid until the next call) */
const s32_scene_frame *s32_scene_build(void);

/* For comparisons: the scene built each time video.c draws a frame (at the same moment, from the same state),
   and the last one built so */
void s32_scene_follow_frames(bool follow);
const s32_scene_frame *s32_scene_last(void);

#ifdef __cplusplus
}
#endif

#endif
