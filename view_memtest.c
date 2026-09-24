/**
 * SPDX-FileCopyrightText: © 2023 Leandre Gohy <leandre.gohy@hexeo.be>
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <stdint.h>
#include "view.h"
#include "bench.h"
#include "draw.h"
#include "io.h"
#include "ki.h"
#include "print.h"
#include "video.h"

/*
 * Physical addresses, the way NOTES.md ("Memory > Map") lists them. SRAM sits
 * at 0x00000000 and DRAM at 0x08000000; bench.c adds 0x80000000 for a cached
 * access and 0xA0000000 for an uncached one.
 *
 * boot.ld describes all 512 KiB as one SRAM with the VRAM banks as two windows
 * inside it, so on that description every row here should report the same
 * speed. Whether a given board or core agrees is what the sweep page exists to
 * answer; nothing in this file assumes either way.
 *
 * Geometry worth knowing when reading a window: a visible framebuffer is
 * 0x25800 bytes (320*240*2) while a bank spans 0x28000, so the last 0x2800 of
 * each bank is never scanned out. A window straddling a boundary reports the
 * blend of whatever rates lie either side of it.
 */
#define PHYS_SRAM 0x00000000u
#define PHYS_VRAM0 0x00030000u
#define PHYS_VRAM1 0x00058000u
#define PHYS_DRAM 0x08000000u

/*
 * SRAM is the one region that cannot be written wherever it likes. boot.ld puts
 * .data and .bss at 0x80000200, the stack growing down from 0x80004000 (with
 * the detour gateway stacks below it) and the 64 KiB heap at 0x80004000, which
 * leaves 0x80014000 up to VRAM bank 0 at 0x80030000 unused: 112 KiB. The window
 * goes at the bottom of that gap, and sram_window_is_free() re-derives the
 * bound from the linker symbols at runtime so that growing the heap turns the
 * write test off instead of quietly corrupting it.
 */
#define PHYS_SRAM_WINDOW 0x00014000u

// Anywhere in DRAM will do; 1 MiB in keeps it clear of the DRAM window itself.
#define PHYS_IDE_BUFFER 0x08100000u

/*
 * Windows for the latency page. ROM=memtest embeds no game image, so the whole
 * 8 MiB of DRAM is free; these are simply placed clear of the region window at
 * 0x08000000 and the IDE buffer at 0x08100000, and clear of each other.
 *
 *   0x08200000  128 KiB   4096 chase nodes, one cache line apart
 *   0x08300000    1 MiB   128 stride nodes, at up to the 8 KiB stride
 *   0x08500000    2 KiB   bank pair, same bank
 *   0x08600000    1 KiB } bank pair, different banks
 *   0x08700000    1 KiB }
 */
#define PHYS_LAT_CHASE 0x08200000u
#define PHYS_LAT_STRIDE 0x08300000u

/*
 * The bank-conflict pair, and the one set of addresses on this page that is not
 * arbitrary.
 *
 * ki_sdram_burst.sv maps row = addr[22:10] and bank = addr[24:23], and holds
 * exactly ONE row open across all four banks. So two addresses in the same bank
 * and different rows cost a PRECHARGE and an ACTIVATE on every alternation,
 * where two in different banks need not -- if the controller ever learns to
 * hold more than one row at a time.
 *
 * Those are SDRAM addresses, though, not CPU physical ones. ki_memory_bridge.sv
 * puts low RAM at storage 0 and the 8 MiB main RAM at storage 0x100000, so the
 * bank bit -- storage 0x800000 -- lands at CPU physical 0x08700000, two thirds
 * of the way up DRAM and on no round number at all.
 *
 * Which makes the obvious pair, two pointers 8 MiB apart, not merely the wrong
 * distance but the worst available one: 0x08000000 + 8 MiB is 0x08800000, and
 * that is KI_MAIN_RAM_ALIAS_BASE, the 1 MiB window the bridge folds back onto
 * storage 0x100000. The two pointers would be the same SDRAM address -- same
 * bank, same row, same line -- and the test would report a cache hit as though
 * it were the prize.
 *
 * On the core as it stands both rows below are expected to come out slow, since
 * one open row is one open row whichever bank it is in. That is the point: the
 * gap between these two numbers is the whole of what multi-bank support could
 * buy, priced before anyone builds it.
 */
#define PHYS_BANK_SAME_A 0x08500000u
#define PHYS_BANK_SAME_B 0x08500400u // +1 KiB: next row, same bank
#define PHYS_BANK_DIFF_A 0x08600000u // storage 0x0700000, bank 0
#define PHYS_BANK_DIFF_B 0x08700000u // storage 0x0800000, bank 1

/*
 * 4096 nodes one cache line apart is 128 KiB, eight times the 16 KiB primary
 * data cache, and one measurement walks it exactly once. Once matters: a second
 * lap would find an eighth of the nodes still resident and report the average of
 * a miss and a hit under the name of a miss.
 */
#define LAT_CHASE_NODES 4096u

// 128 nodes at the 8 KiB stride is exactly the 1 MiB reserved above. Same count
// at every stride, so the nine numbers differ only in the distance walked.
#define LAT_STRIDE_NODES 128u

/*
 * 32 nodes fill one 1 KiB row exactly, and the pair walks 64 lines in all --
 * one lap, with the cache swept first, so every one of them is a compulsory
 * miss. Anything larger would stop the "same bank" pair being two rows.
 */
#define LAT_BANK_NODES 32u

extern uint8_t _heap_vma[];
extern uint8_t _heap_size[];

#define COLOR_TITLE 0x07FFu  // yellow
#define COLOR_HEADER 0x7FE0u // cyan
#define COLOR_VALUE 0x7FFFu  // white
#define COLOR_ERROR 0x001Fu  // red
#define COLOR_NOTE 0x35ADu   // grey
#define COLOR_RULE 0x2529u   // dark grey

#define COL_REGION 2
#define COL_RANGE 40
#define COL_UNC_RD 136
#define COL_UNC_WR 178
#define COL_CAC_RD 220
#define COL_CAC_WR 262
#define COL_VALUE_W 36 // six characters at the 6px advance

// The CPU results are label/value pairs rather than a wide table, so they go two
// pairs to a row and the whole block costs four rows instead of seven.
#define COL_CPU_L_NAME 2
#define COL_CPU_L_VAL 92
#define COL_CPU_R_NAME 150
#define COL_CPU_R_VAL 240
#define COL_CPU_MIPS 216

