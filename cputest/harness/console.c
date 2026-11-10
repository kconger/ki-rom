/* console.c — the console, as a log in uncached DRAM for the KI test ROM.
 *
 * The upstream harness writes the Indy's serial port. The KI board has none,
 * so every byte goes to the log at CPUTEST_LOG_ADDR instead and the ROM draws
 * it once the run is over. The log is written uncached and its length is
 * published after every byte, so that what a run printed before it hung the
 * CPU is still there after the watchdog has reset the board.
 */

#include "console.h"
#include "ki_cputest.h"

#define STATUS ((volatile cputest_status_t *)SEXT_PTR(CPUTEST_STATUS_ADDR))
#define LOG    ((volatile u64 *)SEXT_PTR(CPUTEST_LOG_ADDR))

void con_init(void)
{
    STATUS->log_len = 0;
}

void wdt_kick(void)
{
    u32 addr = STATUS->wdt_addr;
    if (addr) (void)*(volatile u32 *)SEXT_PTR(addr);
}

/* The ROM's status row, where it prints "Measuring ..." between steps. */
#define PROGRESS_Y      204
#define SCREEN_WIDTH    320
#define GLYPH_WIDTH     7
#define GLYPH_HEIGHT    9
#define GLYPH_ADVANCE   6

/* Everything here is uncached, the font reads included, so that drawing
 * between two tests leaves no trace in either cache. */
#define PROGRESS_X      4
#define PROGRESS_CHARS  ((SCREEN_WIDTH - PROGRESS_X - GLYPH_WIDTH) / GLYPH_ADVANCE + 1)

/* Whether pixel (x, y) of the row is lit: glyph pixel (y, i) is bit
 * 63 - (y * GLYPH_WIDTH + i), and each glyph's last column overlaps the next
 * glyph's first. */
static int lit(const u64 *glyphs, unsigned n, unsigned x, unsigned y)
{
    unsigned j, col;

    if (x < PROGRESS_X || y >= GLYPH_HEIGHT) return 0;
    j = (x - PROGRESS_X) / GLYPH_ADVANCE;
    col = (x - PROGRESS_X) % GLYPH_ADVANCE;
    if (j < n && (glyphs[j] >> (63 - (y * GLYPH_WIDTH + col))) & 1) return 1;
    return col == 0 && j > 0 && j - 1 < n &&
           ((glyphs[j - 1] >> (63 - (y * GLYPH_WIDTH + GLYPH_ADVANCE))) & 1);
}

/* The row is drawn four pixels to a doubleword store, every pixel of it
 * written, so that no store is narrower than 64 bits (see ki_cputest.h). */
static void draw_progress(u32 bank, const char *name)
{
    volatile u64 *row = (volatile u64 *)SEXT_PTR(K0_TO_K1(bank)) + PROGRESS_Y * SCREEN_WIDTH / 4;
    const volatile u8 *font = (const volatile u8 *)SEXT_PTR(K0_TO_K1(STATUS->font_addr));
    static const char prefix[] = "CPU test: ";
    u64 glyphs[PROGRESS_CHARS];
    unsigned n = 0, x, y, i;
    const char *s;

    for (s = prefix; *s && n < PROGRESS_CHARS; s++) glyphs[n++] = 0;
    for (s = name; *s && n < PROGRESS_CHARS; s++) glyphs[n++] = 0;
    for (i = 0; i < n; i++) {
        int c = i < sizeof prefix - 1 ? prefix[i] : name[i - (sizeof prefix - 1)];
        unsigned b;
        if (c < 32 || c > 126) c = ' ';
        for (b = 0; b < 8; b++) glyphs[i] |= (u64)font[(c - 32) * 8 + b] << (8 * b);
    }

    for (y = 0; y < GLYPH_HEIGHT + 1; y++) {
        for (x = 0; x < SCREEN_WIDTH; x += 4) {
            u64 quad = 0;
            for (i = 0; i < 4; i++)
                if (lit(glyphs, n, x + i, y)) quad |= (u64)0x7FFF << (16 * i);
            row[(y * SCREEN_WIDTH + x) / 4] = quad;
        }
    }
}

