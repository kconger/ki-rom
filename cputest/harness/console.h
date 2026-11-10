/* console.h — base types and the console.
 *
 * Split out of testlib.h so that code which is not a *test* can use the
 * console without dragging in the CHECK macros and the exception-record
 * plumbing. testlib.h includes this, so every test file sees it.
 *
 * On the KI board the console is a log in uncached DRAM that the test ROM
 * puts on screen afterwards (ki_cputest.h).
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include "iris.h"

typedef unsigned char       u8;
typedef unsigned short      u16;
typedef unsigned int        u32;
typedef unsigned long long  u64;
typedef signed char         s8;
typedef short               s16;
typedef int                 s32;
typedef long long           s64;

/* ── CPU selection ────────────────────────────────────────────────────────── */
/* Bitmask: a test declares which CPUs it is valid on. */
#define CPU_R4400   0x1
#define CPU_R5000   0x2
/* The R4600 is the one part here that no oracle run has covered: its
 * expectations are inferred, and docs/r4600.md says from what. */
#define CPU_R4600   0x4
#define CPU_ALL     (CPU_R4400 | CPU_R5000 | CPU_R4600)

extern u32 cpu_kind;          /* CPU_R4400, CPU_R5000 or CPU_R4600, set at startup */
extern u32 cpu_prid;
extern u32 cpu_fir;
extern u32 cpu_config;
static inline int is_r5000(void) { return cpu_kind == CPU_R5000; }
static inline int is_r4400(void) { return cpu_kind == CPU_R4400; }
static inline int is_r4600(void) { return cpu_kind == CPU_R4600; }
/* MIPS IV is the R5000's alone: the R4400 and the R4600 are both MIPS III
 * parts and must refuse every MIPS IV encoding. */
static inline int has_mips4(void) { return cpu_kind == CPU_R5000; }

/* ── Console ──────────────────────────────────────────────────────────────── */
void con_init(void);
void con_putc(int c);
/* Whether the last character printed was a newline (or nothing has been). */
int con_at_line_start(void);
void con_puts(const char *s);
void con_hex32(u32 v);
void con_hex64(u64 v);
/* 0x and the significant digits only: 0x0, 0x1c, 0xffffffff80000000. */
void con_hexmin64(u64 v);
void con_dec(long long v);
void con_udec(unsigned long long v);
/* Minimal printf: %s %c %d %u %x (32-bit), %X/%lx (64-bit), %% */
void con_printf(const char *fmt, ...);

/* ── KI board ─────────────────────────────────────────────────────────────── */
/* Read the register that restarts the board's watchdog. Called on every line
 * of output and before every test; no single test runs long enough to need
 * more. */
void wdt_kick(void);

/* Name what is about to run on screen, where a hang leaves it (console.c). */
void show_progress(const char *name);

/* Back to the test ROM with `rc`, from any depth (start.S). */
void cputest_exit(u32 rc) __attribute__((noreturn));

/* Halt with a message — unrecoverable harness failure. */
void panic(const char *msg) __attribute__((noreturn));

#endif /* CONSOLE_H */
