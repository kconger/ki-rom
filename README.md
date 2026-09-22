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

It boots straight into the test and runs once. Any P1/P2 button switches
between the two pages; P1/P2 START runs everything again. It never starts a
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
- The VRAM banks are **not** the same speed as the SRAM row, although `boot.ld`
  describes all 512 KiB as one device. See the sweep section below. Each bank is
  measured only while it is the back buffer, which keeps the picture clean and
  keeps video refresh contention out of the figure.
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

It exists to settle a discrepancy on the first page: the low SRAM and the two
VRAM banks report very different speeds even though `boot.ld` describes all of
it as one 512 KiB device, and the low SRAM figures match DRAM to three digits.
A clean step on a bank boundary would mean the split is real; a step anywhere
else, or none, would mean the main page figures are not device speeds.

Measured on an A-19489, the step is exactly at **0x30000**: 17.49 MB/s below it,
99.68 MB/s above, with all twelve points below uniformly slow and both banks
uniformly fast. The split is real, and the main page figures stand.

Two further things fall out of the graph:

- One point in each bank reads at roughly 46% of the bank's rate, both at bank
  offset 0x24000 — that is 0x54000 and 0x7C000. An 8 KiB window there straddles
  0x25800 bytes into the bank, which is 320×240×2 exactly. The blend of 0x1800
  at the fast rate and 0x800 at the slow one works out to 45.9 MB/s, matching
  the bar. **The fast memory is one framebuffer per bank and no more**:
  0x30000–0x557FF and 0x58000–0x7D7FF. The tail of each bank is on the slow
  path, as is everything below 0x30000.
- The low SRAM matching DRAM to three digits is then not a coincidence: both
  are on the same slow path, and only the two framebuffers are not.

An uncached 64-bit read costs 45.8 cycles on the slow path against 8.0 on the
fast one. What makes them differ — separate devices, bus width, wait states — is
a board-level question this ROM cannot answer.

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
