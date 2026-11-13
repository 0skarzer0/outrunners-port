/**
 *    Name:        video_scene.c
 *    Description: the frame as textured quads (s32/video_scene.h): included at the end of video.c with
 *                 ORUNNERS_VIDEO_SCENE, for its registers' readers and its tiles
 *    Created:     13-11-2026
 *
 *    license:BSD-3-Clause
 *    The same reading of the chips as video.c's (which follows MAME's segas32_v.cpp, Aaron Giles): where video.c
 *    works out every pixel, this works out the rectangles of texels that make it, for a GPU to draw.
 *
 *    - Tilemap pages (512 x 256) and the text map (512 x 256 of 8 x 8 characters) are images, a cell redrawn when
 *      its word, its colours or (for the text) its character change.
 *    - NBG0: its scroll and zoom are linear, so its screen splits into the rectangles where the source stays in
 *      one page. NBG2: line by line (row scroll, row select), the lines that go on from each other joined.
 *    - The sprites' list is read when the board draws it, as video.c does (into the buffer the next frame shows);
 *      each sprite is a quad of an image of its graphics in its colours, made once and kept while used.
 *    - Depth: the mixer's priorities (video.c's mix), the sprites above the layers of the same priority, a later
 *      sprite above an earlier one. Colour offsets as each layer's modulate and add.
 */

#include "s32/video_scene.h"

#include <stdio.h>

#define SCENE_QUADS        2048
#define SCENE_SPRITES      512
#define SCENE_PAGES        16        /* page images kept: 4 for NBG0 and 4 for NBG2 at most, and the text map */
#define SCENE_SPRITE_SLOTS 2048      /* sprite images known (a power of two) */
#define BANKS              (S32_PALETTE_ENTRIES / 16)

/* A sprite of the list as the board drew it */
typedef struct scene_sprite
{
	int x0, y0, x1, y1;              /* on the screen, after the clip (x1, y1 out) */
	float u0, v0, u1, v1;            /* the image's texels there */
	uint32_t place;                  /* bank << 20 | the graphics' first word */
	uint16_t words;                  /* source width, in 32-bit words */
	uint16_t rows;                   /* source height */
	bool bits8;
	bool indirect;
	bool opaque;                     /* no end codes (flags bit 8) */
	uint16_t colour;                 /* 0x8000 | the colour word's bits (direct colours) */
	uint16_t transparent_bits;
	uint16_t table[16];
} scene_sprite;

typedef struct scene_page
{
	int image;
	bool text;                       /* the text map, not a tilemap page */
	int page;                        /* 0-127, or the text map's page */
	int bank;                        /* tile bank (text: character bank) */
	uint16_t palette_base;
	uint8_t shift;
	uint32_t used;                   /* the frame it was last used */
	uint16_t cells[2048];            /* their words when drawn (512 for a tilemap page) */
	uint32_t versions[2048];         /* their colours' version when drawn */
	bool drawn;
	uint32_t generation;             /* the palette's when its cells were last checked */
	uint16_t raw[2048] __attribute__((aligned(4)));    /* the cells' words in video RAM as last checked */
	uint64_t row_cells[256];         /* each pixel row of the image: the cells (columns of 16 or 8) with a pixel
	                                    drawn in it, to leave out quads with nothing to show */
} scene_page;

typedef struct scene_slot
{
	uint32_t hash;                   /* 0: free */
	uint32_t place;
	uint16_t words, rows;
	uint16_t colour, transparent_bits;
	uint16_t table[16];
	uint16_t palette_base;
	uint8_t shift, flags;            /* flags: bits8 1, indirect 2, opaque 4 */
	uint32_t version;                /* of the colours it was drawn with */
	int image;
	int width, height;               /* the image's (powers of two) */
	int shadow_image;                /* its shadow pen's texels (opaque black), 0 if it has none */
	uint32_t used;
} scene_slot;

/* A layer's (or a sprite group's) place in the mix */
typedef struct scene_layer
{
	bool shown;
	float z;
	uint16_t palette_base;
	uint8_t shift;
	uint32_t modulate;
	uint32_t add;
} scene_layer;

static struct
{
	const s32_scene_backend *backend;
	bool only;
	bool follow;                     /* built when video.c draws a frame (s32_scene_follow_frames) */
	const s32_scene_frame *last;
	uint32_t frame;
	s32_scene_frame out;
	s32_scene_quad quads[SCENE_QUADS];
	scene_sprite sprites[2][SCENE_SPRITES];
	int sprite_count[2];
	int shown;                       /* the sprite list shown, as video.c's shown_sprites */
	uint16_t palette[S32_PALETTE_ENTRIES] __attribute__((aligned(4)));
	uint32_t bank_version[BANKS];
	uint32_t palette_generation;     /* counted up on every frame some colour changed */
	scene_page pages[SCENE_PAGES];
	scene_slot slots[SCENE_SPRITE_SLOTS];
	uint8_t characters[512 * 32];    /* the text bank's graphics when last seen, in their order */
	uint8_t characters_raw[512 * 32] __attribute__((aligned(4)));    /* ... as they lie in video RAM */
	bool characters_changed;
	uint8_t character_changed[512];
	int character_bank;
	uint16_t pixels[512 * 256];      /* an image as it is made */
	uint16_t shadow_pixels[512 * 256];    /* ... a sprite's shadow image */
	uint32_t missing;
	struct
	{
		int image;
		bool bits8;
		uint16_t palette_base;
		uint8_t shift;
		int colour;
		uint32_t version;
		uint32_t stamp;
		uint32_t row_stamp[512];
		bool drawn;
		uint8_t copy[0x20000] __attribute__((aligned(4)));    /* the bitmap's 128 KB of video RAM when last drawn */
	} bitmap;
	scene_layer layers[LAYERS];
	scene_layer groups[16];
	/* the text layer's blending (mixer 0x4E, 0x30): over which sprite groups, its weight in eighths */
	bool text_blends;
	uint16_t text_sprite_groups;
	uint8_t text_weight;
	scene_page *text_entry;
	int text_quad;    /* its quad, -1 none */
	int group_shift, group_mask, sprite_bits;
} scene;

/* Whether two blocks are the same, a 32-bit word at a time (both 4-byte aligned, 'bytes' a multiple of 16):
   newlib's memcmp goes a byte at a time on the SH-4, and the scene compares some 100 KB a frame */
static bool same(const void *first, const void *second, size_t bytes)
{
	const uint32_t *a = (const uint32_t *)first, *b = (const uint32_t *)second;

	if (((uintptr_t)first | (uintptr_t)second) & 3)
		return memcmp(first, second, bytes) == 0;
	for (size_t words = bytes / 4; words; words -= 4, a += 4, b += 4)
		if ((a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2]) | (a[3] ^ b[3]))
			return false;
	return true;
}

static uint16_t argb1555(uint16_t colour)
{
	return (uint16_t)(0x8000 | (colour & 0x1f) << 10 | (colour & 0x3e0) | (colour >> 10 & 0x1f));
}

static uint16_t palette_index(uint16_t palette_base, int shift, int pixel)
{
	return (uint16_t)((palette_base + ((pixel >> shift) & 0xfff0) + (pixel & 0x0f)) & 0x3fff);
}

/* ---- the palette: a version for each bank of 16 colours, counted up when one of them changes ---- */

static void palette_update(void)
{
	const uint16_t *palette = s32_board.palette[0];

	if (same(scene.palette, palette, sizeof(scene.palette)))
		return;    /* (most frames: one comparison) */
	scene.palette_generation++;
	for (int bank = 0; bank < BANKS; bank++)
		if (!same(&scene.palette[bank * 16], &palette[bank * 16], 32))
		{
			memcpy(&scene.palette[bank * 16], &palette[bank * 16], 32);
			scene.bank_version[bank]++;
		}
}

