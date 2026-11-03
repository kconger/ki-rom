# Killer instinct Boot ROM

This project is an open source Boot ROM for Killer instinct 1 & 2 arcade game developed by Rare and published by Midway.

## Compatibility

### KI-U96 A-19489

| ROM version | Status             | Note                     |
| ----------- | ------------------ | ------------------------ |
| KI l1.5d    | :white_check_mark: |                          |
| KI l1.5di   | :white_check_mark: | unofficial rom           |
| KI l1.4     | :white_check_mark: |                          |
| KI l1.3     | :white_check_mark: |                          |
| KI p47      | :warning:          | need any-ide patch       |
| KI2 l1.0    | :white_check_mark: |                          |
| KI2 l1.1    | :white_check_mark: |                          |
| KI2 l1.3    | :no_entry_sign:    | use KI2 l1.3k            |
| KI2 l1.3k   | :white_check_mark: |                          |
| KI2 l1.4    | :no_entry_sign:    | use KI2 l1.4k            |
| KI2 l1.4k   | :white_check_mark: |                          |
| KI2 l1.4p   | :no_entry_sign:    | unofficial rom           |
| KI2 d1.4p   | :white_check_mark: | unofficial rom           |

### KI2-U96 A-20351

| ROM version | Status             | Note                     |
| ----------- | ------------------ | ------------------------ |
| KI l1.5di   | :white_check_mark: | unofficial rom           |
| KI l1.5d    | :white_check_mark: |                          |
| KI l1.4     | :white_check_mark: |                          |
| KI l1.3     | :white_check_mark: |                          |
| KI p47      | :no_entry_sign:    | need remap patch         |
| KI2 l1.0    | :white_check_mark: |                          |
| KI2 l1.1    | :white_check_mark: |                          |
| KI2 l1.3    | :white_check_mark: |                          |
| KI2 l1.3k   | :no_entry_sign:    | use KI2 l1.3             |
| KI2 l1.4    | :white_check_mark: |                          |
| KI2 l1.4k   | :no_entry_sign:    | use KI2 l1.4             |
| KI2 l1.4p   | :white_check_mark: | unofficial rom           |
| KI2 d1.4p   | :no_entry_sign:    | unofficial rom           |

- :white_check_mark:: Fully supported
- :no_entry_sign:: Not Supported (requires patches)
- :warning:: Partially Supported (requires full original hardware)

## Features

This boot rom introduces new features compared to the stock boot rom.

- LZSS compression/decompression of game ROM (faster boot time)
- In-memory patching of game ROM
- Fixes no sound at boot on MAME
- Replaced "Bong" boot sound
- Pause on boot
- Soft Multiboot K1 & K2 (Requires additional hardware & compilation flag)
- Additional dipswitch configuration bits

## POST

Power On Self Test will check for faulty or missing hardware.

Tested hardware:
- SRAM
- VRAM
- DRAM
- IDE

When S1:7 dipswitch bit is turned on POST result will be kept on screen until any P1 input is triggered.

**NOTE: This has been moved to a dedicated board testing ROM**

## Memory, CPU & IDE speed test ROM

A standalone ROM that measures the throughput of every memory region, the CPU
and the IDE interface, and puts the results on screen. It contains no game
image, so it builds from a clean checkout with an empty `assets/roms`:

```
make memtest                 # A-19489, output/19489-memtest.u98
make memtest BOARD=20351     # A-20351, output/20351-memtest.u98
make memtests                # both boards
```

It boots straight into the test and runs once. Any P1/P2 button steps forward
through the pages and Left or Up steps back; P1/P2 START runs everything again. It never starts a
game and never writes to the disk.

| Region | Physical range      | Measured window                |
| ------ | ------------------- | ------------------------------ |
| SRAM   | 0x00000 - 0x2FFFF   | 0x14000 - 0x23FFF              |
| VRAM 0 | 0x30000 - 0x57FFF   | 0x30000 - 0x3FFFF              |
| VRAM 1 | 0x58000 - 0x7FFFF   | 0x58000 - 0x67FFF              |
| DRAM   | 0x8000000 - 0x87FFFFF | 0x8000000 - 0x800FFFF        |
| IDE    | LBA 0 - 127         | 64 KiB, read only              |

