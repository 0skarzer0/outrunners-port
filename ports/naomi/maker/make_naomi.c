/**
 *    Name:        make_naomi.c
 *    Description: OutRunners NAOMI's work: the netboot image for a NAOMI's NetDIMM out of the game's NAOMI
 *                 program and the player's ROM set (ports/naomi/maker/main.c)
 *    Created:     13-11-2026
 *
 *    license:BSD-3-Clause
 *    The steps of ports/naomi/build_naomi.sh after the build, in C, so that a player needs no tools: the assets
 *    pack of the ROM set (rom_pack.c, as the computer's first run makes it), made the NAOMI's as naomi_assets.py
 *    makes it (the tiles' pens two to a byte, the MultiPCM's samples apart as the chip's 4 MB), the program
 *    (orunners_naomi-prog.bin, naomibintool's, which holds nothing of the ROM set) with the archive /dimm after
 *    it as naomi_pack.py writes it, gzip'd (stored blocks: no compression, any gunzip and the netbooters take
 *    it). The image is made from the player's ROM set: never share it.
 */
#include "rom_pack.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TILE_COUNT        0x8000
#define PCM_HEADERS_SIZE  (0x200 * 12)
#define PCM_SIZE          0x400000
#define NAME_MAX_BYTES    56
#define ARCHIVE_ALIGN     0x10000u

typedef struct
{
	uint8_t *data;
	size_t size, room;
	int failed;
} buffer;

static void put(buffer *out, const void *bytes, size_t size)
{
	if (out->failed)
		return;
	if (out->size + size > out->room)
	{
		size_t room = out->room ? out->room : 1u << 20;
		uint8_t *grown;

		while (room < out->size + size)
			room *= 2;
		if (!(grown = (uint8_t *)realloc(out->data, room)))
		{
			out->failed = 1;
			return;
		}
		out->data = grown;
		out->room = room;
	}
	memcpy(out->data + out->size, bytes, size);
	out->size += size;
}

static void put32(buffer *out, uint32_t value)
{
	const uint8_t bytes[4] = { (uint8_t)value, (uint8_t)(value >> 8), (uint8_t)(value >> 16), (uint8_t)(value >> 24) };

	put(out, bytes, 4);
}

static void pad(buffer *out, size_t multiple)
{
	static const uint8_t zeros[64];

	while (out->size % multiple)
		put(out, zeros, multiple - out->size % multiple > sizeof(zeros) ? sizeof(zeros) : multiple - out->size % multiple);
}

/* The pack, read as naomi_assets.py's Reader: 32-bit little-endian words, raw blocks padded to 4 */
typedef struct
{
	const uint8_t *data;
	size_t size, at;
	int short_read;
} reader;

static uint32_t take32(reader *in)
{
	uint32_t value;

	if (in->at + 4 > in->size)
	{
		in->short_read = 1;
		return 0;
	}
	value = (uint32_t)in->data[in->at] | (uint32_t)in->data[in->at + 1] << 8 | (uint32_t)in->data[in->at + 2] << 16
		| (uint32_t)in->data[in->at + 3] << 24;
	in->at += 4;
	return value;
}

static const uint8_t *take(reader *in, size_t size)
{
	const uint8_t *at = in->data + in->at;

	if (in->at + size > in->size)
	{
		in->short_read = 1;
		return NULL;
	}
	in->at += size + ((4 - size % 4) % 4);
	if (in->at > in->size)
		in->at = in->size;
	return at;
}

static void put_raw(buffer *out, const void *bytes, size_t size)
{
	put(out, bytes, size);
	pad(out, 4);
}