/* ---- the mix: depths and colours of the layers and the sprite groups (video.c's mix) ---- */

/* A layer's colour offsets (0x40-0x44 or 0x46-0x4A, red green blue, -32 to 31 a channel) as a quad's colour:
   a positive offset adds; a negative one, when every layer takes the same offsets (a fade), is the frame's
   'subtract' (exact: the backend takes it from the whole picture), otherwise it scales the channel down
   ((31 + offset) / 31: the same at the ends, darker in between than taking it away) */
static void colour_of(scene_layer *layer, int offsets, bool uniform)
{
	layer->modulate = 0xffffff;
	layer->add = 0;
	if (offsets >= 2)
		return;
	for (int channel = 0; channel < 3; channel++)
	{
		const int offset = sign_extend(mixer_register(0, (offsets ? 0x46u : 0x40u) + 2 * (uint32_t)channel), 6);
		const int shift = 16 - 8 * channel;    /* red, green, blue: 0x00RRGGBB */

		if (offset < 0 && uniform)
			scene.out.subtract |= (uint32_t)(offset <= -31 ? 255 : 255 * -offset / 31) << shift;
		else if (offset < 0)
			layer->modulate = (layer->modulate & ~(0xffu << shift)) | (uint32_t)(offset <= -31 ? 0 : 255 * (31 + offset) / 31) << shift;
		else if (offset > 0)
			layer->add |= (uint32_t)(offset >= 31 ? 255 : 255 * offset / 31) << shift;
	}
}

static void mix_update(int drawn)
{
	const uint16_t sprite_control = mixer_register(0, 0x4c);
	const sprite_grouping grouping = groupings[sprite_control & 0x0f];

	scene.text_blends = false;
	if (bit(mixer_register(0, 0x4e), 11))
		for (int index = LAYER_TEXT; index <= LAYER_BITMAP; index++)
		{
			const uint16_t blend = mixer_register(0, 0x30 + 2 * (uint32_t)index);
			const int with = (blend >> 6) & 0xff;

			if (with == 0 || !bit((uint32_t)drawn, index))
				continue;
			/* the text over the sprites, all OutRunners does (docs/outrunners.md); anything else is not drawn */
			if (index == LAYER_TEXT && with == 1 << LAYER_SPRITES)
			{
				scene.text_blends = true;
				scene.text_sprite_groups = sprite_blend(blend & 0x3f);
				scene.text_weight = (uint8_t)(7 - ((mixer_register(0, 0x4e) >> 8) & 7));
			}
			else
				scene.missing |= S32_SCENE_MISSING_BLEND;
		}
	/* (0x4C bit 2 darkens the sprite pixels whose bit 15 is clear: only the shadow sprites' (left out) and
	   indirect colours without 0x8000, which OutRunners does not use) */
	{
		/* the offsets each layer takes (0, 1, 2 none): the same for all, a fade, is taken away exactly */
		int offsets[LAYERS + 1];    /* (the sprites' last) */
		bool uniform = true;

		for (int index = LAYER_TEXT; index <= LAYER_BITMAP; index++)
		{
			const uint16_t control = mixer_register(0, 0x20 + 2 * (uint32_t)index);
			scene_layer *layer = &scene.layers[index];

			layer->shown = bit((uint32_t)drawn, index) && (control & 0x0f) != 0;
			layer->z = (float)((control & 0x0f) << 3 | (6 - index));
			layer->palette_base = (uint16_t)((control & 0x00f0) << 6);
			layer->shift = (control >> 8) & 3;
			offsets[index] = color_offsets(0, bit(mixer_register(0, 0x3e), index),
				bit(mixer_register(0, 0x30 + 2 * (uint32_t)index), 14));
		}
		{
			scene_layer *backdrop = &scene.layers[LAYER_BACKDROP];

			backdrop->shown = true;
			backdrop->z = 1 << 3;
			backdrop->palette_base = (uint16_t)((mixer_register(0, 0x2c) & 0x00f0) << 6);
			backdrop->shift = (mixer_register(0, 0x2c) >> 8) & 3;
			offsets[LAYER_BACKDROP] = color_offsets(0, bit(mixer_register(0, 0x3e), 8), bit(mixer_register(0, 0x3e), 14));
		}
		scene.group_shift = grouping.shift;
		scene.group_mask = grouping.mask;
		scene.sprite_bits = ((1 << grouping.shift) - 1) & 0x3fff;
		for (int group = 0; group <= grouping.mask; group++)
		{
			const uint16_t control = mixer_register(0, 2 * (uint32_t)(grouping.base | group));
			scene_layer *sprites = &scene.groups[group];

			sprites->shown = true;
			sprites->z = (float)((control & 0x0f) << 3 | 7);
			sprites->palette_base = (uint16_t)((((sprite_control & 3) != 3) ? control : sprite_control) & 0x00f0) << 6;
			sprites->shift = (control >> 8) & 3;
		}
		offsets[LAYERS] = color_offsets(0, bit(mixer_register(0, 0x3e), 6), bit(sprite_control, 15));
		/* (all the shown layers, the sprites and the backdrop the same) */
		for (int index = LAYER_TEXT; index <= LAYER_BITMAP; index++)
			if (scene.layers[index].shown && offsets[index] != offsets[LAYER_BACKDROP])
				uniform = false;
		if (offsets[LAYERS] != offsets[LAYER_BACKDROP])
			uniform = false;
		scene.out.subtract = 0;
		for (int index = LAYER_TEXT; index <= LAYER_BACKDROP; index++)
			if (index != LAYER_SPRITES)
				colour_of(&scene.layers[index], offsets[index], uniform);
		for (int group = 0; group <= grouping.mask; group++)
			colour_of(&scene.groups[group], offsets[LAYERS], uniform);
	}
}

/* ---- quads ---- */

static s32_scene_quad *new_quad(const scene_layer *layer, int image)
{
	s32_scene_quad *quad;

	if (scene.out.count >= SCENE_QUADS)
		return NULL;
	quad = &scene.quads[scene.out.count++];
	quad->z = layer->z;
	quad->image = image;
	quad->colour = 0;
	quad->modulate = layer->modulate;
	quad->add = layer->add;
	quad->weight = 8;
	return quad;
}

/* ---- room for images: the sprites' used longest ago let go ---- */

static scene_slot *oldest_sprite_image(const scene_slot *keep)
{
	scene_slot *oldest = NULL;

	for (int index = 0; index < SCENE_SPRITE_SLOTS; index++)
	{
		scene_slot *at = &scene.slots[index];

		if (at->image && at != keep && at->used != scene.frame && (!oldest || at->used < oldest->used))
			oldest = at;
	}
	return oldest;
}

/* An image, letting old sprite images go while there is no room; 0 if there is none even so */
static int image_with_room(int width, int height, const scene_slot *keep)
{
	int image;

	while (!(image = scene.backend->image_create(scene.backend->context, width, height)))
	{
		scene_slot *oldest = oldest_sprite_image(keep);

		if (!oldest)
		{
			scene.missing |= S32_SCENE_MISSING_IMAGES;
			return 0;
		}
		scene.backend->image_free(scene.backend->context, oldest->image);
		oldest->image = 0;
		if (oldest->shadow_image)
		{
			scene.backend->image_free(scene.backend->context, oldest->shadow_image);
			oldest->shadow_image = 0;
		}
	}
	return image;
}

/* ---- tilemap pages and the text map as images ---- */