Read and write are reported separately for cached (kseg0) and uncached (kseg1)
access, in MB/s where MB is 10^6 bytes. Each figure is the fastest of three
passes of 64 KiB of 64-bit accesses, with the primary data cache swept between
passes so a cached pass always starts cold.

Notes on what the numbers mean:

- The measured window is 64 KiB for every region so the figures are directly
  comparable. It is four times the 16K primary data cache, so a cached pass
  reports memory bandwidth through the cache, not cache bandwidth.
- SRAM is measured at 0x14000 rather than from 0x00000 because everything below
  that is live: `.data`, `.bss`, the stack, the detour gateway stacks and the
  heap. The bound is re-derived from the linker symbols at runtime, so if the
  heap ever grows past it the SRAM write test reports `skip` instead of
  corrupting the allocator.
- `boot.ld` describes the VRAM banks as two windows inside the same 512 KiB SRAM
  as the SRAM row, so on that description they should report the same speed.
  Whether a given board or core agrees is what the sweep page is for. Each bank
  is measured only while it is the back buffer, which keeps the picture clean
  and keeps video refresh contention out of the figure.
- The IDE figure covers a whole `READ SECTORS`, from the command write to the
  last byte, after an untimed warm-up read of the same sectors. With no drive
  fitted it reports `no drive / not ready` rather than hanging: every wait is
  bounded by elapsed time, well inside the watchdog period.

### CPU

The CPU block reports pipeline cycles per operation, plus the instruction rate
in MIPS taken from the ALU kernel:

| Row          | Kernel                                    |
| ------------ | ----------------------------------------- |
| ALU addu     | dependent chain of `addu`                 |
| Load lw (D$) | `lw` over 256 bytes, always a cache hit   |
| Mul multu    | back to back `multu`                      |
| Div divu     | back to back `divu`                       |
| FPU add.s    | dependent chain of `add.s`                |
| Vsync (Hz)   | video refresh rate, measured against Count |

The kernels live in `cpu.S` rather than in C because the figure only means
something if the instruction stream is exactly what the row claims it is, and
`-Os` gives no way to pin that down from C. Each runs a loop of 64 operations
plus two instructions of loop control, so every figure carries roughly 2 cycles
of overhead per 64 operations: about 3% on a single-cycle instruction, and
proportionally less on the slow ones. Cycles are `Count x 2`, since Count
advances once per two pipeline cycles.

Unlike the memory kernels, the CPU kernels do **not** sweep the cache between
passes: the loop has to run from the instruction cache and the load kernel has
to hit in the data cache, otherwise the result is a memory measurement with a
CPU label on it. The first pass warms both and the fastest of three is kept.

### SRAM read sweep

The second page walks an 8 KiB uncached read window across all 512 KiB of SRAM
in 16 KiB steps and plots the result, one bar per point, coloured by region:
white below VRAM bank 0, cyan for bank 0, yellow for bank 1. Underneath it
prints the largest change between neighbouring points, which is the boundary
address if there is one.


The sweep is read only. Reads are safe at any SRAM address, including over
`.data`, `.bss` and the stack, whereas writes are not — and the uncached read is
where the discrepancy appears anyway. It runs entirely within one frame so the
whole sweep sees the same bank being scanned out; the bank that was the back
buffer is named on the page, since the other one carries video refresh
contention.

`Vsync (Hz)` is the one number here measured against something other than the
CPU itself. The video timebase is its own crystal, so if the refresh rate comes
out at the expected value the 50 MHz Count — and therefore the 100 MHz pipeline
— is confirmed. A CPU clocked differently from the assumed 100 MHz shows up
here as a refresh rate that is wrong by the same ratio.

### Heavy frame

The last page times a heavy game frame's memory traffic: the sprite blend of
Killer Instinct 2's slowest frame in the barrel explosions on Orchid's stage,
three vblanks on the real game. Ten sprites in the order the game draws them -
two covering the whole screen, one drawn rightwards and one leftwards, and
eight smaller ones - each run-length coded in DRAM and added into the back
buffer one pixel at a time: a halfword load, a per-channel saturating add, a
halfword store. Each sprite's box, run lengths, direction and coverage are the
game's; the pixels are generated from a fixed seed, so every board and core
blends identical data. 175,445 pixels, 3.21 million instructions.

