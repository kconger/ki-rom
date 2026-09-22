# SPDX-FileCopyrightText: © 2023 Leandre Gohy <leandre.gohy@hexeo.be>
# SPDX-License-Identifier: AGPL-3.0-only

#include "regdef.h"

.text
.set noreorder
.set nomacro
.set noat

# Reset/Soft Reset/NMI exception vector (0xbfc00000)
.org 0x0
.globl _reset_exception_handler
_reset_exception_handler:
#if defined(MEMTEST)
    # A reset is not an exception, and the watchdog arrives through this vector.
    # It has to be told apart from a fault, or every watchdog reset comes back
    # carrying whatever Cause and EPC happened to be left in CP0.
    j _reset_handler
#else
    j _exception_handler
#endif
    nop

# TLB Refill exception vector (0xbfc00200)
.org 0x200
_tlb_refill_exception_handler:
    j _exception_handler
    nop

# XTLB Refill exception vector (0xbfc00280)
.org 0x280
_xtlb_refill_exception_handler:
    j _exception_handler
    nop

# Cache Error exception vector (0xbfc00300)
.org 0x300
_cache_error_exception_handler:
    j _exception_handler
    nop

# General/Others/Common exception vector (0xbfc00380)
.org 0x380
_other_exception_handler:
    j _exception_handler
    nop

#if defined(MEMTEST)
_reset_handler:
    .set macro
    # Clear any stale record, so that a restart arriving through the reset
    # vector reports nothing at all. An empty report is itself the answer: it
    # means the machine was reset (watchdog, NMI, power) rather than faulted.
    lui k1,0xA002
    ori k1,k1,0xF000
    sw zero,0(k1)
    j _restart
    nop
#endif

_exception_handler:
    .set macro
#if defined(MEMTEST)
    # Record what went wrong before anything is reinitialised. The four real
    # exception vectors land here and restart the ROM, so without this a fault
    # inside one of the test kernels is indistinguishable from a watchdog reset:
    # the screen simply starts over. k0/k1 are reserved for this kind of use.
    #
    # The record goes to UNCACHED SRAM at 0x2F000, which sits above everything
    # the test writes (the SRAM window ends at 0x24000) and below VRAM bank 0.
    # Uncached matters: init_data_cache below invalidates the data cache without
    # writing it back, so a cached store here would be thrown away before
    # view_memtest could read it.
    lui k1,0xA002
    ori k1,k1,0xF000
    mfc0 k0,CP0_CAUSE
    sw k0,4(k1)
    mfc0 k0,CP0_EPC
    sw k0,8(k1)
    mfc0 k0,CP0_STATUS
    sw k0,12(k1)
    mfc0 k0,CP0_BAD_V_ADDR
    sw k0,16(k1)
    # Magic written last, so a record that was only half stored is never
    # mistaken for a real one.
    lui k0,0xDEAD
    ori k0,k0,0xBEEF
    sw k0,0(k1)

_restart:
#endif
    li v0,0x0
    mtc0 v0,CP0_COUNT
#if defined(MEMTEST)
    # ROM=memtest links neither view_main nor the patches, and boots straight
    # into the memory and IDE speed test instead of into the game.
    la a0,view_memtest
#else
    la a0,view_main
#endif
    j _reset
    nop

.globl _reset
_reset:
    .set macro

    # Set up the global and stack pointers before anything below calls into
    # compiled C. video_init and video_clear_vrams are C functions; they only
    # happen to compile to leaves that store nothing, so on entry $sp is still
    # whatever the reset left in the register.
    la gp,_gp # Init Global pointer
    la sp,_stack_vma # Init Stack pointer

    or k0,a0,zero

    jal init_cpu
    nop
    jal video_init
    nop
    jal init_data_cache
    nop
    jal init_instr_cache
    nop
    jal jump_cached_memory
    nop
    jal video_clear_vrams
    nop
    jal fill_data_cache
    nop
    j _main
    nop

init_data_cache:
    # Index 512 blocks of data to primary cache
    # 32 bytes per tag
    # from 0x80000000 to 0x80001FE0
    # from 0x80002000 to 0x80003FE0
    lui v0,0x8000
    li v1,0xff
    mtc0 zero,CP0_TAG_LO

    INIT_DATA_CACHE_LOOP:
    cache 0x9,0x0(v0)      # cache op = 01001b ; [17:16] = 01b => D (primary data) ; [20:18] = 010b => Index Store Tag
    cache 0x9,0x2000(v0)
    lw zero,0x0(v0)
    lw zero,0x2000(v0)
    nop
    nop
    nop
    nop
    cache 0x9,0x0(v0)
    cache 0x9,0x2000(v0)
    addiu v0,v0,0x20
    bgtz v1,INIT_DATA_CACHE_LOOP
    addi v1,v1,-0x1

    jr ra
    nop

init_instr_cache:
    # Index and fill 512 blocks of instructions to primary cache
    # 32 bytes per tag
    # from 0x80000000 to 0x80001FE0
    # from 0x80002000 to 0x80003FE0
    mtc0 zero,CP0_TAG_LO
    #lui v0,0x8000
    lui v0,0x9fc0
    li v1,0xff

    INIT_INSTR_CACHE_LOOP:
    cache 0x8,0x0(v0)       # cache op = 01000b ; [17:16] = 00b => I (primary instruction) ; [20:18] = 010b => Index Store Tag
    cache 0x8,0x2000(v0)
    cache 0x14,0x0(v0)      # cache op = 10100b ; [17:16] = 00b => I (primary instruction) ; [20:18] = 101b => Fill
    cache 0x14,0x2000(v0)
    nop
    cache 0x8,0x0(v0)
    cache 0x8,0x2000(v0)
    addiu v0,v0,0x20
    bgtz v1,INIT_INSTR_CACHE_LOOP
    addi v1,v1,-0x1

    jr ra
    nop