static scene_page *page_of(bool text, int page, int bank, const scene_layer *layer)
{
	scene_page *choice = NULL;

	for (int index = 0; index < SCENE_PAGES; index++)
	{
		scene_page *entry = &scene.pages[index];

		if (entry->image && entry->text == text && entry->page == page && entry->bank == bank
			&& entry->palette_base == layer->palette_base && entry->shift == layer->shift)
		{
			entry->used = scene.frame;
			return entry;
		}
	}
	/* a free entry, else the one used longest ago (not in this frame) */
	for (int index = 0; index < SCENE_PAGES; index++)
	{
		scene_page *entry = &scene.pages[index];

		if (!entry->image)
		{
			choice = entry;
			break;
		}
		if (entry->used != scene.frame && (!choice || entry->used < choice->used))
			choice = entry;
	}
	if (!choice)
		return NULL;
	if (!choice->image && !(choice->image = image_with_room(512, 256, NULL)))
		return NULL;
	choice->text = text;
	choice->page = page;
	choice->bank = bank;
	choice->palette_base = layer->palette_base;
	choice->shift = layer->shift;
	choice->used = scene.frame;
	choice->drawn = false;
	memset(choice->row_cells, 0, sizeof(choice->row_cells));
	return choice;
}

/* A tilemap page's cells that changed, redrawn in its image */
static void page_update(scene_page *entry)
{
	const uint16_t *now = (const uint16_t *)&s32_board.video_ram[(size_t)entry->page << 10];

	/* (nothing to do when neither its cells nor any colour changed: one comparison of 1 KB) */
	if (entry->drawn && entry->generation == scene.palette_generation && same(entry->raw, now, 1024))
		return;
	entry->generation = scene.palette_generation;
	memcpy(entry->raw, now, 1024);
	for (uint32_t cell = 0; cell < 512; cell++)
	{
		const uint16_t word = video_word((uint32_t)entry->page << 9 | cell);
		const int colour = (word >> 4) & 0x1ff;
		const uint32_t version = scene.bank_version[palette_index(entry->palette_base, entry->shift, colour << 4) >> 4];
		uint16_t pixels[256];

		if (entry->drawn && entry->cells[cell] == word && entry->versions[cell] == version)
			continue;
		entry->cells[cell] = word;
		entry->versions[cell] = version;
		{
			/* the cell's 16 colours once, then its pens through them, a row at a time */
			const uint32_t tile = ((uint32_t)entry->bank << 13 | (word & 0x1fff)) % video.graphics.tile_count;
			const uint8_t *pens = video.graphics.tile_pens[tile];
			uint16_t colours[16];

			colours[0] = 0;
			for (int pen = 1; pen < 16; pen++)
				colours[pen] = argb1555(scene.palette[palette_index(entry->palette_base, entry->shift, colour << 4 | pen)]);
			for (int y = 0; y < 16; y++)
			{
				const int line = (word & 0x8000) ? 15 - y : y;
				uint16_t *out = &pixels[y * 16];
				uint16_t any = 0;

				for (int x = 0; x < 16; x++)
				{
					const int column = (word & 0x4000) ? 15 - x : x;
#ifdef ORUNNERS_PACKED_PENS
					const int pen = (pens[(line * 16 + column) >> 1] >> ((column & 1) << 2)) & 15;
#else
					const int pen = pens[line * 16 + column];
#endif
					out[x] = colours[pen];
					any |= out[x];
				}
				if (any)
					entry->row_cells[(cell >> 5) * 16 + (uint32_t)y] |= 1ull << (cell & 31);
				else
					entry->row_cells[(cell >> 5) * 16 + (uint32_t)y] &= ~(1ull << (cell & 31));
			}
		}
		scene.backend->image_write(scene.backend->context, entry->image, (int)(cell & 31) * 16, (int)(cell >> 5) * 16,
			16, 16, pixels, 16);
		scene.out.images_drawn++;
		scene.out.texels_drawn += 256;
	}
	entry->drawn = true;
}

/* The text map's cells that changed, or whose character did, redrawn */
static void text_update(scene_page *entry)
{
	const uint16_t control = video_register(0x5c);
	const int bank = control & 7;

	/* the characters: 512 of 32 bytes from bank * 512, compared as they lie in video RAM (16 KB) */
	{
		const uint8_t *raw = &s32_board.video_ram[(size_t)bank << 14];

		memset(scene.character_changed, 0, sizeof(scene.character_changed));
		scene.characters_changed = scene.character_bank != bank || !same(scene.characters_raw, raw, sizeof(scene.characters_raw));
		if (scene.characters_changed)
			for (int character = 0; character < 512; character++)
				if (scene.character_bank != bank || !same(&scene.characters_raw[character * 32], &raw[character * 32], 32))
				{
					memcpy(&scene.characters_raw[character * 32], &raw[character * 32], 32);
					for (int index = 0; index < 32; index++)
						scene.characters[character * 32 + index] = video_byte(((uint32_t)bank << 9 | (uint32_t)character) * 32 + (uint32_t)index);
					scene.character_changed[character] = 1;
				}
	}
	scene.character_bank = bank;
	{
		const uint16_t *now = (const uint16_t *)&s32_board.video_ram[(size_t)entry->page << 12];

		if (entry->drawn && !scene.characters_changed && entry->generation == scene.palette_generation
			&& same(entry->raw, now, 4096))
			return;
		entry->generation = scene.palette_generation;
		memcpy(entry->raw, now, 4096);
	}
	for (uint32_t cell = 0; cell < 2048; cell++)
	{
		const uint16_t word = video_word((uint32_t)entry->page << 11 | cell);
		const int colour = (word >> 9) & 0x7f;
		const int character = word & 0x1ff;
		const uint32_t version = scene.bank_version[palette_index(entry->palette_base, entry->shift, colour << 4) >> 4];
		uint16_t pixels[64];

		if (entry->drawn && entry->cells[cell] == word && entry->versions[cell] == version
			&& !scene.character_changed[character])
			continue;
		entry->cells[cell] = word;
		entry->versions[cell] = version;
		for (int y = 0; y < 8; y++)
			for (int x = 0; x < 8; x++)
			{
				const uint8_t value = scene.characters[character * 32 + y * 4 + x / 2];
				const int pen = (x & 1) ? (value & 0x0f) : (value >> 4);

				pixels[y * 8 + x] = pen ? argb1555(scene.palette[palette_index(entry->palette_base, entry->shift,
					colour << 4 | pen)]) : 0;
			}
		for (int y = 0; y < 8; y++)
		{
			bool any = false;

			for (int x = 0; x < 8; x++)
				any = any || pixels[y * 8 + x] != 0;
			if (any)
				entry->row_cells[(cell >> 6) * 8 + (uint32_t)y] |= 1ull << (cell & 63);
			else
				entry->row_cells[(cell >> 6) * 8 + (uint32_t)y] &= ~(1ull << (cell & 63));
		}
		scene.backend->image_write(scene.backend->context, entry->image, (int)(cell & 63) * 8, (int)(cell >> 6) * 8,
			8, 8, pixels, 8);
		scene.out.images_drawn++;
		scene.out.texels_drawn += 64;
	}
	entry->drawn = true;
}

/* A quad of a page's texels: screen x0-x1, y0-y1, its source from (u0, v0) by (du, dv) a pixel */
static void page_quad(const scene_layer *layer, scene_page *entry, float x0, float y0, float x1, float y1,
	float u0, float v0, float du, float dv)
{
	s32_scene_quad *quad;
	const float u1 = u0 + (x1 - x0) * du, v1 = v0 + (y1 - y0) * dv;

	if (!entry)
		return;
	{
		/* nothing drawn in the texels it covers: no quad */
		const int cell_width = entry->text ? 8 : 16;
		const int first_row = (int)v0 < 0 ? 0 : (int)v0, last_row = (int)v1 > 255 ? 255 : (int)v1;
		const int first_cell = ((int)u0 < 0 ? 0 : (int)u0) / cell_width;
		const int last_cell = ((int)u1 > 511 ? 511 : (int)u1) / cell_width;
		const uint64_t cells = (last_cell >= 63 ? ~0ull : (2ull << last_cell) - 1) & ~((1ull << first_cell) - 1);
		uint64_t drawn = 0;

		for (int row = first_row; row <= last_row && !(drawn & cells); row++)
			drawn |= entry->row_cells[row];
		if (!(drawn & cells))
			return;
	}
	if (!(quad = new_quad(layer, entry->image)))
		return;
	quad->x0 = x0;
	quad->y0 = y0;
	quad->x1 = x1;
	quad->y1 = y1;
	quad->u0 = u0;
	quad->v0 = v0;
	quad->u1 = u1;
	quad->v1 = v1;
}

