/**
 *    Name:        main.c
 *    Description: OutRunners NAOMI: the netboot image for a SEGA NAOMI's NetDIMM, made on the player's computer
 *                 from the game's NAOMI program and the player's ROM set (make_naomi.c)
 *    Created:     13-11-2026
 *
 *    license:BSD-3-Clause
 *    Usage: OutRunners NAOMI [--rom FILE] [--program FILE] [--out FILE]
 *
 *    By default the ROM set (orunners.zip) and the program (orunners_naomi-prog.bin) are beside the tool, and
 *    the image (orunners_naomi.bin.gz) is written beside it. A file dropped on the tool is the ROM set; started
 *    so, or with a double click, its console waits for Enter at the end.
 */
#include <stdio.h>
#include <string.h>

#define PATH_SIZE 1100

const char *make_naomi(const char *rom, const char *program, const char *out, FILE *log);

static int waits = 1;

static int finish(int code)
{
	if (waits)
	{
		printf("\nPress Enter to close.\n");
		(void)getchar();
	}
	return code;
}

/* The folder of a path, with its separator: "" for a bare name */
static void folder_of(const char *path, char *folder, size_t size)
{
	const char *slash = strrchr(path, '/'), *backslash = strrchr(path, '\\');
	const char *end = slash > backslash ? slash : backslash;
	const size_t length = end ? (size_t)(end - path) + 1 : 0;

	snprintf(folder, size, "%.*s", (int)(length < size ? length : size - 1), path);
}

int main(int argc, char **argv)
{
	char here[PATH_SIZE - 64], rom[PATH_SIZE] = "", program[PATH_SIZE] = "", out[PATH_SIZE] = "";
	const char *problem;

	folder_of(argv[0], here, sizeof(here));
	for (int at = 1; at < argc; at++)
	{
		char *target = NULL;

		if (strncmp(argv[at], "--", 2) != 0)
		{
			snprintf(rom, sizeof(rom), "%s", argv[at]);    /* (a file dropped on the tool: the ROM set) */
			continue;
		}
		waits = 0;
		if (strcmp(argv[at], "--rom") == 0)
			target = rom;
		else if (strcmp(argv[at], "--program") == 0)
			target = program;
		else if (strcmp(argv[at], "--out") == 0)
			target = out;
		if (!target || at + 1 >= argc)
		{
			fprintf(stderr, "Usage: OutRunners NAOMI [--rom FILE] [--program FILE] [--out FILE]\n"
				"  --rom      the arcade ROM set (orunners.zip, MAME's; by default beside it)\n"
				"  --program  the game's NAOMI program (orunners_naomi-prog.bin; by default beside it)\n"
				"  --out      the netboot image (by default orunners_naomi.bin.gz beside it)\n");
			return 2;
		}
		snprintf(target, PATH_SIZE, "%s", argv[++at]);
	}
	if (!rom[0])
		snprintf(rom, sizeof(rom), "%sorunners.zip", here);
	if (!program[0])
		snprintf(program, sizeof(program), "%sorunners_naomi-prog.bin", here);
	if (!out[0])
		snprintf(out, sizeof(out), "%sorunners_naomi.bin.gz", here);
	printf("OutRunners NAOMI: the netboot image for a NAOMI's NetDIMM, with the assets of your ROM set\n\n");
	if ((problem = make_naomi(rom, program, out, stdout)) != NULL)
	{
		fprintf(stderr, "Not made: %s\n", problem);
		return finish(1);
	}
	printf("\nPut it on your netbooter (a WiPi's /boot/roms, say) and send it to the NetDIMM. It holds your ROM\n"
		"set's assets: keep it to yourself.\n");
	return finish(0);
}
