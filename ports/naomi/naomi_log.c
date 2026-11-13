/**
 *    Name:        naomi_log.c
 *    Description: the console in a ring of lines, the failure screen, the watchdog and the boot (naomi.h)
 *    Created:     13-11-2026
 *
 *    license:BSD-3-Clause
 *    After naomi_log.c of Daytona USA's native port on the NAOMI (0skarzer0/daytona-arcade-recomp, branch
 *    naomi-native). A cabinet has no serial cable: the console goes to a dbgio device that keeps the last lines,
 *    shown on the screen when something fails or the game stops running frames (with the main thread's pc and pr,
 *    for sh-elf-addr2line), and readable live through the NetDIMM (live.py log).
 */
#include <kos.h>
#include <kos/dbgio.h>
#include <dc/video.h>
#include <arch/arch.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "naomi.h"
#include "naomi_dimmfs.h"

#define LOG_LINES   160
#define LOG_WIDTH   100
#define SHOW_LINES  26
#define HANG_FIRST_S 120    /* the boot reads 26 MB through the cartridge port */
#define HANG_S      10

static char lines[LOG_LINES][LOG_WIDTH];
static volatile unsigned line_count;
static unsigned column;
static kthread_t *main_thread;
volatile uint32_t naomi_frames_alive;

uint32_t naomi_heap_left(void)
{
	const uintptr_t top = (uintptr_t)sbrk(0), mem_top = (uintptr_t)_arch_mem_top;

	return mem_top > top ? (uint32_t)(mem_top - top) : 0;
}

static void put_char(char c)
{
	char *line = lines[line_count % LOG_LINES];

	if (c == '\r')
		return;
	if (c == '\n')
	{
		line[column] = 0;
		++line_count;
		column = 0;
		lines[line_count % LOG_LINES][0] = 0;
		return;
	}
	if (column < LOG_WIDTH - 1)
	{
		line[column++] = c;
		line[column] = 0;
	}
}

static int ring_detected(void) { return 1; }
static int ring_init(void) { return 0; }
static int ring_shutdown(void) { return 0; }
static int ring_set_irq_usage(int mode) { (void)mode; return 0; }
static int ring_flush(void) { return 0; }
static int ring_read_buffer(uint8_t *data, int len) { (void)data; (void)len; return -1; }

static int ring_write_buffer(const uint8_t *data, int len, int xlat)
{
	const irq_mask_t old = irq_disable();

	(void)xlat;
	for (int i = 0; i < len; ++i)
		put_char((char)data[i]);
	irq_restore(old);
	return len;
}

static dbgio_handler_t ring = {
	.name = "naomi-ring",
	.detected = ring_detected,
	.init = ring_init,
	.shutdown = ring_shutdown,
	.set_irq_usage = ring_set_irq_usage,
	.flush = ring_flush,
	.write_buffer = ring_write_buffer,
	.read_buffer = ring_read_buffer,
};

void naomi_show_log(const char *why)
{
	static char copy[LOG_LINES][LOG_WIDTH];
	unsigned count, pick[SHOW_LINES], n = 0;
	const irq_mask_t old = irq_disable();

	count = line_count + (column ? 1u : 0u);
	memcpy(copy, lines, sizeof(copy));
	irq_restore(old);
	vid_init(DM_640x480, PM_RGB565);
	vid_clear(0, 0, 64);
	dbgio_dev_select("fb");
	printf("\n OUTRUNNERS (NAOMI) - %s\n\n", why);
	for (unsigned i = count, oldest = count > LOG_LINES ? count - LOG_LINES : 0; i-- > oldest && n < SHOW_LINES;)
		pick[n++] = i;
	while (n)
	{
		char line[78];

		snprintf(line, sizeof(line), "%s", copy[pick[--n] % LOG_LINES]);
		printf(" %s\n", line);
	}
	for (;;)
		thd_sleep(1000);
}

static void *watchdog(void *unused)
{
	uint32_t last = 0;
	unsigned still = 0;
	int seen = 0;

	(void)unused;
	for (;;)
	{
		thd_sleep(1000);
		if (naomi_frames_alive != last)
		{
			last = naomi_frames_alive;
			still = 0;
			seen = 1;
			continue;
		}
		if (++still >= (seen ? HANG_S : HANG_FIRST_S))
		{
			printf("NAOMI HANG: no game frame for %u s (%lu run), main thread pc %08lx pr %08lx\n", still,
				(unsigned long)last, main_thread ? (unsigned long)main_thread->context.pc : 0ul,
				main_thread ? (unsigned long)main_thread->context.pr : 0ul);
			naomi_show_log("HANG");
		}
	}
	return NULL;
}

int naomi_boot(void)
{
	const kthread_attr_t attributes = { .create_detached = true, .stack_size = 4096, .label = "naomi watchdog" };
	int files;

	setvbuf(stdout, NULL, _IONBF, 0);
	lines[0][0] = 0;
	if (dbgio_add_handler(&ring) == 0)
		dbgio_dev_select("naomi-ring");
	printf("NAOMI: OutRunners, KOS_SUBARCH=naomi\n");
	main_thread = thd_get_current();
	orunners_naomi_live.log_lines = (uint32_t)(uintptr_t)lines;
	orunners_naomi_live.log_count = LOG_LINES;
	orunners_naomi_live.log_width = LOG_WIDTH;
	orunners_naomi_live.log_next = (uint32_t)(uintptr_t)&line_count;
	files = dimmfs_init();
	printf("NAOMI: NetDIMM archive: %d files\n", files);
	if (files <= 0)
		naomi_show_log("NO ASSETS IN THE NETDIMM IMAGE (ports/naomi/build_naomi.sh)");
	if (!thd_create_ex(&attributes, watchdog, NULL))
		printf("NAOMI: no watchdog\n");
	return 0;
}