/* NBG0 or NBG1: video.c's draw_zoomed, as rectangles. Source columns in 12.20 (bit 29: which page across),
   rows too (bit 28: which page down) */
static void zoomed_quads(int number)
{
	const uint32_t scroll = 0x10 + 8 * (uint32_t)number;
	const uint32_t center = 0x30 + 4 * (uint32_t)number;
	const scene_layer *layer = &scene.layers[LAYER_NBG0 + number];
	int pages[4];
	const int bank = pages_of(number, pages);
	bool flip_x, flip_y;
	int x_zoom = video_register(0x50 + 4 * (uint32_t)number) & 0xfff;
	int y_zoom = bit(video_register(0x00), 14) ? video_register(0x52 + 4 * (uint32_t)number) & 0xfff : x_zoom;
	uint32_t x_step, y_step, x_start, source_y;
	scene_page *entries[4];

	if (bit(video_register(0x02), 11 + number))
		scene.missing |= S32_SCENE_MISSING_WINDOWS;
	flips_of(number, &flip_x, &flip_y);
	if (flip_x || flip_y)
	{
		scene.missing |= S32_SCENE_MISSING_FLIP;
		return;
	}
	if (x_zoom < 0x80)
		x_zoom = 0x80;
	if (y_zoom < 0x80)
		y_zoom = 0x80;
	x_step = (0x200u << 20) / (uint32_t)x_zoom;
	y_step = (0x200u << 20) / (uint32_t)y_zoom;
	x_start = (uint32_t)(video_register(scroll + 2) & 0x3ff) << 20;
	x_start += (uint32_t)(video_register(scroll) & 0xff00) << 4;
	source_y = (uint32_t)(video_register(scroll + 6) & 0x1ff) << 20;
	source_y += (uint32_t)(video_register(scroll + 4) & 0xfe00) << 4;
	x_start -= (uint32_t)sign_extend(video_register(center), x_zoom != 0x200 ? 10 : 9) * x_step;
	source_y -= (uint32_t)sign_extend(video_register(center + 2), y_zoom != 0x200 ? 10 : 9) * y_step;

	for (int index = 0; index < 4; index++)
	{
		bool again = false;

		for (int earlier = 0; earlier < index; earlier++)
			if (pages[earlier] == pages[index])
			{
				entries[index] = entries[earlier];
				again = true;
			}
		if (!again && (entries[index] = page_of(false, pages[index], bank, layer)) != NULL)
			page_update(entries[index]);
	}

	/* rows: where the source crosses into the other half (256 rows) */
	for (int y = 0; y < HEIGHT;)
	{
		const uint32_t row = source_y + (uint32_t)y * y_step;
		const uint32_t to_edge = (1u << 28) - (row & ((1u << 28) - 1));
		int rows = (int)((to_edge + y_step - 1) / y_step);
		const int half = (row >> 27) & 2;

		if (rows > HEIGHT - y)
			rows = HEIGHT - y;
		for (int x = 0; x < S32_VIDEO_MAX_WIDTH && x < 320;)
		{
			const uint32_t column = x_start + (uint32_t)x * x_step;
			const uint32_t to_side = (1u << 29) - (column & ((1u << 29) - 1));
			int columns = (int)((to_side + x_step - 1) / x_step);

			if (columns > 320 - x)
				columns = 320 - x;
			/* (pixel k takes column (start + k * step) >> 20: its centre lands there, a 1/512 texel past) */
			page_quad(layer, entries[half + ((column >> 29) & 1)], (float)x, (float)y, (float)(x + columns),
				(float)(y + rows), (float)(column & ((1u << 29) - 1)) / (1 << 20) - 0.5f * (float)x_step / (1 << 20) + 1.0f / 512,
				(float)(row & ((1u << 28) - 1)) / (1 << 20) - 0.5f * (float)y_step / (1 << 20) + 1.0f / 512,
				(float)x_step / (1 << 20), (float)y_step / (1 << 20));
			x += columns;
		}
		y += rows;
	}
}

/* NBG2 or NBG3: video.c's draw_scrolled, line by line; lines that go on from the one above join it */
static void scrolled_quads(int number)
{
	const uint16_t line_control = video_register(0x04);
	const bool line_windows = bit(line_control, number + 2);
	const uint32_t table = (uint32_t)(line_control >> 10) * 0x400 + 0x100 * (uint32_t)(number - 2);
	const bool row_scroll = bit(line_control, number - 2) && !line_windows;
	const bool row_select = bit(line_control, number) && !line_windows;
	const int x_scroll = (video_register(0x12 + 8 * (uint32_t)number) & 0x3ff) - (video_register(0x30 + 4 * (uint32_t)number) & 0x1ff);
	const int y_scroll = video_register(0x16 + 8 * (uint32_t)number) & 0x1ff;
	const scene_layer *layer = &scene.layers[LAYER_NBG0 + number];
	int pages[4];
	const int bank = pages_of(number, pages);
	scene_page *entries[4];
	bool flip_x, flip_y;
	int run_start = 0, run_column = 0, run_source = 0;

	if (bit(video_register(0x02), 11 + number) || line_windows)
		scene.missing |= S32_SCENE_MISSING_WINDOWS;
	flips_of(number, &flip_x, &flip_y);
	if (flip_x || flip_y)
	{
		scene.missing |= S32_SCENE_MISSING_FLIP;
		return;
	}
	for (int index = 0; index < 4; index++)
	{
		bool again = false;

		for (int earlier = 0; earlier < index; earlier++)
			if (pages[earlier] == pages[index])
			{
				entries[index] = entries[earlier];
				again = true;
			}
		if (!again && (entries[index] = page_of(false, pages[index], bank, layer)) != NULL)
			page_update(entries[index]);
	}
	for (int y = 0; y <= HEIGHT; y++)
	{
		int column = 0, source = 0;

		if (y < HEIGHT)
		{
			column = x_scroll;
			source = y_scroll + y;
			if (row_scroll)
				column += video_word(table + (uint32_t)y) & 0x3ff;
			if (row_select)
				source = y_scroll + video_word(table + 0x200 + (uint32_t)y);
			column &= 0x3ff;
			source &= 0x1ff;
		}
		/* a run goes on while the column stays and the source row is the next one in the same half */
		if (y > run_start && y < HEIGHT && column == run_column && source == ((run_source + (y - run_start)) & 0x1ff)
			&& (source & 0x100) == (run_source & 0x100))
			continue;
		if (y > run_start)
		{
			const int half = (run_source >> 7) & 2;
			const int first = 512 - (run_column & 0x1ff);
			const int width = first < 320 ? first : 320;

			page_quad(layer, entries[half + ((run_column >> 9) & 1)], 0, (float)run_start, (float)width, (float)y,
				(float)(run_column & 0x1ff), (float)(run_source & 0xff), 1, 1);
			if (width < 320)
				page_quad(layer, entries[half + (((run_column >> 9) & 1) ^ 1)], (float)width, (float)run_start, 320,
					(float)y, 0, (float)(run_source & 0xff), 1, 1);
		}
		run_start = y;
		run_column = column;
		run_source = source;
	}
}

