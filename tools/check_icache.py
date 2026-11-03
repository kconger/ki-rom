#!/usr/bin/env python3
"""
Checks that the memory test's timed kernels cannot be evicted from the
instruction cache by the code that runs around and between their timed passes.

The core's I-cache is direct-mapped, 16 KiB, 32-byte lines: a line's index is
addr[13:5]. Two different lines with the same index evict each other. boot.ld
links the measurement code as one block, _sbench.._ebench; this reads the
linked ELF's symbols and fails if any line of a timed kernel shares an index
with a different line of that block, or if a function the measurements rely on
has drifted out of it. It also reports whether the block would be just as clean
in one 8 KiB way of a 2-way 16 KiB cache (the board's R4600).

Usage: check_icache.py <elf>
"""

import re
import struct
import sys

LINE = 32
SETS_DIRECT = 16384 // LINE
SETS_WAY = 8192 // LINE

# Timed kernels: cpu.S's entry points.
KERNEL = re.compile(r"^(mem|cpu)_bench_")

# Called around or between timed passes, so must sit in the block too.
PATH = re.compile(r"^(bench_|dcache_writeback_invalidate$|chase_verify$|wdt_reset$|blend_background$)")


def symbols(path):
    with open(path, "rb") as f:
        data = f.read()
    if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        sys.exit(f"{path}: not a little-endian ELF32 file")
    shoff, = struct.unpack_from("<I", data, 0x20)
    shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
    sections = [struct.unpack_from("<IIIIIIIIII", data, shoff + i * shentsize) for i in range(shnum)]

    found = {}
    for sh in sections:
        if sh[1] != 2:  # SHT_SYMTAB
            continue
        strtab = sections[sh[6]]
        for off in range(sh[4], sh[4] + sh[5], 16):
            name_off, value, size, info, _, shndx = struct.unpack_from("<IIIBBH", data, off)
            end = data.index(b"\0", strtab[4] + name_off)
            name = data[strtab[4] + name_off:end].decode()
            if name and info & 0xF != 4:  # skip STT_FILE
                found.setdefault(name, []).append((value, size, info >> 4, info & 0xF))
    return found


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    syms = symbols(sys.argv[1])

    if "_sbench" not in syms or "_ebench" not in syms:
        print("check_icache: no _sbench/_ebench in this image, nothing to check")
        return 0
    start, end = syms["_sbench"][0][0], syms["_ebench"][0][0]

    # Function extents. Assembly labels have size 0: such a global runs to the
    # next global or sized function (its local labels are inside it).
    funcs = {}
    bounds = sorted({end} | {v for n, l in syms.items() for v, s, bind, typ in l
                             if start <= v < end and (bind == 1 or s > 0)})
    for name, entries in syms.items():
        for value, size, bind, typ in entries:
            if not (start <= value < end) or name.startswith("_"):
                continue
            if size == 0:
                if bind != 1:
                    continue
                size = min(b for b in bounds if b > value) - value
            funcs[name] = (value, value + size)

    def lines(lo, hi):
        return set(range(lo // LINE, (hi + LINE - 1) // LINE))

    # The block, plus any measurement function that has strayed out of it.
    block = lines(start, end)
    errors = []
    for name, entries in syms.items():
        if (KERNEL.match(name) or PATH.match(name)) and name not in funcs:
            where = ", ".join(f"0x{v:08X}" for v, *_ in entries)
            errors.append(f"{name} at {where} is outside the measurement block")
            for value, size, *_ in entries:
                block |= lines(value, value + max(size, 4))
    kernels = {n: lines(*r) for n, r in funcs.items() if KERNEL.match(n)}

    def aliases(sets):
        found = []
        for name, own in sorted(kernels.items(), key=lambda k: funcs[k[0]]):
            for line in sorted(own):
                for other in block:
                    if other != line and other % sets == line % sets:
                        found.append((name, line, other))
        return found

    direct = aliases(SETS_DIRECT)
    way = aliases(SETS_WAY)
    for name, line, other in direct:
        errors.append(f"{name}: line 0x{line * LINE:08X} and 0x{other * LINE:08X} share index {line % SETS_DIRECT}")

    print(f"check_icache: measurement block 0x{start:08X}-0x{end:08X}, {end - start} bytes, {len(block)} lines")
    for name, own in sorted(kernels.items(), key=lambda k: funcs[k[0]]):
        lo, hi = funcs[name]
        idx = sorted(l % SETS_DIRECT for l in own)
        print(f"  {name:22} 0x{lo:08X}-0x{hi:08X}  index {idx[0]}-{idx[-1]}")
    print(f"  16 KiB direct-mapped: {'no aliases' if not direct else f'{len(direct)} aliases'}")
    print(f"  8 KiB way (2-way):    {'no aliases' if not way else f'{len(way)} aliases'}")

    for e in errors:
        print(f"check_icache: ERROR: {e}", file=sys.stderr)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
