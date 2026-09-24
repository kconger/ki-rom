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
 * The two write-allocate passes. They step the pointer by the same 32 bytes and
 * so touch the same lines in the same order; the only difference is that one
 * writes all four doublewords of a line and the other writes one and leaves the
 * remaining 24 bytes untouched.
 *
 * That is the whole experiment. A line the CPU only partly writes has to be
 * fetched from SDRAM first, because the bytes that are not stored still have to
 * end up correct. A line that is written end to end does not, and the core's
 * skip-fill exists to notice that and drop the fetch. If it is working the first
 * pass moves 32 bytes per line and the second moves 64, and the per-line cost
 * says so; if the two come out level, it is not firing.
 *
 * ptr[0] is the only store in the sparse pass on purpose: the first doubleword
 * of the line, so a fill that IS issued starts at the same beat in both.
 */
static uint32_t bench_write_line_pass(register volatile uint64_t *ptr, register uint32_t size)
{
    register const uint32_t start = cp0_count();
    for (register uint32_t i = size / CACHE_LINE_SIZE; i != 0u; i--)
    {
        ptr[0] = 0;
        ptr[1] = 0;
        ptr[2] = 0;
        ptr[3] = 0;
        ptr += 4;
    }
    return cp0_elapsed(start);
}

static uint32_t bench_write_qword_pass(register volatile uint64_t *ptr, register uint32_t size)
{
    register const uint32_t start = cp0_count();
    for (register uint32_t i = size / CACHE_LINE_SIZE; i != 0u; i--)
    {
        ptr[0] = 0;
        ptr += 4;
    }
    return cp0_elapsed(start);
}

uint32_t bench_write_lines(uint32_t phys, uint32_t size, bool whole_line)
{
    uint32_t best = 0xFFFFFFFFu;

    for (uint32_t pass = 0; pass < BENCH_PASSES; pass++)
    {
        /*
         * Cached, so the sweep is not optional: a warm start would find a
         * quarter of a 64 KiB window already resident and dirty, and the pass
         * would report write hits rather than the allocate this is about.
         */
        dcache_writeback_invalidate();

        const uint32_t ticks = whole_line
                                   ? bench_write_line_pass(bench_ptr(phys, true), size)
                                   : bench_write_qword_pass(bench_ptr(phys, true), size);
        if (ticks < best)
        {
            best = ticks;
        }
    }

    return best;
}

extern void mem_bench_chase(uint32_t iterations, const void *head);

/*
 * The shuffled chase order is x -> (x * A + C) mod nodes, with `nodes` a power
 * of two, A congruent to 1 mod 4 and C odd. Those are the Hull-Dobell
 * conditions for a modulus that is a power of two, so the sequence has full
 * period and the links it lays down form exactly one cycle through every node.
 *
 * A fixed permutation rather than a random one deliberately: the same walk on
 * every run means two builds of the core are compared on the same addresses in
 * the same order, and that is worth more here than unpredictability. What the
 * order has to defeat is not analysis, it is PREDICTION -- an ascending walk is
 * precisely what the data cache's next-line read-ahead was added to serve, and
 * under it this would stop being a latency test and quietly become bench_read().
 */
#define CHASE_LCG_A 1229u
#define CHASE_LCG_C 1597u

static inline volatile uint32_t *__attribute__((always_inline)) chase_node(register const uint32_t head, register const uint32_t index, register const uint32_t stride)
{
    return (volatile uint32_t *)(uintptr_t)(head + (index * stride));
}

/*
 * Walks the chain that mem_bench_chase() is about to walk, in C, with every
 * pointer checked, and proves it is one cycle of exactly `steps` nodes: back at
 * the head on the last step and never before it. In a functional graph those
 * two facts together mean the cycle through the head has length `steps`, so
 * every node on it is distinct -- which is what makes "one lap" mean one lap.
 *
 * This is not a check on the arithmetic above, it is a check on the memory.
 * mem_bench_chase() puts no bound on where a pointer leads, so a single word
 * that DRAM did not return correctly sends it to an arbitrary address, and the
 * first unaligned or unmapped one takes an exception. On this ROM an exception
 * is a silent restart: the screen would simply begin again, with no row to say
 * which test did it.
 */
