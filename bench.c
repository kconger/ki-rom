/**
 * SPDX-FileCopyrightText: © 2023 Leandre Gohy <leandre.gohy@hexeo.be>
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <stdint.h>
#include "bench.h"
#include "cache.h"
#include "delay.h"
#include "ide.h"
#include "video.h"
#include "wdt.h"

/*
 * Reading Count needs the "memory" clobber for the same reason the cache
 * operations in cache.h do: volatile only stops GCC from deleting or
 * duplicating the asm, it does not stop it from moving the loads and stores
 * being measured across it. A timer read that the accesses have been sunk past
 * measures nothing.
 */
static inline uint32_t __attribute__((always_inline)) cp0_count(void)
{
    register uint32_t count;
    asm volatile("mfc0 %0,$9" : "=r"(count) : : "memory");
    return count;
}

/*
 * Count is 32 bit and wraps every 2^32 / 50MHz = 85.9 seconds. Unsigned
 * subtraction gives the right answer across one wrap, and no measurement here
 * comes close to lasting that long.
 */
static inline uint32_t __attribute__((always_inline)) cp0_elapsed(register const uint32_t start)
{
    return cp0_count() - start;
}

/*
 * Sweep all 512 lines of the 16K two-way primary data cache by index, the way
 * start.S, rom_start and patch_kix_reset already do. Way 0 is at the index
 * address, way 1 at +0x2000 (8K per way).
 *
 * This runs before every pass, cached or not, for two reasons. A cached pass
 * must start cold or it reports the tail of the previous pass still sitting in
 * the cache. An uncached pass must start with no dirty lines for the region, or
 * a writeback lands in the middle of the measurement and perturbs both the
 * timing and the data.
 */
static void dcache_writeback_invalidate(void)
{
    register uint32_t addr = BENCH_KSEG0;
    do
    {
        CACHE_OP(INDEX_WRITEBACK_INVALIDATE_D, addr, 0x0000);
        CACHE_OP(INDEX_WRITEBACK_INVALIDATE_D, addr, 0x2000);
        addr += CACHE_LINE_SIZE;
    } while (addr != BENCH_KSEG0 + 0x2000);
    SYNC();
}

static inline volatile uint64_t *__attribute__((always_inline)) bench_ptr(register const uint32_t phys, register const bool cached)
{
    return (volatile uint64_t *)(uintptr_t)((cached ? BENCH_KSEG0 : BENCH_KSEG1) + phys);
}

/*
 * The accesses are volatile so the compiler has to issue every one of them, in
 * order: a plain load whose result is discarded is dead code, and eight plain
 * stores of the same value to consecutive addresses are a memset the compiler
 * is free to rewrite. Unrolled eight times, so one iteration moves 64 bytes,
 * two 32-byte cache lines, and the pointer bump and branch cost a few percent
 * of the instructions rather than half of them.
 */
static uint32_t bench_read_pass(register volatile uint64_t *ptr, register uint32_t size)
{
    register const uint32_t start = cp0_count();
    for (register uint32_t i = size / 64u; i != 0u; i--)
    {
        (void)ptr[0];
        (void)ptr[1];
        (void)ptr[2];
        (void)ptr[3];
        (void)ptr[4];
        (void)ptr[5];
        (void)ptr[6];
        (void)ptr[7];
        ptr += 8;
    }
    return cp0_elapsed(start);
}

static uint32_t bench_write_pass(register volatile uint64_t *ptr, register uint32_t size)
{
    register const uint32_t start = cp0_count();
    for (register uint32_t i = size / 64u; i != 0u; i--)
    {
        ptr[0] = 0;
        ptr[1] = 0;
        ptr[2] = 0;
        ptr[3] = 0;
        ptr[4] = 0;
        ptr[5] = 0;
        ptr[6] = 0;
        ptr[7] = 0;
        ptr += 8;
    }
    return cp0_elapsed(start);
}

uint32_t bench_read(uint32_t phys, uint32_t size, bool cached)
{
    uint32_t best = 0xFFFFFFFFu;

    for (uint32_t pass = 0; pass < BENCH_PASSES; pass++)
    {
        dcache_writeback_invalidate();
        const uint32_t ticks = bench_read_pass(bench_ptr(phys, cached), size);
        if (ticks < best)
        {
            best = ticks;
        }
    }

    return best;
}

uint32_t bench_write(uint32_t phys, uint32_t size, bool cached)
{
    uint32_t best = 0xFFFFFFFFu;

    for (uint32_t pass = 0; pass < BENCH_PASSES; pass++)
    {
        dcache_writeback_invalidate();
        const uint32_t ticks = bench_write_pass(bench_ptr(phys, cached), size);
        if (ticks < best)
        {
            best = ticks;
        }
    }

    return best;
}

