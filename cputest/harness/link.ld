/*
 * link.ld — the suite links and runs at SUITE_KSEG0_BASE (0x88010000).
 *
 * KSEG0 is unmapped and cached: the TLB tests can rewrite every entry without
 * unmapping the code executing them, and the cache tests can reach the same
 * physical bytes through KSEG1 by clearing bit 29.
 *
 * On the KI board DRAM is 8 MB at physical 0x08000000, and SRAM 512 KB at 0
 * (the exception vectors live at KSEG0 0x80000000, in the 0x200 bytes the ROM
 * keeps free below its .data). The image, its stack and its scratch pages,
 * about 530 KB in all, go at physical 0x08010000: between the memory test's
 * DRAM window at 0x08000000 and its IDE buffer at 0x08100000, and clear of
 * every other DRAM window it uses and of the log (ki_cputest.h).
 *
 * start.S relocates the image here from the boot ROM, so this address is what
 * the code is *linked* for regardless of where it is entered.
 */

/* No OUTPUT_FORMAT: its name differs between toolchains (elf32-ntradlittlemips
 * on Linux, elf32-nlittlemips on bare-metal ones); -EL and the n32 objects
 * select it. */
OUTPUT_ARCH(mips)
ENTRY(_start)

SECTIONS
{
    . = 0x88010000;

    _ftext = .;
    .init : { *(.init) }
    .text : {
        *(.text)
        *(.text.*)
    }
    _etext = .;

    . = ALIGN(16);
    _fdata = .;
    .rodata : { *(.rodata) *(.rodata.*) }
    .data   : { *(.data) *(.data.*) }

    /* -G0 means nothing should land in the small-data sections, but catch
     * anything that slips through rather than silently dropping it. */
    _gp = ALIGN(16) + 0x7ff0;
    .lit8   : { *(.lit8) }
    .lit4   : { *(.lit4) }
    .sdata  : { *(.sdata) *(.sdata.*) }
    _edata = .;

    . = ALIGN(16);
    _fbss = .;
    .sbss : { *(.sbss) *(.sbss.*) *(.scommon) }
    .bss  : {
        *(.bss)
        *(.bss.*)
        *(COMMON)
        . = ALIGN(16);
        /* Suite stack: 64 KB, generous for deeply-nested test helpers. */
        _stack_bottom = .;
        . += 0x10000;
        _stack_top = .;
        /* Scratch pages for the mem/tlb/cache tests: 16 MB-aligned so a
         * 16 MB TLB page can map them, and large enough for a 4-way cache
         * eviction sweep. */
        . = ALIGN(0x4000);
        _scratch_start = .;
        . += 0x40000;              /* 256 KB */
        _scratch_end = .;
    }
    _end = .;
    ASSERT(_end <= 0x88100000, "cputest overruns the memory test's IDE buffer")

    /* Nothing should reference these; if a real .got or dynamic section shows
     * up, the link is wrong (we build -mno-abicalls -fno-pic). */
    /DISCARD/ : {
        *(.MIPS.abiflags)
        *(.reginfo)
        *(.comment)
        *(.note*)
        *(.gnu.attributes)
        *(.pdr)
        *(.eh_frame)
    }
}