static bool chase_verify(const uint32_t head, const uint32_t steps, const uint32_t lo, const uint32_t hi, const uint32_t stride)
{
    uint32_t p = head;

    for (uint32_t n = 0; n < steps; n++)
    {
        p = *(volatile uint32_t *)(uintptr_t)p;

        if (p < lo || p > hi || ((p - lo) % stride) != 0u)
        {
            return false;
        }

        if (p == head && (n + 1u) != steps)
        {
            return false;
        }
    }

    return p == head;
}

bool bench_chase_build(uint32_t phys, uint32_t nodes, uint32_t stride, bool shuffle)
{
    const uint32_t head = BENCH_KSEG0 + phys;

    // One lap has to be a whole number of kernel iterations, and a node has to
    // own its cache line or two of them share a miss and the count is wrong.
    if (nodes == 0u || (nodes % BENCH_CPU_OPS_PER_ITER) != 0u || stride < CACHE_LINE_SIZE)
    {
        return false;
    }

    // The full-period argument for the LCG needs a power of two modulus. A
    // shuffle over anything else would still link a chain, just not necessarily
    // a single one covering every node.
    if (shuffle && (nodes & (nodes - 1u)) != 0u)
    {
        return false;
    }

    uint32_t index = 0;
    for (uint32_t n = 0; n < nodes; n++)
    {
        const uint32_t next = shuffle
                                  ? (((index * CHASE_LCG_A) + CHASE_LCG_C) & (nodes - 1u))
                                  : (((index + 1u) == nodes) ? 0u : (index + 1u));

        *chase_node(head, index, stride) = head + (next * stride);
        index = next;
    }

    return chase_verify(head, nodes, head, head + ((nodes - 1u) * stride), stride);
}

bool bench_chase_build_pair(uint32_t phys_a, uint32_t phys_b, uint32_t nodes)
{
    const uint32_t head_a = BENCH_KSEG0 + phys_a;
    const uint32_t head_b = BENCH_KSEG0 + phys_b;

    if (nodes == 0u || ((nodes * 2u) % BENCH_CPU_OPS_PER_ITER) != 0u || phys_b <= phys_a)
    {
        return false;
    }

    // A[n] -> B[n] -> A[n+1]: every single step crosses between the two bases,
    // so there is no run of accesses that could sit in one open row.
    for (uint32_t n = 0; n < nodes; n++)
    {
        const uint32_t next = ((n + 1u) == nodes) ? 0u : (n + 1u);

        *chase_node(head_a, n, CACHE_LINE_SIZE) = head_b + (n * CACHE_LINE_SIZE);
        *chase_node(head_b, n, CACHE_LINE_SIZE) = head_a + (next * CACHE_LINE_SIZE);
    }

    return chase_verify(head_a, nodes * 2u, head_a,
                        head_b + ((nodes - 1u) * CACHE_LINE_SIZE), CACHE_LINE_SIZE);
}

