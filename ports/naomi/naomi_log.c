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

static void __attribute__((noreturn)) show_log(const char *why, int in_exception)
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
		if (in_exception)
			continue;
		else
			thd_sleep(1000);
}

void naomi_show_log(const char *why)
{
	show_log(why, 0);
}

/* ---- misaligned loads and stores ----

   The V60 reads and writes words at any address, and the game's C does as its code did (src/objects/scenery.c's
   flashing scenery reads a palette list at an odd address, found on the board, 2026-11-13); the SH-4 takes an
   address error instead. The handler does the access a byte at a time (little endian, as the SH-4's) and goes
   on after the instruction (in a branch's delay slot too: then the branch is done as well). Not the FPU's
   moves: those stay errors. */

static uint32_t load_bytes(uint32_t address, int size)
{
	const volatile uint8_t *at = (const volatile uint8_t *)(uintptr_t)address;
	uint32_t value = 0;

	for (int index = size - 1; index >= 0; index--)
		value = value << 8 | at[index];
	return size == 2 ? (uint32_t)(int32_t)(int16_t)value : value;
}

static void store_bytes(uint32_t address, uint32_t value, int size)
{
	volatile uint8_t *at = (volatile uint8_t *)(uintptr_t)address;

	for (int index = 0; index < size; index++, value >>= 8)
		at[index] = (uint8_t)value;
}

static bool emulate_access(irq_context_t *context, uint16_t op)
{
	const int n = op >> 8 & 15, m = op >> 4 & 15, low = op & 15;
	uint32_t *r = context->r;

	switch (op >> 12)
	{
		case 0x6:    /* mov.w/l @Rm,Rn and @Rm+,Rn */
			if (low == 1 || low == 2)
				r[n] = load_bytes(r[m], low == 1 ? 2 : 4);
			else if (low == 5 || low == 6)
			{
				const int size = low == 5 ? 2 : 4;
				const uint32_t value = load_bytes(r[m], size);

				if (n != m)
					r[m] += (uint32_t)size;
				r[n] = value;
			}
			else
				return false;
			break;
		case 0x5:    /* mov.l @(disp,Rm),Rn */
			r[n] = load_bytes(r[m] + (uint32_t)(low * 4), 4);
			break;
		case 0x2:    /* mov.w/l Rm,@Rn and Rm,@-Rn */
			if (low == 1 || low == 2)
				store_bytes(r[n], r[m], low == 1 ? 2 : 4);
			else if (low == 5 || low == 6)
			{
				const int size = low == 5 ? 2 : 4;

				store_bytes(r[n] - (uint32_t)size, r[m], size);
				r[n] -= (uint32_t)size;
			}
			else
				return false;
			break;
		case 0x1:    /* mov.l Rm,@(disp,Rn) */
			store_bytes(r[n] + (uint32_t)(low * 4), r[m], 4);
			break;
		case 0x0:    /* @(R0,Rm) and @(R0,Rn) */
			if (low == 0xd || low == 0xe)
				r[n] = load_bytes(r[0] + r[m], low == 0xd ? 2 : 4);
			else if (low == 5 || low == 6)
				store_bytes(r[0] + r[n], r[m], low == 5 ? 2 : 4);
			else
				return false;
			break;
		case 0x8:
			if (n == 5)         /* mov.w @(disp,Rm),R0 */
				r[0] = load_bytes(r[m] + (uint32_t)(low * 2), 2);
			else if (n == 1)    /* mov.w R0,@(disp,Rn) */
				store_bytes(r[m] + (uint32_t)(low * 2), r[0], 2);
			else
				return false;
			break;
		case 0xc:
			switch (n)
			{
				case 1: store_bytes(context->gbr + (uint32_t)(op & 0xff) * 2, r[0], 2); break;
				case 2: store_bytes(context->gbr + (uint32_t)(op & 0xff) * 4, r[0], 4); break;
				case 5: r[0] = load_bytes(context->gbr + (uint32_t)(op & 0xff) * 2, 2); break;
				case 6: r[0] = load_bytes(context->gbr + (uint32_t)(op & 0xff) * 4, 4); break;
				default: return false;
			}
			break;
		default:
			return false;
	}
	return true;
}

/* A delayed branch, its target if it is taken (in *target; *taken), whether it is one at all */
static bool delayed_branch(const irq_context_t *context, uint16_t op, uint32_t *target, bool *taken)
{
	const uint32_t pc = context->pc;
	const int n = op >> 8 & 15;

	*taken = true;
	if ((op & 0xf000) == 0xa000 || (op & 0xf000) == 0xb000)    /* bra, bsr */
		*target = pc + 4 + (uint32_t)((int32_t)((op & 0xfff) << 20) >> 19);
	else if ((op & 0xff00) == 0x8d00 || (op & 0xff00) == 0x8f00)    /* bt/s, bf/s */
	{
		*target = pc + 4 + (uint32_t)((int32_t)(int8_t)(op & 0xff) * 2);
		*taken = ((context->sr & 1) != 0) == ((op & 0xff00) == 0x8d00);
	}
	else if ((op & 0xf0ff) == 0x402b || (op & 0xf0ff) == 0x400b)    /* jmp, jsr @Rn */
		*target = context->r[n];
	else if (op == 0x000b)    /* rts */
		*target = context->pr;
	else if ((op & 0xf0ff) == 0x0023 || (op & 0xf0ff) == 0x0003)    /* braf, bsrf Rn */
		*target = pc + 4 + context->r[n];
	else
		return false;
	return true;
}

