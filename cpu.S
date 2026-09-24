# SPDX-FileCopyrightText: © 2023 Leandre Gohy <leandre.gohy@hexeo.be>
# SPDX-License-Identifier: AGPL-3.0-only

#include "regdef.h"

# Instruction throughput kernels for the CPU section of the memory speed test
# ROM (bench.c drives them, view_memtest.c displays the results).
#
# These are assembly rather than C with volatile because the figure being
# reported is "how many cycles does this instruction take", and that only means
# something if the instruction stream is exactly what it claims to be. At -Os,
# GCC is free to reassociate, hoist, unroll differently or pick a different
# instruction entirely, and there is no portable way to pin that down from C.
#
# EVERY instruction inside a .rept below must be a native MIPS instruction and
# not a GAS macro. This is not a style preference. `divu t0,t1` looks like the
# DIVU instruction and is not: the two-operand form of div/divu is the macro,
# and it expands to a zero-divisor test, the real divide, a break, and an mflo.
# The raw instruction is the three-operand form with $zero as the destination,
# `divu zero,t0,t1`, which is what GAS emits inside its own expansion. Getting
# this wrong measures four instructions while claiming to measure one, and .set
# nomacro only warns about it. Check the disassembly in output*.txt after
# touching these loops.
#
# Loop shape, 64 operations plus two instructions of control per iteration:
#
#     addiu a0,a0,-1        pre-decrement, so the count works out exactly
#   1:
#     .rept 64
#     <operation>
#     .endr
#     bnez a0,1b
#     addiu a0,a0,-1        branch delay slot
#
# The delay slot holds the loop counter rather than a 64th operation. It could
# hold an operation and save an instruction, but then a macro that sneaked into
# the .rept would expand into the delay slot, and a branch in a branch delay
# slot is undefined -- which is exactly how the divu bug above turned a wrong
# number into a crash. Keeping the slot boring means a future mistake of that
# kind is merely wrong, not fatal.
#
# a0 must be at least 1. The two control instructions are inside the timed
# region and are not subtracted, so every figure carries about 2 cycles of
# overhead per 64 operations -- roughly 3% on a single-cycle instruction, and
# proportionally less on the slow ones.
#
# a1 is a base address, for the kernels that take one: cpu_bench_load() and
# cpu_bench_load_indep() read 256 bytes from it, and mem_bench_chase() treats it
# as the head of a linked chain. The rest ignore it, so that bench.c can call
# all of them through one function pointer type.
#
# All of t0-t3 ($8-$11) are scratch under o64: the first four arguments arrive
# in a0-a3 and these kernels take two, so nothing live is being trampled. No
# callee-saved register is touched, so none of these need a prologue.

.text
.set noreorder
.set nomacro
.set noat

# Integer ALU. A dependent chain of addu, which on a fully forwarded scalar
# pipeline still issues one per cycle -- this is the baseline the other kernels
# are read against, and it doubles as a check that the harness itself is sane.
.balign 4
.globl cpu_bench_alu
cpu_bench_alu:
    lui t0,0x1234
    ori t0,t0,0x5678
    ori t1,zero,0xabcd
    addiu a0,a0,-1
1:
    .rept 64
    addu t0,t0,t1
    .endr
    bnez a0,1b
    addiu a0,a0,-1

    jr ra
    nop

# Load from the primary data cache. The 64 loads walk 256 bytes, eight 32-byte
# lines, and the same 256 bytes are re-read every iteration, so everything after
# the first pass hits. Consecutive loads all target t0, which is a
# write-after-write on an in-order pipeline and therefore does not stall: what
# this measures is the issue rate of a cache hit, not load-use latency.
.balign 4
.globl cpu_bench_load
cpu_bench_load:
    addiu a0,a0,-1
1:
    .set lwoff,0
    .rept 64
    lw t0,lwoff(a1)
    .set lwoff,lwoff+4
    .endr
    bnez a0,1b
    addiu a0,a0,-1

    jr ra
    nop

# Load from the primary data cache, into EIGHT destinations in rotation instead
# of one. Same 64 loads over the same 256 bytes as cpu_bench_load above, and the
# results are just as dead; the only difference is that no load overwrites the
# register the one before it wrote.
#
# This pair exists to settle a number. cpu_bench_load reports about 2.0 cycles
# per load on the core while cpu_bench_alu reports 1.0, and a scalar R4600
# pipelines the data cache well enough to retire one hitting load per cycle. So
# either the write-after-write on t0 is being treated as a real dependency, in
# which case THIS kernel comes out near 1.0, or the core pays the second cycle
# on every load whatever its destination, in which case this one reports 2.0 as
# well and the cost is in the load path itself.
#
# t0-t7 are $8-$15. Under o64 the first EIGHT arguments arrive in $4-$11, so
# t0-t3 are also a4-a7 -- but this kernel is called with two arguments, so all
# of $8-$15 are dead on entry and caller-saved on exit either way.
.balign 4
.globl cpu_bench_load_indep
cpu_bench_load_indep:
    addiu a0,a0,-1
