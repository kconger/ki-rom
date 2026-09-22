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
 * boot.ld describes all 512 KiB as one SRAM, and the VRAM banks as two windows
 * inside it. Measured on an A-19489, that is not how the board behaves: an
 * uncached 64-bit read costs 45.8 cycles below 0x30000 and 8.0 cycles above it,
 * a step the sweep page puts exactly on the bank boundary. The slow half also
 * matches DRAM to three digits, so whatever sits below 0x30000 is on the same
 * path as DRAM rather than on the video memory's.
 *
 * The fast region is one framebuffer per bank and no more -- 0x30000-0x557FF
 * and 0x58000-0x7D7FF, 0x25800 bytes each, which is 320*240*2 exactly. A sweep
 * window straddling either of those ends reads at the blend of the two rates.
 * Keep the measured windows clear of the tails if that ever matters.
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
    STEP_DONE,
};

static const char *const cpu_names[BENCH_CPU_COUNT] = {
    [BENCH_CPU_ALU] = "ALU addu",
    [BENCH_CPU_LOAD] = "Load lw (D$)",
    [BENCH_CPU_MUL] = "Mul multu",
    [BENCH_CPU_DIV] = "Div divu",
    [BENCH_CPU_FPU] = "FPU add.s",
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

static uint32_t sweep[SWEEP_POINTS];
static uint32_t sweep_back_bank = 0;

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

    measure_sweep();
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

static void draw_status(void)
{
    if (step >= STEP_DONE)
    {
        set_text_color(color_fade_in_out(0x03E0, 0x0, FADE_SPEED_2S), 0xAAAA);
        print_xy(37, ROW_STATUS, "Any button: other page   START: run again");
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
    else
    {
        draw_sweep();
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
        page ^= 1;
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