/*
 * bytes / (ticks / 50e6) / 1e6 == bytes * 50 / ticks, so the whole conversion
 * to MB/s is one multiply and one divide, and multiplying by 100 first keeps
 * two decimal places without any floating point. 64 KiB * 5000 is 3.3e8, so the
 * product has to be computed in 64 bits; -march=r4600 -mabi=o64 gives a real
 * 64-bit divide instruction for it rather than a libgcc call, which is the same
 * reason time.c can divide its 64-bit tick count.
 */
uint32_t bench_rate_x100(uint32_t count, uint32_t ticks)
{
    if (ticks == 0u)
    {
        return 0u;
    }

    return (uint32_t)(((uint64_t)count * (uint64_t)(BENCH_TICKS_PER_US * 100u)) / (uint64_t)ticks);
}

uint32_t bench_cycles_x100(uint32_t ops, uint32_t ticks)
{
    if (ops == 0u)
    {
        return 0u;
    }

    // ticks * 2 pipeline cycles per tick, * 100 for the two decimal places.
    return (uint32_t)(((uint64_t)ticks * 200ull) / (uint64_t)ops);
}

uint32_t bench_hz_x100(uint32_t ticks)
{
    if (ticks == 0u)
    {
        return 0u;
    }

    return (uint32_t)((BENCH_TICKS_PER_US * 1000000ull * 100ull) / (uint64_t)ticks);
}

/*
 * The 256 bytes cpu_bench_load() walks, which is eight 32-byte lines out of the
 * 512 the primary data cache holds. Aligned so that it is exactly those eight
 * lines and not nine.
 */
static _Alignas(CACHE_LINE_SIZE) const uint64_t cpu_load_buffer[32] = {0};

extern void cpu_bench_alu(uint32_t iterations, const void *base);
extern void cpu_bench_load(uint32_t iterations, const void *base);
extern void cpu_bench_mul(uint32_t iterations, const void *base);
extern void cpu_bench_div(uint32_t iterations, const void *base);
extern void cpu_bench_fpu(uint32_t iterations, const void *base);

typedef struct
{
    void (*kernel)(uint32_t iterations, const void *base);
    uint32_t iterations;
} cpu_test_t;

/*
 * Iteration counts are picked so every kernel runs for a few milliseconds:
 * long enough that the 20ns tick is noise, short enough that a frame is not
 * held up. They differ because the instructions do: a divide costs tens of
 * cycles where an addu costs one.
 */
static const cpu_test_t cpu_tests[BENCH_CPU_COUNT] = {
    [BENCH_CPU_ALU] = {cpu_bench_alu, 2048},
    [BENCH_CPU_LOAD] = {cpu_bench_load, 2048},
    [BENCH_CPU_MUL] = {cpu_bench_mul, 512},
    [BENCH_CPU_DIV] = {cpu_bench_div, 256},
    [BENCH_CPU_FPU] = {cpu_bench_fpu, 1024},
};

/*
 * Status.CU1. start.S enables it on every board, so this should always be true
 * -- but cpu_bench_fpu() is the one kernel that would take an unrecoverable
 * Coprocessor Unusable exception if it were not, and on this ROM any exception
 * means a silent restart. Cheap enough to check.
 */
bool bench_fpu_available(void)
{
    register uint32_t status;
    asm volatile("mfc0 %0,$12" : "=r"(status));

    return (status & (1u << 29)) != 0u;
}

uint32_t bench_cpu(bench_cpu_test_t test, uint32_t *ops)
{
    const cpu_test_t *const entry = &cpu_tests[test];
    uint32_t best = 0xFFFFFFFFu;

    *ops = entry->iterations * BENCH_CPU_OPS_PER_ITER;

    for (uint32_t pass = 0; pass < BENCH_PASSES; pass++)
    {
        /*
         * Deliberately no cache sweep here, unlike the memory kernels. These
         * loops have to run out of the instruction cache, and cpu_bench_load()
         * has to hit in the data cache, or what comes back is a memory
         * measurement wearing a CPU label. The first pass warms both caches and
         * keeping the fastest of three discards it.
         */
        const uint32_t start = cp0_count();
        entry->kernel(entry->iterations, cpu_load_buffer);
        const uint32_t ticks = cp0_elapsed(start);

        if (ticks < best)
        {
            best = ticks;
        }
    }

    return best;
}

/*
 * Waits out one frame to land on an edge, then times `frames` whole periods.
 * This blocks for as many frames as it measures -- a fraction of a second, far
 * inside the watchdog period, and it only runs once per test run.
 */
