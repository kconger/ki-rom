#include <stdint.h>
#include "timing.h"
#include "bench.h"
#include "cache.h"
#include "delay.h"
#include "ide.h"
#include "io.h"
#include "sound.h"
#include "video.h"
#include "wdt.h"

/*
 * Cycle-level timing tests for comparing the original board with the MiSTer
 * core. See timing.h for what is measured and view_memtest.c for the pages.
 *
 * Every kernel here is GENERATED: built as machine code into a DRAM buffer just
 * before it runs, then written back from the data cache and invalidated in the
 * instruction cache. Three reasons:
 *
 * - The measurement block boot.ld links from cpu.S and bench.c has to stay
 *   under 8 KiB and free of I-cache aliasing (NOTES.md), and is nearly full.
 *   Generated code costs the block nothing.
 * - Each kernel times itself. It runs its loop once untimed, which loads its
 *   own lines into both caches, then reads Count around the timed run. Nothing
 *   else runs in between, so where the ROM's own code sits in the I-cache does
 *   not matter, on the core's direct-mapped cache or the board's 2-way one.
 * - Some tests need code at exact addresses (the I-cache set tests) or the
 *   same loop with dozens of different instructions, which is a table here
 *   rather than dozens of hand-written loops.
 *
 * The code is written through KSEG0, never KSEG1: on the KI board an uncached
 * store narrower than 64 bits rewrites its whole doubleword, so writing
 * instruction words uncached would zero their neighbours.
 */

/*
 * DRAM used here, all of it clear of the regions view_memtest.c uses (the
 * window at 0x08000000, the IDE buffer at 0x08100000 and the latency windows
 * from 0x08200000 up) and of the CPU test suite, which runs after this.
 *
 *   0x08110000  24 KiB  D-cache set tests (lines 0, 8 and 16 KiB apart)
 *   0x08116000          one doubleword pointing at itself, for load chains
 *   0x08117000   1 KiB  a loop body being put together
 *   0x08117400   1 KiB  the exception handler the vector jumps to
 *   0x08120000  64 KiB  sixteen 4 KiB pages for the TLB tests
 *   0x08130000   8 KiB  uncached read window
 *   0x08138000   1 KiB  uncached store window
 *   0x08140000 256 KiB  generated kernels, 2 KiB apiece
 *   0x08180000 320 KiB  the I-cache tests, 64 KiB apiece
 *
 * Every kernel built in a run gets a slot of its own rather than all reusing
 * one address. Hardware is told about rewritten code by the cache operations
 * in publish(), but an emulator that translates code ahead (MAME's
 * recompiler) may keep running its translation of whatever ran at an address
 * before; a fresh address never has one.
 */
#define TM_DCACHE_PHYS 0x08110000u
#define TM_CELL_PHYS 0x08116000u
#define TM_BODY_PHYS 0x08117000u
#define TM_HANDLER_PHYS 0x08117400u
#define TM_TLB_PHYS 0x08120000u
#define TM_READ_PHYS 0x08130000u
#define TM_STORE_PHYS 0x08138000u
#define TM_SLOT_PHYS 0x08140000u
#define TM_SLOT_SIZE 0x800u
#define TM_SLOTS 128u
#define TM_ICACHE_PHYS 0x08180000u
#define TM_ICACHE_SIZE 0x10000u

// SRAM: read anywhere below the VRAM banks; written only in view_memtest.c's
// window, and only when the heap leaves it free.
#define TM_SRAM_READ_PHYS 0x00018000u
#define TM_SRAM_STORE_PHYS 0x00014000u
#define TM_VRAM0_PHYS 0x00030000u
#define TM_VRAM1_PHYS 0x00058000u

// Kuseg pages the TLB tests map, and the entries they use.
#define TM_TLB_VA 0x00400000u
#define TM_TLB_FIRST 40u
#define TM_TLB_ENTRIES 8u

// The general exception vector with Status.BEV clear, which the interrupt and
// syscall tests install a handler at. The 0x200 bytes below .data are free.
#define TM_VECTOR 0x80000180u

#define KSEG0(phys) (BENCH_KSEG0 + (phys))
#define KSEG1(phys) (BENCH_KSEG1 + (phys))

tm_results_t tm;

// ── instruction encoding ─────────────────────────────────────────────────────

enum
{
    ZERO = 0,
    V0 = 2,
    V1 = 3,
    A0 = 4,
    A1 = 5,
    A2 = 6,
    A3 = 7,
    T0 = 8,
    T1 = 9,
    T2 = 10,
    T3 = 11,
    T8 = 24,
    T9 = 25,
    K0 = 26,
    K1 = 27,
    RA = 31,
};

#define R_(s, t, d, sa, fn) ((uint32_t)(((uint32_t)(s) << 21) | ((uint32_t)(t) << 16) | ((uint32_t)(d) << 11) | ((uint32_t)(sa) << 6) | (uint32_t)(fn)))
#define I_(op, s, t, imm) ((uint32_t)(((uint32_t)(op) << 26) | ((uint32_t)(s) << 21) | ((uint32_t)(t) << 16) | ((uint32_t)(imm) & 0xFFFFu)))

#define NOP 0u
#define ADDU(d, s, t) R_(s, t, d, 0, 0x21)
#define SUBU(d, s, t) R_(s, t, d, 0, 0x23)
#define OR(d, s, t) R_(s, t, d, 0, 0x25)
#define SLTU(d, s, t) R_(s, t, d, 0, 0x2B)
#define MFHI(d) R_(0, 0, d, 0, 0x10)
#define MFLO(d) R_(0, 0, d, 0, 0x12)
#define MULT(s, t) R_(s, t, 0, 0, 0x18)
#define DIVU(s, t) R_(s, t, 0, 0, 0x1B) // the bare instruction: divu $0,s,t
#define DMULT(s, t) R_(s, t, 0, 0, 0x1C)
#define DDIVU(s, t) R_(s, t, 0, 0, 0x1F)
#define JR(s) R_(s, 0, 0, 0, 0x08)
#define SYSCALL 0x0000000Cu
#define SYNC_ 0x0000000Fu
#define ERET 0x42000018u

#define BEQ(s, t, off) I_(0x04, s, t, off)
#define BNE(s, t, off) I_(0x05, s, t, off)
#define BNEL(s, t, off) I_(0x15, s, t, off)
#define ADDIU(t, s, imm) I_(0x09, s, t, imm)
#define ANDI(t, s, imm) I_(0x0C, s, t, imm)
#define ORI(t, s, imm) I_(0x0D, s, t, imm)
#define XORI(t, s, imm) I_(0x0E, s, t, imm)
#define LUI(t, imm) I_(0x0F, 0, t, imm)
#define LB_(t, off, b) I_(0x20, b, t, off)
#define LWL(t, off, b) I_(0x22, b, t, off)
#define LW(t, off, b) I_(0x23, b, t, off)
#define LBU(t, off, b) I_(0x24, b, t, off)
#define LHU(t, off, b) I_(0x25, b, t, off)
#define LWR(t, off, b) I_(0x26, b, t, off)
#define LDL(t, off, b) I_(0x1A, b, t, off)
#define LDR(t, off, b) I_(0x1B, b, t, off)
#define SB(t, off, b) I_(0x28, b, t, off)
#define SH(t, off, b) I_(0x29, b, t, off)
#define SW(t, off, b) I_(0x2B, b, t, off)
#define CACHE(op, off, b) I_(0x2F, b, op, off)
#define LWC1(ft, off, b) I_(0x31, b, ft, off)
#define LDC1(ft, off, b) I_(0x35, b, ft, off)
#define LD(t, off, b) I_(0x37, b, t, off)
#define SD(t, off, b) I_(0x3F, b, t, off)

