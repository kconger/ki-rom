/**
 * SPDX-FileCopyrightText: © 2023 Leandre Gohy <leandre.gohy@hexeo.be>
 * SPDX-License-Identifier: AGPL-3.0-only
 */

#include <stdint.h>
#include "cputest.h"
#include "font.h"
#include "io.h"

/*
 * The status block is written by the suite uncached, so it is read the same
 * way. The log is read cached: the suite writes it uncached and empties both
 * caches on the way out, so no stale line of it can be left behind.
 */
#define STATUS ((volatile cputest_status_t *)(uintptr_t)CPUTEST_STATUS_ADDR)
#define LOG ((const char *)(uintptr_t)(CPUTEST_LOG_ADDR - 0x20000000u))

/*
 * The report is indexed once, when a run ends, so that drawing a page reads
 * only its own lines. One word per screen line, in DRAM straight after the log
 * and as clear of the memory benchmarks' windows as the log is:
 *
 *   bits 31..14  offset of the line in the log (the log is 256 KiB)
 *   bits 13..8   its length, at most CPUTEST_COLUMNS
 *   bit   2      it continues the line above, and is drawn indented
 *   bits  1..0   its cputest_line_kind_t
 */
#define INDEX ((uint32_t *)(uintptr_t)(CPUTEST_LOG_ADDR - 0x20000000u + CPUTEST_LOG_SIZE))
#define INDEX_MAX 16384u
#define INDEX_MORE 0x4u

// Where each failed test's name line sits in INDEX, in order: the list of
// failed tests the first page shows. Straight after INDEX.
#define FAILS (INDEX + INDEX_MAX)
#define FAILS_MAX 1024u

_Static_assert(CPUTEST_LOG_SIZE <= (1u << 18), "log offsets must fit the index");
_Static_assert(CPUTEST_COLUMNS < 64u, "line lengths must fit the index");

// status.state for a run that is known to have hung: no longer RUNNING, so a
// later reset is not taken for a second hang, and still not DONE.
#define CPUTEST_HUNG_STATE 3u

// The suite's first word; it copies itself to DRAM and returns the rc.
extern const uint32_t cputest_image[];

// The framebuffer banks (boot.ld), which the suite names each test on.
extern uint16_t gVramBank0[];
extern uint16_t gVramBank1[];

static cputest_state_t state = CPUTEST_PENDING;
static uint32_t line_count = 0;
static uint32_t fail_count = 0;

static void build_index(void);

void cputest_load(void)
{
    if (STATUS->magic != CPUTEST_MAGIC)
    {
        return;
    }

    if (STATUS->state == CPUTEST_RUNNING || STATUS->state == CPUTEST_HUNG_STATE)
    {
        STATUS->state = CPUTEST_HUNG_STATE;
        state = CPUTEST_HUNG;
        build_index();
    }
}

void cputest_reset(void)
{
    if (state == CPUTEST_HUNG)
    {
        STATUS->state = 0;
    }

    state = CPUTEST_PENDING;
    line_count = 0;
    fail_count = 0;
}

void cputest_run(void)
{
    if (state == CPUTEST_HUNG)
    {
        return;
    }

    STATUS->wdt_addr = (uint32_t)(uintptr_t)&gIO.dipSwitch;
    STATUS->font_addr = (uint32_t)(uintptr_t)font;
    STATUS->screen0 = (uint32_t)(uintptr_t)gVramBank0;
    STATUS->screen1 = (uint32_t)(uintptr_t)gVramBank1;

    ((uint32_t(*)(void))(uintptr_t)cputest_image)();

    state = CPUTEST_FINISHED;
    build_index();
}

cputest_state_t cputest_state(void)
{
    return state;
}

const volatile cputest_status_t *cputest_status(void)
{
    return STATUS;
}

static bool head_is(const char *head, const char *prefix)
{
    for (uint32_t i = 0; prefix[i] != '\0'; i++)
    {
        if (head[i] != prefix[i])
        {
            return false;
        }
    }

    return true;
}

// `tail` holds a line's last six characters, oldest first.
static bool tail_is(const char *tail, const char *suffix)
{
    uint32_t n = 0;

    while (suffix[n] != '\0')
    {
        n++;
    }

    return head_is(tail + 6u - n, suffix);
}

/*
 * Passes and skips are the bulk of the log and say nothing a count does not.
 * The banner, the header and the RESULT line are what the summary already
 * shows. Everything else is a failure or something the suite chose to report.
 *
 * `head` is the line's first 16 characters and `tail` its last 6, each padded
 * with NULs when the line is shorter.
 */
static bool line_is_shown(const char *head, const char *tail, uint32_t length, bool blank)
{
    if (length == 0u || blank || head[0] == '=')
    {
        return false;
    }

    if (tail_is(tail, " PASS") || tail_is(tail, " only)"))
    {
        return false;
    }

    return !head_is(head, " CPU test suite") &&
           !head_is(head, "   PRId") &&
           !head_is(head, "   Config") &&
           !head_is(head, " RESULT:");
}

static cputest_line_kind_t line_kind(const char *head, const char *tail)
{
    if (tail_is(tail, " FAIL"))
    {
        return CPUTEST_LINE_FAIL;
    }

    if (head_is(head, "  "))
    {
        return CPUTEST_LINE_DETAIL;
    }

    return CPUTEST_LINE_NOTE;
}

/*
 * Whether a long line may break at the space at `at`: any space, except the
 * one between "got" or "want" and its value, which stay together.
 */