#define ROW_TITLE 4
#define ROW_BOARD 16
#define ROW_RULE_TOP 28
#define ROW_HDR1 32
#define ROW_HDR2 44
#define ROW_RULE_HDR 56
#define ROW_DATA 60
#define ROW_PITCH 12
#define ROW_RULE_IDE 108
#define ROW_IDE 112
#define ROW_RULE_CPU 124
#define ROW_CPU 128
#define ROW_CPU_DATA 140
#define ROW_RULE_NOTES 176
#define ROW_NOTES 180
#define ROW_STATUS 204

// A measurement that has not run yet, and one that was deliberately skipped.
#define RESULT_PENDING 0u
#define RESULT_SKIPPED 0xFFFFFFFFu

/*
 * The second page walks a small read window across the whole 512 KiB of SRAM to
 * find out where its speed changes. It exists because the main page reports the
 * low SRAM and the two VRAM banks at very different speeds, even though boot.ld
 * describes them as one device: either there is a real boundary in there or the
 * measurement is wrong, and 32 points either side of it settle which.
 *
 * Read only, deliberately. Reads are safe at any SRAM address -- including over
 * .data, .bss and the stack -- whereas writes are not, and the uncached read is
 * where the discrepancy showed up anyway.
 */
#define SWEEP_POINTS 32u
#define SWEEP_STEP 0x4000u   // 16 KiB between points; 32 of them span 512 KiB
#define SWEEP_WINDOW 0x2000u // 8 KiB read at each point

#define SWEEP_GRAPH_LEFT 40
#define SWEEP_GRAPH_PITCH 8
#define SWEEP_BAR_WIDTH 6
#define SWEEP_GRAPH_TOP 44
#define SWEEP_GRAPH_BASE 180
#define SWEEP_GRAPH_HEIGHT (SWEEP_GRAPH_BASE - SWEEP_GRAPH_TOP)

#define ROW_SWEEP_TITLE 4
#define ROW_SWEEP_SUB 16
#define ROW_SWEEP_BANK 28
#define ROW_SWEEP_ADDR 184
#define ROW_SWEEP_STEP 194

/*
 * The third page is five blocks of the same shape: a header row naming what is
 * being measured and a value row under it, on one grid of five 48px columns.
 * Six characters at the 6px advance is 36px, so a value fits a column with room
 * to spare and a label fits the column before it.
 */
#define COL_LAT_NAME 2
#define COL_LAT_V0 56
#define COL_LAT_PITCH 48
#define COL_LAT_VAL(i) (COL_LAT_V0 + ((i) * COL_LAT_PITCH))

// The three-pair rows put a label in one column and its value in the next.
#define COL_LAT_P0_NAME COL_LAT_NAME
#define COL_LAT_P0_VAL COL_LAT_VAL(0)
#define COL_LAT_P1_NAME COL_LAT_VAL(1)
#define COL_LAT_P1_VAL COL_LAT_VAL(2)
#define COL_LAT_P2_NAME COL_LAT_VAL(3)
#define COL_LAT_P2_VAL COL_LAT_VAL(4)

#define ROW_LAT_TITLE 4
#define ROW_LAT_SUB 16
#define ROW_LAT_RULE_CHASE 28
#define ROW_LAT_CHASE 32
#define ROW_LAT_RULE_STRIDE 44
#define ROW_LAT_STRIDE_HDR0 48
#define ROW_LAT_STRIDE_VAL0 60
#define ROW_LAT_STRIDE_HDR1 72
#define ROW_LAT_STRIDE_VAL1 84
#define ROW_LAT_RULE_BANK 96
#define ROW_LAT_BANK_HDR 100
#define ROW_LAT_BANK 112
#define ROW_LAT_RULE_LOAD 124
#define ROW_LAT_LOAD_HDR 128
#define ROW_LAT_LOAD 140
#define ROW_LAT_RULE_WR 152
#define ROW_LAT_WR_HDR 156
#define ROW_LAT_WR 168
#define ROW_LAT_RULE_NOTES 176
#define ROW_LAT_NOTES 180

/*
 * The stride sweep. A row in this controller is 1024 bytes, so the step between
 * 512 and 1 KiB is the point where consecutive accesses stop sharing an open row
 * and start paying a PRECHARGE and an ACTIVATE each. Everything below 1 KiB is
 * the same row cost amortised over more or fewer accesses; everything above it
 * is flat, because one row per access is one row per access.
 *
 * The 32 byte point is the exception and should be read as such: it is the only
 * stride the data cache's next-line read-ahead can predict, so it is the only
 * one of the nine that is not a pure dependent miss. If it comes out well under
 * its neighbours, that is the read-ahead being priced, not the row.
 *
 * Split five and four across two rows because nine six-character columns do not
 * fit in 320 pixels.
 */
#define LAT_STRIDE_COUNT 9u
#define LAT_STRIDE_ROW0 5u

static const uint32_t lat_strides[LAT_STRIDE_COUNT] = {
    32u, 64u, 128u, 256u, 512u, 1024u, 2048u, 4096u, 8192u};

// Right aligned in six characters, to sit over print_result_inline()'s output.
static const char *const lat_stride_names[LAT_STRIDE_COUNT] = {
    "    32", "    64", "   128", "   256", "   512", "   1Ki", "   2Ki", "   4Ki", "   8Ki"};

typedef struct
{
    const char *name;
    const char *range;
    uint32_t window; // physical base of the measured window
    uint32_t bank;   // VRAM bank base that has to be the back buffer, or 0
} region_t;

static const region_t regions[] = {
    {"SRAM", "00000-2FFFF", PHYS_SRAM_WINDOW, 0},
    {"VRAM0", "30000-57FFF", PHYS_VRAM0, PHYS_VRAM0},
    {"VRAM1", "58000-7FFFF", PHYS_VRAM1, PHYS_VRAM1},
    {"DRAM", "8000000-87FFFFF", PHYS_DRAM, 0},
};

#define REGION_COUNT (sizeof(regions) / sizeof(regions[0]))

typedef struct
{
    uint32_t uncached_read;
    uint32_t uncached_write;
    uint32_t cached_read;
    uint32_t cached_write;
} result_t;

/*
 * The CPU kernels get a step each rather than sharing one. They are short, but
 * one step per kernel means that if a kernel ever faults, the rows that did
 * fill in name the one that did not -- and since every exception restarts this
 * ROM through _exception_handler, that is otherwise hard to see.
 */