#define MFC0(t, d) ((uint32_t)((0x10u << 26) | (0u << 21) | ((uint32_t)(t) << 16) | ((uint32_t)(d) << 11)))
#define MTC0(t, d) ((uint32_t)((0x10u << 26) | (4u << 21) | ((uint32_t)(t) << 16) | ((uint32_t)(d) << 11)))
#define CP0_COUNT 9
#define CP0_COMPARE 11
#define CP0_STATUS 12
#define CP0_CAUSE 13
#define CP0_EPC 14

// COP1: fmt S = 16, D = 17, W = 20.
#define F_(fmt, ft, fs, fd, fn) ((uint32_t)((0x11u << 26) | ((uint32_t)(fmt) << 21) | ((uint32_t)(ft) << 16) | ((uint32_t)(fs) << 11) | ((uint32_t)(fd) << 6) | (uint32_t)(fn)))
#define FS 16
#define FD 17
#define FW 20
#define ADD_F(fmt, fd, fs, ft) F_(fmt, ft, fs, fd, 0x00)
#define MUL_F(fmt, fd, fs, ft) F_(fmt, ft, fs, fd, 0x02)
#define DIV_F(fmt, fd, fs, ft) F_(fmt, ft, fs, fd, 0x03)
#define SQRT_F(fmt, fd, fs) F_(fmt, 0, fs, fd, 0x04)
#define MOV_F(fmt, fd, fs) F_(fmt, 0, fs, fd, 0x06)
#define NEG_F(fmt, fd, fs) F_(fmt, 0, fs, fd, 0x07)
#define CVT_S(fmt, fd, fs) F_(fmt, 0, fs, fd, 0x20)
#define CVT_D(fmt, fd, fs) F_(fmt, 0, fs, fd, 0x21)
#define CVT_W(fmt, fd, fs) F_(fmt, 0, fs, fd, 0x24)
#define C_EQ(fmt, fs, ft) F_(fmt, ft, fs, 0, 0x32)
#define MFC1(t, fs) ((uint32_t)((0x11u << 26) | (0u << 21) | ((uint32_t)(t) << 16) | ((uint32_t)(fs) << 11)))
#define MTC1(t, fs) ((uint32_t)((0x11u << 26) | (4u << 21) | ((uint32_t)(t) << 16) | ((uint32_t)(fs) << 11)))
#define BC1T(off) ((uint32_t)((0x11u << 26) | (8u << 21) | (1u << 16) | ((uint32_t)(off) & 0xFFFFu)))

// ── the code buffer ──────────────────────────────────────────────────────────

/*
 * The registers a kernel starts with. The prologue loads t0-t3, a2, a3 and
 * f0, f2, f4, f6 from here (8-byte aligned for ld and ldc1). FR is 1, so a
 * single-precision value sits in the low word of its register.
 */
typedef struct
{
    uint64_t t[4];
    uint64_t a2;
    uint64_t a3;
    uint64_t f[4];
} tm_regs_t;

typedef uint32_t (*tm_fn_t)(uint32_t iterations, const tm_regs_t *regs);

typedef struct
{
    uint32_t *w;
    uint32_t n;
} emit_t;

// The slot the last build() wrote, and the I-cache region in use.
static uint32_t slot;
static uint32_t ic_region;

static inline uint32_t *slot_code(void)
{
    return (uint32_t *)(uintptr_t)KSEG0(TM_SLOT_PHYS + slot * TM_SLOT_SIZE);
}

static inline uint32_t *code_at(const uint32_t offset)
{
    return (uint32_t *)(uintptr_t)KSEG0(TM_ICACHE_PHYS + ic_region * TM_ICACHE_SIZE + offset);
}

// A fresh slot for the next kernel.
static uint32_t *next_slot(void)
{
    slot = (slot + 1u) % TM_SLOTS;
    return slot_code();
}

static inline void emit(emit_t *e, const uint32_t word)
{
    e->w[e->n++] = word;
}

// Branch offset from the branch about to be emitted at e->n to word `target`.
static inline uint32_t to(const emit_t *e, const uint32_t target)
{
    return target - (e->n + 1u);
}

/*
 * Out of the data cache to DRAM, and out of the instruction cache, so the
 * next fetch reads what was just written. Hit operations: only these lines.
 */
static void publish(const uint32_t *words, const uint32_t count)
{
    uint32_t addr = (uint32_t)(uintptr_t)words & ~(uint32_t)(CACHE_LINE_SIZE - 1);
    const uint32_t end = (uint32_t)(uintptr_t)(words + count);

    for (; addr < end; addr += CACHE_LINE_SIZE)
    {
        CACHE_OP(HIT_WRITEBACK_INVALIDATE_D, addr, 0);
        CACHE_OP(HIT_INVALIDATE_I, addr, 0);
    }
    SYNC();
}

/*
 * Every line of both caches: the data cache written back and invalidated, the
 * instruction cache invalidated, by index. 16 KiB of KSEG0 indexes every line
 * of a 16 KiB cache, 2-way or direct-mapped.
 */
static void flush_all(void)
{
    for (uint32_t addr = BENCH_KSEG0; addr < BENCH_KSEG0 + 0x4000u; addr += CACHE_LINE_SIZE)
    {
        CACHE_OP(INDEX_WRITEBACK_INVALIDATE_D, addr, 0);
        CACHE_OP(INDEX_INVALIDATE_I, addr, 0);
    }
    SYNC();
}

static inline uint32_t cp0_count(void)
{
    register uint32_t count;
    asm volatile("mfc0 %0,$9" : "=r"(count) : : "memory");
    return count;
}

static inline uint32_t cp0_status(void)
{
    register uint32_t value;
    asm volatile("mfc0 %0,$12" : "=r"(value) : : "memory");
    return value;
}

static inline void cp0_status_set(const uint32_t value)
{
    asm volatile(".set push\n.set noreorder\nmtc0 %0,$12\nnop\nnop\n.set pop" : : "r"(value) : "memory");
}

static inline void cp0_compare_set(const uint32_t value)
{
    asm volatile(".set push\n.set noreorder\nmtc0 %0,$11\nnop\nnop\n.set pop" : : "r"(value) : "memory");
}

static inline uint32_t cp0_cause(void)
{
    register uint32_t value;
    asm volatile("mfc0 %0,$13" : "=r"(value) : : "memory");
    return value;
}

static void prologue(emit_t *e)
{
    emit(e, LD(T0, 0, A1));
    emit(e, LD(T1, 8, A1));
    emit(e, LD(T2, 16, A1));
    emit(e, LD(T3, 24, A1));
    emit(e, LD(A2, 32, A1));
    emit(e, LD(A3, 40, A1));
    emit(e, LDC1(0, 48, A1));
    emit(e, LDC1(2, 56, A1));
    emit(e, LDC1(4, 64, A1));
    emit(e, LDC1(6, 72, A1));
}

/*
 * The standard kernel: `body` in a loop, run once untimed and then
 * `iterations` times timed, returning the timed run in Count ticks. The loop
 * control is cpu.S's, two instructions per pass:
 *
 *       prologue
 *       or    t8,a0,zero        timed count
 *       ori   a0,zero,1         the untimed pass
 *       ori   t9,zero,2
 *   outer:
 *       mfc0  v1,Count
 *       nop
 *       addiu a0,a0,-1
 *   loop:
 *       body
 *       bne   a0,zero,loop
 *       addiu a0,a0,-1
 *       mfc0  v0,Count
 *       nop
 *       addiu t9,t9,-1
 *       bne   t9,zero,outer
 *       or    a0,t8,zero
 *       jr    ra
 *       subu  v0,v0,v1
 *
 * A body may use t0-t3, a2, a3 and f0-f15, and nothing else.
 */
