/**
 *    Name:        video_scene_soft.c
 *    Description: the scene's quads drawn in software (s32/video_scene_soft.h): the images in RAM, the quads
 *                 by depth, nearest texels at the pixels' centres as the PowerVR2 takes them (blending ones over
 *                 what is drawn under them). For the computers,
 *                 to set a scene's frame against video.c's
 *    Created:     13-11-2026
 *
 *    license:BSD-3-Clause
 */
#include "s32/video_scene_soft.h"

#include <stdlib.h>
#include <string.h>

#define IMAGES 4096

static struct
{
	uint16_t *texels[IMAGES];
	int width[IMAGES], height[IMAGES];
	bool pens[IMAGES];    /* an image of pens (a byte's worth in each texel), coloured by the quad's palette */
	long budget;          /* texels left, as a video memory would have */
} soft;

static int soft_create(void *context, int width, int height)
{
	(void)context;
	if ((long)width * height > soft.budget)
		return 0;
	for (int image = 1; image < IMAGES; image++)
		if (!soft.texels[image])
		{
			soft.texels[image] = (uint16_t *)calloc((size_t)width * (size_t)height, sizeof(uint16_t));
			soft.width[image] = width;
			soft.height[image] = height;
			soft.budget -= (long)width * height;
			return image;
		}
	return 0;
}

/* (an image of pens costs a quarter: 4 bits a texel, as on the PowerVR2) */
static int soft_create_pens(void *context, int width, int height)
{
	int image;
	const long budget = soft.budget;

	soft.budget = soft.budget * 4;
	image = soft_create(context, width, height);
	soft.budget = image ? budget - (long)width * height / 4 : budget;
	if (image)
		soft.pens[image] = true;
	return image;
}

static void soft_write_pens(void *context, int image, int width, int height, const uint8_t *pens)
{
	(void)context;
	for (long index = 0; index < (long)width * height; index++)
		soft.texels[image][index] = pens[index];
}

static void soft_free(void *context, int image)
{
	(void)context;
	if (image > 0 && image < IMAGES && soft.texels[image])
	{
		soft.budget += (long)soft.width[image] * soft.height[image] / (soft.pens[image] ? 4 : 1);
		soft.pens[image] = false;
		free(soft.texels[image]);
		soft.texels[image] = NULL;
	}
}

static void soft_write(void *context, int image, int x, int y, int width, int height, const uint16_t *pixels, int stride)
{
	(void)context;
	for (int row = 0; row < height; row++)
		memcpy(&soft.texels[image][(size_t)(y + row) * (size_t)soft.width[image] + (size_t)x], &pixels[(size_t)row * (size_t)stride],
			(size_t)width * sizeof(uint16_t));
}

static const s32_scene_backend backend = { NULL, soft_create, soft_free, soft_write, NULL, NULL };
static const s32_scene_backend backend_pens = { NULL, soft_create, soft_free, soft_write, soft_create_pens, soft_write_pens };
static bool with_pens;

const s32_scene_backend *s32_scene_soft_backend(long texels)
{
	soft.budget = texels;
	with_pens = !getenv("ORUNNERS_SCENE_NO_PENS");
	return with_pens ? &backend_pens : &backend;
}

static int by_depth(const void *a, const void *b)
{
	const s32_scene_quad *const *first = (const s32_scene_quad *const *)a;
	const s32_scene_quad *const *second = (const s32_scene_quad *const *)b;

	if ((*first)->z != (*second)->z)
		return (*first)->z < (*second)->z ? -1 : 1;
	return *first < *second ? -1 : 1;
}

/* a 5-bit channel after the quad's colour: as video.c's frames hold it (the value << 3) */
static uint32_t channel(int value, int modulate, int add)
{
	value = value * modulate / 255 + (add * 31 + 127) / 255;
	return (uint32_t)(value > 31 ? 31 : value) << 3;
}