enum
{
    STEP_CPU = REGION_COUNT,
    STEP_VSYNC = STEP_CPU + BENCH_CPU_COUNT,
    STEP_IDE_INIT,
    STEP_IDE_READ,
    STEP_SWEEP,
    // Building the 4096 node chain walks 128 KiB twice, once to write the links
    // and once to prove them, which is about as much work again as measuring
    // them. Its own step, for the same reason STEP_IDE_INIT is one.
    STEP_CHASE_BUILD,
    STEP_CHASE_RUN,
    STEP_STRIDE,
    STEP_BANK,
    STEP_WRALLOC,
    STEP_DONE,
};

static const char *const cpu_names[BENCH_CPU_COUNT] = {
    [BENCH_CPU_ALU] = "ALU addu",
    [BENCH_CPU_LOAD] = "Load lw (D$)",
    [BENCH_CPU_MUL] = "Mul multu",
    [BENCH_CPU_DIV] = "Div divu",
    [BENCH_CPU_FPU] = "FPU add.s",
    [BENCH_CPU_LOAD_8] = "Load lw x8",
    [BENCH_CPU_LOAD_DEP] = "Load lw chain",
};

/*
 * Where _exception_handler leaves its record, through the uncached alias. The
 * layout is magic, Cause, EPC, Status, BadVAddr.
 */
#define EXC_RECORD ((volatile uint32_t *)(uintptr_t)(BENCH_KSEG1 + 0x0002F000u))
#define EXC_MAGIC 0xDEADBEEFu

static result_t results[REGION_COUNT];
static uint32_t cpu_cycles[BENCH_CPU_COUNT];
static uint32_t cpu_mips = RESULT_PENDING;
static uint32_t vsync_hz = RESULT_PENDING;
static uint32_t ide_mbps = RESULT_PENDING;
static bench_ide_status_t ide_status = BENCH_IDE_OK;
static uint8_t step = 0;
static uint8_t page = 0;

// Table, SRAM sweep, latency. Any button steps through them in that order.
#define PAGE_COUNT 3u

static uint32_t sweep[SWEEP_POINTS];
static uint32_t sweep_back_bank = 0;

// Everything on the latency page is cycles per access, in hundredths, except the
// two ratios -- which are also in hundredths, and print the same way.
static uint32_t lat_chase = RESULT_PENDING;
static uint32_t lat_stride[LAT_STRIDE_COUNT];
static uint32_t lat_bank_same = RESULT_PENDING;
static uint32_t lat_bank_diff = RESULT_PENDING;
static uint32_t lat_bank_ratio = RESULT_PENDING;
static uint32_t lat_wr_line = RESULT_PENDING;
static uint32_t lat_wr_qword = RESULT_PENDING;
static uint32_t lat_wr_ratio = RESULT_PENDING;
static bool lat_chase_ready = false;

static bool exc_seen = false;
static uint32_t exc_cause = 0;
static uint32_t exc_epc = 0;
static uint32_t exc_bad = 0;

/*
 * The write window has to sit above the heap and below VRAM bank 0. Both bounds
 * come from the linker rather than from the constant above, so that if the heap
 * ever grows past 0x80014000 the SRAM write test reports "skip" instead of
 * overwriting live allocations.
 */
static bool sram_window_is_free(void)
{
    const uint32_t heap_end = BENCH_PHYS(_heap_vma) + (uint32_t)(uintptr_t)_heap_size;

    return PHYS_SRAM_WINDOW >= heap_end && (PHYS_SRAM_WINDOW + BENCH_WINDOW_SIZE) <= PHYS_VRAM0;
}

static void measure_region(const uint8_t index)
{
    const region_t *const region = &regions[index];
    result_t *const result = &results[index];
    const bool writable = (region->window != PHYS_SRAM_WINDOW) || sram_window_is_free();

    result->uncached_read = bench_rate_x100(BENCH_WINDOW_SIZE, bench_read(region->window, BENCH_WINDOW_SIZE, false));
    result->cached_read = bench_rate_x100(BENCH_WINDOW_SIZE, bench_read(region->window, BENCH_WINDOW_SIZE, true));

    if (!writable)
    {
        result->uncached_write = RESULT_SKIPPED;
        result->cached_write = RESULT_SKIPPED;
        return;
    }

    result->uncached_write = bench_rate_x100(BENCH_WINDOW_SIZE, bench_write(region->window, BENCH_WINDOW_SIZE, false));
    result->cached_write = bench_rate_x100(BENCH_WINDOW_SIZE, bench_write(region->window, BENCH_WINDOW_SIZE, true));
}

static void measure_cpu(const bench_cpu_test_t test)
{
    if (test == BENCH_CPU_FPU && !bench_fpu_available())
    {
        cpu_cycles[test] = RESULT_SKIPPED;
        return;
    }

    uint32_t ops = 0;
    const uint32_t ticks = bench_cpu(test, &ops);

    cpu_cycles[test] = bench_cycles_x100(ops, ticks);

    if (test == BENCH_CPU_ALU)
    {
        // The headline instruction rate comes from the one kernel that issues
        // nothing but a single-cycle instruction.
        cpu_mips = bench_rate_x100(ops, ticks);
    }
}

/*
 * One frame, all 32 points, so that the whole sweep sees the same bank being
 * scanned out. Splitting it across frames would swap the back buffer underneath
 * the measurement and put a video-contention step in the middle of the graph
 * that has nothing to do with the memory.
 */
static void measure_sweep(void)
{
    sweep_back_bank = BENCH_PHYS(gBackBuffer);

    for (uint8_t i = 0; i < SWEEP_POINTS; i++)
    {
        const uint32_t base = (uint32_t)i * SWEEP_STEP;
        sweep[i] = bench_rate_x100(SWEEP_WINDOW, bench_read(base, SWEEP_WINDOW, false));
    }
}

/*
 * A ratio in hundredths, printed by the same six-character formatter as the
 * cycle counts. Guarded rather than clamped: a zero denominator here means the
 * measurement it came from was skipped, and saying so beats printing infinity.
 */
static uint32_t ratio_x100(const uint32_t numerator, const uint32_t denominator)
{
    if (numerator == RESULT_SKIPPED || denominator == RESULT_SKIPPED || denominator == 0u)
    {
        return RESULT_SKIPPED;
    }

    return (numerator * 100u) / denominator;
}

