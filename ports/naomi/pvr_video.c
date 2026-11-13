/**
 *    Name:        pvr_video.c
 *    Description: the frame on the NAOMI's PowerVR2 (CLX2): the scene's images (s32/video_scene.h) as textures in
 *                 video memory, its quads as punch-through polygons, 320 x 224 doubled to 640 x 448 in a 640 x 480
 *                 screen (KallistiOS's pvr)
 *    Created:     13-11-2026
 *
 *    license:BSD-3-Clause
 *    Images are ARGB1555 textures, not twiddled (the CPU writes their texels as they change: a tile, a row of the
 *    bitmap, a sprite). Bit 15 is the punch-through's alpha: the pens that are not drawn. Depth is the mixer's
 *    priority (higher in front, compared GEQUAL); the background, untextured quads, the backdrop's colour.
 *    Colour offsets: the vertex colour modulates the texels, the offset colour adds.
 *    The quads that blend (the text over the sprites, the sprites' shadows) go in the translucent list, their
 *    weight as the vertices' alpha. A fade's subtraction (the scene's 'subtract') is three translucent quads over
 *    the whole screen after everything else: 1 - d, plus the amount (the blend saturates), 1 - d again.
 */
#include <kos.h>
#include <dc/pvr.h>
#include <arch/timer.h>

#include "s32/video_scene.h"
#include "pvr_video.h"

#include <stdio.h>
#include <string.h>

#define IMAGES 2048
#define TOP    16          /* (480 - 448) / 2 */

static struct
{
	pvr_ptr_t base[IMAGES];
	int width[IMAGES], height[IMAGES];
	size_t free_at_start;
	int images, failed;
} pvr_images;

static int image_create(void *context, int width, int height)
{
	(void)context;
	for (int image = 1; image < IMAGES; image++)
		if (!pvr_images.base[image])
		{
			pvr_ptr_t base;

			/* (leave 64 KB: KallistiOS's own) */
			if (pvr_mem_available() < (size_t)width * (size_t)height * 2 + (64 << 10)
				|| !(base = pvr_mem_malloc((size_t)width * (size_t)height * 2)))
			{
				pvr_images.failed++;
				return 0;
			}
			pvr_images.base[image] = base;
			pvr_images.width[image] = width;
			pvr_images.height[image] = height;
			pvr_images.images++;
			return image;
		}
	return 0;
}

static void image_free(void *context, int image)
{
	(void)context;
	if (image > 0 && image < IMAGES && pvr_images.base[image])
	{
		pvr_mem_free(pvr_images.base[image]);
		pvr_images.base[image] = NULL;
		pvr_images.images--;
	}
}

/* Texels into video memory, 32 bits at a time (it takes no byte writes; x and width are even) */
static void image_write(void *context, int image, int x, int y, int width, int height, const uint16_t *pixels, int stride)
{
	(void)context;
	for (int row = 0; row < height; row++)
	{
		volatile uint32_t *out = (volatile uint32_t *)((uint16_t *)pvr_images.base[image]
			+ (size_t)(y + row) * (size_t)pvr_images.width[image] + (size_t)x);
		const uint16_t *in = &pixels[(size_t)row * (size_t)stride];

		for (int column = 0; column < width; column += 2)
			*out++ = (uint32_t)in[column] | (uint32_t)in[column + 1] << 16;
	}
}

static const s32_scene_backend backend = { NULL, image_create, image_free, image_write };

void pvr_video_start(void)
{
	pvr_init_params_t params = {
		/* opaque, opaque modifiers, translucent, translucent modifiers, punch-through */
		.opb_sizes = { PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16, PVR_BINSIZE_0, PVR_BINSIZE_16 },
		.vertex_buf_size = 512 * 1024,
		.opb_overflow_count = 3,
	};

	pvr_init(&params);
	pvr_set_bg_color(0, 0, 0);
	pvr_images.free_at_start = pvr_mem_available();
	printf("NAOMI: PowerVR2 on, %lu bytes of video memory for textures\n", (unsigned long)pvr_images.free_at_start);
	s32_scene_start(&backend, true);
}