Each row is one sprite: its pixels, cycles per pixel and thousands of cycles.
The frame row is all ten, fastest of three passes, each from a cold data cache
over the same background; underneath it the frame in vblanks at the measured
refresh rate, and a checksum of the bank afterwards. Two machines that print
the same checksum blended the same pixels (`0x727B3D0B`).

The blend is 65-77% of the game's frame; its logic and background
decompression are not reproduced.

### CPU tests

The pages after the heavy frame run the
[iris CPU test suite](https://github.com/techomancer/iris/tree/main/cpu-tests)
(BSD 3-Clause): about 260 bare-metal tests of the R4600's integer unit, FPU,
exceptions, CP0, TLB, User mode and caches, each with expectations validated
on real R4400 and R5000 silicon or derived for the R4600. It runs last, after
every measurement is on screen, and takes the CPU over completely - its own
exception vectors, the whole TLB, the caches - before handing it back.

The first page sums it up: the CPU as PRId reports it, checks passed and
failed, tests run, failed and skipped as another CPU's, and the list of the
tests that failed with the page each one's report is on. The reports follow:
the test, then a line for each failed check with its source line, the value
it got and the one it wanted. A CPU that passes everything gets one page
saying so. On these pages as on the others, Left or Up goes back a page.

If a test hangs the board, the watchdog resets it and the page names the test
it was in instead of running the suite again; START retries. `cputest/README.md`
has the details: the port to little-endian, the memory it uses, and why MAME
cannot run all of it.

## Patches

| ROM version | I/O Remap          | A-20383 Bypass     | AnyIDE             | 2In1 HDD           | Reset              | No Music Fade out  | No Whiteblood      |
| ----------- | ------------------ | ------------------ | ------------------ | ------------------ | ------------------ | ------------------ | ------------------ | 
| KI l1.5d    | :white_check_mark: |                    | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: |
| KI l1.5di   | :white_check_mark: |                    | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: |
| KI l1.4     | :white_check_mark: |                    | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: |                    |
| KI l1.3     | :white_check_mark: |                    | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: |                    |
| KI2 l1.4    |                    |                    | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: |                    |
| KI2 l1.4k   |                    | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: |                    |
| KI2 l1.3    |                    |                    | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: |                    |
| KI2 l1.3k   |                    | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: |                    |
| KI2 l1.1    | :white_check_mark: |                    | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: |                    |
| KI2 l1.0    | :white_check_mark: |                    | :white_check_mark: | :white_check_mark: | :white_check_mark: | :white_check_mark: |                    |

### KI2-U96 A-20351 remap

This patch remaps I/O memory addresses to allow KI1 ROM to run on KI2 dedicated hardware and vice versa.

### K12-U1 A-20383 protection bypass

This patch disables the copy protection of the KI1 to KI2 upgrade kit.

NOTE: This patch is not based and/or related to a patch made by the member of the arcade-projects.com forum @[DogP](https://www.arcade-projects.com/members/dogp.2487/)

### Any IDE

This patch disables the IDE drive model check, allowing to run the game with any IDE compatible drive model.

### Soft reset

This patch adds an input combination allowing to soft reset the game by pressing
P1 UP + P1 START + P1 FP.

### Infinite attract mode music

This patch disables the fade out of the music in attract mode (demo mode), allowing the music to play indefinitely.

## DipSwitch S1

| bit | description               | Off    | On        |
| --- | ------------------------- | ------ | --------- |
| 6   | Disable boot sounds       | Sounds | No sounds |
| 7   | Wait for input on boot    | Wait   | Continue  |

## To do

- Write any-ide patches (KI1 p47)
- Write remap patches (KI1 p47)

## Acknowledgments

- [github.com/DS-Homebrew](https://github.com/DS-Homebrew/nds-bootstrap/blob/master/lzss.c) for the LZSS ROM compression tool
- [github.com/PeterLemon](https://github.com/PeterLemon/N64/blob/master/Compress/LZ77/LZ77Decode/LZ77Decode.asm) for the LZSS mips assembly decompression
- [arcade-projects.com](https://www.arcade-projects.com/) members for their support and great ideas
- My wife for letting me spending evenings and nights on this projet