jump_cached_memory:
    # This function modifies the return address (ra register)
    # ra register contains an address in kseg1 (unmapped/uncached) memory region
    # by turning off the 29th bit in the return address it makes it jump
    # to an address in the kseg0 (unmapped/cached) memory region
    # i.e 0xbfc00000 & 0xdfffffff => 0x9fc00000
    lui v1,0xdfff
    ori v1,v1,0xffff
    and ra,ra,v1
    jr ra
    nop

fill_data_cache:
    # load every word (4 bytes) from 0x80000000 to 0x80008000 (0x2000 * 0x4) to void (zero register)
    lui v0,0x8000
    li v1,0x2000

    FILL_DATA_LOOP:
    addiu v1,v1,-0x1
    lw zero,0x0(v0)
    bne v1,zero,FILL_DATA_LOOP
    addiu v0,v0,0x4

    jr ra
    nop

init_cpu:
    lui v0,0x3440
    # CU = 3 (Coprocessor 0 and 1 enabled)
    # FR = 1 (Enable additionnal floating-point registers)
    # BEV = 1 (Bootstrap)
    # DE = 0 (Parity remains enabled)
    # IM = 0
    # KX = 0
    # SX = 0
    # UX = 0
    # KSU = 0 (Kernel mode)
    # ERL = 0 (Error level normal)
    # EXL = 0 (Exception level normal)
    # IE = 0 (Interrupts disabled)
    mtc0 v0,CP0_STATUS 
    # 0 wired TLB entries         
    mtc0 zero,CP0_WIRED
    li v1,0x2f
    li a0,0xff
    
    TLB_LOOP:
    mtc0 zero,CP0_ENTRY_LO_0
    mtc0 zero,CP0_ENTRY_LO_1
    mtc0 zero,CP0_PAGE_MASK
    mtc0 a0,CP0_ENTRY_HI
    mtc0 v1,CP0_INDEX
    nop
    tlbwi
    nop
    addi v1,v1,-0x1
    bgez v1,TLB_LOOP
    addi a0,a0,0x2000

    li v0,0x0
    mtc0 v0,CP0_INDEX
    li v0,0x0
    mtc0 v0,CP0_ENTRY_LO_0
    li v0,0x0
    mtc0 v0,CP0_ENTRY_LO_1
    li v0,0x0
    mtc0 v0,CP0_CONTEXT
    li v0,0x0
    mtc0 v0,CP0_PAGE_MASK
    li v0,0x0
    mtc0 v0,CP0_WIRED
    li v0,0x0
    mtc0 v0,CP0_BAD_V_ADDR
    #li v0,0x0
    #mtc0 v0,CP0_COUNT
    li v0,0x0
    mtc0 v0,CP0_ENTRY_HI
    li v0,-0x1
    mtc0 v0,CP0_COMPARE
    li v0,0x38
    # ExcCode = 0xE (Reserved ?)
    mtc0 v0,CP0_CAUSE
    li v0,0x0
    mtc0 v0,CP0_EPC
    li v0,0x3
    # EC = 0 (Processor clock frequency divided by 2)
    # EP = 0 (Write back data rate doubleword every cycle)
    # BE = 0 (Little endian)
    # K0 = 3
    mtc0 v0,CP0_CONFIG
    li v0,0x0
    mtc0 v0,CP0_LLADDR
    li v0,0x0
    mtc0 v0,CP0_XCONTEXT
    li v0,0x0
    mtc0 v0,CP0_ECC
    li v0,0x0
    mtc0 v0,CP0_CACHE_ERR
    li v0,0x0
    mtc0 v0,CP0_TAG_LO
    li v0,0x0
    mtc0 v0,CP0_TAG_HI
    li v0,0x0
    mtc0 v0,CP0_ERROR_EPC
    lui v0,0x100
    # FS = 1
    ctc1 v0,$31
    jr ra
    nop

_main:
    .set macro

    la gp,_gp # Init Global pointer
    la sp,_stack_vma # Init Stack pointer

    # Reset watchdog 
    jal wdt_reset
    nop

    # Copy data segment to ram
    la a0,_sdata_vma
    la a1,_sdata_lma
    la a2,_data_size
    jal memcpy
    nop

    # Clear bss segment
    la a0,_sbss_vma
    la a2,_bss_size
    jal memset
    or a1,zero,zero

    # Clear heap segment
    la a0,_heap_vma
    la a2,_heap_size
    jal memset
    or a1,zero,zero

    # Setup heap allocator
    la a0,_heap_vma
    la a1,_heap_size
    jal umm_init_heap
    nop

    # fill screen green
    la a0,0x03e003e0
    or a1,zero,a0
    dsll32 a0,a0,0
    jal video_clear_framebuffer
    or a0,a1,a0

    jal video_swap_buffers
    nop

    # jump to main function
    or a0,zero,k0
    j main
    move ra,zero