/* A header, straight to the tile accelerator through the store queues (direct rendering) */
static void send_header(const pvr_poly_hdr_t *header)
{
	uint32_t *out = (uint32_t *)pvr_dr_target();
	const uint32_t *in = (const uint32_t *)header;

	for (int index = 0; index < 8; index++)
		out[index] = in[index];
	pvr_dr_commit(out);
}

static void send_vertex(uint32_t flags, float x, float y, float z, float u, float v, uint32_t argb, uint32_t oargb)
{
	pvr_vertex_t *vertex = (pvr_vertex_t *)pvr_dr_target();

	vertex->flags = flags;
	vertex->x = x;
	vertex->y = y;
	vertex->z = z;
	vertex->u = u;
	vertex->v = v;
	vertex->argb = argb;
	vertex->oargb = oargb;
	pvr_dr_commit(vertex);
}

/* One quad as a strip of four vertices, 320 x 224 to the screen's 640 x 448 */
static void quad_vertices(const s32_scene_quad *quad, float width, float height)
{
	const float x0 = quad->x0 * 2, x1 = quad->x1 * 2, y0 = quad->y0 * 2 + TOP, y1 = quad->y1 * 2 + TOP;
	const float u0 = quad->u0 / width, u1 = quad->u1 / width, v0 = quad->v0 / height, v1 = quad->v1 / height;
	const uint32_t argb = (quad->weight >= 8 ? 0xff000000u : (uint32_t)(quad->weight * 255 / 8) << 24) | quad->modulate;

	send_vertex(PVR_CMD_VERTEX, x0, y1, quad->z, u0, v1, argb, quad->add);
	send_vertex(PVR_CMD_VERTEX, x0, y0, quad->z, u0, v0, argb, quad->add);
	send_vertex(PVR_CMD_VERTEX, x1, y1, quad->z, u1, v1, argb, quad->add);
	send_vertex(PVR_CMD_VERTEX_EOL, x1, y0, quad->z, u1, v0, argb, quad->add);
}