/* Name the test about to run on the ROM's status row, in both banks, so that
 * one that hangs the CPU stays on screen. */
void show_progress(const char *name)
{
    if (!STATUS->font_addr) return;
    if (STATUS->screen0) draw_progress(STATUS->screen0, name);
    if (STATUS->screen1) draw_progress(STATUS->screen1, name);
}

/* The doubleword of the log being filled, as written so far. */
static u64 log_word;

static int at_line_start = 1;

int con_at_line_start(void)
{
    return at_line_start;
}

void con_putc(int c)
{
    u32 n = STATUS->log_len;

    /* A full log keeps its head: that is where the first failure is. Each
     * character rewrites its whole doubleword, the ones before it included
     * (see ki_cputest.h). */
    if (n < CPUTEST_LOG_SIZE) {
        if ((n & 7) == 0) log_word = 0;
        log_word |= (u64)(u8)c << (8 * (n & 7));
        LOG[n >> 3] = log_word;
        STATUS->log_len = n + 1;
    }
    at_line_start = c == '\n';
    if (c == '\n') wdt_kick();
}

void con_puts(const char *s)
{
    while (*s) con_putc(*s++);
}

static const char hexd[] = "0123456789abcdef";

void con_hex32(u32 v)
{
    int i;
    con_puts("0x");
    for (i = 28; i >= 0; i -= 4) con_putc(hexd[(v >> i) & 0xF]);
}

void con_hex64(u64 v)
{
    int i;
    con_puts("0x");
    for (i = 60; i >= 0; i -= 4) con_putc(hexd[(int)((v >> i) & 0xF)]);
}

void con_hexmin64(u64 v)
{
    int i = 60;
    con_puts("0x");
    while (i > 0 && ((v >> i) & 0xF) == 0) i -= 4;
    for (; i >= 0; i -= 4) con_putc(hexd[(int)((v >> i) & 0xF)]);
}

void con_udec(unsigned long long v)
{
    char buf[24];
    int n = 0;
    if (v == 0) { con_putc('0'); return; }
    while (v) { buf[n++] = (char)('0' + (int)(v % 10)); v /= 10; }
    while (n) con_putc(buf[--n]);
}

void con_dec(long long v)
{
    if (v < 0) { con_putc('-'); con_udec((unsigned long long)(-v)); }
    else con_udec((unsigned long long)v);
}

/* Freestanding varargs: we have no stdarg.h from a libc, but GCC's builtins
 * are always available. */
#define va_list   __builtin_va_list
#define va_start  __builtin_va_start
#define va_arg    __builtin_va_arg
#define va_end    __builtin_va_end

void con_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        if (*fmt != '%') { con_putc(*fmt); continue; }
        fmt++;
        switch (*fmt) {
        case 's': con_puts(va_arg(ap, const char *)); break;
        case 'c': con_putc(va_arg(ap, int)); break;
        case 'd': con_dec(va_arg(ap, int)); break;
        case 'u': con_udec((unsigned)va_arg(ap, unsigned int)); break;
        case 'x': con_hex32(va_arg(ap, u32)); break;
        case 'X': con_hex64(va_arg(ap, u64)); break;
        case 'l':
            /* %lx and %ld both mean 64-bit here. */
            fmt++;
            if (*fmt == 'x') con_hex64(va_arg(ap, u64));
            else if (*fmt == 'd') con_dec(va_arg(ap, long long));
            else if (*fmt == 'u') con_udec(va_arg(ap, unsigned long long));
            else { con_putc('%'); con_putc('l'); con_putc(*fmt); }
            break;
        case '%': con_putc('%'); break;
        case '\0': con_putc('%'); return;
        default:  con_putc('%'); con_putc(*fmt); break;
        }
    }
    va_end(ap);
}

void panic(const char *msg)
{
    con_puts("\nPANIC: ");
    con_puts(msg);
    con_puts("\n");
    cputest_exit(CPUTEST_RC_PANIC);
}