static void measure_chase(void)
{
    lat_chase = lat_chase_ready
                    ? bench_cycles_x100(LAT_CHASE_NODES, bench_chase_run(PHYS_LAT_CHASE, LAT_CHASE_NODES))
                    : RESULT_SKIPPED;
}

/*
 * Nine chains over the same nodes and the same buffer, one stride at a time, so
 * only one of them exists at once and the 1 MiB reserved for the widest covers
 * all of them.
 */
static void measure_stride(void)
{
    for (uint8_t i = 0; i < LAT_STRIDE_COUNT; i++)
    {
        // Ascending, not shuffled: a stride sweep that did not walk in stride
        // order would not be measuring a stride. See the note by lat_strides[].
        if (!bench_chase_build(PHYS_LAT_STRIDE, LAT_STRIDE_NODES, lat_strides[i], false))
        {
            lat_stride[i] = RESULT_SKIPPED;
            continue;
        }

        lat_stride[i] = bench_cycles_x100(LAT_STRIDE_NODES,
                                          bench_chase_run(PHYS_LAT_STRIDE, LAT_STRIDE_NODES));
    }
}

static void measure_bank(void)
{
    const uint32_t steps = LAT_BANK_NODES * 2u;

    lat_bank_same = bench_chase_build_pair(PHYS_BANK_SAME_A, PHYS_BANK_SAME_B, LAT_BANK_NODES)
                        ? bench_cycles_x100(steps, bench_chase_run(PHYS_BANK_SAME_A, steps))
                        : RESULT_SKIPPED;

    lat_bank_diff = bench_chase_build_pair(PHYS_BANK_DIFF_A, PHYS_BANK_DIFF_B, LAT_BANK_NODES)
                        ? bench_cycles_x100(steps, bench_chase_run(PHYS_BANK_DIFF_A, steps))
                        : RESULT_SKIPPED;

    // Same over different: above 1.00 means the bank bit bought something.
    lat_bank_ratio = ratio_x100(lat_bank_same, lat_bank_diff);
}

/*
 * Both streams are measured over the DRAM window the region table already
 * writes, so nothing new has to be shown to be free. They cover the same 2048
 * lines and differ only in how much of each one they store to.
 */
static void measure_write_alloc(void)
{
    const uint32_t lines = BENCH_WINDOW_SIZE / CACHE_LINE_SIZE;

    lat_wr_line = bench_cycles_x100(lines, bench_write_lines(PHYS_DRAM, BENCH_WINDOW_SIZE, true));
    lat_wr_qword = bench_cycles_x100(lines, bench_write_lines(PHYS_DRAM, BENCH_WINDOW_SIZE, false));

    // Sparse over whole-line. Skip-fill saves the whole-line stream a 32 byte
    // read per line, so if it is firing this lands near 2.00; if it is not, the
    // two streams do identical memory work and this lands near 1.00.
    lat_wr_ratio = ratio_x100(lat_wr_qword, lat_wr_line);
}

/*
 * Picks up whatever _exception_handler stored on the way through, then clears
 * the magic so the next fault is distinguishable from this one. The values are
 * copied into .bss, which the restart has already zeroed by the time this runs.
 */
static void capture_exception(void)
{
    if (EXC_RECORD[0] != EXC_MAGIC)
    {
        return;
    }

    exc_cause = EXC_RECORD[1];
    exc_epc = EXC_RECORD[2];
    exc_bad = EXC_RECORD[4];
    exc_seen = true;

    EXC_RECORD[0] = 0;
}

static void measure_ide(void)
{
    uint32_t best = 0xFFFFFFFFu;

    for (uint32_t pass = 0; pass < BENCH_PASSES; pass++)
    {
        uint32_t ticks = 0;
        ide_status = bench_ide_read(0, BENCH_IDE_SECTORS, PHYS_IDE_BUFFER, &ticks);
        if (ide_status != BENCH_IDE_OK)
        {
            return;
        }
        if (ticks < best)
        {
            best = ticks;
        }
    }

    ide_mbps = bench_rate_x100(BENCH_IDE_SECTORS * 512u, best);
}

/*
 * One step per frame. Splitting the work this way keeps every frame short
 * enough that the watchdog reset main() issues each iteration is never late,
 * and it lets the table fill in as the numbers arrive instead of freezing on a
 * green screen until everything is done.
 */
static void run_step(void)
{
    if (step >= STEP_DONE)
    {
        return;
    }

    if (step < REGION_COUNT)
    {
        /*
         * A VRAM bank may only be written while it is the back buffer. Writing
         * to the bank being scanned out would splash 64 KiB of black across the
         * picture for a frame, and it would also measure the contended case
         * without saying so. The banks swap every frame, so simply waiting for
         * the right one costs at most one frame per region.
         */
        if (regions[step].bank != 0u && BENCH_PHYS(gBackBuffer) != regions[step].bank)
        {
            return;
        }

        measure_region(step);
        step++;
        return;
    }

    if (step < STEP_VSYNC)
    {
        measure_cpu((bench_cpu_test_t)(step - STEP_CPU));
        step++;
        return;
    }

    if (step == STEP_VSYNC)
    {
        // This one blocks for BENCH_VSYNC_FRAMES frames by construction, so it
        // gets a step to itself rather than stretching the CPU frame.
        vsync_hz = bench_hz_x100(bench_vsync_ticks(BENCH_VSYNC_FRAMES));
        step++;
        return;
    }

    if (step == STEP_IDE_INIT)
    {
        ide_status = bench_ide_init();
        if (ide_status == BENCH_IDE_OK)
        {
            // Untimed warm-up. It confirms the drive answers and pulls the
            // sectors into its own buffer, so the timed passes that follow
            // measure the PIO path rather than one-off rotational latency.
            uint32_t ticks = 0;
            ide_status = bench_ide_read(0, BENCH_IDE_SECTORS, PHYS_IDE_BUFFER, &ticks);
        }
        step++;
        return;
    }

    if (step == STEP_IDE_READ)
    {
        if (ide_status == BENCH_IDE_OK)
        {
            measure_ide();
        }
        step++;
        return;
    }

    if (step == STEP_SWEEP)
    {
        measure_sweep();
        step++;
        return;
    }

    if (step == STEP_CHASE_BUILD)
    {
        lat_chase_ready = bench_chase_build(PHYS_LAT_CHASE, LAT_CHASE_NODES, CACHE_LINE_SIZE, true);
        step++;
        return;
    }

    if (step == STEP_CHASE_RUN)
    {
        measure_chase();
        step++;
        return;
    }

    if (step == STEP_STRIDE)
    {
        measure_stride();
        step++;
        return;
    }

    if (step == STEP_BANK)
    {
        measure_bank();
        step++;
        return;
    }

    measure_write_alloc();
    step++;
}

