#pragma once
#ifndef _TIMING_H_
#define _TIMING_H_

#include <stdint.h>

/*
 * Cycle-level timing tests, run the same way on the original board and on the
 * MiSTer core so the two can be compared number by number: instruction
 * latencies, cache and TLB behaviour, bus and I/O timing, interrupts, the
 * video timing, the sound board's response, and the spread of repeated runs.
 *
 * Every kernel is generated into DRAM at run time (see timing.c), so none of
 * them lives in the measurement block that boot.ld keeps clear of I-cache
 * aliasing, and each one warms its own caches and times itself with Count.
 *
 * Results follow view_memtest.c's convention: TM_PENDING (0) until measured,
 * TM_SKIPPED when a test could not run. Cycle figures are pipeline cycles in
 * hundredths, like bench_cycles_x100().
 */

#define TM_PENDING 0u
#define TM_SKIPPED 0xFFFFFFFFu

#define TM_INT_COUNT 16u
#define TM_FPU_COUNT 16u
#define TM_DC_COUNT 7u
#define TM_IC_COUNT 6u
#define TM_TLB_COUNT 6u
#define TM_IO_COUNT 6u
#define TM_SPREAD_COUNT 5u
#define TM_SPREAD_BUCKETS 8u

// Region columns of the contention and store tables.
#define TM_REGION_SRAM 0u
#define TM_REGION_VRAM_FRONT 1u
#define TM_REGION_VRAM_BACK 2u
#define TM_REGION_DRAM 3u
#define TM_REGION_COUNT 4u

#define TM_STORE_WIDTHS 4u // sb, sh, sw, sd

typedef struct
{
    uint32_t min;  // cycles per op, x100
    uint32_t med;
    uint32_t max;
    uint8_t hist[TM_SPREAD_BUCKETS]; // runs per bucket, min to max
} tm_spread_t;

typedef struct
{
    uint32_t int_cyc[TM_INT_COUNT];
    uint32_t fpu_cyc[TM_FPU_COUNT];
    uint32_t dc_cyc[TM_DC_COUNT];
    uint32_t ic_cyc[TM_IC_COUNT];
    uint32_t tlb_cyc[TM_TLB_COUNT];

    uint32_t scan_vblank[TM_REGION_COUNT]; // uncached ld, cycles each
    uint32_t scan_active[TM_REGION_COUNT];
    uint32_t store_sram[TM_STORE_WIDTHS]; // uncached store, cycles each
    uint32_t store_dram[TM_STORE_WIDTHS];
    uint32_t io_cyc[TM_IO_COUNT];

    uint32_t irq_cmp_min; // cycles from Count = Compare to the handler
    uint32_t irq_cmp_max;
    uint32_t irq_vbl_min; // cycles from the last loop pass to the handler
    uint32_t irq_vbl_max;
    uint32_t syscall_cyc; // syscall, handler, eret

    uint32_t frame_us;  // x100
    uint32_t vblank_us; // x100

    uint32_t dcs_ack_min; // us x100, strobe to the DCS taking the byte
    uint32_t dcs_ack_max;
    uint32_t dcs_boot;    // ms x100, boot command to the DCS taking it

    tm_spread_t spread[TM_SPREAD_COUNT];
    bool spread_done;
} tm_results_t;

extern tm_results_t tm;

extern const char *const tm_int_names[TM_INT_COUNT];
extern const char *const tm_fpu_names[TM_FPU_COUNT];
extern const char *const tm_dc_names[TM_DC_COUNT];
extern const char *const tm_ic_names[TM_IC_COUNT];
extern const char *const tm_tlb_names[TM_TLB_COUNT];
extern const char *const tm_io_names[TM_IO_COUNT];
extern const char *const tm_spread_names[TM_SPREAD_COUNT];

// One step per frame, like view_memtest.c's own.
#define TM_STEPS 11u

void timing_reset(void);
void timing_run_step(uint8_t index);
const char *timing_step_name(uint8_t index);

#endif