1:
    .set lwoff,0
    .rept 8
    lw t0,lwoff(a1)
    .set lwoff,lwoff+4
    lw t1,lwoff(a1)
    .set lwoff,lwoff+4
    lw t2,lwoff(a1)
    .set lwoff,lwoff+4
    lw t3,lwoff(a1)
    .set lwoff,lwoff+4
    lw t4,lwoff(a1)
    .set lwoff,lwoff+4
    lw t5,lwoff(a1)
    .set lwoff,lwoff+4
    lw t6,lwoff(a1)
    .set lwoff,lwoff+4
    lw t7,lwoff(a1)
    .set lwoff,lwoff+4
    .endr
    bnez a0,1b
    addiu a0,a0,-1

    jr ra
    nop

# Dependent load chain -- the pointer chase, and the one kernel here that is not
# only a CPU test. Each node holds the address of the next in its first word, so
# the address of load n+1 IS the result of load n. Nothing in the machine can
# overlap two of them: there is no address for the data cache's next-line
# read-ahead to predict, and the R4600's primary cache blocks, so exactly one
# miss is ever outstanding. What comes back is the round trip of a single miss.
#
# bench.c aims it at two different chains. Over a chain in DRAM that is several
# times the size of the data cache, every step misses and this reports SDRAM
# miss latency -- where bench_read() reports how many misses per second the
# machine can keep in flight, which is a different question and a much kinder
# one. Over a 256 byte chain that stays in the cache, every step hits and this
# reports load-use latency, the third of the three load variants above.
#
# bench.c builds the chain AND proves it is a single cycle of the expected
# length before calling this. It has to: nothing below bounds where a pointer
# leads, so one word DRAM did not return correctly walks to an arbitrary
# address, and the first unaligned or unmapped one takes an exception -- which
# on this ROM is a silent restart, not a message.
#
# lw sign-extends its result, and init_cpu leaves Status.KX/SX/UX clear, so the
# 32-bit kseg0 address stored in the node is used as a 32-bit address. Nothing
# here needs ld, and the chain stays half the size for it.
.balign 4
.globl mem_bench_chase
mem_bench_chase:
    addiu a0,a0,-1
1:
    .rept 64
    lw a1,0(a1)
    .endr
    bnez a0,1b
    addiu a0,a0,-1

    jr ra
    nop

# Integer multiply. The two-operand form of multu IS the native instruction --
# MULTU has no destination register, so there is no three-operand macro to fall
# into, and the disassembly confirms it assembles one for one. HI/LO are
# overwritten each time and never read back, which keeps the MFHI/MFLO hazard
# out of the measurement: what is timed is how fast the multiplier accepts work.
.balign 4
.globl cpu_bench_mul
cpu_bench_mul:
    lui t0,0x1234
    ori t0,t0,0x5678
    ori t1,zero,0xabcd
    addiu a0,a0,-1
1:
    .rept 64
    multu t0,t1
    .endr
    bnez a0,1b
    addiu a0,a0,-1

    jr ra
    nop

# Integer divide. Note the three-operand form with $zero as the destination:
# that is the bare DIVU instruction, with no zero-divisor check and no mflo. See
# the warning at the top of this file -- `divu t0,t1` would be the macro.
.balign 4
.globl cpu_bench_div
cpu_bench_div:
    lui t0,0x1234
    ori t0,t0,0x5678
    ori t1,zero,0x0abc
    addiu a0,a0,-1
1:
    .rept 64
    divu zero,t0,t1
    .endr
    bnez a0,1b
    addiu a0,a0,-1

    jr ra
    nop

# Single precision floating point add. start.S enables CP1 (Status.CU1) with
# FR=1 and sets FS in the FP control register, so denormals flush to zero and
# every FP exception is disabled: this cannot trap whatever lands in the
# registers. It starts from 3.14159 and adds 1.0, so the running total stays
# finite and normal for far longer than any run of this kernel.
#
# The chain is dependent, so this reports add.s latency rather than its issue
# rate. The mtc1 pair needs one instruction of separation before the value is
# readable by an FP operation, which the second mtc1 and the addiu provide.
.balign 4
.globl cpu_bench_fpu
cpu_bench_fpu:
    lui t0,0x4049
    ori t0,t0,0x0fdb            # 3.14159 as IEEE-754 single
    lui t1,0x3f80               # 1.0
    mtc1 t0,$f0
    mtc1 t1,$f2
    addiu a0,a0,-1
1:
    .rept 64
    add.s $f0,$f0,$f2
    .endr
    bnez a0,1b
    addiu a0,a0,-1

    jr ra
    nop
