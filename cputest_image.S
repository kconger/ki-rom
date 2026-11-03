# The CPU test suite's image (cputest/, built by its own Makefile for the n32
# ABI). cputest.c calls its first word where it sits; the image copies itself
# to DRAM, runs, and returns.

# Its own section, which boot.ld places after all code: at 200 KiB it would
# otherwise sit between objects and shift the code that follows it.
.section .cputest, "ax"
.balign 16
.globl cputest_image
cputest_image:
    .incbin "cputest/build/cputest.bin"