// Right aligned in six characters, two decimal places: "123.45", " 12.34".
static void print_value_x100(const uint32_t value_x100)
{
    const uint32_t whole = value_x100 / 100u;
    const uint32_t frac = value_x100 % 100u;

    if (whole < 100u)
    {
        print_str(" ");
    }
    if (whole < 10u)
    {
        print_str(" ");
    }

    print_dec(whole);
    print_str(".");
    if (frac < 10u)
    {
        print_str("0");
    }
    print_dec(frac);
}

// Prints at the current position, six characters wide whatever the value is.
static void print_result_inline(const uint32_t value)
{
    if (value == RESULT_PENDING)
    {
        set_text_color(COLOR_NOTE, 0xAAAA);
        print_str("    --");
        return;
    }

    if (value == RESULT_SKIPPED)
    {
        set_text_color(COLOR_NOTE, 0xAAAA);
        print_str("  skip");
        return;
    }

    set_text_color(COLOR_VALUE, 0xAAAA);
    print_value_x100(value);
}

static void print_result(const uint16_t x, const uint16_t y, const uint32_t value)
{
    set_xy(x, y);
    print_result_inline(value);
}

static const char *ide_status_str(void)
{
    switch (ide_status)
    {
    case BENCH_IDE_NOT_READY:
        return "no drive / not ready";
    case BENCH_IDE_NO_DATA:
        return "transfer timed out";
    case BENCH_IDE_ERROR:
        return "drive reported an error";
    default:
        return "";
    }
}

static void draw_header(void)
{
    set_text_color(COLOR_TITLE, 0xAAAA);
    print_xy(22, ROW_TITLE, "KILLER INSTINCT - MEMORY, CPU & IDE SPEED TEST");

    set_text_color(COLOR_NOTE, 0xAAAA);
    print_xy(61, ROW_BOARD, "Board " KI_BOARD_STR "   Built " __DATE__);

    draw_horizontal_line(COL_REGION, ROW_RULE_TOP, 296, COLOR_RULE);

    set_text_color(COLOR_HEADER, 0xAAAA);
    print_xy(COL_UNC_RD + 15, ROW_HDR1, "Uncached");
    print_xy(COL_CAC_RD + 21, ROW_HDR1, "Cached");

    print_xy(COL_REGION, ROW_HDR2, "Region");
    print_xy(COL_RANGE, ROW_HDR2, "Range (phys)");
    print_xy(COL_UNC_RD + 24, ROW_HDR2, "Rd");
    print_xy(COL_UNC_WR + 24, ROW_HDR2, "Wr");
    print_xy(COL_CAC_RD + 24, ROW_HDR2, "Rd");
    print_xy(COL_CAC_WR + 24, ROW_HDR2, "Wr");

    draw_horizontal_line(COL_REGION, ROW_RULE_HDR, 296, COLOR_RULE);
}

static void draw_table(void)
{
    for (uint8_t i = 0; i < REGION_COUNT; i++)
    {
        const uint16_t y = ROW_DATA + (i * ROW_PITCH);

        set_text_color(COLOR_VALUE, 0xAAAA);
        print_xy(COL_REGION, y, regions[i].name);
        set_text_color(COLOR_NOTE, 0xAAAA);
        print_xy(COL_RANGE, y, regions[i].range);

        print_result(COL_UNC_RD, y, results[i].uncached_read);
        print_result(COL_UNC_WR, y, results[i].uncached_write);
        print_result(COL_CAC_RD, y, results[i].cached_read);
        print_result(COL_CAC_WR, y, results[i].cached_write);
    }
}

static void draw_ide(void)
{
    draw_horizontal_line(COL_REGION, ROW_RULE_IDE, 296, COLOR_RULE);

    set_text_color(COLOR_VALUE, 0xAAAA);
    print_xy(COL_REGION, ROW_IDE, "IDE");
    set_text_color(COLOR_NOTE, 0xAAAA);
    print_xy(COL_RANGE, ROW_IDE, "LBA 0-127 (64K)");

    if (ide_status != BENCH_IDE_OK)
    {
        set_text_color(COLOR_ERROR, 0xAAAA);
        print_xy(COL_UNC_RD, ROW_IDE, ide_status_str());
        return;
    }

    print_result(COL_UNC_RD, ROW_IDE, ide_mbps);
    if (ide_mbps != RESULT_PENDING)
    {
        set_text_color(COLOR_NOTE, 0xAAAA);
        print_str(" MB/s");
    }
}

/*
 * Two label/value pairs per row. The left column is the one that is read
 * against the ALU baseline, the right one holds the slow instructions plus the
 * vsync cross-check -- which is a frequency, not a cycle count, hence the unit
 * in its label.
 */
static void draw_cpu(void)
{
    const char *const left_names[] = {
        cpu_names[BENCH_CPU_ALU],
        cpu_names[BENCH_CPU_LOAD],
        cpu_names[BENCH_CPU_MUL],
    };
    const char *const right_names[] = {
        cpu_names[BENCH_CPU_DIV],
        cpu_names[BENCH_CPU_FPU],
        "Vsync (Hz)",
    };

    const uint32_t left_values[] = {
        cpu_cycles[BENCH_CPU_ALU],
        cpu_cycles[BENCH_CPU_LOAD],
        cpu_cycles[BENCH_CPU_MUL],
    };
    const uint32_t right_values[] = {
        cpu_cycles[BENCH_CPU_DIV],
        cpu_cycles[BENCH_CPU_FPU],
        vsync_hz,
    };

    draw_horizontal_line(COL_REGION, ROW_RULE_CPU, 296, COLOR_RULE);

    set_text_color(COLOR_VALUE, 0xAAAA);
    print_xy(COL_CPU_L_NAME, ROW_CPU, "CPU");
    set_text_color(COLOR_HEADER, 0xAAAA);
    print_xy(COL_CPU_L_VAL, ROW_CPU, "cycles/op");

    print_result(COL_CPU_MIPS, ROW_CPU, cpu_mips);
    if (cpu_mips != RESULT_PENDING)
    {
        set_text_color(COLOR_NOTE, 0xAAAA);
        print_str(" MIPS");
    }

    for (uint8_t i = 0; i < 3; i++)
    {
        const uint16_t y = ROW_CPU_DATA + (i * ROW_PITCH);

        set_text_color(COLOR_NOTE, 0xAAAA);
        print_xy(COL_CPU_L_NAME, y, left_names[i]);
        print_xy(COL_CPU_R_NAME, y, right_names[i]);

        print_result(COL_CPU_L_VAL, y, left_values[i]);
        print_result(COL_CPU_R_VAL, y, right_values[i]);
    }
}

