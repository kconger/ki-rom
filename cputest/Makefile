# cputest — the iris CPU test suite, built as an image for the KI test ROM.
#
#   make            build/cputest.bin (and the .elf it comes from)
#   make clean
#
# The top-level Makefile builds this for ROM=memtest and embeds the .bin;
# nothing here needs to be run by hand. CC/LD/OBJCOPY come from there.
#
# The flags are upstream's (see docs/toolchain.md) except -EL: the KI board's
# R4600 runs little-endian.
#
# -march=mips3 MIPS IV instructions are emitted only inside explicit `.set
#              mips4` regions in the mips4/ tests, which are gated at runtime
#              on PRId, so a stray MIPS IV opcode from the compiler would be a
#              silent failure on a MIPS III part.
# -mabi=n32    NOT o32 and not the ROM's o64: n32 keeps a 64-bit value in one
#              register, which is what the tests are about, and is the ABI the
#              tests were written and validated under.
# -mno-abicalls -fno-pic   no GOT, no PIC prologues: we link at a fixed address.
# -G0          nothing in .sdata/.sbss, so no $gp-relative addressing.
# -msoft-float the *compiler* must not emit FP instructions on its own; the FPU
#              tests drive the FPU by hand with explicit asm.

CC      ?= mipsel-linux-gnu-gcc-14
LD      ?= mipsel-linux-gnu-ld
OBJCOPY ?= mipsel-linux-gnu-objcopy

BUILD   := build
TARGET  := $(BUILD)/cputest.elf

ARCHFLAGS := -march=mips3 -mabi=n32 -EL -mno-abicalls -fno-pic -G0 -msoft-float
CFLAGS    := $(ARCHFLAGS) -ffreestanding -nostdlib -nostdinc \
             -fno-builtin -fno-stack-protector -fno-strict-aliasing \
             -fno-delete-null-pointer-checks -fomit-frame-pointer \
             -O1 -std=gnu11 \
             -Wall -Wextra -Werror -Wno-unused-parameter \
             -Iharness -Itests
ASFLAGS   := $(ARCHFLAGS) -ffreestanding -nostdlib -Iharness -Wa,--fatal-warnings
# The n32 emulation is named differently by Linux (elf32ltsmipn32) and
# bare-metal (elf32bmipn32) binutils; take whichever this ld has, preferring a
# little-endian one (-EL makes a big-endian one write little-endian anyway).
LDEMUS    := $(shell $(LD) -V | grep -o '[a-z0-9]*mipn32')
LDEMU     := $(firstword $(filter %lmipn32 %ltsmipn32,$(LDEMUS)) $(LDEMUS))
LDFLAGS   := -EL $(if $(LDEMU),-m $(LDEMU)) -T harness/link.ld -nostdlib

HARNESS_C := harness/console.c harness/testlib.c harness/tests.c
HARNESS_S := harness/start.S
TEST_C    := $(sort $(wildcard tests/*/*.c))

OBJS := $(patsubst %.c,$(BUILD)/%.o,$(HARNESS_C) $(TEST_C)) \
        $(patsubst %.S,$(BUILD)/%.o,$(HARNESS_S))
DEPS := $(OBJS:.o=.d)

# No libgcc: under n32 every 64-bit operation the C code performs is a native
# instruction, so if a helper reference ever appears the link fails loudly.

.PHONY: all clean vectors
all: $(BUILD)/cputest.bin

# Regenerate the IEEE-754 expectation tables in tests/fpu/. Needs python3; the
# generated files are checked in.
vectors:
	python3 gen/fpvectors.py --check
	python3 gen/fpvectors.py

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD)/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -MMD -MP -c -o $@ $<

$(TARGET): $(OBJS) harness/link.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

# Everything from _ftext to _edata; start.S zeroes .bss itself.
$(BUILD)/cputest.bin: $(TARGET)
	$(OBJCOPY) -O binary $< $@

clean:
	rm -rf $(BUILD)

-include $(DEPS)