static bool is_break(const char *line, uint32_t at)
{
    if (line[at] != ' ')
    {
        return false;
    }

    return !(at >= 4u && line[at - 4u] == ' ' && line[at - 3u] == 'g' && line[at - 2u] == 'o' && line[at - 1u] == 't') &&
           !(at >= 5u && line[at - 5u] == ' ' && line[at - 4u] == 'w' && line[at - 3u] == 'a' && line[at - 2u] == 'n' &&
             line[at - 1u] == 't');
}

/*
 * One pass over the log, reading each byte once: on the board the log is in
 * DRAM and the scan is the whole cost. A line is judged by its first 16 and
 * last 6 characters, which is all the filter looks at.
 */
static void build_index(void)
{
    uint32_t length = STATUS->log_len;
    char head[17];
    char tail[7];
    uint32_t start = 0;
    bool blank = true;

    if (length > CPUTEST_LOG_SIZE)
    {
        length = CPUTEST_LOG_SIZE;
    }

    uint32_t test_start = 0;

    line_count = 0;
    fail_count = 0;
    head[16] = '\0';
    tail[6] = '\0';
    for (uint32_t i = 0; i < 16u; i++)
    {
        head[i] = '\0';
    }
    for (uint32_t i = 0; i < 6u; i++)
    {
        tail[i] = '\0';
    }

    // One past the end counts as a newline, so that a log cut short by a hang
    // still shows its last line.
    for (uint32_t pos = 0; pos <= length; pos++)
    {
        const char c = pos < length ? LOG[pos] : '\n';

        if (c != '\n')
        {
            const uint32_t column = pos - start;
            if (column < 16u)
            {
                head[column] = c;
            }
            for (uint32_t i = 0; i < 5u; i++)
            {
                tail[i] = tail[i + 1u];
            }
            tail[5] = c;
            if (c != ' ')
            {
                blank = false;
            }
            continue;
        }

        const uint32_t line_length = pos - start;

        // A test's name starts at column 0 and everything it prints under it
        // is indented. A test that printed notes and passed ends on a note
        // with PASS run onto it: nothing there is needed to fix anything, so
        // its name and notes come off the screen again. The log keeps them.
        if (head[0] != ' ')
        {
            test_start = line_count;
        }

        if (head[0] == ' ' && tail_is(tail, "PASS"))
        {
            line_count = test_start;
            while (fail_count > 0u && FAILS[fail_count - 1u] >= test_start)
            {
                fail_count--;
            }
        }
        else if (line_is_shown(head, tail, line_length, blank))
        {
            const uint32_t kind = line_kind(head, tail);

            uint32_t offset = 0;

            if (kind == CPUTEST_LINE_FAIL && fail_count < FAILS_MAX && line_count < INDEX_MAX)
            {
                FAILS[fail_count++] = line_count;
            }

            while (offset < line_length && line_count < INDEX_MAX)
            {
                const bool more = offset != 0u;
                const uint32_t width = more ? CPUTEST_COLUMNS - CPUTEST_INDENT : CPUTEST_COLUMNS;
                uint32_t segment = line_length - offset;
                uint32_t next;

                if (segment > width)
                {
                    // Break at the last space that fits, so that a value is
                    // never split; a line with no space in reach is cut.
                    segment = width;
                    while (segment > CPUTEST_INDENT && !is_break(LOG + start + offset, segment))
                    {
                        segment--;
                    }
                    if (segment <= CPUTEST_INDENT)
                    {
                        segment = width;
                        next = offset + width;
                    }
                    else
                    {
                        next = offset + segment + 1u;
                    }
                }
                else
                {
                    next = line_length;
                }

                INDEX[line_count++] = ((start + offset) << 14) | (segment << 8) | (more ? INDEX_MORE : 0u) | kind;
                offset = next;
            }
        }

        start = pos + 1u;
        blank = true;
        for (uint32_t i = 0; i < 16u; i++)
        {
            head[i] = '\0';
        }
        for (uint32_t i = 0; i < 6u; i++)
        {
            tail[i] = '\0';
        }
    }
}

uint32_t cputest_line_count(void)
{
    return line_count;
}

uint32_t cputest_lines(uint32_t first, cputest_line_t *lines, uint32_t max)
{
    uint32_t filled = 0;

    for (uint32_t i = first; i < line_count && filled < max; i++, filled++)
    {
        const uint32_t entry = INDEX[i];

        lines[filled].text = LOG + (entry >> 14);
        lines[filled].length = (uint8_t)((entry >> 8) & 0x3Fu);
        lines[filled].kind = (cputest_line_kind_t)(entry & 0x3u);
        lines[filled].continued = (entry & INDEX_MORE) != 0u;
    }

    return filled;
}

uint32_t cputest_fail_count(void)
{
    return fail_count;
}

uint32_t cputest_fails(uint32_t first, cputest_fail_t *fails, uint32_t max)
{
    uint32_t filled = 0;

    for (uint32_t i = first; i < fail_count && filled < max; i++, filled++)
    {
        const uint32_t line = FAILS[i];
        const uint32_t entry = INDEX[line];
        const char *const text = LOG + (entry >> 14);
        const uint32_t length = (entry >> 8) & 0x3Fu;
        uint32_t name = 0;

        // The name runs up to the dots that lead to FAIL.
        while (name < length && text[name] != ' ')
        {
            name++;
        }

        fails[filled].name = text;
        fails[filled].length = (uint8_t)name;
        fails[filled].line = line;
    }

    return filled;
}