// ExcCode, Cause bits 6:2.
static const char *exc_name(const uint32_t code)
{
    switch (code)
    {
    case 0:
        return "Int";
    case 4:
        return "AdEL";
    case 5:
        return "AdES";
    case 6:
        return "IBE";
    case 7:
        return "DBE";
    case 8:
        return "Sys";
    case 9:
        return "Bp";
    case 10:
        return "RI";
    case 11:
        return "CpU";
    case 12:
        return "Ov";
    case 15:
        return "FPE";
    default:
        return "?";
    }
}

static void draw_notes(void)
{
    draw_horizontal_line(COL_REGION, ROW_RULE_NOTES, 296, COLOR_RULE);

    set_text_color(COLOR_NOTE, 0xAAAA);
    print_xy(COL_REGION, ROW_NOTES, "64KiB window, best of 3, MB=10^6 B, cyc = 2 x Count");

    /*
     * The second note line doubles as the fault report. A restart through
     * _exception_handler looks exactly like a watchdog reset from the outside,
     * so when there is something to say, saying it matters more than the note.
     */
    if (!exc_seen)
    {
        print_xy(COL_REGION, ROW_NOTES + ROW_PITCH, "SRAM window 0x14000-0x23FFF; VRAM as back buffer");
        return;
    }

    const uint32_t code = (exc_cause >> 2) & 0x1Fu;

    set_text_color(COLOR_ERROR, 0xAAAA);
    set_xy(COL_REGION, ROW_NOTES + ROW_PITCH);
    print_str("EXC ");
    print_str(exc_name(code));
    print_str("/");
    print_dec(code);
    // Cause.CE, bits 29:28: which coprocessor a CpU refers to. CU0 and CU1 are
    // both enabled, so anything other than 2 or 3 here means the instruction at
    // EPC is not what the source says it is.
    print_str(" ce=");
    print_dec((exc_cause >> 28) & 0x3u);
    print_str(" epc=");
    print_hex(exc_epc, 32);
    print_str(" bad=");
    print_hex(exc_bad, 32);
}

/*
 * A bar per point, coloured by which region the address falls in, so a step at
 * a region boundary is obvious and a step anywhere else -- or none at all --
 * is equally obvious. Reading exact figures off bars is hopeless, so the
 * largest adjacent change is printed underneath in numbers.
 */
static void draw_sweep(void)
{
    set_text_color(COLOR_TITLE, 0xAAAA);
    print_xy(88, ROW_SWEEP_TITLE, "SRAM UNCACHED READ SWEEP");

    set_text_color(COLOR_NOTE, 0xAAAA);
    print_xy(28, ROW_SWEEP_SUB, "8KiB window, 16KiB steps, uncached read only");

    uint32_t max = 0;
    for (uint8_t i = 0; i < SWEEP_POINTS; i++)
    {
        if (sweep[i] > max)
        {
            max = sweep[i];
        }
    }

    if (max == 0)
    {
        print_xy(76, ROW_SWEEP_BANK, "Sweep has not run yet");
        return;
    }

    set_xy(64, ROW_SWEEP_BANK);
    print_str("Back buffer during sweep: ");
    print_str(sweep_back_bank == PHYS_VRAM0 ? "VRAM0" : "VRAM1");

    // Scale labels and baseline.
    set_text_color(COLOR_NOTE, 0xAAAA);
    print_result(2, SWEEP_GRAPH_TOP, max);
    print_xy(14, SWEEP_GRAPH_BASE - 8, "0.00");
    draw_horizontal_line(SWEEP_GRAPH_LEFT, SWEEP_GRAPH_BASE, SWEEP_POINTS * SWEEP_GRAPH_PITCH, COLOR_RULE);

    for (uint8_t i = 0; i < SWEEP_POINTS; i++)
    {
        if (sweep[i] == 0)
        {
            continue;
        }

        const uint32_t base = (uint32_t)i * SWEEP_STEP;
        uint16_t height = (uint16_t)((sweep[i] * SWEEP_GRAPH_HEIGHT) / max);
        if (height == 0)
        {
            height = 1;
        }

        uint16_t color = COLOR_VALUE;
        if (base >= PHYS_VRAM1)
        {
            color = COLOR_TITLE;
        }
        else if (base >= PHYS_VRAM0)
        {
            color = COLOR_HEADER;
        }

        const uint16_t x = SWEEP_GRAPH_LEFT + (i * SWEEP_GRAPH_PITCH);
        set_text_color(color, color);
        draw_box(x, SWEEP_GRAPH_BASE - height, x + SWEEP_BAR_WIDTH - 1, SWEEP_GRAPH_BASE);
    }

    // Ticks and labels at the two VRAM bank boundaries.
    set_text_color(COLOR_RULE, 0xAAAA);
    draw_vertical_line(SWEEP_GRAPH_LEFT + ((PHYS_VRAM0 / SWEEP_STEP) * SWEEP_GRAPH_PITCH), SWEEP_GRAPH_BASE + 1, 3);
    draw_vertical_line(SWEEP_GRAPH_LEFT + ((PHYS_VRAM1 / SWEEP_STEP) * SWEEP_GRAPH_PITCH), SWEEP_GRAPH_BASE + 1, 3);

    set_text_color(COLOR_NOTE, 0xAAAA);
    print_xy(SWEEP_GRAPH_LEFT, ROW_SWEEP_ADDR, "00000");
    print_xy(SWEEP_GRAPH_LEFT + ((PHYS_VRAM0 / SWEEP_STEP) * SWEEP_GRAPH_PITCH), ROW_SWEEP_ADDR, "30000");
    print_xy(SWEEP_GRAPH_LEFT + ((PHYS_VRAM1 / SWEEP_STEP) * SWEEP_GRAPH_PITCH), ROW_SWEEP_ADDR, "58000");
    print_xy(264, ROW_SWEEP_ADDR, "7FFFF");

    /*
     * The largest change between neighbouring points, by ratio. That address is
     * the boundary, if there is one. Starting the search at 1.5x rather than 1x
     * means ordinary measurement scatter does not get reported as a step.
     */
    uint32_t worst_ratio = 150u;
    uint8_t worst = 0;
    for (uint8_t i = 1; i < SWEEP_POINTS; i++)
    {
        const uint32_t a = sweep[i - 1];
        const uint32_t b = sweep[i];
        if (a == 0 || b == 0)
        {
            continue;
        }

        const uint32_t ratio = (a > b) ? ((a * 100u) / b) : ((b * 100u) / a);
        if (ratio > worst_ratio)
        {
            worst_ratio = ratio;
            worst = i;
        }
    }

    set_xy(2, ROW_SWEEP_STEP);
    if (worst == 0)
    {
        print_str("No step found: flat across the whole 512 KiB");
        return;
    }

    print_str("Step at ");
    print_hex((uint32_t)worst * SWEEP_STEP, 24);
    print_str(":");
    print_result_inline(sweep[worst - 1]);
    print_str(" ->");
    print_result_inline(sweep[worst]);
    print_str(" MB/s");
}

