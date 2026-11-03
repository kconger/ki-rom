/*
 * Deliberately NOT OUTPUT_FORMAT(binary). Under the binary format ld silently
 * resolves undefined symbols to 0 and does not report overlapping sections --
 * both of which this project has been bitten by. Linking as ELF keeps those
 * diagnostics, and the Makefile converts the result with objcopy -O binary.
 */

ENTRY(_reset_exception_handler)

MEMORY {
    rom (rx) : ORIGIN = 0x9fc00000, LENGTH = 512K
    sram (rw) : ORIGIN = 0x80000000, LENGTH = 512K
    dram (rwx) : ORIGIN = 0x88000000, LENGTH = 8M
}

SECTIONS {
    .text : ALIGN(4) {
        # start.S first: the reset and exception vectors sit at fixed offsets
        # from the start of the ROM. lzss.S next, as it always was, so the
        # block below starts where the kernels have always been.
        *-start.o(.text .text.*);
        *-lzss.o(.text .text.*);

        # The measurement code, as one contiguous block: the timed kernels
        # (cpu.S), everything that runs around and between their timed passes
        # (bench.c: the cache sweep, best-of-3 loops, chain build and verify)
        # and wdt_reset. The core's instruction cache is direct-mapped, 16 KiB
        # with 32-byte lines (index = addr[13:5]); the board's R4600 is 2-way.
        # Code that is contiguous and smaller than the cache cannot evict
        # itself, so a kernel is never refetched from the EPROM inside its
        # timed window because the sweep before it shared its cache line, and
        # both machines measure memory rather than I-cache conflicts. Kept at
        # or under 8 KiB so it also fits one way of a 2-way 16 KiB cache.
        # tools/check_icache.py checks the linked result.
        _sbench = .;
        *-cpu.o(.text .text.*);
        *-bench.o(.text .text.*);
        *-wdt.o(.text .text.*);
        _ebench = .;

        # .text.* matters: -Os enables -freorder-functions, so GCC emits
        # .text.startup (main) and .text.unlikely. Matching only *(.text) left
        # those as orphan sections, placed by ld's heuristic rather than by
        # this script -- and orphan placement differs between output formats.
        *(.text .text.*);
        *(.assets);

        # Large data that is executed only once (the CPU test suite's image,
        # which copies itself to DRAM) goes after all code, so it never moves
        # code relative to the measurement block.
        *(.cputest);
    } > rom AT > rom

    .data ORIGIN(sram) + 0x200 : ALIGN(8) {
        _sdata = .;
        *(.sdata);
        *(.data);
        *(.rodata);
        *(.rodata.*);
        *(.detour.*);
        _edata = .;
    } > sram AT > rom

    .bss ALIGN(ORIGIN(sram) + 0x200 + SIZEOF(.data), 8) (NOLOAD) : ALIGN(8) {
        _sbss = .;
        *(.sbss);
        *(.bss);
        *(COMMON);
        _ebss = .;
    } > sram

    # In this configuration the stack and heap do grow in opposide direction and away of each other
    .heap ORIGIN(sram) + 0x4000 (NOLOAD) : ALIGN(8) {
        _heap_vma = .; # Base address for the heap (growing up)
        . += 0x10000;
    } > sram

    # The stack grows down from the top of the first 16K of cached SRAM towards
    # _ebss. This is deliberately a bare symbol rather than an output section:
    # as a section it spanned 0x80000000-0x80004000 and therefore overlapped
    # .data and .bss, which ld reports as a hard error under any non-binary
    # output format. OUTPUT_FORMAT(binary) merely suppressed the diagnostic.
    _stack_vma = ORIGIN(sram) + 0x4000; # Base address for the stack (growing down)

    .fill : {
        LONG(0x0);
        FILL(0x00);
        . = ORIGIN(rom) + LENGTH(rom);
    } > rom

    _gp = _sdata + 0x7ff0;
    
    PROVIDE(_sdata_vma = ADDR(.data));
    PROVIDE(_sdata_lma = LOADADDR(.data));
    PROVIDE(_data_size = SIZEOF(.data));
    PROVIDE(_sbss_vma = ADDR(.bss));
    PROVIDE(_bss_size = SIZEOF(.bss));
    PROVIDE(_heap_size = SIZEOF(.heap));

    PROVIDE(gIO = 0xB0000080);
    PROVIDE(gIDE = 0xB0000100);
    PROVIDE(gIDEControl = 0xB0000170);
    PROVIDE(gVramBank0 = 0x80030000);
    PROVIDE(gVramBank1 = 0x80058000);

    # .data + .bss grow up from 0x80000200 into the region the stack grows down
    # through, and nothing else bounds them. Reserve 8K: the detour gateway
    # stacks sit at _stack_vma - 0x1000 and below (see detour.h).
    ASSERT(_ebss <= _stack_vma - 0x2000, "ERROR: .data + .bss leave less than 8K for the stack")
    ASSERT(_heap_vma + _heap_size <= 0x80030000, "ERROR: heap overruns VRAM bank 0")
    ASSERT(SIZEOF(.text) + SIZEOF(.data) <= LENGTH(rom), "ERROR: image does not fit in ROM")
    ASSERT(_ebench - _sbench <= 0x2000, "ERROR: the measurement block is over 8 KiB and can alias itself in the I-cache")

    /DISCARD/ : { 
        *(.reginfo)
        *(.MIPS.abiflags)
        *(.rel.*)
        *(.pdr)
        *(.shstrtab)
        *(.strtab)
        *(.symtab)
        *(.gnu.*)
        *(.note.*)
        *(.comment)
        *(.mdebug.*)
    }
}