static void build(const uint32_t *body, const uint32_t count)
{
    emit_t e = {next_slot(), 0};

    prologue(&e);
    emit(&e, OR(T8, A0, ZERO));
    emit(&e, ORI(A0, ZERO, 1));
    emit(&e, ORI(T9, ZERO, 2));
    const uint32_t outer = e.n;
    emit(&e, MFC0(V1, CP0_COUNT));
    emit(&e, NOP);
    emit(&e, ADDIU(A0, A0, -1));
    const uint32_t loop = e.n;
    for (uint32_t i = 0; i < count; i++)
    {
        emit(&e, body[i]);
    }
    emit(&e, BNE(A0, ZERO, to(&e, loop)));
    emit(&e, ADDIU(A0, A0, -1));
    emit(&e, MFC0(V0, CP0_COUNT));
    emit(&e, NOP);
    emit(&e, ADDIU(T9, T9, -1));
    emit(&e, BNE(T9, ZERO, to(&e, outer)));
    emit(&e, OR(A0, T8, ZERO));
    emit(&e, JR(RA));
    emit(&e, SUBU(V0, V0, V1));

    publish(e.w, e.n);
}

static inline uint32_t run(const uint32_t iterations, const tm_regs_t *regs)
{
    return ((tm_fn_t)(uintptr_t)slot_code())(iterations, regs);
}

// Fastest of BENCH_PASSES runs, in ticks.
static uint32_t best(const uint32_t iterations, const tm_regs_t *regs)
{
    uint32_t fastest = 0xFFFFFFFFu;
    for (uint32_t pass = 0; pass < BENCH_PASSES; pass++)
    {
        const uint32_t ticks = run(iterations, regs);
        if (ticks < fastest)
        {
            fastest = ticks;
        }
    }
    return fastest;
}

/*
 * Where a kernel's loop body is put together before build() copies it into
 * the code buffer. In DRAM rather than .bss: .data and .bss share low SRAM
 * with the stack, and there is no room for it there.
 */
#define body ((uint32_t *)(uintptr_t)KSEG0(TM_BODY_PHYS))

// `pattern` repeated `reps` times, timed: cycles per pattern, x100.
static uint32_t measure(const uint32_t *pattern, const uint32_t length, const uint32_t reps,
                        const uint32_t iterations, const tm_regs_t *regs)
{
    uint32_t n = 0;
    for (uint32_t r = 0; r < reps; r++)
    {
        for (uint32_t i = 0; i < length; i++)
        {
            body[n++] = pattern[i];
        }
    }
    build(body, n);
    return bench_cycles_x100(iterations * reps, best(iterations, regs));
}

// The prepared body, `ops` operations in it.
static uint32_t measure_body(const uint32_t count, const uint32_t ops, const uint32_t iterations,
                             const tm_regs_t *regs)
{
    build(body, count);
    return bench_cycles_x100(iterations * ops, best(iterations, regs));
}

static inline uint64_t sext(const uint32_t address)
{
    return (uint64_t)(int64_t)(int32_t)address;
}

// The doubleword at TM_CELL_PHYS holds its own address, so `ld a2,0(a2)` and
// `lw a2,0(a2)` from it go nowhere, forever: a load chain that always hits.
static uint64_t cell(void)
{
    volatile uint64_t *const p = (volatile uint64_t *)(uintptr_t)KSEG0(TM_CELL_PHYS);
    p[0] = sext(KSEG0(TM_CELL_PHYS));
    p[1] = 0;
    return sext(KSEG0(TM_CELL_PHYS));
}

// Kernel iterations for about 100k cycles at `est` cycles per op.
static uint32_t iters_for(const uint32_t ops, const uint32_t est)
{
    const uint32_t n = 100000u / (ops * est);
    return n < 2u ? 2u : n;
}

// ── step 0: integer instructions ─────────────────────────────────────────────

typedef struct
{
    uint8_t length;
    uint8_t est; // cycles per pattern, roughly, to size the run
    uint32_t w[4];
} tm_op_t;

const char *const tm_int_names[TM_INT_COUNT] = {
    "mult>mflo", "mult>mfhi", "dmult>mflo", "divu>mflo", "ddivu>mflo", "beq taken",
    "bne untaken", "bnel untaken", "lw>addu", "lbu>addu", "ld chain", "lwl+lwr",
    "ldl+ldr", "sw>lw", "sync", "mfc0 Count"};

/*
 * Each is a pattern repeated to fill the loop. A pair such as mult>mflo is
 * timed as a pair: mflo waits for the multiply, so the pair costs the
 * multiply's latency. The branches skip only their own delay slot, so taken
 * and not taken run the same instructions.
 */
static const tm_op_t int_ops[TM_INT_COUNT] = {
    {2, 12, {MULT(T0, T1), MFLO(T2)}},
    {2, 12, {MULT(T0, T1), MFHI(T2)}},
    {2, 20, {DMULT(T0, T1), MFLO(T2)}},
    {2, 40, {DIVU(T0, T1), MFLO(T2)}},
    {2, 80, {DDIVU(T0, T1), MFLO(T2)}},
    {2, 3, {BEQ(ZERO, ZERO, 1), NOP}},
    {2, 2, {BNE(ZERO, ZERO, 1), NOP}},
    {2, 2, {BNEL(ZERO, ZERO, 1), NOP}},
    {2, 3, {LW(T0, 0, A2), ADDU(T1, T0, T0)}},
    {2, 3, {LBU(T0, 0, A2), ADDU(T1, T0, T0)}},
    {1, 3, {LD(A2, 0, A2)}},
    {2, 3, {LWL(T0, 3, A2), LWR(T0, 0, A2)}},
    {2, 3, {LDL(T0, 7, A2), LDR(T0, 0, A2)}},
    {2, 3, {SW(T0, 8, A2), LW(T1, 8, A2)}},
    {1, 4, {SYNC_}},
    {1, 3, {MFC0(T0, CP0_COUNT)}},
};

static void measure_int(void)
{
    const tm_regs_t regs = {.t = {0x12345678u, 0x00000ABCu, 0, 0}, .a2 = cell()};

    for (uint32_t i = 0; i < TM_INT_COUNT; i++)
    {
        const tm_op_t *const op = &int_ops[i];
        const uint32_t reps = 64u / op->length;
        tm.int_cyc[i] = measure(op->w, op->length, reps, iters_for(reps, op->est), &regs);
        wdt_reset();
    }
}

// ── step 1: floating point ───────────────────────────────────────────────────

const char *const tm_fpu_names[TM_FPU_COUNT] = {
    "add.d", "mul.s", "mul.d", "div.s", "div.d", "sqrt.s", "sqrt.d", "cvt.d.s/s.d",
    "cvt.w.s/s.w", "mov.s", "neg.s", "c.eq+bc1t", "mtc1>mfc1", "lwc1>add.s",
    "add.s x4 dest", "mul.d x4 dest"};

// Which register set each runs with: single or double precision values.
static const uint8_t fpu_double[TM_FPU_COUNT] = {1, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1};

/*
 * Dependent chains, f0 = f0 op f2, with f2 = 1.0 so every value stays put:
 * sqrt converges on 1.0 and stays there, and the conversions round trip. The
 * last two are throughput, four destinations from the same two sources.
 */
