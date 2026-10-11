# Star Wars Episode I: The Phantom Menace (PSX, SLUS_008.84 USA Rev 1)
#
#   make setup     split the executable (needs disks/us/SLUS_008.84, see README)
#   make           rebuild it and check the result against the original
#   make clean     remove build output
#   make distclean remove build output and everything splat generated

VERSION    ?= us
BUILD      := build/$(VERSION)
TARGET     := disks/$(VERSION)/SLUS_008.84
CONFIG     := config/splat.$(VERSION).yaml
CROSS      ?= $(patsubst %as,%,$(firstword $(wildcard tools/binutils/bin/mips-linux-gnu-as)))$(if $(wildcard tools/binutils/bin/mips-linux-gnu-as),,mips-linux-gnu-)
AS         := $(CROSS)as
LD         := $(CROSS)ld
OBJCOPY    := $(CROSS)objcopy
PYTHON     ?= $(firstword $(wildcard .venv/bin/python) python3)
GCC_DIR    ?= tools/gcc-2.8.1-psx
CPP        := $(GCC_DIR)/cpp
CC1        := $(GCC_DIR)/cc1
MASPSX     := $(PYTHON) tools/maspsx/maspsx.py --aspsx-version=2.79

# PSY-Q 4.3's CCPSX (GCC 2.8.1); the game is built -G0 (no $gp-relative data).
CPPFLAGS   := -P -undef -Wall -lang-c -Iinclude -D_LANGUAGE_C -DUSE_INCLUDE_ASM -D__GNUC__=2
CC1FLAGS   := -quiet -O2 -G0 -mcpu=3000 -w -funsigned-char -fpeephole -ffunction-cse \
              -fpcc-struct-return -fcommon -msoft-float

ASFLAGS    := -EL -march=r3000 -mtune=r3000 -no-pad-sections -Iinclude -G0 -O1
LDFLAGS    := -EL --no-check-sections -nostdlib

ASM_SRCS   := $(shell find asm/$(VERSION) -name '*.s' -not -path '*/nonmatchings/*' -not -path '*/matchings/*' 2>/dev/null)
C_SRCS     := $(shell find src -name '*.c' 2>/dev/null)
OBJS       := $(patsubst %.s,$(BUILD)/%.s.o,$(ASM_SRCS)) $(patsubst %.c,$(BUILD)/%.c.o,$(C_SRCS))

.PHONY: all setup check clean distclean

all: check

setup:
	$(PYTHON) -m splat split $(CONFIG)
	$(PYTHON) tools/gte_words.py asm/$(VERSION)

$(BUILD)/%.s.o: %.s include/macro.inc
	@mkdir -p $(dir $@)
	$(AS) $(ASFLAGS) -o $@ $<

# cc1 must read a file: fed through a pipe it waits on stdin forever.
$(BUILD)/%.c.o: %.c include/macro.inc
	@mkdir -p $(dir $@)
	$(CPP) $(CPPFLAGS) $< $(BUILD)/$*.i
	$(CC1) $(CC1FLAGS) $(BUILD)/$*.i -o $(BUILD)/$*.cc1.s
	$(MASPSX) $(BUILD)/$*.cc1.s < /dev/null > $(BUILD)/$*.ms.s
	$(AS) $(ASFLAGS) -I. -o $@ $(BUILD)/$*.ms.s

$(BUILD)/tpm.elf: $(OBJS) $(BUILD)/tpm.ld
	$(LD) $(LDFLAGS) -T $(BUILD)/tpm.ld -T $(BUILD)/undefined_syms_auto.txt \
	  -T $(BUILD)/undefined_funcs_auto.txt -Map $(BUILD)/tpm.map -o $@

$(BUILD)/SLUS_008.84: $(BUILD)/tpm.elf
	$(OBJCOPY) -O binary $< $@

check: $(BUILD)/SLUS_008.84
	@if cmp -s $< $(TARGET); then echo "$<: OK (matches $(TARGET))"; \
	else echo "$<: MISMATCH"; cmp $< $(TARGET) | head -5; exit 1; fi

clean:
	rm -rf $(BUILD)/asm $(BUILD)/src $(BUILD)/tpm.elf $(BUILD)/tpm.map $(BUILD)/SLUS_008.84

distclean:
	rm -rf build asm