uint32_t bench_chase_run(uint32_t phys, uint32_t steps)
{
    const void *const head = (const void *)(uintptr_t)(BENCH_KSEG0 + phys);
    uint32_t best = 0xFFFFFFFFu;

    for (uint32_t pass = 0; pass < BENCH_PASSES; pass++)
    {
        wdt_reset();

        /*
         * The sweep is what makes every step a miss. It is also what gets the
         * chain itself out to DRAM: it writes back before it invalidates, so
         * the pointers this pass is about to follow are in memory and not only
         * in the cache that is being thrown away.
         */
        dcache_writeback_invalidate();

        const uint32_t start = cp0_count();
        mem_bench_chase(steps / BENCH_CPU_OPS_PER_ITER, head);
        const uint32_t ticks = cp0_elapsed(start);

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
 * The 256 bytes cpu_bench_load() and cpu_bench_load_indep() walk, which is eight
 * 32-byte lines out of the 512 the primary data cache holds. Aligned so that it
 * is exactly those eight lines and not nine.
 */
static _Alignas(CACHE_LINE_SIZE) const uint64_t cpu_load_buffer[32] = {0};

/*
 * The same eight lines again, but writable, so BENCH_CPU_LOAD_DEP can chain
 * them: cpu_load_buffer is const and lives in .rodata. All three load variants
 * therefore work over 256 bytes of resident data cache and differ only in how
 * their loads depend on each other.
 */
static _Alignas(CACHE_LINE_SIZE) uint32_t cpu_chase_buffer[64] = {0};

#define CPU_CHASE_NODES 8u

extern void cpu_bench_alu(uint32_t iterations, const void *base);
extern void cpu_bench_load(uint32_t iterations, const void *base);
extern void cpu_bench_load_indep(uint32_t iterations, const void *base);
extern void cpu_bench_mul(uint32_t iterations, const void *base);
extern void cpu_bench_div(uint32_t iterations, const void *base);
extern void cpu_bench_fpu(uint32_t iterations, const void *base);

typedef struct
{
    void (*kernel)(uint32_t iterations, const void *base);
    const void *base;
    uint32_t iterations;
} cpu_test_t;

/*
 * Iteration counts are picked so every kernel runs for a few milliseconds:
 * long enough that the 20ns tick is noise, short enough that a frame is not
 * held up. They differ because the instructions do: a divide costs tens of
 * cycles where an addu costs one.
 *
 * `base` is carried per test rather than passed by the caller because
 * BENCH_CPU_LOAD_DEP is the one kernel that needs a buffer it can write.
 */
static const cpu_test_t cpu_tests[BENCH_CPU_COUNT] = {
    [BENCH_CPU_ALU] = {cpu_bench_alu, cpu_load_buffer, 2048},
    [BENCH_CPU_LOAD] = {cpu_bench_load, cpu_load_buffer, 2048},
    [BENCH_CPU_MUL] = {cpu_bench_mul, cpu_load_buffer, 512},
    [BENCH_CPU_DIV] = {cpu_bench_div, cpu_load_buffer, 256},
    [BENCH_CPU_FPU] = {cpu_bench_fpu, cpu_load_buffer, 1024},
    [BENCH_CPU_LOAD_8] = {cpu_bench_load_indep, cpu_load_buffer, 2048},
    [BENCH_CPU_LOAD_DEP] = {mem_bench_chase, cpu_chase_buffer, 2048},
};

/*
 * Eight nodes, one cache line apart, linked three apart so the walk is 0, 3, 6,
 * 1, 4, 7, 2, 5 and back: three and eight are coprime, so that is a single
 * cycle over all eight. The order buys nothing here -- everything hits either
 * way -- but a chain that ran 0,1,2,... would be indistinguishable at a glance
 * from the straight-line kernels it is meant to be contrasted with.
 *
 * Built at measurement time and not once at boot because .bss starts zeroed,
 * and a chain of nulls is an lw from address 0 followed by a restart.
 */
static void cpu_chase_build(void)
{
    for (uint32_t i = 0; i < CPU_CHASE_NODES; i++)
    {
        const uint32_t next = (i + 3u) & (CPU_CHASE_NODES - 1u);

        cpu_chase_buffer[i * (CACHE_LINE_SIZE / sizeof(uint32_t))] =
            (uint32_t)(uintptr_t)&cpu_chase_buffer[next * (CACHE_LINE_SIZE / sizeof(uint32_t))];
    }
}

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

    if (test == BENCH_CPU_LOAD_DEP)
    {
        cpu_chase_build();
    }

    for (uint32_t pass = 0; pass < BENCH_PASSES; pass++)
    {
        /*
         * Deliberately no cache sweep here, unlike the memory kernels. These
         * loops have to run out of the instruction cache, and the three load
         * variants have to hit in the data cache, or what comes back is a
         * memory measurement wearing a CPU label. The first pass warms both
         * caches -- and writes back the lines cpu_chase_build() just dirtied --
         * and keeping the fastest of three discards it.
         */
        const uint32_t start = cp0_count();
        entry->kernel(entry->iterations, entry->base);
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