static const tm_op_t fpu_ops[TM_FPU_COUNT] = {
    {1, 4, {ADD_F(FD, 0, 0, 2)}},
    {1, 8, {MUL_F(FS, 0, 0, 2)}},
    {1, 8, {MUL_F(FD, 0, 0, 2)}},
    {1, 40, {DIV_F(FS, 0, 0, 2)}},
    {1, 70, {DIV_F(FD, 0, 0, 2)}},
    {1, 40, {SQRT_F(FS, 0, 0)}},
    {1, 70, {SQRT_F(FD, 0, 0)}},
    {2, 8, {CVT_D(FS, 0, 0), CVT_S(FD, 0, 0)}},
    {2, 8, {CVT_W(FS, 0, 0), CVT_S(FW, 0, 0)}},
    {1, 4, {MOV_F(FS, 0, 0)}},
    {1, 4, {NEG_F(FS, 0, 0)}},
    {3, 6, {C_EQ(FS, 0, 0), BC1T(1), NOP}},
    {2, 6, {MTC1(T0, 8), MFC1(T0, 8)}},
    {2, 6, {LWC1(8, 0, A2), ADD_F(FS, 10, 8, 2)}},
    {4, 8, {ADD_F(FS, 8, 0, 2), ADD_F(FS, 10, 0, 2), ADD_F(FS, 12, 0, 2), ADD_F(FS, 14, 0, 2)}},
    {4, 12, {MUL_F(FD, 8, 0, 2), MUL_F(FD, 10, 0, 2), MUL_F(FD, 12, 0, 2), MUL_F(FD, 14, 0, 2)}},
};

static void measure_fpu(void)
{
    if (!bench_fpu_available())
    {
        for (uint32_t i = 0; i < TM_FPU_COUNT; i++)
        {
            tm.fpu_cyc[i] = TM_SKIPPED;
        }
        return;
    }

    const uint64_t a2 = cell();
    const tm_regs_t single = {.t = {0x12345678u, 0, 0, 0}, .a2 = a2,
                              .f = {0x40490FDBu, 0x3F800000u, 0x40400000u, 0x40000000u}};
    const tm_regs_t dbl = {.t = {0x12345678u, 0, 0, 0}, .a2 = a2,
                           .f = {0x400921FB54442D18ull, 0x3FF0000000000000ull, 0x4008000000000000ull, 0x4000000000000000ull}};

    for (uint32_t i = 0; i < TM_FPU_COUNT; i++)
    {
        const tm_op_t *const op = &fpu_ops[i];
        const uint32_t reps = 63u / op->length;
        tm.fpu_cyc[i] = measure(op->w, op->length, reps, iters_for(reps, op->est), fpu_double[i] ? &dbl : &single);
        wdt_reset();
    }
}

// ── step 2: data cache ───────────────────────────────────────────────────────

const char *const tm_dc_names[TM_DC_COUNT] = {
    "2 lines/set", "3 lines/set", "A B A C", "dirty victim", "idx wb-inv",
    "sw+hit wb-inv", "lw+hit inv"};

/*
 * Loads at 0, 8 KiB and 16 KiB fall in the same set of a 16 KiB 2-way cache
 * with 8 KiB ways. Two of them fit; three do not, and how many of the three
 * still hit says which one replacement picks: least recently used misses
 * every time on A B C and only on B and C in A B A C, random hits sometimes.
 * "dirty victim" is A B C with A stored to, so every eviction of A also
 * writes it back. The cache operations walk 64 lines, one operation each.
 */
#define DC_A 0
#define DC_B 0x2000
#define DC_C 0x4000

static void measure_dcache(void)
{
    const tm_regs_t regs = {.a2 = sext(KSEG0(TM_DCACHE_PHYS))};
    uint32_t n;

    n = 0;
    for (uint32_t i = 0; i < 32u; i++)
    {
        body[n++] = LW(T0, DC_A, A2);
        body[n++] = LW(T0, DC_B, A2);
    }
    tm.dc_cyc[0] = measure_body(n, n, 256, &regs);

    n = 0;
    for (uint32_t i = 0; i < 21u; i++)
    {
        body[n++] = LW(T0, DC_A, A2);
        body[n++] = LW(T0, DC_B, A2);
        body[n++] = LW(T0, DC_C, A2);
    }
    tm.dc_cyc[1] = measure_body(n, n, 32, &regs);

    n = 0;
    for (uint32_t i = 0; i < 16u; i++)
    {
        body[n++] = LW(T0, DC_A, A2);
        body[n++] = LW(T0, DC_B, A2);
        body[n++] = LW(T0, DC_A, A2);
        body[n++] = LW(T0, DC_C, A2);
    }
    tm.dc_cyc[2] = measure_body(n, n, 32, &regs);
    wdt_reset();

    n = 0;
    for (uint32_t i = 0; i < 21u; i++)
    {
        body[n++] = SW(ZERO, DC_A, A2);
        body[n++] = LW(T0, DC_B, A2);
        body[n++] = LW(T0, DC_C, A2);
    }
    tm.dc_cyc[3] = measure_body(n, n, 32, &regs);

    n = 0;
    for (uint32_t i = 0; i < 64u; i++)
    {
        body[n++] = CACHE(INDEX_WRITEBACK_INVALIDATE_D, i * CACHE_LINE_SIZE, A2);
    }
    tm.dc_cyc[4] = measure_body(n, n, 64, &regs);

    n = 0;
    for (uint32_t i = 0; i < 32u; i++)
    {
        body[n++] = SW(ZERO, i * CACHE_LINE_SIZE, A2);
        body[n++] = CACHE(HIT_WRITEBACK_INVALIDATE_D, i * CACHE_LINE_SIZE, A2);
    }
    tm.dc_cyc[5] = measure_body(n, 32, 32, &regs);

    n = 0;
    for (uint32_t i = 0; i < 32u; i++)
    {
        body[n++] = LW(T0, i * CACHE_LINE_SIZE, A2);
        body[n++] = CACHE(HIT_INVALIDATE_D, i * CACHE_LINE_SIZE, A2);
    }
    tm.dc_cyc[6] = measure_body(n, 32, 32, &regs);
    wdt_reset();
}

// ── step 3: instruction cache ────────────────────────────────────────────────

const char *const tm_ic_names[TM_IC_COUNT] = {
    "1 block", "2 blk +8K", "2 blk +16K", "3 blk 0/8/16K", "32K stream", "ROM uncached"};

/*
 * Straight-line blocks of nops at chosen offsets into the code buffer, each
 * ending in a branch to the next; the last one closes the loop. The core's
 * I-cache is direct-mapped, 16 KiB: lines 16 KiB apart evict each other. The
 * board's is 2-way with 8 KiB ways: lines 8 or 16 KiB apart share a set, and
 * two of them fit. So
 *
 *   2 blocks 8 KiB apart     fit both
 *   2 blocks 16 KiB apart    fit the board, thrash the core
 *   3 blocks 0, 8, 16 KiB    thrash the board (3 in a 2-way set) and two of
 *                            the three on the core
 *   32 KiB in a row          misses every line on both: the refill from DRAM
 *
 * The timing code sits at +0xD000, whose lines none of the blocks share.
 * Reported in cycles per instruction executed.
 */
#define IC_BLOCK_WORDS 64u
#define IC_HOME 0xD000u

