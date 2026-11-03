# cputest — the iris CPU test suite on the KI board

This is the bare-metal MIPS III / MIPS IV test suite from
[techomancer/iris](https://github.com/techomancer/iris/tree/main/cpu-tests)
(commit `30b4d84`, BSD 3-Clause, see `LICENSE`), ported to run inside the KI
memory test ROM (`make memtest`). The tests themselves are upstream's; what
changed is the harness around them and the byte order. Upstream's README is in
[docs/upstream-README.md](docs/upstream-README.md), and the rest of `docs/` is
upstream's too: read [docs/gotchas.md](docs/gotchas.md) before believing a new
failure, and [docs/r4600.md](docs/r4600.md) for where each R4600 expectation
comes from, including what the first real R4600, on a KI board, measured.

The suite reads `PRId` and picks its expectations: R4600 (imp 0x20) on a KI
board. Tests for the R4400 or R5000 only are skipped, not failed.

## Building

Nothing to do by hand: the top-level Makefile builds this directory with the
same toolchain for `ROM=memtest` and embeds `build/cputest.bin` through
`cputest_image.S`. The image is about 205 KB of the 512 KB boot ROM.

It is built with upstream's flags (`-march=mips3 -mabi=n32 -msoft-float -O1`,
see [docs/toolchain.md](docs/toolchain.md)) except `-EL`. The ROM itself is
o64; the two never share a calling convention beyond one call with no
arguments, and everything else crosses through `harness/ki_cputest.h`.

## How the ROM runs it

The CPU tests are the last step of the memory test, so every other figure is
on screen before anything here can hang the board. The ROM calls the image's
first word in the boot ROM; `harness/start.S` then

1. copies the image to DRAM at physical `0x08010000` (KSEG0 `0x88010000`),
   which gives every run a fresh `.data`;
2. saves the ROM's registers, Status, Config, Count, Compare, Wired, EntryHi
   and FCSR, and writes back the whole data cache, since the cache tests drop
   lines without writing them;
3. installs its exception vectors at KSEG0 `0x80000000`, in the 0x200 bytes the
   ROM keeps free below its `.data`, and runs every test with BEV clear;
4. on the way out, invalidates the TLB, puts back the saved state, empties both
   caches from uncached code, restores Count to its value on entry, and returns
   to the ROM.

| Where (physical)        | What                                              |
| ----------------------- | ------------------------------------------------- |
| `0x00000000 - 0x001FF`  | exception vectors while the suite runs            |
| `0x0002F100`            | status block (`ki_cputest.h`), uncached           |
| `0x08010000 - 0x08094000` | the image, its 64 KB stack and 256 KB of scratch |
| `0x08780000 - 0x087BFFFF` | the log: what upstream prints on its serial port |
| `0x087C0000 - 0x087D0FFF` | the ROM's index of the log lines it shows, and of the failed tests |

The status block and the log are written uncached and outlive a watchdog
reset. They are written only with whole doublewords: on the KI board an
uncached store narrower than 64 bits to SRAM or DRAM rewrites its whole
doubleword, and the bytes it did not name read back as 0. So every status
field is 64 bits, the test's name is packed eight characters to a doubleword,
the log is rewritten a doubleword per character, and the progress line on
screen is drawn four pixels to a store. The tests themselves are not changed:
those that store bytes, halfwords or words uncached to memory see the board's
behaviour and report it. The suite publishes the name of each test before running it, so if a
test hangs the CPU, the next boot finds the run still marked as running, does
not start it again, and the CPU test page says which test it was in, with the
log up to that point. START runs everything again, the suite included. The
suite reads the watchdog register before every test and on every line of
output.

## Results on screen

The pages after the heavy frame page are the CPU tests. The first is a
summary - CPU, PRId, FIR, Config, checks passed and failed, tests run, failed
and skipped as other CPUs' - and the list of the tests that failed, each with
the page its report starts on; the list continues over as many pages as it
needs. Left or Up on either joystick goes back a page, any other button
forward. After the list comes the report of every failed test: its name in
red, then one line for each check that failed, such as

    alu.c:168 CAUSE_EXC(exc.cause) got 0x0 want 0xc

with the values in hex without leading zeros (a sign-extended value still
shows all 16 digits), and whatever else the test printed to explain itself.
A line too long for the screen breaks at a space and continues indented, never
between "got" or "want" and its value. Passes, skips, and the notes a passing
test prints (for instance which LWR behaviour the R4600 showed) stay in the
log but are not drawn. A CPU that passes everything shows "Every test
passed." and a single page.

The log format is the one change to upstream's output: upstream prints a
check's got and want on lines of their own, each padded to 16 digits.

## Little-endian

Upstream runs big-endian on SGI machines. Every expectation that depends on
byte order now has both answers under `#if ENDIAN_BIG` (`tests/endian.h`), with
the big-endian code unchanged: byte and halfword offsets within a doubleword,
the LWL/LWR/SWL/SWR/LDL/LDR/SDL/SDR tables and their canonical unaligned pairs,
and the partial-load offsets in `mem/lwr_all_offsets` (offsets 1-3 in
little-endian, where big-endian has 0-2). One test is new:
`identity/config_be` checks that Config.BE matches the build, so a byte-order
mismatch shows up first, before the `mem/` failures it would cause.

## Changes from upstream, in short

- `harness/`: `console.c` writes the log instead of the Indy's serial port;
  `arcs.*` and `scsilog.*` (Indy PROM and SCSI log sinks) and the IRIS test
  device are gone; `start.S` has the entry and return above; `testlib.c`'s
  runner publishes progress to the status block; `link.ld` links at
  `0x88010000` and asserts the image stays clear of the memory test's IDE
  buffer; `ki_cputest.h` is new.
- `tests/`: the byte-order changes above and `identity/config_be`; R4600
  expectations measured on a KI board, and `umode/*_load_third` logging its
  address with `sw` (see [docs/r4600.md](docs/r4600.md)).
- Not carried over: `oracle/` (the Indy hardware logs), `run/` (IRIS and PROM
  launch scripts), `PLAN.md`.

## In MAME

MAME runs the ROM but is not a reference for this suite. Its MIPS core aborts
on `0x80000000 / -1` (a host integer overflow, `muldiv/div_overflow_no_trap`)
and on a reserved opcode (`excep/reserved_instruction`) instead of emulating
them, so a MAME run needs those disabled. With them disabled the suite runs to
the end and returns to the ROM; MAME then fails about 78 tests (no overflow or
address-error exceptions, a 32-bit EPC, FPU exception flags and traps, MIPS IV
on an R4600), while every byte-order test passes.