/* naomi_assets.py: the NAOMI's pack ("ORAN") and the MultiPCM's 4 MB, from the pack. NULL or what failed */
static const char *naomi_assets(const uint8_t *pack, size_t size, buffer *out, uint8_t *pcm)
{
	reader in = { pack, size, 0, 0 };
	const uint8_t *magic = take(&in, 4), *present, *headers;

	if (!magic || memcmp(magic, "ORAP", 4) != 0)
		return "the assets pack is not one";
	put_raw(out, "ORAN", 4);
	put32(out, take32(&in));    /* the pack's version */
	put32(out, take32(&in));    /* the tables' version */
	for (int region = 0; region < 2; region++)    /* the V70's pieces, then the sound program's */
	{
		const uint32_t count = take32(&in);

		put32(out, count);
		for (uint32_t piece = 0; piece < count && !in.short_read; piece++)
		{
			const uint32_t address = take32(&in), length = take32(&in);
			const uint8_t *bytes = take(&in, length);

			put32(out, address);
			put32(out, length);
			if (bytes)
				put_raw(out, bytes, length);
		}
	}
	if (!(present = take(&in, TILE_COUNT / 8)))
		return "the assets pack ends early";
	put_raw(out, present, TILE_COUNT / 8);
	for (int tile = 0; tile < TILE_COUNT; tile++)
		if ((present[tile / 8] >> (tile % 8)) & 1)
		{
			const uint8_t *pens = take(&in, 256);
			uint8_t packed[128];

			if (!pens)
				return "the assets pack ends early";
			for (int index = 0; index < 128; index++)
				packed[index] = (uint8_t)((pens[2 * index] & 15) | (pens[2 * index + 1] & 15) << 4);
			put_raw(out, packed, sizeof(packed));
		}
	{
		const uint32_t blocks = take32(&in);

		put32(out, blocks);
		for (uint32_t block = 0; block < blocks && !in.short_read; block++)
		{
			const uint32_t first = take32(&in), count = take32(&in);
			const uint8_t *words = take(&in, (size_t)count * 4);

			put32(out, first);
			put32(out, count);
			if (words)
				put_raw(out, words, (size_t)count * 4);
		}
	}
	if (!(headers = take(&in, PCM_HEADERS_SIZE)))
		return "the assets pack ends early";
	put_raw(out, headers, PCM_HEADERS_SIZE);
	put32(out, 0);    /* no sample blocks: they go to the AICA */
	memset(pcm, 0xff, PCM_SIZE);
	memcpy(pcm, headers, PCM_HEADERS_SIZE);
	{
		const uint32_t blocks = take32(&in);

		for (uint32_t block = 0; block < blocks && !in.short_read; block++)
		{
			const uint32_t offset = take32(&in), length = take32(&in);
			const uint8_t *bytes = take(&in, length);

			if (bytes && offset <= PCM_SIZE && length <= PCM_SIZE - offset)
				memcpy(pcm + offset, bytes, length);
		}
	}
	if (in.short_read)
		return "the assets pack ends early";
	return out->failed ? "not enough memory" : NULL;
}

static uint8_t *read_file(const char *path, size_t *size)
{
	FILE *file = fopen(path, "rb");
	uint8_t *data = NULL;
	long length;

	if (!file)
		return NULL;
	if (fseek(file, 0, SEEK_END) == 0 && (length = ftell(file)) > 0 && fseek(file, 0, SEEK_SET) == 0
		&& (data = (uint8_t *)malloc((size_t)length)) != NULL)
	{
		if (fread(data, 1, (size_t)length, file) != (size_t)length)
		{
			free(data);
			data = NULL;
		}
		else
			*size = (size_t)length;
	}
	fclose(file);
	return data;
}

static uint32_t crc32_of(uint32_t crc, const uint8_t *data, size_t size)
{
	static uint32_t table[256];

	if (!table[1])
		for (uint32_t index = 0; index < 256; index++)
		{
			uint32_t value = index;

			for (int bit = 0; bit < 8; bit++)
				value = (value & 1) ? 0xedb88320u ^ (value >> 1) : value >> 1;
			table[index] = value;
		}
	crc = ~crc;
	for (size_t index = 0; index < size; index++)
		crc = table[(crc ^ data[index]) & 0xff] ^ (crc >> 8);
	return ~crc;
}

/* The image as gzip with stored deflate blocks (65535 bytes each) */
static int write_gzip(const char *path, const uint8_t *data, size_t size)
{
	static const uint8_t header[10] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255 };
	FILE *file = fopen(path, "wb");
	size_t at = 0;
	int ok;

	if (!file)
		return 0;
	ok = fwrite(header, 1, sizeof(header), file) == sizeof(header);
	do
	{
		const size_t length = size - at > 65535 ? 65535 : size - at;
		const uint8_t block[5] = { (uint8_t)(at + length == size), (uint8_t)length, (uint8_t)(length >> 8),
			(uint8_t)~length, (uint8_t)(~length >> 8) };

		ok = ok && fwrite(block, 1, 5, file) == 5 && fwrite(data + at, 1, length, file) == length;
		at += length;
	} while (at < size && ok);
	{
		const uint32_t crc = crc32_of(0, data, size);
		const uint8_t trailer[8] = { (uint8_t)crc, (uint8_t)(crc >> 8), (uint8_t)(crc >> 16), (uint8_t)(crc >> 24),
			(uint8_t)size, (uint8_t)(size >> 8), (uint8_t)(size >> 16), (uint8_t)(size >> 24) };

		ok = ok && fwrite(trailer, 1, 8, file) == 8;
	}
	return fclose(file) == 0 && ok;
}