static void text_quads(void)
{
	const uint16_t control = video_register(0x5c);
	const scene_layer *layer = &scene.layers[LAYER_TEXT];
	scene_page *entry;

	if (bit(video_register(0x00), 9))
	{
		scene.missing |= S32_SCENE_MISSING_FLIP;
		return;
	}
	if ((entry = page_of(true, (control >> 4) & 0x1f, control & 7, layer)) == NULL)
		return;
	text_update(entry);
	scene.text_entry = entry;
	scene.text_quad = scene.out.count;
	page_quad(layer, entry, 0, 0, 320, HEIGHT, 0, 0, 1, 1);
	if (scene.text_quad == scene.out.count)
		scene.text_quad = -1;    /* (nothing drawn) */
}

/* The text blends where a sprite is under it (video.c's mix: the layer under its pixel is the sprites): its
   8 x 8 cells over the rectangles of the sprites of the groups it blends with that are under it and over the
   tilemaps go in quads of their own that blend, by runs along each row of cells */
static void text_blend_quads(void)
{
	static uint8_t under[HEIGHT / 8][40];
	const scene_layer *layer = &scene.layers[LAYER_TEXT];
	float highest_tilemap = 0;
	bool any = false;

	if (!scene.text_blends || scene.text_quad < 0)
		return;
	for (int index = LAYER_NBG0; index <= LAYER_BITMAP; index++)
		if (scene.layers[index].shown && scene.layers[index].z < layer->z && scene.layers[index].z > highest_tilemap)
			highest_tilemap = scene.layers[index].z;
	memset(under, 0, sizeof(under));
	for (int index = 0; index < scene.out.count; index++)
	{
		const s32_scene_quad *quad = &scene.quads[index];
		int group = -1;

		if (quad->z >= layer->z || quad->z <= highest_tilemap || quad->image == 0)
			continue;
		for (int at = 0; at <= scene.group_mask; at++)
			if (quad->z >= scene.groups[at].z && quad->z < scene.groups[at].z + 1)
				group = at;
		if (group < 0 || !bit(scene.text_sprite_groups, group))
			continue;
		{
			const float left = quad->x0 < quad->x1 ? quad->x0 : quad->x1, right = quad->x0 < quad->x1 ? quad->x1 : quad->x0;
			const float top = quad->y0 < quad->y1 ? quad->y0 : quad->y1, bottom = quad->y0 < quad->y1 ? quad->y1 : quad->y0;
			const int column0 = left < 0 ? 0 : (int)left / 8, column1 = right > 320 ? 39 : ((int)right - 1) / 8;
			const int row0 = top < 0 ? 0 : (int)top / 8, row1 = bottom > HEIGHT ? HEIGHT / 8 - 1 : ((int)bottom - 1) / 8;

			for (int row = row0; row <= row1; row++)
				for (int column = column0; column <= column1; column++)
				{
					under[row][column] = 1;
					any = true;
				}
		}
	}
	if (!any)
		return;
	/* the text's quad goes (the last one takes its place), its runs come in */
	scene.quads[scene.text_quad] = scene.quads[--scene.out.count];
	for (int row = 0; row < HEIGHT / 8; row++)
		for (int column = 0; column < 40;)
		{
			const int state = under[row][column];
			const int start = column;
			const int before = scene.out.count;

			while (column < 40 && under[row][column] == state)
				column++;
			page_quad(layer, scene.text_entry, (float)start * 8, (float)row * 8, (float)column * 8, (float)row * 8 + 8,
				(float)start * 8, (float)row * 8, 1, 1);
			if (state && scene.out.count > before)
				scene.quads[before].weight = scene.text_weight;
		}
}

/* ---- the bitmap layer: 512 pixels a row from the start of video RAM (video.c's draw_bitmap) ---- */

/* The colours' version of the bitmap: its bank, or the 16 of its 8-bit pixels */
static uint32_t bitmap_version(void)
{
	uint32_t version = 0;

	for (int high = 0; high < (scene.bitmap.bits8 ? 16 : 1); high++)
		version = version * 31 + scene.bank_version[palette_index(scene.bitmap.palette_base, scene.bitmap.shift,
			(scene.bitmap.colour + (high << 4)) & 0x1fff) >> 4];
	return version;
}

/* The rows of the bitmap that changed (or all, when its colours did), into its image */
static void bitmap_update(void)
{
	const scene_layer *layer = &scene.layers[LAYER_BITMAP];
	const bool bits8 = bit(video_register(0x00), 11);
	const int colour = (video_register(0x8c) << 4) & 0x1fff0 & ~((1 << (bits8 ? 8 : 4)) - 1);
	const int rows = bits8 ? 256 : 512;
	const int row_bytes = bits8 ? 512 : 256;
	static uint16_t colours[256];
	bool all;
	uint32_t version;

	if (scene.bitmap.image && scene.bitmap.bits8 != bits8)
	{
		scene.backend->image_free(scene.backend->context, scene.bitmap.image);
		scene.bitmap.image = 0;
	}
	if (!scene.bitmap.image)
	{
		if (!(scene.bitmap.image = image_with_room(512, rows, NULL)))
			return;
		scene.bitmap.drawn = false;
	}
	scene.bitmap.bits8 = bits8;
	/* a change of its colours (or a new image) changes the stamp: each row is redrawn when it is on the screen
	   with an older stamp, or its pixels changed */
	all = !scene.bitmap.drawn || scene.bitmap.colour != colour || scene.bitmap.palette_base != layer->palette_base
		|| scene.bitmap.shift != layer->shift;
	scene.bitmap.colour = colour;
	scene.bitmap.palette_base = layer->palette_base;
	scene.bitmap.shift = layer->shift;
	version = bitmap_version();
	if (all || version != scene.bitmap.version)
		scene.bitmap.stamp++;
	scene.bitmap.version = version;
	{
		/* (its colours once: 16, or 256 for 8 bits) */
		colours[0] = 0;
		for (int value = 1; value < (bits8 ? 256 : 16); value++)
			colours[value] = argb1555(scene.palette[palette_index(layer->palette_base, layer->shift, (value + colour) & 0x1fff)]);
	}
	for (int line = 0; line < HEIGHT; line++)
	{
		const int row = (line + (video_register(0x8a) & 0x1ff)) & (rows - 1);
		const uint8_t *now = &s32_board.video_ram[row * row_bytes];
		uint8_t *copy = &scene.bitmap.copy[row * row_bytes];
		uint16_t *out = scene.pixels;

		if (scene.bitmap.row_stamp[row] == scene.bitmap.stamp && same(now, copy, (size_t)row_bytes))
			continue;
		scene.bitmap.row_stamp[row] = scene.bitmap.stamp;
		memcpy(copy, now, (size_t)row_bytes);
		for (int column = 0; column < 512; column++)
		{
			int value;

			if (bits8)
				value = video_byte((uint32_t)(row * 512 + column));
			else
				value = (video_word((uint32_t)(row * 128 + column / 4)) >> (4 * (column & 3))) & 0x0f;
			out[column] = colours[value];
		}
		scene.backend->image_write(scene.backend->context, scene.bitmap.image, 0, row, 512, 1, out, 512);
		scene.out.images_drawn++;
		scene.out.texels_drawn += 512;
	}
	scene.bitmap.drawn = true;
}

/* Quads of a plane that wraps (width x height texels, scrolled by x_scroll, y_scroll) over a screen rectangle */
static void wrapped_quads(const scene_layer *layer, int image, int width, int height, int x_scroll, int y_scroll,
	int left, int top, int right, int bottom)
{
	for (int y = top; y < bottom;)
	{
		const int v = ((y + y_scroll) % height + height) % height;
		const int rows = (height - v) < (bottom - y) ? (height - v) : (bottom - y);

		for (int x = left; x < right;)
		{
			const int u = ((x + x_scroll) % width + width) % width;
			const int columns = (width - u) < (right - x) ? (width - u) : (right - x);
			s32_scene_quad *quad = new_quad(layer, image);

			if (quad)
			{
				quad->x0 = (float)x;
				quad->y0 = (float)y;
				quad->x1 = (float)(x + columns);
				quad->y1 = (float)(y + rows);
				quad->u0 = (float)u;
				quad->v0 = (float)v;
				quad->u1 = (float)(u + columns);
				quad->v1 = (float)(v + rows);
			}
			x += columns;
		}
		y += rows;
	}
}