static uint32_t ic_build(const uint32_t *offsets, const uint32_t blocks, const uint32_t words)
{
    const uint32_t home_word = IC_HOME / 4u;
    const uint32_t first_word = offsets[0] / 4u;

    // The blocks, addressed in words from the start of the buffer.
    for (uint32_t b = 0; b < blocks; b++)
    {
        const uint32_t start = offsets[b] / 4u;
        emit_t e = {code_at(0), start};
        const bool last = (b + 1u) == blocks;
        const uint32_t pad = last ? (words - 4u) : (words - 2u);

        for (uint32_t i = 0; i < pad; i++)
        {
            emit(&e, NOP);
        }
        if (!last)
        {
            emit(&e, BEQ(ZERO, ZERO, to(&e, offsets[b + 1u] / 4u)));
            emit(&e, NOP);
        }
        else
        {
            emit(&e, BNE(A0, ZERO, to(&e, first_word)));
            emit(&e, ADDIU(A0, A0, -1));
            emit(&e, BEQ(ZERO, ZERO, to(&e, home_word + 13u)));
            emit(&e, NOP);
        }
        publish(code_at(offsets[b]), words);
    }

    // The timing code, the standard kernel's with the body replaced by a
    // branch into the first block. Word 13 is where the last block returns.
    emit_t e = {code_at(0), home_word};
    emit(&e, OR(T8, A0, ZERO));        // 0
    emit(&e, ORI(A0, ZERO, 1));        // 1
    emit(&e, ORI(T9, ZERO, 2));        // 2
    const uint32_t outer = e.n;        //
    emit(&e, MFC0(V1, CP0_COUNT));     // 3
    emit(&e, NOP);                     // 4
    emit(&e, ADDIU(A0, A0, -1));       // 5
    emit(&e, BEQ(ZERO, ZERO, to(&e, first_word))); // 6
    emit(&e, NOP);                     // 7
    emit(&e, NOP);                     // 8..12: room, never executed
    emit(&e, NOP);
    emit(&e, NOP);
    emit(&e, NOP);
    emit(&e, NOP);
    emit(&e, MFC0(V0, CP0_COUNT));     // 13
    emit(&e, NOP);
    emit(&e, ADDIU(T9, T9, -1));
    emit(&e, BNE(T9, ZERO, to(&e, outer)));
    emit(&e, OR(A0, T8, ZERO));
    emit(&e, JR(RA));
    emit(&e, SUBU(V0, V0, V1));
    publish(code_at(IC_HOME), e.n - home_word);

    // Instructions executed per pass: every word but the last block's exit.
    return blocks * words - 2u;
}

static uint32_t ic_measure(const uint32_t *offsets, const uint32_t blocks, const uint32_t words, const uint32_t iterations)
{
    ic_region++;
    const uint32_t per_pass = ic_build(offsets, blocks, words);
    const tm_regs_t regs = {0};
    const uint32_t ticks = ((tm_fn_t)(uintptr_t)code_at(IC_HOME))(iterations, &regs);

    // Fastest of three, the way best() does it, but entering at IC_HOME.
    uint32_t fastest = ticks;
    for (uint32_t pass = 1; pass < BENCH_PASSES; pass++)
    {
        const uint32_t t = ((tm_fn_t)(uintptr_t)code_at(IC_HOME))(iterations, &regs);
        if (t < fastest)
        {
            fastest = t;
        }
    }
    return bench_cycles_x100(iterations * per_pass, fastest);
}

extern void cpu_bench_alu(uint32_t iterations, const void *base);

static void measure_icache(void)
{
    ic_region = 0u - 1u; // ic_measure() moves to region 0 first
    static const uint32_t one[] = {0};
    static const uint32_t two8[] = {0, 0x2000};
    static const uint32_t two16[] = {0, 0x4000};
    static const uint32_t three[] = {0, 0x2000, 0x4000};

    tm.ic_cyc[0] = ic_measure(one, 1, IC_BLOCK_WORDS, 512);
    tm.ic_cyc[1] = ic_measure(two8, 2, IC_BLOCK_WORDS, 256);
    tm.ic_cyc[2] = ic_measure(two16, 2, IC_BLOCK_WORDS, 256);
    tm.ic_cyc[3] = ic_measure(three, 3, IC_BLOCK_WORDS, 128);
    wdt_reset();
    tm.ic_cyc[4] = ic_measure(one, 1, 0x8000u / 4u, 8);
    wdt_reset();

    /*
     * cpu.S's addu loop, fetched through KSEG1: every instruction straight
     * from the boot EPROM, no cache. 64 addu and the two loop instructions a
     * pass, counted as 64 like cpu.S's own figures.
     */
    void (*const uncached)(uint32_t, const void *) =
        (void (*)(uint32_t, const void *))(uintptr_t)(((uint32_t)(uintptr_t)&cpu_bench_alu) | 0x20000000u);
    uint32_t fastest = 0xFFFFFFFFu;
    for (uint32_t pass = 0; pass < BENCH_PASSES; pass++)
    {
        const uint32_t start = cp0_count();
        uncached(4, 0);
        const uint32_t ticks = cp0_count() - start;
        if (ticks < fastest)
        {
            fastest = ticks;
        }
    }
    tm.ic_cyc[5] = bench_cycles_x100(4u * BENCH_CPU_OPS_PER_ITER, fastest);
}

// ── step 4: TLB ──────────────────────────────────────────────────────────────

const char *const tm_tlb_names[TM_TLB_COUNT] = {
    "kseg0 16 pages", "mapped 1 page", "mapped 2 pages", "mapped 4 pages", "mapped 8 pages", "mapped 16 pages"};

static void tlb_write(const uint32_t index, const uint32_t hi, const uint32_t lo0, const uint32_t lo1)
{
    asm volatile(".set push\n.set noreorder\n"
                 "mtc0 $0,$5\n"
                 "mtc0 %0,$10\n"
                 "mtc0 %1,$2\n"
                 "mtc0 %2,$3\n"
                 "mtc0 %3,$0\n"
                 "nop\nnop\n"
                 "tlbwi\n"
                 "nop\nnop\n"
                 ".set pop"
                 : : "r"(hi), "r"(lo0), "r"(lo1), "r"(index) : "memory");
}

// Cacheable noncoherent (3), dirty, valid, global.
static inline uint32_t tlb_lo(const uint32_t phys)
{
    return ((phys >> 12) << 6) | (3u << 3) | 0x7u;
}

/*
 * Loads that rotate over N pages, within one line of each and at a different
 * line in each so that all of them stay in the data cache: what is left is the
 * cost of translating. Sixteen pages in eight entries, mapped at TM_TLB_VA;
 * the same physical pages through KSEG0 are the unmapped baseline.
 */
static uint32_t tlb_body(const uint32_t pages)
{
    uint32_t n = 0;
    for (uint32_t k = 0; k < 64u; k++)
    {
        const uint32_t p = k % pages;
        const uint32_t word = (k / pages) % 8u;
        const int32_t off = (int32_t)(p * 4096u + p * CACHE_LINE_SIZE + word * 4u) - 0x8000;
        body[n++] = LW(T0, (uint32_t)off, A2);
    }
    return n;
}

static void measure_tlb(void)
{
    for (uint32_t i = 0; i < TM_TLB_ENTRIES; i++)
    {
        const uint32_t va = TM_TLB_VA + i * 0x2000u;
        const uint32_t pa = TM_TLB_PHYS + i * 0x2000u;
        tlb_write(TM_TLB_FIRST + i, va, tlb_lo(pa), tlb_lo(pa + 0x1000u));
    }

    const tm_regs_t unmapped = {.a2 = sext(KSEG0(TM_TLB_PHYS) + 0x8000u)};
    const tm_regs_t mapped = {.a2 = sext(TM_TLB_VA + 0x8000u)};

    tm.tlb_cyc[0] = measure_body(tlb_body(16), 64, 256, &unmapped);
    for (uint32_t i = 0; i < 5u; i++)
    {
        tm.tlb_cyc[1u + i] = measure_body(tlb_body(1u << i), 64, 256, &mapped);
    }

    // Retire the entries: invalid, and at addresses nothing uses.
    for (uint32_t i = 0; i < TM_TLB_ENTRIES; i++)
    {
        tlb_write(TM_TLB_FIRST + i, 0x7FF00000u + i * 0x2000u, 0, 0);
    }
    asm volatile("mtc0 $0,$10" : : : "memory");
}

