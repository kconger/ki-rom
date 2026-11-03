/**
 * SPDX-FileCopyrightText: © 2023 Leandre Gohy <leandre.gohy@hexeo.be>
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#pragma once
#ifndef _CPUTEST_H_
#define _CPUTEST_H_

#include <stdbool.h>
#include <stdint.h>
#include "cputest/harness/ki_cputest.h"

/*
 * The iris CPU test suite (cputest/), run from the memory test ROM.
 *
 * The suite is a separate n32 image embedded in the ROM. cputest_run() hands
 * it the CPU for the whole run and gets it back with every register, the CP0
 * and FPU state the ROM depends on, and the caches put back. Results come back
 * through the status block and log in ki_cputest.h, which outlive a watchdog
 * reset: a run that hangs the CPU is reported on the next boot, by the name of
 * the test it was in, instead of the board restarting it forever.
 */

typedef enum
{
    CPUTEST_PENDING, // not run yet
    CPUTEST_FINISHED,
    CPUTEST_HUNG, // the board restarted during the previous run
} cputest_state_t;

// The suite's lines worth showing: everything but passes, skips and the
// banner. A line longer than the screen comes back as several, broken at a
// space, the later ones drawn CPUTEST_INDENT characters in.
#define CPUTEST_COLUMNS 52u
#define CPUTEST_INDENT 4u

typedef enum
{
    CPUTEST_LINE_FAIL,   // a test name, ending in FAIL
    CPUTEST_LINE_DETAIL, // a failed check: where, what it got and wanted
    CPUTEST_LINE_NOTE,   // anything else the suite printed
} cputest_line_kind_t;

typedef struct
{
    const char *text; // not NUL-terminated
    uint8_t length;
    cputest_line_kind_t kind;
    bool continued; // the rest of the line above
} cputest_line_t;

// At view load: picks up a run the watchdog cut short.
void cputest_load(void);

// START: forget the results, and a hang, so that the next step runs it again.
void cputest_reset(void);

// Runs the suite unless the last run hung; blocks for the whole run.
void cputest_run(void);

cputest_state_t cputest_state(void);
const volatile cputest_status_t *cputest_status(void);

uint32_t cputest_line_count(void);
uint32_t cputest_lines(uint32_t first, cputest_line_t *lines, uint32_t max);

// The failed tests, in the order they ran, each with the report line its
// name is on.
typedef struct
{
    const char *name; // not NUL-terminated
    uint8_t length;
    uint32_t line;
} cputest_fail_t;

uint32_t cputest_fail_count(void);
uint32_t cputest_fails(uint32_t first, cputest_fail_t *fails, uint32_t max);

#endif
