/*
 * start.S — entry from the KI test ROM, self-relocation into KSEG0, the return
 * to the ROM, and exception dispatch.
 *
 * The ROM calls the first word of the image where it sits in the boot ROM.
 * A suite that rewrites all 48 TLB entries and sweeps the caches wants to run
 * from unmapped RAM it owns, so whatever address we are entered at, we copy
 * ourselves to a fixed KSEG0 address in DRAM and jump there before running any
 * test. Copying on every entry also gives every run a fresh .data.
 *
 * The ROM is o64 and the suite n32. The call carries no arguments and returns
 * the rc in $v0 (ki_cputest.h has the rest), and every register either ABI
 * expects preserved is saved here and restored by cputest_return, along with
 * the CP0 and FPU state the tests change and the ROM relies on.
 */

#include "iris.h"
#include "excoff.h"

    .set noreorder
    .set noat

/*
 * Temporaries are written as numeric registers ($8-$14) rather than $t0-$t6:
 * the o32 and n32/n64 ABIs disagree about which hardware register each $tN
 * name means, and under n32 the names $t4-$t7 do not exist at all. Numeric
 * names assemble identically under every ABI, which is what a bootstrap file
 * that predates any ABI's runtime should be doing anyway.
 */

/* rom_ctx layout */
#define CTX_RA        0
#define CTX_SP        8
#define CTX_GP       16
#define CTX_FP       24
#define CTX_S0       32     /* s0-s7 at 32..88 */
#define CTX_STATUS   96
#define CTX_CONFIG  100
#define CTX_FCSR    104
#define CTX_COUNT   108
#define CTX_COMPARE 112
#define CTX_WIRED   116
#define CTX_ENTRYHI 120

/* Primary caches, both 16 KB on the KI's R4600. An index operation on a
 * KSEG0 address picks the way with bit 13, so 16 KB of addresses covers both
 * ways; the 16-byte step suits a 16- or a 32-byte line. */
#define CACHE_BYTES 0x4000

    .section .init, "ax"
    .globl _start
    .ent _start
_start:
    move    $25, $ra                /* the ROM's return; bal clobbers $ra  */

    /* Where are we actually executing? bal gives a PC-relative anchor. */
    bal     1f
    nop
1:  move    $8, $ra                 /* runtime address of label 1          */
    lui     $9, %hi(1b)
    addiu   $9, $9, %lo(1b)        /* link-time address of label 1        */
    subu    $10, $8, $9            /* load bias (0 if already in place)   */

    beqz    $10, relocated
    nop

    /* Copy [_ftext, _edata) from (link + bias) down to link. .bss is zeroed
     * below, and the boot ROM holds nothing past _edata to copy. */
    lui     $11, %hi(_ftext)
    addiu   $11, $11, %lo(_ftext)    /* dst     */
    lui     $12, %hi(_edata)
    addiu   $12, $12, %lo(_edata)    /* dst end */
    addu    $13, $11, $10            /* src     */
1:  lw      $14, 0($13)
    sw      $14, 0($11)
    addiu   $13, $13, 4
    addiu   $11, $11, 4
    bne     $11, $12, 1b
    nop

    /* The copy landed in the D-cache; instruction fetch must see it. Write
     * back D and invalidate I across the range. Step 16 = the R4400 line size,
     * which is also correct (merely redundant) for 32-byte lines. */
    lui     $11, %hi(_ftext)
    addiu   $11, $11, %lo(_ftext)
1:  cache   0x15, 0($11)             /* Hit_Writeback_Inv_D  = 1 | (5<<2)   */
    cache   0x10, 0($11)             /* Hit_Invalidate_I     = 0 | (4<<2)   */
    addiu   $11, $11, 16
    sltu    $13, $11, $12
    bnez    $13, 1b
    nop

    lui     $11, %hi(relocated)
    addiu   $11, $11, %lo(relocated)
    jr      $11
    nop