uint32_t bench_vsync_ticks(uint32_t frames)
{
    if (frames == 0u)
    {
        return 0u;
    }

    video_vsync_wait();

    const uint32_t start = cp0_count();
    for (uint32_t frame = 0; frame < frames; frame++)
    {
        video_vsync_wait();
    }

    return cp0_elapsed(start) / frames;
}

/*
 * The IDE waits below are bounded by elapsed time instead of by an iteration
 * count, and they deliberately do not reuse ide_wait_ready()/ide_ack().
 *
 * Those two spin for up to 0x5f0000 and 0x2460000 iterations, and with no drive
 * fitted the longer of them runs for something like two seconds. The MAX705 on
 * both boards resets the machine if the watchdog is not kicked for roughly
 * 1.6s, so calling them with no disk attached would turn this test ROM into a
 * reset loop. 500ms is far longer than any drive needs to answer and leaves a
 * wide margin under the watchdog, and the caller kicks it between sectors.
 */
#define BENCH_IDE_TIMEOUT_TICKS (500u * 1000u * BENCH_TICKS_PER_US)

static bool bench_ide_wait_ready(void)
{
    const uint32_t start = cp0_count();
    do
    {
        // BSY clear and DRDY set. An absent drive floats the bus to 0x00 or
        // 0xFF, and neither satisfies this, so it times out instead of hanging.
        if (((gIDEControl.alternateStatus ^ 0x40u) & 0xC0u) == 0u)
        {
            return true;
        }
    } while (cp0_elapsed(start) < BENCH_IDE_TIMEOUT_TICKS);

    return false;
}

/*
 * The drive raises INTRQ when a sector is ready. Interrupts are masked, so the
 * pending line is read straight out of the Cause register the way ide_ack()
 * does; reading the status register afterwards is what clears it.
 */
static bool bench_ide_wait_irq(void)
{
    const uint32_t start = cp0_count();
    register uint32_t cause;
    do
    {
        asm volatile("mfc0 %0,$13" : "=r"(cause));
        if ((cause & 0x800u) != 0u)
        {
            return true;
        }
    } while (cp0_elapsed(start) < BENCH_IDE_TIMEOUT_TICKS);

    return false;
}

/*
 * Same sequence as ide_init(), with the bounded waits above. The geometry has
 * to stay in step with ide_seek(): 0x28 sectors per track, 0xd as the highest
 * head number.
 */
bench_ide_status_t bench_ide_init(void)
{
    gIDEControl.deviceControl = 0xC; // Put device in Soft reset state
    delay(10);                       // Datasheet says 5ms before exiting reset state
    gIDEControl.deviceControl = 0x8; // Disable reset (!SRST) & Interrupt enabled (!IEN)

    wdt_reset();
    if (!bench_ide_wait_ready())
    {
        return BENCH_IDE_NOT_READY;
    }

    gIDE.sectorCount = 0x28; // Number of sector per track
    gIDE.device = 0xd;       // Number of head - 1 per cylinder
    gIDE.command = 0x91;     // Initialize device parameters

    wdt_reset();
    if (!bench_ide_wait_irq())
    {
        return BENCH_IDE_NOT_READY;
    }

    return (gIDE.status & 0x1u) != 0u ? BENCH_IDE_ERROR : BENCH_IDE_OK;
}

/*
 * Times a READ SECTORS from the command write to the last byte of the last
 * sector, so the figure covers command overhead and the drive latency as well
 * as the PIO transfer itself, which is everything the game pays for the same
 * read.
 *
 * `sectors` must be 1..256, and the destination is written through kseg0, which
 * is what the game does and what ide_read_sector_bytes() expects.
 */
bench_ide_status_t bench_ide_read(uint32_t lba, uint32_t sectors, uint32_t dst_phys, uint32_t *ticks)
{
    uint16_t *dst = (uint16_t *)(uintptr_t)(BENCH_KSEG0 + dst_phys);

    wdt_reset();
    if (!bench_ide_wait_ready())
    {
        return BENCH_IDE_NOT_READY;
    }

    ide_seek(lba, (uint8_t)(sectors & 0xFFu));

    const uint32_t start = cp0_count();
    gIDE.command = 0x20; // Read sectors command

    for (uint32_t sector = 0; sector < sectors; sector++)
    {
        // One kick per sector. Each sector wait is bounded by the 500ms above,
        // so the gap between kicks stays far below the watchdog period even if
        // the drive is having a bad day.
        wdt_reset();

        if (!bench_ide_wait_irq())
        {
            return BENCH_IDE_NO_DATA;
        }

        if ((gIDE.status & 0x1u) != 0u)
        {
            return BENCH_IDE_ERROR;
        }

        ide_read_sector_bytes(dst);
        dst += 0x100; // 256 halfwords = one 512 byte sector
    }

    *ticks = cp0_elapsed(start);
    return BENCH_IDE_OK;
}