// A label and its value, for the three-pair rows the bottom half is built from.
static void draw_lat_pair(const uint16_t name_x, const uint16_t value_x, const uint16_t y, const char *const name, const uint32_t value)
{
    set_text_color(COLOR_NOTE, 0xAAAA);
    print_xy(name_x, y, name);
    print_result(value_x, y, value);
}

/*
 * The latency page. Every number on it is cycles per single access, where page
 * one is bytes per second: the same machine measured with the read-ahead and the
 * overlap deliberately taken away, because that is the half of the memory system
 * the SDRAM work is actually trying to move.
 */
static void draw_latency(void)
{
    set_text_color(COLOR_TITLE, 0xAAAA);
    print_xy(70, ROW_LAT_TITLE, "SDRAM LATENCY - DEPENDENT MISS");

    set_text_color(COLOR_NOTE, 0xAAAA);
    print_xy(28, ROW_LAT_SUB, "cycles per dependent load, best of 3, cold D$");

    // Pointer chase. The one number on this page the whole optimisation rests
    // on, so it gets a row and a unit to itself rather than a column.
    draw_horizontal_line(COL_REGION, ROW_LAT_RULE_CHASE, 296, COLOR_RULE);

    set_text_color(COLOR_VALUE, 0xAAAA);
    print_xy(COL_LAT_NAME, ROW_LAT_CHASE, "Chase");
    set_text_color(COLOR_NOTE, 0xAAAA);
    print_xy(44, ROW_LAT_CHASE, "128KiB shuffled, 4096 nodes");
    print_result(218, ROW_LAT_CHASE, lat_chase);
    if (lat_chase != RESULT_PENDING)
    {
        set_text_color(COLOR_NOTE, 0xAAAA);
        print_str(" cyc");
    }

    // Stride sweep, five columns then four.
    draw_horizontal_line(COL_REGION, ROW_LAT_RULE_STRIDE, 296, COLOR_RULE);

    for (uint8_t i = 0; i < LAT_STRIDE_COUNT; i++)
    {
        const bool second = i >= LAT_STRIDE_ROW0;
        const uint8_t column = second ? (uint8_t)(i - LAT_STRIDE_ROW0) : i;
        const uint16_t x = COL_LAT_VAL(column);

        set_text_color(COLOR_HEADER, 0xAAAA);
        print_xy(x, second ? ROW_LAT_STRIDE_HDR1 : ROW_LAT_STRIDE_HDR0, lat_stride_names[i]);
        print_result(x, second ? ROW_LAT_STRIDE_VAL1 : ROW_LAT_STRIDE_VAL0, lat_stride[i]);
    }

    set_text_color(COLOR_HEADER, 0xAAAA);
    print_xy(COL_LAT_NAME, ROW_LAT_STRIDE_HDR0, "Stride");
    print_xy(COL_LAT_NAME, ROW_LAT_STRIDE_HDR1, "Stride");
    set_text_color(COLOR_VALUE, 0xAAAA);
    print_xy(COL_LAT_NAME, ROW_LAT_STRIDE_VAL0, "cyc");
    print_xy(COL_LAT_NAME, ROW_LAT_STRIDE_VAL1, "cyc");

    // Bank pair.
    draw_horizontal_line(COL_REGION, ROW_LAT_RULE_BANK, 296, COLOR_RULE);

    set_text_color(COLOR_VALUE, 0xAAAA);
    print_xy(COL_LAT_NAME, ROW_LAT_BANK_HDR, "Bank");
    set_text_color(COLOR_HEADER, 0xAAAA);
    print_xy(44, ROW_LAT_BANK_HDR, "alternating pair, cycles per access");

    draw_lat_pair(COL_LAT_P0_NAME, COL_LAT_P0_VAL, ROW_LAT_BANK, "1KiB", lat_bank_same);
    draw_lat_pair(COL_LAT_P1_NAME, COL_LAT_P1_VAL, ROW_LAT_BANK, "bank01", lat_bank_diff);
    draw_lat_pair(COL_LAT_P2_NAME, COL_LAT_P2_VAL, ROW_LAT_BANK, "ratio", lat_bank_ratio);

    // Load issue rate, the three variants read against each other.
    draw_horizontal_line(COL_REGION, ROW_LAT_RULE_LOAD, 296, COLOR_RULE);

    set_text_color(COLOR_VALUE, 0xAAAA);
    print_xy(COL_LAT_NAME, ROW_LAT_LOAD_HDR, "Load");
    set_text_color(COLOR_HEADER, 0xAAAA);
    print_xy(44, ROW_LAT_LOAD_HDR, "lw hitting in D$, cycles per load");

    draw_lat_pair(COL_LAT_P0_NAME, COL_LAT_P0_VAL, ROW_LAT_LOAD, "1 dest", cpu_cycles[BENCH_CPU_LOAD]);
    draw_lat_pair(COL_LAT_P1_NAME, COL_LAT_P1_VAL, ROW_LAT_LOAD, "8 dest", cpu_cycles[BENCH_CPU_LOAD_8]);
    draw_lat_pair(COL_LAT_P2_NAME, COL_LAT_P2_VAL, ROW_LAT_LOAD, "chain", cpu_cycles[BENCH_CPU_LOAD_DEP]);

    // Write allocate.
    draw_horizontal_line(COL_REGION, ROW_LAT_RULE_WR, 296, COLOR_RULE);

    set_text_color(COLOR_VALUE, 0xAAAA);
    print_xy(COL_LAT_NAME, ROW_LAT_WR_HDR, "Write");
    set_text_color(COLOR_HEADER, 0xAAAA);
    print_xy(44, ROW_LAT_WR_HDR, "cached store, cycles per 32B line");

    draw_lat_pair(COL_LAT_P0_NAME, COL_LAT_P0_VAL, ROW_LAT_WR, "4 of 4", lat_wr_line);
    draw_lat_pair(COL_LAT_P1_NAME, COL_LAT_P1_VAL, ROW_LAT_WR, "1 of 4", lat_wr_qword);
    draw_lat_pair(COL_LAT_P2_NAME, COL_LAT_P2_VAL, ROW_LAT_WR, "ratio", lat_wr_ratio);

    draw_horizontal_line(COL_REGION, ROW_LAT_RULE_NOTES, 296, COLOR_RULE);

    set_text_color(COLOR_NOTE, 0xAAAA);
    print_xy(COL_LAT_NAME, ROW_LAT_NOTES, "Row 1KiB; bank bit at phys 8700000, not +8MiB");
    /*
     * NOT a skip-fill verdict, in either direction. 25 cycles for a 32-byte
     * line is less than the 32 a write-back of that line costs at 50MHz (16
     * words), and page 1's 139 MB/s is above the part's ~100 MB/s ceiling, so
     * what is being timed is stores landing in the cache and the write FIFO -
     * the dirty lines drain after the timer stops. A ratio read off that says
     * nothing about whether the line was fetched first.
     *
     * To make it a verdict the write-backs have to be inside the window:
     * follow the store stream with an INDEX_WRITEBACK_INVALIDATE sweep before
     * reading the timer. Then skip-fill shows up as the one-qword case moving
     * 8 bytes a line against the four-qword case's 32, rather than both moving
     * 64 because each fetched first.
     */
    print_xy(COL_LAT_NAME, ROW_LAT_NOTES + ROW_PITCH, "Write times stores into cache, not the write-backs");
}

