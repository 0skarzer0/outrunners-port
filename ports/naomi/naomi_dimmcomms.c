/**
 *    Name:        naomi_dimmcomms.c
 *    Description: answers the NetDIMM's peeks and pokes, so that the netbooter's Raspberry can read the live figures
 *                 (naomi.h, live.py) and the console ring while the game runs
 *    Created:     13-11-2026
 *
 *    license:BSD-3-Clause
 *    From naomi_dimmcomms.c of Daytona USA's native port on the NAOMI (0skarzer0/daytona-arcade-recomp, branch
 *    naomi-native), its protocol as libnaomi's dimmcomms.c (DragonMinded) and dc2naomi's resident:
 *      - the DIMM raises bit 3 of Holly's external interrupt status (0xa05f6904) and leaves a command in 0xa05f703c:
 *        0x8000 = pending, bits 14-9 = id (4/5/6 peek 8/16/32, 8/9/10 poke 8/16/32), bits 8-0 with 0xa05f7040 the
 *        address's offset; the poke's value in 0xa05f7044 (low) / 0xa05f7048 (high);
 *      - firmware 3.17 sends only the low 25 bits: an offset into the 32 MB of RAM, unless a 32-bit poke to the
 *        window offset 0x01fffffc set another base;
 *      - only memory that cannot fault is touched (the rest reads 0xbadadd00);
 *      - the answer: id | 1 in the command word, the value in 0xa05f7044/48, bit 8 of 0xa05f704c set, wait for the
 *        interrupt to drop, bit 0 of 0xa05f704c cleared.
 *    Polled every 2 ms by a thread of its own, under the lock of the /dimm reads (the cartridge port's registers).
 */
#include <kos.h>
#include <arch/timer.h>
#include <stdint.h>
#include <stdio.h>

#include "naomi.h"
#include "naomi_dimmfs.h"

#define REG8(a) (*(volatile uint8_t *)(uintptr_t)(a))
#define REG16(a) (*(volatile uint16_t *)(uintptr_t)(a))
#define REG32(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define DIMM_COMMAND 0xa05f703cu
#define DIMM_OFFSETL 0xa05f7040u
#define DIMM_PARAML 0xa05f7044u
#define DIMM_PARAMH 0xa05f7048u
#define DIMM_STATUS 0xa05f704cu
#define ISTEXT 0xa05f6904u
#define DIMM_WINDOW 0x01fffffcu

/* Peek at it: 32-bit words, little-endian (naomi.h). */
volatile orunners_naomi_live_t orunners_naomi_live = {.magic = 0x4e55524fu /* "ORUN" */, .version = 1, .draw_every = 4};

static uint32_t dimm_base;

static uint32_t dimm_addr(uint32_t a) {
    const uint32_t phys = a & 0x1fffffffu;
    if (a >= 0xff000000u) return a;
    if (phys >= 0x0c000000u && phys < 0x0e000000u) return ((a >> 29) == 5 ? 0xa0000000u : 0x80000000u) | phys;
    if ((phys >= 0x005f6800u && phys < 0x005fa000u) || (phys >= 0x00700000u && phys < 0x00711000u) ||
        (phys >= 0x00800000u && phys < 0x00a00000u) || (phys >= 0x04000000u && phys < 0x05800000u))
        return 0xa0000000u | phys;
    return 0;
}

static void dimm_service(void) {
    if (!(REG32(ISTEXT) & 8)) return;
    const uint32_t cmd = REG16(DIMM_COMMAND);
    if (cmd == 0xffff || !(cmd & 0x8000)) return;
    uint32_t id = (cmd >> 9) & 0x3f, ret = 1, pl = 0, ph = 0;
    const uint32_t off = ((cmd & 0x1ff) << 16) | REG16(DIMM_OFFSETL);
    const uint32_t a = off + (dimm_base ? dimm_base : 0x0c000000u);
    const uint32_t val = (uint32_t)REG16(DIMM_PARAMH) << 16 | REG16(DIMM_PARAML);
    const uint32_t t = dimm_addr(a);
    if (off == DIMM_WINDOW && id == 10) { dimm_base = val; id = 0; }
    if (off == DIMM_WINDOW && id == 6) { pl = dimm_base & 0xffff; ph = dimm_base >> 16; id = 0; }
    switch (id) {
    case 0: case 1: break;
    case 3: dimm_base = val; break;
    case 4: pl = t ? REG8(t) : 0xad; break;
    case 5: if (a & 1) ret = 0; else pl = t ? REG16(t) : 0xbadd; break;
    case 6:
        if (a & 3) { ret = 0; break; }
        { const uint32_t v = t ? REG32(t) : 0xbadadd00u; pl = v & 0xffff; ph = v >> 16; }
        break;
    case 8: if (t) REG8(t) = (uint8_t)val; break;
    case 9: if (a & 1) ret = 0; else if (t) REG16(t) = (uint16_t)val; break;
    case 10: if (a & 3) ret = 0; else if (t) REG32(t) = val; break;
    default: ret = 0xff; break;
    }
    ++orunners_naomi_live.dimm_requests;
    REG16(DIMM_COMMAND) = (uint16_t)((cmd & 0x7e00) | ret);
    REG16(DIMM_OFFSETL) = 0;
    REG16(DIMM_PARAML) = (uint16_t)pl;
    REG16(DIMM_PARAMH) = (uint16_t)ph;
    REG16(DIMM_STATUS) = (uint16_t)(REG16(DIMM_STATUS) | 0x100);
    for (uint32_t i = 0; i < 200000 && (REG32(ISTEXT) & 8); i++) {}
    REG16(DIMM_STATUS) = (uint16_t)(REG16(DIMM_STATUS) & 0xfffe);
}

/* The figures of the last second are kept by main.c (naomi_live_second); this thread only answers peeks */
extern void naomi_live_second(void);

static void *service(void *unused) {
    (void)unused;
    uint64_t next = timer_ms_gettime64() + 1000;
    for (;;) {
        dimmfs_lock();
        dimm_service();
        dimmfs_unlock();
        if (timer_ms_gettime64() >= next) { next += 1000; naomi_live_second(); }
        thd_sleep(2);
    }
    return NULL;
}

void naomi_dimmcomms_start(void) {
    const kthread_attr_t attributes = {.create_detached = true, .stack_size = 4096, .label = "naomi dimm",
                                       .prio = PRIO_DEFAULT - 1};
    if (!thd_create_ex(&attributes, service, NULL)) { printf("NAOMI: no NetDIMM peek thread\n"); return; }
    printf("NAOMI: NetDIMM peek/poke on; live figures at %p\n", (void *)&orunners_naomi_live);
}