/* The bitmap through window 4 when its clip is on (0x02 bit 15; bit 10 draws it outside the window instead) */
static void bitmap_quads(void)
{
	const scene_layer *layer = &scene.layers[LAYER_BITMAP];
	const int x_scroll = video_register(0x88) & 0x1ff;
	const int y_scroll = video_register(0x8a) & 0x1ff;
	area rectangles[4];
	int count = 0;

	bitmap_update();
	if (!scene.bitmap.image)
		return;
	if (!bit(video_register(0x02), 15))
		rectangles[count++] = (area){ 0, 320, 0, HEIGHT };
	else
	{
		const area edges = { 0, 320, 0, HEIGHT };
		area window;

		window_of(4, false, &window);
		clip_area(&window, &edges);
		if (!bit(video_register(0x02), 10))
		{
			if (window.left < window.right && window.top < window.bottom)
				rectangles[count++] = window;
		}
		else
		{
			/* outside it: above, below, and the two sides of its rows */
			if (window.left >= window.right || window.top >= window.bottom)
				rectangles[count++] = edges;
			else
			{
				rectangles[count++] = (area){ 0, 320, 0, window.top };
				rectangles[count++] = (area){ 0, 320, window.bottom, HEIGHT };
				rectangles[count++] = (area){ 0, window.left, window.top, window.bottom };
				rectangles[count++] = (area){ window.right, 320, window.top, window.bottom };
			}
		}
	}
	for (int index = 0; index < count; index++)
		if (rectangles[index].left < rectangles[index].right && rectangles[index].top < rectangles[index].bottom)
			wrapped_quads(layer, scene.bitmap.image, 512, scene.bitmap.bits8 ? 256 : 512, x_scroll, y_scroll,
				rectangles[index].left, rectangles[index].top, rectangles[index].right, rectangles[index].bottom);
}

static void backdrop_quads(void)
{
	const uint16_t control = video_register(0x5e);
	const scene_layer *layer = &scene.layers[LAYER_BACKDROP];
	int run_start = 0;
	uint16_t run_colour = 0;

	for (int y = 0; y <= HEIGHT; y++)
	{
		uint16_t colour = 0;

		if (y < HEIGHT)
		{
			const int pixel = (control & 0x1e00) + (bit(control, 15) ? (control + y) & 0x1ff : 0);

			colour = argb1555(scene.palette[palette_index(layer->palette_base, layer->shift, pixel)]);
			if (y > run_start && colour == run_colour)
				continue;
		}
		if (y > run_start)
		{
			s32_scene_quad *quad = new_quad(layer, 0);

			if (quad)
			{
				quad->x0 = 0;
				quad->y0 = (float)run_start;
				quad->x1 = 320;
				quad->y1 = (float)y;
				quad->u0 = quad->v0 = quad->u1 = quad->v1 = 0;
				quad->colour = run_colour;
			}
		}
		run_start = y;
		run_colour = colour;
	}
}

/* ---- sprites ---- */

/* The list as video.c's draw_sprite_list reads it, each sprite kept as a scene_sprite in list 'into' */
static int scene_sprite_entry(int into, uint32_t entry, int x_offset, int y_offset, const area *inside, const area *outside)
{
	static const uint16_t transparent_bits[4][4] = {
		{ 0x7fff, 0x3fff, 0x1fff, 0x0fff },
		{ 0x3fff, 0x1fff, 0x0fff, 0x07ff },
		{ 0x3fff, 0x1fff, 0x0fff, 0x07ff },
		{ 0x1fff, 0x0fff, 0x07ff, 0x03ff }
	};
	const uint8_t *latched = s32_board.sprite_control_latched;
	const uint16_t flags = sprite_word(entry);
	const uint16_t size = sprite_word(entry + 1);
	const uint16_t height_word = sprite_word(entry + 2);
	const uint16_t width_word = sprite_word(entry + 3);
	const bool table_inline = bit(flags, 12);
	const int taken = (bit(flags, 13) && table_inline) ? 2 : 0;
	const int source_height = size >> 8;
	const int height = height_word & 0x3ff;
	const int width = width_word & 0x3ff;
	const uint32_t bank = ((width_word & 0x2000) >> 13 | (width_word & 0x8000) >> 14) % (video.graphics.sprite_banks ? video.graphics.sprite_banks : 1);
	scene_sprite *sprite;
	int source_width, x, y;
	float u0, u1, v0, v1;
	area clip = *inside;

	if (bit(width_word, 11))
		return taken;    /* (the right screen's) */
	if (scene.sprite_count[into] >= SCENE_SPRITES)
		return taken;
	sprite = &scene.sprites[into][scene.sprite_count[into]];
	sprite->bits8 = bit(flags, 9);
	sprite->indirect = bit(flags, 13);
	sprite->opaque = bit(flags, 8);
	source_width = sprite->bits8 ? (size & 0x3f) : ((size >> 1) & 0x3f);
	if (source_width == 0 || source_height == 0 || width == 0 || height == 0)
		return taken;
	if (bit(flags, 10))
	{
		scene.missing |= S32_SCENE_MISSING_FROM_RAM;
		return taken;
	}
	if (bit(flags, 11) && bit(latched[5], 0))
	{
		scene.missing |= S32_SCENE_MISSING_SHADOW;
		return taken;
	}
	sprite->colour = (uint16_t)(0x8000 | (sprite_word(entry + 7) & (sprite->bits8 ? 0x7f00 : 0x7ff0)));
	sprite->transparent_bits = transparent_bits[latched[4] & 3][latched[5] & 3];
	if (sprite->bits8)
		sprite->transparent_bits &= 0xfff0;
	memset(sprite->table, 0, sizeof(sprite->table));
	if (sprite->indirect)
	{
		const uint32_t table = table_inline ? entry + 8 : 8 * (uint32_t)(sprite_word(entry + 7) & 0x1fff);

		for (int index = 0; index < 16; index++)
			sprite->table[index] = (uint16_t)((sprite_word(table + (uint32_t)index) & (sprite->bits8 ? 0xfff0 : 0xffff)) |
				(bit(latched[5], 0) ? 0x8000 : 0));
		sprite->colour = 0;
	}
	sprite->place = bank << 20 | ((sprite_word(entry + 6) | (uint32_t)(height_word & 0xf000) << 4) & 0xfffff);
	sprite->words = (uint16_t)source_width;
	sprite->rows = (uint16_t)source_height;

	x = sign_extend(sprite_word(entry + 5), 12) + (bit(flags, 4) ? x_offset : 0);
	y = sign_extend(sprite_word(entry + 4), 12) + (bit(flags, 5) ? y_offset : 0);
	switch (flags & 3)
	{
		case 0:
		case 3: x -= (width - 1) / 2; break;
		case 1: x -= width - 1; break;
		default: break;
	}
	switch ((flags >> 2) & 3)
	{
		case 0:
		case 3: y -= (height - 1) / 2; break;
		case 1: y -= height - 1; break;
		default: break;
	}
	/* the screen rectangle and the texels at its edges. video.c steps through the source by its zoom
	   (pens a pixel, in 16.16, rounded down): pixel k of the sprite takes pen k * zoom >> 16, counted from the
	   right edge when flipped. The quad's texels are set so that the pixels' centres land on those (a 1/512
	   texel past them: no texel's edge) */
	{
		const float x_step = (float)(((sprite->bits8 ? 4 : 8) * source_width << 16) / width) / 65536.0f;
		const float y_step = (float)((source_height << 16) / height) / 65536.0f;
		const float near_x = -0.5f * x_step + 1.0f / 512, far_x = ((float)width - 0.5f) * x_step + 1.0f / 512;
		const float near_y = -0.5f * y_step + 1.0f / 512, far_y = ((float)height - 0.5f) * y_step + 1.0f / 512;

		u0 = bit(flags, 6) ? far_x : near_x;
		u1 = bit(flags, 6) ? near_x : far_x;
		v0 = bit(flags, 7) ? far_y : near_y;
		v1 = bit(flags, 7) ? near_y : far_y;
	}
	if (outside->right >= outside->left && outside->bottom >= outside->top
		&& outside->right >= x && outside->left < x + width && outside->bottom >= y && outside->top < y + height)
		scene.missing |= S32_SCENE_MISSING_OUTSIDE;
	{
		int x0 = x, y0 = y, x1 = x + width, y1 = y + height;
		const float du = (u1 - u0) / (float)width, dv = (v1 - v0) / (float)height;

		if (x0 < clip.left)
			x0 = clip.left;
		if (y0 < clip.top)
			y0 = clip.top;
		if (x1 > clip.right + 1)
			x1 = clip.right + 1;
		if (y1 > clip.bottom + 1)
			y1 = clip.bottom + 1;
		if (x0 >= x1 || y0 >= y1)
			return taken;
		sprite->x0 = x0;
		sprite->y0 = y0;
		sprite->x1 = x1;
		sprite->y1 = y1;
		sprite->u0 = u0 + (float)(x0 - x) * du;
		sprite->u1 = u0 + (float)(x1 - x) * du;
		sprite->v0 = v0 + (float)(y0 - y) * dv;
		sprite->v1 = v0 + (float)(y1 - y) * dv;
	}
	scene.sprite_count[into]++;
	return taken;
}