static bool emulate_misaligned(irq_context_t *context)
{
	const uint16_t op = *(const uint16_t *)(uintptr_t)context->pc;
	uint32_t target;
	bool taken;

	if (delayed_branch(context, op, &target, &taken))
	{
		/* the access is in the branch's delay slot: it, then the branch */
		const uint32_t after = context->pc + 4;
		const bool subroutine = (op & 0xf000) == 0xb000 || (op & 0xf0ff) == 0x400b || (op & 0xf0ff) == 0x0003;

		if (!emulate_access(context, *(const uint16_t *)(uintptr_t)(context->pc + 2)))
			return false;
		if (subroutine)
			context->pr = after;
		context->pc = taken ? target : after;
	}
	else
	{
		if (!emulate_access(context, op))
			return false;
		context->pc += 2;
	}
	orunners_naomi_live.misaligned++;
	return true;
}

/* An exception nothing handles (an address error, an illegal instruction...): where it was, then the console on
   the screen. Without this KallistiOS prints the registers and stops, and a cabinet shows black */
static void on_exception(irq_t code, irq_context_t *context, void *data)
{
	(void)data;
	if ((code == EXC_DATA_ADDRESS_READ || code == EXC_DATA_ADDRESS_WRITE) && emulate_misaligned(context))
	{
		static uint32_t told;

		if (told < 8)
		{
			told++;
			printf("NAOMI: a misaligned access (address %08lx), done a byte at a time\n",
				(unsigned long)*(volatile uint32_t *)0xff00000c);
		}
		return;
	}
	printf("NAOMI EXCEPTION %03lx at pc %08lx pr %08lx, address %08lx, r15 %08lx, thread %s\n",
		(unsigned long)code, (unsigned long)context->pc, (unsigned long)context->pr,
		(unsigned long)*(volatile uint32_t *)0xff00000c, (unsigned long)context->r[15],
		thd_get_current() ? thd_get_current()->label : "?");
	orunners_naomi_live.version = 0xdead0000u | (uint32_t)code;
	/* (no screen from here: vid_init waits for interrupts that an exception has off; the peeks instead) */
	naomi_dimmcomms_forever();
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
			printf("NAOMI HANG: no game frame for %u s (%lu run), game thread pc %08lx pr %08lx\n", still,
				(unsigned long)last, main_thread ? (unsigned long)main_thread->context.pc : 0ul,
				main_thread ? (unsigned long)main_thread->context.pr : 0ul);
			naomi_show_log("HANG");
		}
	}
	return NULL;
}

void naomi_watch_thread(kthread_t *thread)
{
	main_thread = thread;
}

/* A sampling profiler: every millisecond, where the game's thread was when it was interrupted (its saved pc),
   into a ring the Raspberry reads (profile.py) and sh-elf-addr2line names */
#define PROFILE_SAMPLES 4096
static uint32_t profile_pcs[PROFILE_SAMPLES];

static void *profiler(void *unused)
{
	(void)unused;
	for (;;)
	{
		thd_sleep(1);
		if (main_thread && main_thread != thd_get_current())
		{
			profile_pcs[orunners_naomi_live.profile_count % PROFILE_SAMPLES] = main_thread->context.pc;
			orunners_naomi_live.profile_count++;
		}
	}
	return NULL;
}

int naomi_boot(void)
{
	const kthread_attr_t attributes = { .create_detached = true, .stack_size = 8192, .prio = PRIO_DEFAULT - 2,
		.label = "naomi watchdog" };    /* (above the game's thread, which never waits) */
	int files;

	setvbuf(stdout, NULL, _IONBF, 0);
	lines[0][0] = 0;
	if (dbgio_add_handler(&ring) == 0)
		dbgio_dev_select("naomi-ring");
	printf("NAOMI: OutRunners, KOS_SUBARCH=naomi\n");
	main_thread = thd_get_current();
	irq_set_handler(EXC_UNHANDLED_EXC, on_exception, NULL);
	irq_set_handler(EXC_DATA_ADDRESS_READ, on_exception, NULL);
	irq_set_handler(EXC_DATA_ADDRESS_WRITE, on_exception, NULL);
	irq_set_handler(EXC_DOUBLE_FAULT, on_exception, NULL);
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
	{
		const kthread_attr_t profiling = { .create_detached = true, .stack_size = 4096, .prio = PRIO_DEFAULT - 3,
			.label = "naomi profiler" };

		orunners_naomi_live.profile_pcs = (uint32_t)(uintptr_t)profile_pcs;
		orunners_naomi_live.profile_size = PROFILE_SAMPLES;
		if (!thd_create_ex(&profiling, profiler, NULL))
			printf("NAOMI: no profiler\n");
	}
	return 0;
}