static void draw_status(void)
{
    if (step >= STEP_DONE)
    {
        set_text_color(color_fade_in_out(0x03E0, 0x0, FADE_SPEED_2S), 0xAAAA);
        print_xy(40, ROW_STATUS, "Any button: next page   START: run again");
        return;
    }

    const char *name = "sweep";
    if (step < REGION_COUNT)
    {
        name = regions[step].name;
    }
    else if (step < STEP_VSYNC)
    {
        name = cpu_names[step - STEP_CPU];
    }
    else if (step == STEP_VSYNC)
    {
        name = "vsync";
    }
    else if (step < STEP_SWEEP)
    {
        name = "IDE";
    }
    else if (step == STEP_CHASE_BUILD)
    {
        name = "chain build";
    }
    else if (step == STEP_CHASE_RUN)
    {
        name = "chase";
    }
    else if (step == STEP_STRIDE)
    {
        name = "strides";
    }
    else if (step == STEP_BANK)
    {
        name = "bank pair";
    }
    else if (step == STEP_WRALLOC)
    {
        name = "write allocate";
    }

    set_text_color(COLOR_HEADER, 0xAAAA);
    set_xy(88, ROW_STATUS);
    print_str("Measuring ");
    print_str(name);
    print_str("...");
}

/*
 * Buttons are held low, so an idle pad reads 0x7FF. Returns the buttons that
 * went down since the last call, from either player.
 *
 * The first call only records: whatever is already held when the view loads is
 * not a press, and without that a stuck button would fire on frame one and keep
 * firing.
 */
static uint16_t buttons_just_pressed(void)
{
    static uint16_t previous = 0;
    static bool primed = false;

    const uint16_t current = (uint16_t)((~gIO.player1 & 0x7FF) | (~gIO.player2 & 0x7FF));

    if (!primed)
    {
        previous = current;
        primed = true;
        return 0;
    }

    const uint16_t pressed = (uint16_t)(current & ~previous);
    previous = current;

    return pressed;
}

static void reset_results(void)
{
    for (uint8_t i = 0; i < REGION_COUNT; i++)
    {
        results[i].uncached_read = RESULT_PENDING;
        results[i].uncached_write = RESULT_PENDING;
        results[i].cached_read = RESULT_PENDING;
        results[i].cached_write = RESULT_PENDING;
    }

    for (uint8_t i = 0; i < BENCH_CPU_COUNT; i++)
    {
        cpu_cycles[i] = RESULT_PENDING;
    }

    for (uint8_t i = 0; i < SWEEP_POINTS; i++)
    {
        sweep[i] = RESULT_PENDING;
    }

    for (uint8_t i = 0; i < LAT_STRIDE_COUNT; i++)
    {
        lat_stride[i] = RESULT_PENDING;
    }

    lat_chase = RESULT_PENDING;
    lat_chase_ready = false;
    lat_bank_same = RESULT_PENDING;
    lat_bank_diff = RESULT_PENDING;
    lat_bank_ratio = RESULT_PENDING;
    lat_wr_line = RESULT_PENDING;
    lat_wr_qword = RESULT_PENDING;
    lat_wr_ratio = RESULT_PENDING;

    cpu_mips = RESULT_PENDING;
    vsync_hz = RESULT_PENDING;
    ide_mbps = RESULT_PENDING;
    ide_status = BENCH_IDE_OK;
    step = 0;
}

static void render(const uint64_t frame_count)
{
    /*
     * Measure first, then clear and redraw. The VRAM write test scribbles over
     * the back buffer, so the frame has to be painted after it, not before.
     */
    run_step();

    video_clear_framebuffer(0x0);

    if (page == 0)
    {
        draw_header();
        draw_table();
        draw_ide();
        draw_cpu();
        draw_notes();
    }
    else if (page == 1)
    {
        draw_sweep();
    }
    else
    {
        draw_latency();
    }

    draw_status();

    const uint16_t pressed = buttons_just_pressed();
    if ((pressed & BTN_START) != 0)
    {
        // Clearing the fault report here and not in reset_results(): load()
        // calls that straight after capture_exception(), so clearing it there
        // would wipe the record before it was ever shown.
        exc_seen = false;
        page = 0;
        reset_results();
    }
    else if (pressed != 0)
    {
        page = (uint8_t)((page + 1u) % PAGE_COUNT);
    }
}

static void load(void)
{
    capture_exception();
    reset_results();
}

static void unload(void)
{
}

view_t view_memtest = {
    .render = &render,
    .load = &load,
    .unload = &unload,
};