static void scene_sprite_list(int into, int number)
{
	const area screen = { 0, 319, 0, HEIGHT - 1 };
	area inside = screen;
	area outside = { 0, -1, 0, -1 };
	int x_offset = 0, y_offset = 0;

	for (int entries = 0; entries < 0x20000 / 16; entries++)
	{
		const uint32_t entry = 8 * (uint32_t)(number & 0x1fff);
		const uint16_t command = sprite_word(entry);

		switch (command >> 14)
		{
			case 0:
				number += 1 + scene_sprite_entry(into, entry, x_offset, y_offset, &inside, &outside);
				break;
			case 1:
				if (bit(command, 12))
				{
					inside.left = sign_extend(sprite_word(entry + 2), 12);
					inside.right = sign_extend(sprite_word(entry + 3), 12);
					inside.top = sign_extend(command, 12);
					inside.bottom = sign_extend(sprite_word(entry + 1), 12);
					clip_area(&inside, &screen);
				}
				if (bit(command, 13))
				{
					outside.left = sign_extend(sprite_word(entry + 6), 12);
					outside.right = sign_extend(sprite_word(entry + 7), 12);
					outside.top = sign_extend(sprite_word(entry + 4), 12);
					outside.bottom = sign_extend(sprite_word(entry + 5), 12);
				}
				number++;
				break;
			case 2:
				if (bit(command, 13))
				{
					y_offset = sign_extend(sprite_word(entry + 1), 12);
					x_offset = sign_extend(sprite_word(entry + 2), 12);
				}
				number = command & 0x1fff;
				break;
			default:
				return;
		}
	}
}

/* video.c's s32_video_sprites, for the scene's lists */
static void scene_sprites(bool erase, bool draw)
{
	if (!scene.backend)
		return;
	if (erase)
		scene.sprite_count[scene.shown] = 0;
	if (draw)
	{
		scene.shown ^= 1;
		scene_sprite_list(scene.shown ^ 1, 0);
	}
}

/* The pixel a pen of a sprite puts in the buffer (video.c's put_sprite_pixel), or -1 for none */
static int sprite_pixel(const scene_slot *slot, int pen)
{
	int pixel;

	if (!(slot->flags & 2))
		return pen ? (slot->colour | pen) : -1;
	pixel = (slot->flags & 1) ? slot->table[pen >> 4] | (pen & 0x0f) : slot->table[pen];
	return (pixel & slot->transparent_bits) == slot->transparent_bits ? -1 : pixel;
}

/* The sprite's group: of its colour word, or of its table's first colour */
static int sprite_group(const scene_sprite *sprite)
{
	const int pixel = sprite->indirect ? sprite->table[1] : sprite->colour;

	return (pixel >> scene.group_shift) & scene.group_mask;
}

/* The version of the colours a sprite's image takes: of the banks its pens' colours are in */
static uint32_t sprite_version(const scene_slot *slot)
{
	uint32_t version = 0;

	if (!(slot->flags & 2))
	{
		/* (a direct colour: one bank, or 16 for 8 bits, the pens' high nibble over the colour's) */
		for (int high = 0; high < ((slot->flags & 1) ? 16 : 1); high++)
			version = version * 31 + scene.bank_version[palette_index(slot->palette_base, slot->shift,
				(slot->colour | high << 4) & scene.sprite_bits) >> 4];
		return version;
	}
	for (int index = 0; index < 16; index++)
		version = version * 31 + scene.bank_version[palette_index(slot->palette_base, slot->shift,
			slot->table[index] & scene.sprite_bits) >> 4];
	return version;
}

static int power_of_two(int value)
{
	int power = 8;

	while (power < value)
		power <<= 1;
	return power;
}