// ── step 5: scanout contention ───────────────────────────────────────────────

/*
 * 4 KiB of uncached doubleword reads, timed once inside vblank and once in the
 * middle of the visible frame. If scanout takes memory cycles from the CPU,
 * the second is slower. The front buffer is the bank being displayed.
 */
static inline bool vblank(void)
{
    return (cp0_cause() & 0x400u) != 0u;
}

static void wait_vblank(const bool level)
{
    const uint32_t start = cp0_count();
    while (vblank() != level)
    {
        if (cp0_count() - start > 2000000u) // 40 ms: no video
        {
            return;
        }
    }
}

static uint32_t read_body(void)
{
    uint32_t n = 0;
    for (uint32_t k = 0; k < 64u; k++)
    {
        body[n++] = LD(T0, k * 8u, A2);
    }
    body[n++] = ADDIU(A2, A2, 512);
    return n;
}

#define SCAN_SAMPLES 3u

static uint32_t scan_one(const tm_regs_t *regs, const bool in_vblank)
{
    uint32_t fastest = 0xFFFFFFFFu;
    for (uint32_t s = 0; s < SCAN_SAMPLES; s++)
    {
        wdt_reset();
        if (in_vblank)
        {
            wait_vblank(false);
            wait_vblank(true);
        }
        else
        {
            wait_vblank(true);
            wait_vblank(false);
            const uint32_t start = cp0_count();
            while (cp0_count() - start < 150000u) // 3 ms into the frame
            {
            }
        }
        const uint32_t ticks = run(8, regs);
        if (ticks < fastest)
        {
            fastest = ticks;
        }
    }
    return bench_cycles_x100(8u * 64u, fastest);
}

static void measure_scanout(void)
{
    const uint32_t back = BENCH_PHYS(gBackBuffer);
    const uint32_t front = back == TM_VRAM0_PHYS ? TM_VRAM1_PHYS : TM_VRAM0_PHYS;
    const uint32_t bases[TM_REGION_COUNT] = {TM_SRAM_READ_PHYS, front, back, TM_READ_PHYS};

    build(body, read_body());
    for (uint32_t r = 0; r < TM_REGION_COUNT; r++)
    {
        const tm_regs_t regs = {.a2 = sext(KSEG1(bases[r]))};
        tm.scan_vblank[r] = scan_one(&regs, true);
        tm.scan_active[r] = scan_one(&regs, false);
    }
}

// ── step 6: uncached stores and I/O reads ────────────────────────────────────

const char *const tm_io_names[TM_IO_COUNT] = {
    "player 1", "DIP switches", "IDE status", "sound status", "ROM word", "SRAM word"};

extern uint8_t _heap_vma[];
extern uint8_t _heap_size[];

static bool sram_store_free(void)
{
    const uint32_t heap_end = BENCH_PHYS(_heap_vma) + (uint32_t)(uintptr_t)_heap_size;
    return TM_SRAM_STORE_PHYS >= heap_end && TM_SRAM_STORE_PHYS + 0x400u <= TM_VRAM0_PHYS;
}

static void store_row(uint32_t *out, const uint32_t phys)
{
    const tm_regs_t regs = {.a2 = sext(KSEG1(phys))};

    for (uint32_t w = 0; w < TM_STORE_WIDTHS; w++)
    {
        const uint32_t size = 1u << w;
        for (uint32_t k = 0; k < 64u; k++)
        {
            const uint32_t off = k * size;
            body[k] = w == 0 ? SB(ZERO, off, A2) : w == 1 ? SH(ZERO, off, A2) : w == 2 ? SW(ZERO, off, A2) : SD(ZERO, off, A2);
        }
        out[w] = measure_body(64, 64, 16, &regs);
    }
}

static void measure_bus(void)
{
    if (sram_store_free())
    {
        store_row(tm.store_sram, TM_SRAM_STORE_PHYS);
    }
    else
    {
        for (uint32_t w = 0; w < TM_STORE_WIDTHS; w++)
        {
            tm.store_sram[w] = TM_SKIPPED;
        }
    }
    store_row(tm.store_dram, TM_STORE_PHYS);
    wdt_reset();

    // The widths the ROM itself reads these registers with.
    const uint32_t addresses[TM_IO_COUNT] = {
        (uint32_t)(uintptr_t)&gIO.player1,
        (uint32_t)(uintptr_t)&gIO.dipSwitch,
        (uint32_t)(uintptr_t)&gIDEControl.alternateStatus,
        (uint32_t)(uintptr_t)&gIO.soundControl,
        KSEG1(0x1FC00000u),
        KSEG1(TM_SRAM_READ_PHYS),
    };
    const uint8_t kinds[TM_IO_COUNT] = {1, 0, 2, 1, 2, 2}; // lbu, lhu, lw

    for (uint32_t i = 0; i < TM_IO_COUNT; i++)
    {
        const tm_regs_t regs = {.a2 = sext(addresses[i])};
        const uint32_t load = kinds[i] == 0 ? LBU(T0, 0, A2) : kinds[i] == 1 ? LHU(T0, 0, A2) : LW(T0, 0, A2);
        tm.io_cyc[i] = measure(&load, 1, 64, 16, &regs);
    }
}

// ── step 7: interrupts and syscall ───────────────────────────────────────────

/*
 * The handler goes at the general vector with BEV clear. It records Count and
 * Cause at entry; an interrupt then clears IE and writes Compare (which
 * acknowledges the timer) and returns, a syscall returns past itself, and
 * anything else goes on to the ROM's own vector, which records it and
 * restarts. The record is cached and shared with the wait loop below.
 */
typedef struct
{
    uint32_t entry;   // Count in the handler; nonzero once it has run
    uint32_t last;    // Count the wait loop stored last
    uint32_t cause;
    uint32_t at_entry; // `last` as the handler found it
    uint32_t pad[4];
} tm_irq_t;

static _Alignas(CACHE_LINE_SIZE) volatile tm_irq_t irq_rec;

/*
 * The vector itself is a jump to the handler in DRAM: 128 bytes at 0x180 is
 * not room for the handler with the spacing it needs. Every CP0 write that
 * eret depends on (EPC for a syscall, Status and Compare for an interrupt) is
 * followed by eight instructions before the eret, well past what either CPU
 * needs for the write to reach it: an eret that read the old EPC would
 * return to the syscall and take it again, forever.
 */
#define HAZARD 8u