void pvr_video_frame(pvr_video_stats *stats)
{
	const uint64_t start = timer_us_gettime64();
	const s32_scene_frame *frame = s32_scene_build();
	const uint64_t built = timer_us_gettime64();
	uint64_t ready;
	pvr_poly_cxt_t context;
	pvr_poly_hdr_t header;
	int last_image = -1;
	bool last_add = false;
	static uint16_t order[4096];
	int ordered;

	if (stats)
	{
		stats->quads = frame->count;
		stats->images_drawn = frame->images_drawn;
		stats->texels_drawn = frame->texels_drawn;
		stats->missing = frame->missing;
		stats->images = pvr_images.images;
		stats->failed = pvr_images.failed;
		stats->video_memory_free = (uint32_t)pvr_mem_available();
	}
	pvr_wait_ready();
	ready = timer_us_gettime64();
	pvr_scene_begin();

	/* the plain colours (the backdrop) in the opaque list, the textured quads punch-through */
	pvr_list_begin(PVR_LIST_OP_POLY);
	pvr_poly_cxt_col(&context, PVR_LIST_OP_POLY);
	context.depth.comparison = PVR_DEPTHCMP_GEQUAL;
	pvr_poly_compile(&header, &context);
	send_header(&header);
	for (int index = 0; index < frame->count; index++)
	{
		const s32_scene_quad *quad = &frame->quads[index];
		s32_scene_quad plain;
		const uint16_t c = quad->colour;
		const uint32_t red = (uint32_t)(c >> 10 & 31) * 255 / 31, green = (uint32_t)(c >> 5 & 31) * 255 / 31,
			blue = (uint32_t)(c & 31) * 255 / 31;

		if (quad->image)
			continue;
		/* (the colour itself times the modulate, plus the add, worked out here) */
		plain = *quad;
		plain.modulate = 0;
		for (int shift = 0; shift <= 16; shift += 8)
		{
			const uint32_t value = (shift == 16 ? red : shift == 8 ? green : blue) * (quad->modulate >> shift & 255) / 255
				+ (quad->add >> shift & 255);

			plain.modulate |= (value > 255 ? 255 : value) << shift;
		}
		plain.add = 0;
		quad_vertices(&plain, 1, 1);
	}
	pvr_list_finish();

	/* textured quads by image, so that a header is sent once an image (and offset colour): the order does not
	   matter, depth decides (quads of a layer do not overlap; sprites have depths of their own) */
	{
		static uint16_t count[IMAGES + 1];
		int at = 0;

		memset(count, 0, sizeof(count));
		for (int index = 0; index < frame->count; index++)
			if (frame->quads[index].image)
				count[frame->quads[index].image]++;
		for (int image = 0; image <= IMAGES; image++)
		{
			const int many = count[image];

			count[image] = (uint16_t)at;
			at += many;
		}
		for (int index = 0; index < frame->count; index++)
			if (frame->quads[index].image)
				order[count[frame->quads[index].image]++] = (uint16_t)index;
		ordered = at;
	}
	pvr_list_begin(PVR_LIST_PT_POLY);
	for (int place = 0; place < ordered; place++)
	{
		const s32_scene_quad *quad = &frame->quads[order[place]];
		const bool add = quad->add != 0;

		if (!quad->image || !pvr_images.base[quad->image] || quad->weight < 8)
			continue;
		if (quad->image != last_image || add != last_add)
		{
			pvr_poly_cxt_txr(&context, PVR_LIST_PT_POLY, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
				pvr_images.width[quad->image], pvr_images.height[quad->image], pvr_images.base[quad->image],
				PVR_FILTER_NONE);
			context.depth.comparison = PVR_DEPTHCMP_GEQUAL;
			context.txr.env = PVR_TXRENV_MODULATE;
			context.gen.specular = add;
			pvr_poly_compile(&header, &context);
			send_header(&header);
			last_image = quad->image;
			last_add = add;
		}
		quad_vertices(quad, (float)pvr_images.width[quad->image], (float)pvr_images.height[quad->image]);
	}
	pvr_list_finish();

	/* the quads that blend, then the fade */
	pvr_list_begin(PVR_LIST_TR_POLY);
	last_image = -1;
	for (int place = 0; place < ordered; place++)
	{
		const s32_scene_quad *quad = &frame->quads[order[place]];

		if (!quad->image || !pvr_images.base[quad->image] || quad->weight >= 8)
			continue;
		if (quad->image != last_image)
		{
			pvr_poly_cxt_txr(&context, PVR_LIST_TR_POLY, PVR_TXRFMT_ARGB1555 | PVR_TXRFMT_NONTWIDDLED,
				pvr_images.width[quad->image], pvr_images.height[quad->image], pvr_images.base[quad->image],
				PVR_FILTER_NONE);
			context.depth.comparison = PVR_DEPTHCMP_GEQUAL;
			context.depth.write = false;
			context.txr.env = PVR_TXRENV_MODULATEALPHA;
			context.gen.specular = true;
			pvr_poly_compile(&header, &context);
			send_header(&header);
			last_image = quad->image;
		}
		quad_vertices(quad, (float)pvr_images.width[quad->image], (float)pvr_images.height[quad->image]);
	}
	if (frame->subtract)
	{
		static const struct { pvr_blend_mode_t src, dst; } passes[3] = {
			{ PVR_BLEND_INVDESTCOLOR, PVR_BLEND_ZERO }, { PVR_BLEND_ONE, PVR_BLEND_ONE }, { PVR_BLEND_INVDESTCOLOR, PVR_BLEND_ZERO }
		};

		for (int pass = 0; pass < 3; pass++)
		{
			s32_scene_quad whole;

			memset(&whole, 0, sizeof(whole));
			whole.x1 = 320;
			whole.y1 = 224;
			whole.z = 1000.0f + (float)pass;    /* (in front of everything, in this order: autosort) */
			whole.weight = 8;
			whole.modulate = pass == 1 ? frame->subtract : 0xffffff;
			pvr_poly_cxt_col(&context, PVR_LIST_TR_POLY);
			context.depth.comparison = PVR_DEPTHCMP_ALWAYS;
			context.depth.write = false;
			context.blend.src = passes[pass].src;
			context.blend.dst = passes[pass].dst;
			pvr_poly_compile(&header, &context);
			send_header(&header);
			quad_vertices(&whole, 1, 1);
		}
	}
	pvr_list_finish();
	pvr_scene_finish();
	if (stats)
	{
		stats->build_us = (uint32_t)(built - start);
		stats->wait_us = (uint32_t)(ready - built);
		stats->submit_us = (uint32_t)(timer_us_gettime64() - ready);
	}
}
