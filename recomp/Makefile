# Desktop build of the recompiled game (headless; see host/main.c).
#   make -C recomp            -> build/recomp/tpm-host
#   build/recomp/tpm-host disks/us/SLUS_008.84 <disc.bin> --dump out/
EXE   ?= ../disks/us/SLUS_008.84
DISC  ?=
BUILD ?= ../build/recomp
PSYZ  := psyz/psyz
CC    ?= cc
OPT   ?= -O2
DEBUG ?= 1
CFLAGS := $(OPT) -g -Iruntime -I$(BUILD)/gen -I$(PSYZ)/include -I$(PSYZ)/src \
          -Ipsyz/decomp/src/libspu \
          -Wno-unused-label -Wno-unused-variable -Wno-unused-but-set-variable \
          -Wno-parentheses-equality -Wno-tautological-compare \
          $(if $(filter 1,$(DEBUG)),-DRT_DEBUG)

NGEN := 16
GEN := $(foreach n,$(shell seq -w 0 $$(($(NGEN)-1))),$(BUILD)/gen/recomp_$(n).c) $(BUILD)/gen/recomp_table.c
RT  := $(wildcard runtime/*.c) $(PSYZ)/src/psyz/libgte.c $(PSYZ)/src/psyz/psyz_spu.c host/main.c
OBJ := $(patsubst %.c,$(BUILD)/obj/%.o,$(notdir $(GEN) $(RT)))

all: $(BUILD)/tpm-host

SITES ?= ../config/scratch_sites.us.txt
$(BUILD)/gen/.stamp: $(EXE) ../tools/recomp.py ../config/recomp_entries.us.txt hle.us.txt $(SITES)
	python3 ../tools/recomp.py $(EXE) $(BUILD)/gen --files $(NGEN) $(if $(DISC),--disc "$(DISC)") \
	    $(if $(SITES),--scratch-sites $(SITES)) $(RECOMP_FLAGS)
	touch $@
$(GEN): $(BUILD)/gen/.stamp
	@true

$(BUILD)/obj/recomp_%.o: $(BUILD)/gen/recomp_%.c $(wildcard runtime/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

vpath %.c runtime $(PSYZ)/src/psyz host
$(BUILD)/obj/%.o: %.c $(BUILD)/gen/.stamp $(wildcard runtime/*.h) $(PSYZ)/src/platform/soft_raster.inc.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/tpm-host: $(OBJ)
	$(CC) -o $@ $^

clean:
	rm -rf $(BUILD)
.PHONY: all clean