static void install_handler(void)
{
    const uint32_t rec = (uint32_t)(uintptr_t)&irq_rec;
    const uint32_t handler = KSEG0(TM_HANDLER_PHYS);
    emit_t e = {(uint32_t *)(uintptr_t)handler, 0};

    emit(&e, MFC0(K0, CP0_COUNT));
    emit(&e, LUI(K1, (rec + 0x8000u) >> 16));
    emit(&e, ADDIU(K1, K1, rec & 0xFFFFu));
    emit(&e, SW(K0, 0, K1)); // entry
    emit(&e, LW(K0, 4, K1));
    emit(&e, SW(K0, 12, K1)); // last, as it was at entry
    emit(&e, MFC0(K0, CP0_CAUSE));
    emit(&e, NOP);
    emit(&e, SW(K0, 8, K1));
    emit(&e, ANDI(K0, K0, 0x7C));
    const uint32_t to_irq = e.n;
    emit(&e, BEQ(K0, ZERO, 0)); // patched below
    emit(&e, XORI(K0, K0, 0x20)); // ExcCode 8, syscall
    const uint32_t to_sys = e.n;
    emit(&e, BEQ(K0, ZERO, 0)); // patched below
    emit(&e, NOP);

    // Anything else: on to the ROM's own vector, which records it and restarts.
    emit(&e, LUI(K0, 0xBFC0));
    emit(&e, ORI(K0, K0, 0x0380));
    emit(&e, JR(K0));
    emit(&e, NOP);

    // An interrupt: IE off, Compare written (which acknowledges the timer).
    e.w[to_irq] = BEQ(K0, ZERO, e.n - (to_irq + 1u));
    emit(&e, MFC0(K0, CP0_STATUS));
    emit(&e, NOP);
    emit(&e, ORI(K0, K0, 1));
    emit(&e, XORI(K0, K0, 1));
    emit(&e, MTC0(K0, CP0_STATUS));
    emit(&e, MTC0(K0, CP0_COMPARE));
    for (uint32_t i = 0; i < HAZARD; i++)
    {
        emit(&e, NOP);
    }
    emit(&e, ERET);
    emit(&e, NOP);

    // A syscall: return past it.
    e.w[to_sys] = BEQ(K0, ZERO, e.n - (to_sys + 1u));
    emit(&e, MFC0(K0, CP0_EPC));
    emit(&e, NOP);
    emit(&e, ADDIU(K0, K0, 4));
    emit(&e, MTC0(K0, CP0_EPC));
    for (uint32_t i = 0; i < HAZARD; i++)
    {
        emit(&e, NOP);
    }
    emit(&e, ERET);
    emit(&e, NOP);
    publish(e.w, e.n);

    // The vector: k0 and k1 are the handler's to use.
    emit_t v = {(uint32_t *)(uintptr_t)TM_VECTOR, 0};
    emit(&v, LUI(K0, handler >> 16));
    emit(&v, ORI(K0, K0, handler & 0xFFFFu));
    emit(&v, JR(K0));
    emit(&v, NOP);
    publish(v.w, v.n);

    // Nothing of an earlier fetch near the vector may survive in either cache.
    flush_all();
}

/*
 * Spins storing Count to irq_rec.last until the handler has stored its own,
 * or `a3` ticks pass. The interrupt lands somewhere in the loop, so the
 * handler's Count minus the last one the loop stored before it is the latency
 * plus up to one pass of the loop; the smallest of several samples is the
 * closest to the latency.
 */
static void build_wait(void)
{
    emit_t e = {next_slot(), 0};

    prologue(&e);
    emit(&e, MFC0(T0, CP0_COUNT));
    emit(&e, NOP);
    const uint32_t loop = e.n;
    emit(&e, MFC0(T1, CP0_COUNT));
    emit(&e, NOP);
    emit(&e, SW(T1, 4, A2));
    emit(&e, LW(T2, 0, A2));
    const uint32_t exit_branch = e.n;
    emit(&e, BNE(T2, ZERO, 0)); // patched below
    emit(&e, SUBU(T3, T1, T0));
    emit(&e, SLTU(T3, T3, A3));
    emit(&e, BNE(T3, ZERO, to(&e, loop)));
    emit(&e, NOP);
    const uint32_t out = e.n;
    emit(&e, JR(RA));
    emit(&e, NOP);
    e.w[exit_branch] = BNE(T2, ZERO, out - (exit_branch + 1u));

    publish(e.w, e.n);
}

#define ST_IE 0x00000001u
#define ST_IM2 0x00000400u
#define ST_IM7 0x00008000u
#define ST_BEV 0x00400000u
#define IRQ_SAMPLES 8u

static void irq_record_clear(void)
{
    irq_rec.entry = 0;
    irq_rec.last = 0;
    irq_rec.cause = 0;
    irq_rec.at_entry = 0;
}

static void measure_irq(void)
{
    const uint32_t saved = cp0_status();
    const tm_regs_t regs_cmp = {.a2 = sext((uint32_t)(uintptr_t)&irq_rec), .a3 = 100000u};
    const tm_regs_t regs_vbl = {.a2 = sext((uint32_t)(uintptr_t)&irq_rec), .a3 = 2000000u};

    install_handler();
    build_wait();

    uint32_t lo = 0xFFFFFFFFu, hi = 0;
    for (uint32_t s = 0; s < IRQ_SAMPLES; s++)
    {
        irq_record_clear();
        cp0_status_set((saved & ~(ST_BEV | ST_IE)) | ST_IM7);
        const uint32_t target = cp0_count() + 2000u;
        cp0_compare_set(target);
        cp0_status_set((saved & ~ST_BEV) | ST_IM7 | ST_IE);
        run(0, &regs_cmp);
        cp0_status_set(saved & ~ST_IE);
        cp0_compare_set(0xFFFFFFFFu);
        if (irq_rec.entry != 0u)
        {
            const uint32_t cycles = 2u * (irq_rec.entry - target);
            lo = cycles < lo ? cycles : lo;
            hi = cycles > hi ? cycles : hi;
        }
    }
    tm.irq_cmp_min = hi == 0u ? TM_SKIPPED : lo * 100u;
    tm.irq_cmp_max = hi == 0u ? TM_SKIPPED : hi * 100u;
    wdt_reset();

    lo = 0xFFFFFFFFu;
    hi = 0;
    for (uint32_t s = 0; s < IRQ_SAMPLES; s++)
    {
        wdt_reset();
        wait_vblank(false);
        irq_record_clear();
        cp0_status_set((saved & ~(ST_BEV | ST_IE)) | ST_IM2);
        cp0_status_set((saved & ~ST_BEV) | ST_IM2 | ST_IE);
        run(0, &regs_vbl);
        cp0_status_set(saved & ~ST_IE);
        cp0_compare_set(0xFFFFFFFFu);
        /*
         * The handler's own copy of `last`: the loop may store one more
         * Count after the handler returns, if the interrupt landed between
         * its load of `entry` and its branch on it.
         */
        if (irq_rec.entry != 0u && irq_rec.at_entry != 0u)
        {
            const uint32_t cycles = 2u * (irq_rec.entry - irq_rec.at_entry);
            lo = cycles < lo ? cycles : lo;
            hi = cycles > hi ? cycles : hi;
        }
    }
    tm.irq_vbl_min = hi == 0u ? TM_SKIPPED : lo * 100u;
    tm.irq_vbl_max = hi == 0u ? TM_SKIPPED : hi * 100u;

    // 64 syscalls a pass, each through the handler and back.
    cp0_status_set(saved & ~(ST_BEV | ST_IE));
    const uint32_t sys = SYSCALL;
    const tm_regs_t none = {0};
    tm.syscall_cyc = measure(&sys, 1, 64, 4, &none);
    cp0_status_set(saved);
}

// ── step 8: video timing ─────────────────────────────────────────────────────

/*
 * Vblank is Cause.IP2, high for the blanking interval: the frame is rising
 * edge to rising edge, the vblank rising edge to falling edge.
 */
static void measure_video(void)
{
    uint32_t frame = 0, blank = 0;
    const uint32_t frames = 4;

    for (uint32_t f = 0; f < frames; f++)
    {
        wait_vblank(false);
        wait_vblank(true);
        const uint32_t t0 = cp0_count();
        wait_vblank(false);
        const uint32_t t1 = cp0_count();
        wait_vblank(true);
        const uint32_t t2 = cp0_count();
        frame += t2 - t0;
        blank += t1 - t0;
    }
    // Ticks are 20 ns: us x100 = ticks x 2.
    tm.frame_us = (frame / frames) * 2u;
    tm.vblank_us = (blank / frames) * 2u;
}