relocated:
    /* Save the ROM's context. rom_ctx is in .data, so zeroing .bss below
     * cannot wipe it. */
    lui     $8, %hi(rom_ctx)
    addiu   $8, $8, %lo(rom_ctx)
    sd      $25, CTX_RA($8)
    sd      $sp, CTX_SP($8)
    sd      $gp, CTX_GP($8)
    sd      $30, CTX_FP($8)
    sd      $16, CTX_S0+0($8)
    sd      $17, CTX_S0+8($8)
    sd      $18, CTX_S0+16($8)
    sd      $19, CTX_S0+24($8)
    sd      $20, CTX_S0+32($8)
    sd      $21, CTX_S0+40($8)
    sd      $22, CTX_S0+48($8)
    sd      $23, CTX_S0+56($8)
    mfc0    $9, $12                  /* Status   */
    sw      $9, CTX_STATUS($8)
    mfc0    $9, $16                  /* Config   */
    sw      $9, CTX_CONFIG($8)
    mfc0    $9, $9                   /* Count    */
    sw      $9, CTX_COUNT($8)
    mfc0    $9, $11                  /* Compare  */
    sw      $9, CTX_COMPARE($8)
    mfc0    $9, $6                   /* Wired    */
    sw      $9, CTX_WIRED($8)
    mfc0    $9, $10                  /* EntryHi  */
    sw      $9, CTX_ENTRYHI($8)
    /* The ROM runs with CU1 set; the tests change FCSR. */
    cfc1    $9, $31
    sw      $9, CTX_FCSR($8)

    /* The ROM's .data, .bss and stack live in cached SRAM. Push every dirty
     * line out now: the cache tests drop lines without writing them back. */
    lui     $11, 0x8000
    li      $12, CACHE_BYTES
    addu    $12, $11, $12
1:  cache   0x01, 0($11)             /* Index_Writeback_Inv_D = 1 | (0<<2)  */
    addiu   $11, $11, 16
    bne     $11, $12, 1b
    nop

    /* Known CP0 state: CU1+CU0 on, BEV=0 (RAM vectors), KX/SX/UX on for
     * 64-bit addressing, FR=1 (the FR=0 aliasing tests flip it back
     * deliberately), IE off, EXL off. Count keeps running: the ROM keeps time
     * with it, and cputest_return puts back the value it had here. */
    li      $8, (ST_CU1 | ST_CU0 | ST_KX | ST_SX | ST_UX | ST_FR)
    mtc0    $8, $12                 /* Status */
    nop
    nop
    mtc0    $zero, $13               /* Cause: clear pending software ints */
    nop
    mtc0    $zero, $18               /* WatchLo — no stray watchpoints     */
    mtc0    $zero, $19               /* WatchHi                            */
    li      $8, -1
    mtc0    $8, $11                 /* Compare far away                   */
    nop

    /* Stack, with the 32 bytes of argument slop the o32 ABI expects. */
    lui     $sp, %hi(_stack_top)
    addiu   $sp, $sp, %lo(_stack_top)
    addiu   $sp, $sp, -32
    move    $fp, $sp
    lui     $gp, %hi(_gp)
    addiu   $gp, $gp, %lo(_gp)

    /* Zero .bss. */
    lui     $8, %hi(_fbss)
    addiu   $8, $8, %lo(_fbss)
    lui     $9, %hi(_end)
    addiu   $9, $9, %lo(_end)
1:  beq     $8, $9, 2f
    nop
    sw      $zero, 0($8)
    b       1b
    addiu   $8, $8, 4
2:
    jal     suite_main
    nop
    jal     cputest_exit             /* records the rc, never returns      */
    move    $4, $2
1:  b       1b
    nop
    .end _start

/*
 * cputest_return(rc) — put the ROM's world back and return to it with rc.
 *
 * Reached from cputest_exit, so from any depth. After the ROM's registers are
 * reloaded nothing here touches memory, and the tail that empties the caches
 * runs uncached, so that it is not fetching from the I-cache it invalidates.
 */
    .text
    .globl cputest_return
    .ent cputest_return
cputest_return:
    move    $2, $4                   /* rc */

    /* Kernel mode, exceptions to the boot ROM again, interrupts off. */
    li      $8, (ST_CU1 | ST_CU0 | ST_BEV)
    mtc0    $8, $12
    nop
    nop

    /* Leave the TLB as the ROM's reset code does: every entry invalid, each
     * at its own VPN2 so that no two can ever match together. */
    mtc0    $zero, $6                /* Wired    */
    mtc0    $zero, $5                /* PageMask */
    mtc0    $zero, $2                /* EntryLo0 */
    mtc0    $zero, $3                /* EntryLo1 */
    li      $9, TLB_ENTRIES - 1
    li      $10, 0xff
