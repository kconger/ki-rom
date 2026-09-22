/**
 * SPDX-FileCopyrightText: © 2023 Leandre Gohy <leandre.gohy@hexeo.be>
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once
#ifndef _BENCH_H_
#define _BENCH_H_

#include <stdint.h>

/*
 * Throughput measurement kernels for the memory regions and the IDE interface.
 *
 * Everything here is expressed in PHYSICAL addresses, the way the memory map in
 * NOTES.md lists them (SRAM at 0x00000000, DRAM at 0x08000000). The kernels
 * build the virtual address themselves so the same region can be measured both
 * through kseg0 (cached) and kseg1 (uncached) without the caller juggling two
 * sets of constants.
 */

#define BENCH_KSEG0 0x80000000u // cached, unmapped
#define BENCH_KSEG1 0xA0000000u // uncached, unmapped

// Strips the segment from a kseg0/kseg1 address.
#define BENCH_PHYS(addr) ((uint32_t)(uintptr_t)(addr) & 0x1FFFFFFFu)

/*
 * Every region is measured over a window of this size so that the numbers are
 * directly comparable. 64 KiB is four times the 16K primary data cache, so a
 * cached sweep misses on every line instead of reporting cache bandwidth, and
 * it still fits in the 112 KiB of SRAM that is free at runtime (view_memtest.c
 * checks that at runtime rather than trusting the arithmetic here).
 */
#define BENCH_WINDOW_SIZE 0x10000u

// Same number of bytes over IDE, so the disk figure is on the same scale.
#define BENCH_IDE_SECTORS (BENCH_WINDOW_SIZE / 512u)

/*
 * Each measurement is repeated and the fastest pass is kept. Video refresh
 * steals memory cycles from the CPU and an occasional pass lands badly; the
 * minimum is the closest this can get to the undisturbed figure.
 */
#define BENCH_PASSES 3u

/*
 * CP0 Count increments once every two pipeline cycles, i.e. at the 50MHz input
 * clock rather than the 100MHz pipeline (R4600 datasheet; the same ratio time.c
 * relies on for clock() to return microseconds). One tick is therefore 20ns.
 */
#define BENCH_TICKS_PER_US 50u

// Operations per loop iteration in every cpu.S kernel.
#define BENCH_CPU_OPS_PER_ITER 64u

/*
 * Return the fastest of BENCH_PASSES passes over `size` bytes at physical
 * address `phys`, in CP0 Count ticks. `size` must be a multiple of 64.
 *
 * bench_write() overwrites the whole window: it is the caller's job to know
 * that those bytes are free.
 */
uint32_t bench_read(uint32_t phys, uint32_t size, bool cached);
uint32_t bench_write(uint32_t phys, uint32_t size, bool cached);

/*
 * Millions of things per second, in hundredths, for `count` things done in
 * `ticks`. The same arithmetic serves bytes and instructions, so this gives
 * MB/s (with MB meaning 10^6 bytes) for the memory and IDE tests and MIPS for
 * the CPU ones.
 */
uint32_t bench_rate_x100(uint32_t count, uint32_t ticks);

/*
 * Pipeline cycles per operation, in hundredths. Count advances once per two
 * pipeline cycles, so this is where the factor of two lives.
 */
uint32_t bench_cycles_x100(uint32_t ops, uint32_t ticks);

/*
 * CPU instruction throughput. Each kernel is a loop of one instruction class;
 * see cpu.S for what exactly is issued and what the figure includes.
 */
typedef enum
{
    BENCH_CPU_ALU = 0,  // addu, dependent chain
    BENCH_CPU_LOAD,     // lw, data cache hit
    BENCH_CPU_MUL,      // multu
    BENCH_CPU_DIV,      // divu
    BENCH_CPU_FPU,      // add.s, dependent chain
    BENCH_CPU_COUNT,
} bench_cpu_test_t;

// Fastest of BENCH_PASSES runs in ticks; *ops gets the operation count.
uint32_t bench_cpu(bench_cpu_test_t test, uint32_t *ops);

// Whether Status.CU1 is set, i.e. whether BENCH_CPU_FPU is safe to run.
bool bench_fpu_available(void);

/*
 * Average Count ticks per video frame, measured over `frames` of them. The
 * video timebase comes off its own crystal, so this is the one number here that
 * checks the CPU clock against something other than itself: divide it into the
 * Count frequency and the result should land on the real refresh rate.
 */
#define BENCH_VSYNC_FRAMES 4u
uint32_t bench_vsync_ticks(uint32_t frames);

// Frequency in hundredths of a Hz for something that takes `ticks` per period.
uint32_t bench_hz_x100(uint32_t ticks);

typedef enum
{
    BENCH_IDE_OK = 0,
    BENCH_IDE_NOT_READY, // no drive, or it never came out of BSY
    BENCH_IDE_NO_DATA,   // the drive stopped raising INTRQ mid-transfer
    BENCH_IDE_ERROR,     // the drive reported ERR
} bench_ide_status_t;

bench_ide_status_t bench_ide_init(void);
bench_ide_status_t bench_ide_read(uint32_t lba, uint32_t sectors, uint32_t dst_phys, uint32_t *ticks);

#endif