void s32_scene_soft_draw(const s32_scene_frame *frame, uint32_t *rgb, int stride)
{
	static const s32_scene_quad *order[4096];
	const int count = frame->count < 4096 ? frame->count : 4096;

	for (int y = 0; y < 224; y++)
		memset(&rgb[(size_t)y * (size_t)stride], 0, 320 * sizeof(uint32_t));
	for (int index = 0; index < count; index++)
		order[index] = &frame->quads[index];
	qsort(order, (size_t)count, sizeof(order[0]), by_depth);
	for (int index = 0; index < count; index++)
	{
		const s32_scene_quad *quad = order[index];
		const float du = (quad->u1 - quad->u0) / (quad->x1 - quad->x0);
		const float dv = (quad->v1 - quad->v0) / (quad->y1 - quad->y0);

		for (int y = (int)quad->y0; y < (int)quad->y1 && y < 224; y++)
			for (int x = (int)quad->x0; x < (int)quad->x1 && x < 320; x++)
			{
				uint16_t texel = quad->colour;

				if (x < 0 || y < 0)
					continue;
				if (quad->image)
				{
					int u = (int)(quad->u0 + ((float)x + 0.5f - quad->x0) * du);
					int v = (int)(quad->v0 + ((float)y + 0.5f - quad->y0) * dv);
					const int width = soft.width[quad->image], height = soft.height[quad->image];

					u = u < 0 ? 0 : (u >= width ? width - 1 : u);
					v = v < 0 ? 0 : (v >= height ? height - 1 : v);
					texel = soft.texels[quad->image][(size_t)v * (size_t)width + (size_t)u];
					if (soft.pens[quad->image])
						texel = quad->palette >= 0 && quad->palette < frame->palette_count
							? frame->palettes[quad->palette][texel & 15] : 0;
				}
				if (!(texel & 0x8000))
					continue;
				{
					uint32_t *at = &rgb[(size_t)y * (size_t)stride + (size_t)x];
					uint32_t value =
						channel(texel >> 10 & 31, (int)(quad->modulate >> 16 & 255), (int)(quad->add >> 16 & 255)) << 16 |
						channel(texel >> 5 & 31, (int)(quad->modulate >> 8 & 255), (int)(quad->add >> 8 & 255)) << 8 |
						channel(texel & 31, (int)(quad->modulate & 255), (int)(quad->add & 255));

					if (quad->weight < 8)
					{
						/* video.c's blend: (top * weight + under * (8 - weight)) / 8, a 5-bit channel each */
						uint32_t mixed = 0;

						for (int shift = 3; shift <= 19; shift += 8)
						{
							const uint32_t top = value >> shift & 31, under = *at >> shift & 31;

							mixed |= ((top * quad->weight + under * (8u - quad->weight)) >> 3) << shift;
						}
						value = mixed;
					}
					*at = value;
				}
			}
	}
	if (frame->subtract)
	{
		/* the fade: each 5-bit channel less the offset, floored at zero (video.c adds a negative offset) */
		int take[3];

		for (int channel = 0; channel < 3; channel++)
			take[channel] = (int)(((frame->subtract >> (16 - 8 * channel) & 255) * 31 + 127) / 255);
		for (int y = 0; y < 224; y++)
			for (int x = 0; x < 320; x++)
			{
				uint32_t *at = &rgb[(size_t)y * (size_t)stride + (size_t)x];
				uint32_t value = 0;

				for (int channel = 0; channel < 3; channel++)
				{
					const int shift = 19 - 8 * channel;
					const int left = (int)(*at >> shift & 31) - take[channel];

					value |= (uint32_t)(left > 0 ? left : 0) << shift;
				}
				*at = value;
			}
	}
}

const uint16_t *s32_scene_soft_image(int image, int *width, int *height)
{
	if (image <= 0 || image >= IMAGES || !soft.texels[image])
		return NULL;
	*width = soft.width[image];
	*height = soft.height[image];
	return soft.texels[image];
}