/* naomi_pack.py: the archive /dimm (its files by name) after the program segment, at 64 KB */
static const char *with_archive(const uint8_t *program, size_t program_size, const char *const *names,
	const buffer *files, int count, buffer *image)
{
	uint32_t segment_offset, segment_length, base, offset;
	const size_t header_size = 16 + (size_t)count * (NAME_MAX_BYTES + 8);

	if (program_size < 0x370 || memcmp(program, "NAOMI", 5) != 0)
		return "the program is not a NAOMI image (orunners_naomi-prog.bin, naomibintool's)";
	memcpy(&segment_offset, program + 0x360, 4);
	memcpy(&segment_length, program + 0x368, 4);
	base = (segment_offset + segment_length + ARCHIVE_ALIGN - 1) & ~(ARCHIVE_ALIGN - 1);
	if (program_size > base)
		return "the program has data after its segment: not naomibintool's plain image";
	put(image, program, program_size);
	while (image->size < base && !image->failed)
		put(image, "", 1);
	put(image, "CBDIMMFS", 8);
	put32(image, 1);
	put32(image, (uint32_t)count);
	offset = (uint32_t)((header_size + 31) & ~(size_t)31);
	for (int file = 0; file < count; file++)
	{
		char name[NAME_MAX_BYTES];

		memset(name, 0, sizeof(name));
		strncpy(name, names[file], sizeof(name) - 1);
		put(image, name, sizeof(name));
		put32(image, offset);
		put32(image, (uint32_t)files[file].size);
		offset += (uint32_t)((files[file].size + 31) & ~(size_t)31);
	}
	while ((image->size - base) % 32 && !image->failed)
		put(image, "", 1);
	for (int file = 0; file < count; file++)
	{
		put(image, files[file].data, files[file].size);
		while ((image->size - base) % 32 && !image->failed)
			put(image, "", 1);
	}
	return image->failed ? "not enough memory" : NULL;
}

const char *make_naomi(const char *rom, const char *program_path, const char *out, FILE *log)
{
	/* (the archive's files in naomi_pack.py's order: by name) */
	static const char *const names[2] = { "orunners.pcm", "orunners_naomi.assets" };
	char pack_path[1200];
	uint8_t *pack = NULL, *program = NULL;
	size_t pack_size = 0, program_size = 0;
	buffer files[2], image;
	const char *problem;

	memset(files, 0, sizeof(files));
	memset(&image, 0, sizeof(image));
	if (!(program = read_file(program_path, &program_size)))
		return "the program cannot be read (orunners_naomi-prog.bin beside the tool)";
	snprintf(pack_path, sizeof(pack_path), "%s.assets-making", out);
	if (log)
		fprintf(log, "ROM set   %s\n", rom);
	if ((problem = rom_pack_make(rom, pack_path)) != NULL)
	{
		remove(pack_path);
		free(program);
		return problem;
	}
	pack = read_file(pack_path, &pack_size);
	remove(pack_path);
	if (!pack)
	{
		free(program);
		return "the assets pack cannot be read back";
	}
	files[0].data = (uint8_t *)malloc(PCM_SIZE);
	files[0].size = PCM_SIZE;
	if (!files[0].data)
		problem = "not enough memory";
	else
		problem = naomi_assets(pack, pack_size, &files[1], files[0].data);
	free(pack);
	if (!problem)
		problem = with_archive(program, program_size, names, files, 2, &image);
	if (!problem && log)
		fprintf(log, "program   %s (%lu bytes)\nimage     %s (%.1f MB)\n", program_path, (unsigned long)program_size,
			out, (double)image.size / (1024.0 * 1024.0));
	if (!problem && !write_gzip(out, image.data, image.size))
		problem = "the image cannot be written";
	free(program);
	free(files[0].data);
	free(files[1].data);
	free(image.data);
	return problem;
}