// ── step 9: sound board ──────────────────────────────────────────────────────

/*
 * soundControl bit 1 is the DCS's "ready for a byte". sound.c writes a byte
 * by latching it, pulsing soundControl 1 then 3, and waits for ready before
 * the next. This times each byte from the strobe until ready drops and comes
 * back: how long the sound board takes to answer, which on the board is the
 * ADSP-2105's interrupt response and on the core is the DCS core's.
 */
#define DCS_READY 0x2u

static bool dcs_wait_ready(const uint32_t ticks)
{
    const uint32_t start = cp0_count();
    while ((gIO.soundControl & DCS_READY) == 0u)
    {
        if (cp0_count() - start > ticks)
        {
            return false;
        }
    }
    return true;
}

// Ticks from the strobe to ready again, or 0 if ready never dropped.
static uint32_t dcs_send(const uint8_t data, const uint32_t timeout)
{
    gIO.soundData = data;
    gIO.soundControl = 1;
    const uint32_t start = cp0_count();
    bool busy = false;

    while (cp0_count() - start < 56u * BENCH_TICKS_PER_US)
    {
        if ((gIO.soundControl & DCS_READY) == 0u)
        {
            busy = true;
        }
    }
    gIO.soundControl = 3;

    uint32_t kicked = start;
    while (cp0_count() - start < timeout)
    {
        const bool ready = (gIO.soundControl & DCS_READY) != 0u;
        if (!ready)
        {
            busy = true;
        }
        else if (busy)
        {
            return cp0_count() - start;
        }
        if (cp0_count() - kicked > 2500000u)
        {
            wdt_reset();
            kicked = cp0_count();
        }
    }
    return 0;
}

static void measure_dcs(void)
{
    // The volume command sound_init() sends, maximum volume: harmless to repeat.
    static const uint8_t bytes[4] = {0x55, 0xAA, 0x80, 0x7F};
    uint32_t lo = 0xFFFFFFFFu, hi = 0;

    for (uint32_t i = 0; i < 4u; i++)
    {
        if (!dcs_wait_ready(25000000u))
        {
            break;
        }
        udelay(8);
        const uint32_t ticks = dcs_send(bytes[i], 250000u);
        if (ticks != 0u)
        {
            lo = ticks < lo ? ticks : lo;
            hi = ticks > hi ? ticks : hi;
        }
        udelay(8);
    }
    tm.dcs_ack_min = hi == 0u ? TM_SKIPPED : lo * 2u;
    tm.dcs_ack_max = hi == 0u ? TM_SKIPPED : hi * 2u;
    wdt_reset();

    /*
     * Boot: sound_init()'s reset, then its 0x00 boot byte, timed to the DCS
     * taking it. Volume is set again afterwards, as sound_init() does.
     */
    gIO.soundReset = 0;
    udelay(400);
    gIO.soundReset = 1;
    for (uint32_t i = 0; i < 8u; i++)
    {
        udelay(10000);
        wdt_reset();
    }
    gIO.soundData = 1;
    const uint32_t boot = dcs_send(0x00, 50000000u); // 1 s
    tm.dcs_boot = boot == 0u ? TM_SKIPPED : boot / 500u; // ms x100: ticks / 50000 x 100
    for (uint32_t i = 0; i < 10u; i++)
    {
        udelay(10000);
        wdt_reset();
    }
    sound_set_volume(0x80);
}

// ── step 10: spread ──────────────────────────────────────────────────────────

const char *const tm_spread_names[TM_SPREAD_COUNT] = {
    "addu chain", "D$ lw chain", "unc SRAM ld", "unc VRAM ld", "unc DRAM ld"};

#define SPREAD_RUNS 64u

static void spread_one(tm_spread_t *out, const uint32_t iterations, const uint32_t ops, const tm_regs_t *regs)
{
    uint32_t v[SPREAD_RUNS];

    for (uint32_t r = 0; r < SPREAD_RUNS; r++)
    {
        const uint32_t ticks = run(iterations, regs);
        uint32_t i = r;
        while (i > 0u && v[i - 1u] > ticks) // insertion sort as they come
        {
            v[i] = v[i - 1u];
            i--;
        }
        v[i] = ticks;
    }

    const uint32_t lo = v[0], hi = v[SPREAD_RUNS - 1u];
    for (uint32_t b = 0; b < TM_SPREAD_BUCKETS; b++)
    {
        out->hist[b] = 0;
    }
    for (uint32_t r = 0; r < SPREAD_RUNS; r++)
    {
        const uint32_t b = (uint32_t)(((uint64_t)(v[r] - lo) * TM_SPREAD_BUCKETS) / (uint64_t)(hi - lo + 1u));
        out->hist[b]++;
    }
    const uint32_t total = iterations * ops;
    out->min = bench_cycles_x100(total, lo);
    out->med = bench_cycles_x100(total, v[SPREAD_RUNS / 2u]);
    out->max = bench_cycles_x100(total, hi);
}

static void measure_spread(void)
{
    uint32_t n;

    n = 0;
    for (uint32_t k = 0; k < 64u; k++)
    {
        body[n++] = ADDU(T0, T0, T1);
    }
    build(body, n);
    const tm_regs_t alu = {.t = {0x12345678u, 0xABCDu, 0, 0}};
    spread_one(&tm.spread[0], 64, 64, &alu);

    n = 0;
    for (uint32_t k = 0; k < 64u; k++)
    {
        body[n++] = LW(A2, 0, A2);
    }
    build(body, n);
    const tm_regs_t chain = {.a2 = cell()};
    spread_one(&tm.spread[1], 64, 64, &chain);
    wdt_reset();

    const uint32_t back = BENCH_PHYS(gBackBuffer);
    const uint32_t front = back == TM_VRAM0_PHYS ? TM_VRAM1_PHYS : TM_VRAM0_PHYS;
    const uint32_t bases[3] = {TM_SRAM_READ_PHYS, front, TM_READ_PHYS};

    build(body, read_body());
    for (uint32_t i = 0; i < 3u; i++)
    {
        const tm_regs_t regs = {.a2 = sext(KSEG1(bases[i]))};
        spread_one(&tm.spread[2u + i], 8, 64, &regs);
        wdt_reset();
    }
    tm.spread_done = true;
}

// ── steps ────────────────────────────────────────────────────────────────────

static const char *const step_names[TM_STEPS] = {
    "integer timing", "FPU timing", "D-cache", "I-cache", "TLB", "scanout",
    "bus and I/O", "interrupts", "video timing", "sound board", "spread"};

void timing_reset(void)
{
    uint8_t *p = (uint8_t *)&tm;
    for (uint32_t i = 0; i < sizeof(tm); i++)
    {
        p[i] = 0;
    }
}

const char *timing_step_name(const uint8_t index)
{
    return index < TM_STEPS ? step_names[index] : "";
}

void timing_run_step(const uint8_t index)
{
    switch (index)
    {
    case 0:
        measure_int();
        break;
    case 1:
        measure_fpu();
        break;
    case 2:
        measure_dcache();
        break;
    case 3:
        measure_icache();
        break;
    case 4:
        measure_tlb();
        break;
    case 5:
        measure_scanout();
        break;
    case 6:
        measure_bus();
        break;
    case 7:
        measure_irq();
        break;
    case 8:
        measure_video();
        break;
    case 9:
        measure_dcs();
        break;
    case 10:
        measure_spread();
        break;
    default:
        break;
    }
}