1:  mtc0    $10, $10                 /* EntryHi  */
    mtc0    $9, $0                   /* Index    */
    nop
    tlbwi
    nop
    addiu   $9, $9, -1
    bgez    $9, 1b
    addiu   $10, $10, 0x2000

    lui     $8, %hi(rom_ctx)
    addiu   $8, $8, %lo(rom_ctx)
    lw      $9, CTX_WIRED($8)
    mtc0    $9, $6
    lw      $9, CTX_ENTRYHI($8)
    mtc0    $9, $10
    lw      $9, CTX_CONFIG($8)
    mtc0    $9, $16
    lw      $9, CTX_COMPARE($8)
    mtc0    $9, $11
    mtc0    $zero, $13               /* Cause: no software interrupts      */
    lw      $9, CTX_FCSR($8)
    ctc1    $9, $31
    lw      $14, CTX_STATUS($8)      /* restored last, below               */
    lw      $13, CTX_COUNT($8)

    ld      $ra, CTX_RA($8)
    ld      $sp, CTX_SP($8)
    ld      $gp, CTX_GP($8)
    ld      $30, CTX_FP($8)
    ld      $16, CTX_S0+0($8)
    ld      $17, CTX_S0+8($8)
    ld      $18, CTX_S0+16($8)
    ld      $19, CTX_S0+24($8)
    ld      $20, CTX_S0+32($8)
    ld      $21, CTX_S0+40($8)
    ld      $22, CTX_S0+48($8)
    ld      $23, CTX_S0+56($8)

    /* Continue through KSEG1. */
    lui     $8, %hi(2f)
    addiu   $8, $8, %lo(2f)
    lui     $9, 0x2000
    or      $8, $8, $9
    jr      $8
    nop
2:
    /* Empty both caches without writing anything back. The ROM's lines were
     * written back on entry and the ROM has not run since, while what the
     * tests left may carry any tag at all: written back, it could land
     * anywhere. Everything the ROM reads of the run is uncached. */
    mtc0    $zero, $28               /* TagLo */
    mtc0    $zero, $29               /* TagHi */
    nop
    lui     $11, 0x8000
    li      $12, CACHE_BYTES
    addu    $12, $11, $12
1:  cache   0x09, 0($11)             /* Index_Store_Tag_D */
    cache   0x08, 0($11)             /* Index_Store_Tag_I */
    addiu   $11, $11, 16
    bne     $11, $12, 1b
    nop

    mtc0    $13, $9                  /* Count, as the ROM left it          */
    mtc0    $14, $12                 /* Status                             */
    nop
    nop
    jr      $ra
    nop
    .end cputest_return

    .data
    .align 3
rom_ctx:
    .space 128

/* ──────────────────────────────────────────────────────────────────────────
 * Vector trampolines, copied to 0x80000000 / 0x80000080 / 0x80000180 by
 * exc_install(). A vector slot is only 128 bytes, so each trampoline just
 * tags itself and jumps to the real handler in the suite image.
 * ────────────────────────────────────────────────────────────────────────── */
    .text

#define VEC_TRAMPOLINE(name, vecid)           \
    .globl name;                              \
    .globl name##_end;                        \
name:                                         \
    li      $k1, vecid;                       \
    lui     $k0, %hi(exc_dispatch);           \
    addiu   $k0, $k0, %lo(exc_dispatch);      \
    jr      $k0;                              \
    nop;                                      \
name##_end:

VEC_TRAMPOLINE(tramp_tlb,     VECID_TLB)
VEC_TRAMPOLINE(tramp_xtlb,    VECID_XTLB)
VEC_TRAMPOLINE(tramp_general, VECID_GENERAL)

/*
 * exc_dispatch — the real handler.
 *
 * $k1 = vector id (from the trampoline), $k0 = scratch. No other register may
 * be disturbed: several tests assert that the destination register of the
 * faulting instruction was NOT written, and all of them check GPRs across the
 * exception. $at/$v0/$v1 are saved to exc_save and restored before ERET.
 */
    .globl exc_dispatch
    .ent exc_dispatch