/* The sprite's image in its group's colours: found, or made (an old one let go when there is no room) */
static scene_slot *sprite_image(const scene_sprite *sprite, const scene_layer *group)
{
	scene_slot key;
	uint32_t hash = 2166136261u;
	scene_slot *slot = NULL;

	memset(&key, 0, sizeof(key));
	key.place = sprite->place;
	key.words = sprite->words;
	key.rows = sprite->rows;
	key.colour = sprite->colour;
	key.transparent_bits = sprite->transparent_bits;
	memcpy(key.table, sprite->table, sizeof(key.table));
	key.palette_base = group->palette_base;
	key.shift = group->shift;
	key.flags = (uint8_t)((sprite->bits8 ? 1 : 0) | (sprite->indirect ? 2 : 0) | (sprite->opaque ? 4 : 0));
	{
		const uint8_t *bytes = (const uint8_t *)&key.place;
		const size_t length = (size_t)((const uint8_t *)&key.version - bytes);

		for (size_t index = 0; index < length; index++)
			hash = (hash ^ bytes[index]) * 16777619u;
	}
	if (!hash)
		hash = 1;
	key.hash = hash;
	for (uint32_t probe = 0; probe < SCENE_SPRITE_SLOTS; probe++)
	{
		scene_slot *at = &scene.slots[(hash + probe) & (SCENE_SPRITE_SLOTS - 1)];

		if (at->hash == hash && at->place == key.place && at->words == key.words && at->rows == key.rows
			&& at->colour == key.colour && at->flags == key.flags && at->palette_base == key.palette_base
			&& at->shift == key.shift && at->transparent_bits == key.transparent_bits
			&& memcmp(at->table, key.table, sizeof(key.table)) == 0)
		{
			slot = at;
			break;
		}
		if (at->hash == 0)
			break;
	}
	key.version = sprite_version(&key);
	if (slot && slot->image && slot->version == key.version)
	{
		slot->used = scene.frame;
		return slot;
	}
	if (!slot)
		for (uint32_t probe = 0; probe < SCENE_SPRITE_SLOTS && !slot; probe++)
		{
			scene_slot *at = &scene.slots[(hash + probe) & (SCENE_SPRITE_SLOTS - 1)];

			if (at->hash == 0)
				slot = at;
		}
	if (!slot)
	{
		scene.missing |= S32_SCENE_MISSING_IMAGES;
		return NULL;
	}
	{
		const int image = slot->image, shadow_image = slot->shadow_image;
		const int width = slot->width, height = slot->height;

		*slot = key;
		slot->image = image;
		slot->shadow_image = shadow_image;
		slot->width = width;
		slot->height = height;
	}
	{
		const int wide = sprite->words * ((slot->flags & 1) ? 4 : 8);
		const int bits = (slot->flags & 1) ? 8 : 4;
		const int pens = 32 / bits;
		const int pen_mask = (1 << bits) - 1;
		const int end_pen = (slot->flags & 4) ? 0 : pen_mask;
		const int width = power_of_two(wide), height = power_of_two(sprite->rows);
		bool shadowed = false;

		if (slot->image && (slot->width != width || slot->height != height))
		{
			scene.backend->image_free(scene.backend->context, slot->image);
			slot->image = 0;
		}
		if (slot->shadow_image)
		{
			scene.backend->image_free(scene.backend->context, slot->shadow_image);
			slot->shadow_image = 0;
		}
		if (!slot->image && !(slot->image = image_with_room(width, height, slot)))
			return NULL;
		slot->width = width;
		slot->height = height;
		for (int row = 0; row < sprite->rows; row++)
		{
			uint16_t *out = &scene.pixels[row * width];
			uint16_t *shadow = &scene.shadow_pixels[row * width];
			const uint32_t bank = sprite->place >> 20;
			uint32_t address = (sprite->place & 0xfffff) + (uint32_t)(row * sprite->words);
			int x = 0;
			bool ended = false;

			for (int word_index = 0; word_index < sprite->words; word_index++)
			{
				const uint32_t word = ended ? 0 : sprite_graphics(false, bank, address & 0xfffff);
				int pen = 0;

				address++;
				for (int index = 0; index < pens; index++, x++)
				{
					const int skipped = (index == 0 || index == pens - 1) ? end_pen : 0;
					int pixel;

					pen = (int)(word >> (32 - bits * (index + 1))) & pen_mask;
					shadow[x] = 0;
					pixel = (ended || pen == skipped) ? -1 : sprite_pixel(slot, pen);
					if (pixel >= 0 && ((pixel & 0x7fff) == 0x7fff || ((pixel & scene.sprite_bits) & 0x7ffe) == (0x7ffe & scene.sprite_bits)))
					{
						/* all ones shows what is below; the shadow pen too, darkened: in the shadow's image */
						if ((pixel & 0x7fff) != 0x7fff)
						{
							shadow[x] = 0x8000;
							shadowed = true;
						}
						pixel = -1;
					}
					out[x] = pixel < 0 ? 0 : argb1555(scene.palette[palette_index(slot->palette_base, slot->shift,
						pixel & scene.sprite_bits)]);
				}
				if (end_pen != 0 && pen == pen_mask)
					ended = true;
			}
			for (; x < width; x++)
				out[x] = shadow[x] = 0;
		}
		scene.backend->image_write(scene.backend->context, slot->image, 0, 0, width, sprite->rows, scene.pixels, width);
		scene.out.images_drawn++;
		scene.out.texels_drawn += width * sprite->rows;
		if (shadowed && (slot->shadow_image = image_with_room(width, height, slot)) != 0)
		{
			scene.backend->image_write(scene.backend->context, slot->shadow_image, 0, 0, width, sprite->rows,
				scene.shadow_pixels, width);
			scene.out.texels_drawn += width * sprite->rows;
		}
	}
	slot->used = scene.frame;
	return slot;
}

static void sprite_quads(void)
{
	const int list = scene.shown;
	const int count = scene.sprite_count[list];

	for (int index = 0; index < count; index++)
	{
		const scene_sprite *sprite = &scene.sprites[list][index];
		const scene_layer *group = &scene.groups[sprite_group(sprite)];
		const scene_slot *slot = sprite_image(sprite, group);
		s32_scene_quad *quad;

		if (!slot || !(quad = new_quad(group, slot->image)))
			continue;
		quad->z = group->z + 0.5f * (float)(index + 1) / (float)(count + 1);    /* (later ones in front) */
		quad->x0 = (float)sprite->x0;
		quad->y0 = (float)sprite->y0;
		quad->x1 = (float)sprite->x1;
		quad->y1 = (float)sprite->y1;
		quad->u0 = sprite->u0;
		quad->v0 = sprite->v0;
		quad->u1 = sprite->u1;
		quad->v1 = sprite->v1;
		if (slot->shadow_image)
		{
			/* the shadow pen: what is under it at half (video.c's mix: shift_down by 1) */
			const s32_scene_quad drawn = *quad;
			s32_scene_quad *shade = new_quad(group, slot->shadow_image);

			if (shade)
			{
				const int image = shade->image;

				*shade = drawn;
				shade->image = image;
				shade->modulate = 0;
				shade->add = 0;
				shade->weight = 4;
			}
		}
	}
}

/* ---- the frame ---- */

static bool scene_only(void)
{
	return scene.backend && scene.only;
}

void s32_scene_follow_frames(bool follow)
{
	scene.follow = follow;
}

const s32_scene_frame *s32_scene_last(void)
{
	return scene.last;
}

/* (video.c, after drawing a frame) */
static void scene_after_draw(void)
{
	if (scene.backend && scene.follow)
		scene.last = s32_scene_build();
}

void s32_scene_start(const s32_scene_backend *backend, bool only)
{
	memset(&scene, 0, sizeof(scene));
	scene.backend = backend;
	scene.only = only;
	scene.character_bank = -1;
	/* (the palette's copy starts at zero: every bank counts as changed on the first frame) */
	for (int bank = 0; bank < BANKS; bank++)
		scene.bank_version[bank] = 1;
}

const s32_scene_frame *s32_scene_build(void)
{
	const uint16_t control = video_register(0x00);
	const uint16_t off = video_register(0x02);
	const uint16_t more_off = video_register(0x8e);
	int drawn = 0;

	scene.frame++;
	scene.out.quads = scene.quads;
	scene.out.count = 0;
	scene.out.images_drawn = 0;
	scene.out.texels_drawn = 0;
	scene.missing = 0;
	scene.out.subtract = 0;
	scene.text_quad = -1;
	if (!s32_display_enabled(0))
	{
		scene.out.missing = 0;
		return &scene.out;    /* (no picture: nothing drawn, the backend clears) */
	}
	if (!bit(off, 0) && !bit(more_off, 1))
		drawn |= 1 << LAYER_NBG0;
	if (!bit(off, 1) && !bit(more_off, 2))
		drawn |= 1 << LAYER_NBG1;
	if (!bit(off, 2) && !bit(more_off, 3) && !bit(control, 12))
		drawn |= 1 << LAYER_NBG2;
	if (!bit(off, 3) && !bit(more_off, 4) && !bit(control, 13))
		drawn |= 1 << LAYER_NBG3;
	if (!bit(off, 4) && !bit(more_off, 0))
		drawn |= 1 << LAYER_TEXT;
	if (!bit(off, 5) && !bit(more_off, 5))
		drawn |= 1 << LAYER_BITMAP;
	palette_update();
	mix_update(drawn);

	if (scene.layers[LAYER_NBG1].shown)
		scene.missing |= S32_SCENE_MISSING_NBG1;
	if (scene.layers[LAYER_NBG3].shown)
		scene.missing |= S32_SCENE_MISSING_NBG3;
	backdrop_quads();
	if (scene.layers[LAYER_NBG0].shown)
		zoomed_quads(0);
	if (scene.layers[LAYER_NBG2].shown)
		scrolled_quads(2);
	if (scene.layers[LAYER_TEXT].shown)
		text_quads();
	if (scene.layers[LAYER_BITMAP].shown)
		bitmap_quads();
	sprite_quads();
	text_blend_quads();
	scene.out.missing = scene.missing;
	return &scene.out;
}