exc_dispatch:
    lui     $k0, %hi(exc_save)
    addiu   $k0, $k0, %lo(exc_save)
    sd      $at, 0($k0)
    sd      $v0, 8($k0)
    sd      $v1, 16($k0)

    lui     $at, %hi(exc)
    addiu   $at, $at, %lo(exc)

    lw      $v0, EXC_O_COUNT($at)
    addiu   $v0, $v0, 1
    sw      $v0, EXC_O_COUNT($at)
    sw      $k1, EXC_O_VECTOR($at)

    mfc0    $v0, $12                 /* Status   */
    sw      $v0, EXC_O_STATUS($at)
    mfc0    $v1, $13                 /* Cause    */
    sw      $v1, EXC_O_CAUSE($at)
    dmfc0   $v1, $14                 /* EPC      */
    sd      $v1, EXC_O_EPC($at)
    dmfc0   $v1, $8                  /* BadVAddr */
    sd      $v1, EXC_O_BADVADDR($at)
    dmfc0   $v1, $30                 /* ErrorEPC */
    sd      $v1, EXC_O_ERROREPC($at)
    dmfc0   $v1, $10                 /* EntryHi  */
    sd      $v1, EXC_O_ENTRYHI($at)
    dmfc0   $v1, $4                  /* Context  */
    sd      $v1, EXC_O_CONTEXT($at)
    dmfc0   $v1, $20                 /* XContext */
    sd      $v1, EXC_O_XCONTEXT($at)

    /*
     * FP state, but ONLY if CU1 is enabled. The coprocessor-unusable tests
     * deliberately run with CU1 clear — a cfc1 here would fault inside the
     * handler and never return. ($v0 still holds Status.)
     */
    li      $k1, ST_CU1
    and     $k1, $v0, $k1
    beqz    $k1, no_fpu
    nop
    cfc1    $v1, $31
    sw      $v1, EXC_O_FCSR($at)
    /* Clear FP cause bits, else a trapped FP exception re-fires forever. */
    li      $v1, ~0x0003F000
    cfc1    $v0, $31
    and     $v0, $v0, $v1
    ctc1    $v0, $31
no_fpu:

    /* A test-supplied handler takes over from here if installed; it owns the
     * ERET and the register restore. */
    lui     $k0, %hi(exc_user_handler)
    addiu   $k0, $k0, %lo(exc_user_handler)
    lw      $k0, 0($k0)
    beqz    $k0, 1f
    nop
    jr      $k0
    nop
1:

    /* Resume policy: SKIP steps over the faulting instruction (EPC += 8 when
     * the fault was in a branch delay slot, since EPC points at the branch);
     * RETRY re-executes it. */
    lui     $k0, %hi(exc_resume_mode)
    addiu   $k0, $k0, %lo(exc_resume_mode)
    lw      $k0, 0($k0)
    bnez    $k0, 2f
    nop
    dmfc0   $v0, $14
    lw      $v1, EXC_O_CAUSE($at)
    srl     $v1, $v1, 31             /* Cause.BD */
    beqz    $v1, 1f
    nop
    daddiu  $v0, $v0, 8
    b       3f
    nop
1:  daddiu  $v0, $v0, 4
3:  dmtc0   $v0, $14
2:

    lui     $k0, %hi(exc_save)
    addiu   $k0, $k0, %lo(exc_save)
    ld      $at, 0($k0)
    ld      $v0, 8($k0)
    ld      $v1, 16($k0)
    ssnop
    ssnop
    ssnop
    ssnop
    eret
    nop
    .end exc_dispatch

/*
 * exl_resume_handler — a handler for tests that deliberately break the normal
 * resume path.
 *
 * Taking an exception while Status.EXL is already set does NOT update EPC (the
 * first exception's EPC has to survive). The default handler resumes via EPC,
 * so with EXL pre-set it would ERET to whatever EPC happened to hold — looping
 * forever. A test that wants to observe the EPC-preservation rule installs
 * this instead, and says where to continue by setting exl_resume_pc.
 *
 * Entered from exc_dispatch with $k0/$k1 free and $at/$v0/$v1 already saved.
 */
    .globl exl_resume_handler
    .ent exl_resume_handler
exl_resume_handler:
    lui     $k0, %hi(exl_resume_pc)
    addiu   $k0, $k0, %lo(exl_resume_pc)
    ld      $k0, 0($k0)
    dmtc0   $k0, $14                 /* EPC = the caller's resume point */
    nop
    nop
    lui     $k0, %hi(exc_save)
    addiu   $k0, $k0, %lo(exc_save)
    ld      $at, 0($k0)
    ld      $v0, 8($k0)
    ld      $v1, 16($k0)
    ssnop
    ssnop
    ssnop
    ssnop
    eret                             /* also clears EXL */
    nop
    .end exl_resume_handler

    .section .bss
    .align 4
    .globl exc_save
exc_save:
    .space 32
    .globl exl_resume_pc
exl_resume_pc:
    .space 8
