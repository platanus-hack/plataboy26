# ===================================================================================
# Arduventure (TEAM a.r.g.) for the CH32V003 game console
# ===================================================================================
#   make         fetch the game (fetch.sh), build bin/arduventure.bin
#   make clean
#
# The game's own files are fetched from TEAM a.r.g.'s repository (fetch.sh); this
# directory holds only the port: port/ (Arduboy2, ATMlib, EEPROM, Arduino layer)
# and platform/ (this console's drivers).

ROOT     := $(abspath $(CURDIR)/../..)
XPACK    := $(wildcard $(ROOT)/tools/xpack-riscv-none-elf-gcc-*/bin)
PREFIX   ?= $(if $(XPACK),$(firstword $(XPACK))/,)riscv-none-elf-
CC       = $(PREFIX)gcc
CXX      = $(PREFIX)g++
OBJCOPY  = $(PREFIX)objcopy
F_CPU    = 48000000
FAR_EE   ?= 0x197E2           # region 0x197C2..0x1EFFF: top of bank 1, ending at the save area
ARCH     = -march=rv32ec_zicsr -mabi=ilp32e
LTO      ?= -flto
COMMON   = -Os -g $(LTO) -msave-restore -fno-jump-tables -fipa-pta -fno-tree-switch-conversion -fno-tree-tail-merge -ffunction-sections -fdata-sections -fno-builtin $(ARCH) \
           -DF_CPU=$(F_CPU) -DARDU_FAR_EEPROM=$(FAR_EE) -Iport -Iplatform -Wall -Wno-unused $(DEFS)
CXXFLAGS = $(COMMON) -fno-ipa-icf -std=gnu++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics \
           -fno-use-cxa-atexit -Wno-narrowing -Wno-parentheses -Wno-sequence-point
CFLAGS   = $(COMMON)
B        = build

PLAT_C   = $(wildcard platform/*.c)
UP       = upstream/ARDU_AB
KBASE    = 0x3C40
NM       = $(PREFIX)nm
OBJDUMP  = $(PREFIX)objdump
SONGLESS =

all: savecheck bin/arduventure.bin

# the save fits its EEPROM slot (tools/save_check.cpp): compile-time only
savecheck: $(B)/src2/ARDU_AB.ino tools/save_check.cpp
	@$(CXX) $(filter-out $(LTO),$(CXXFLAGS)) $(P2FLAGS) -Dmain=arduventure_main -x c++ -include port/Arduboy2.h -fsyntax-only tools/save_check.cpp

$(UP)/ARDU_AB.ino:
	@sh fetch.sh

# ---- pass 1: no LTO, one section per function -> which overlay group each goes to ----
$(B)/src/ARDU_AB.ino: $(UP)/ARDU_AB.ino $(wildcard $(UP)/*.h) tools/patch_upstream.py
	@python3 tools/patch_upstream.py $(UP) $(B)/src

$(B)/p1/game.o: $(B)/src/ARDU_AB.ino $(wildcard port/*.h) Makefile
	@mkdir -p $(B)/p1
	@$(CXX) $(CXXFLAGS) -fno-lto -Dmain=arduventure_main -x c++ -include port/Arduboy2.h -c -o $@ $<
$(B)/p1/platform_cpp.o: port/platform.cpp $(wildcard port/*.h) ../common/sysx.c ../common/sysx.h Makefile
	@mkdir -p $(B)/p1
	@$(CXX) $(CXXFLAGS) -fno-lto -c -o $@ $<
$(B)/p1/%.o: platform/%.c Makefile
	@mkdir -p $(B)/p1
	@$(CC) $(CFLAGS) -fno-lto -c -o $@ $<
P1OBJS = $(B)/p1/game.o $(B)/p1/platform_cpp.o $(patsubst platform/%.c,$(B)/p1/%.o,$(PLAT_C))

$(B)/assign.json: $(P1OBJS) tools/overlays.py
	@sed 's/LENGTH = 16K/LENGTH = 64K/' platform/ch32v003.ld > $(B)/p1.ld
	@$(CXX) $(CXXFLAGS) -fno-lto -nostdlib -o $(B)/p1.elf $(P1OBJS) -T $(B)/p1.ld -lgcc \
	  -Wl,--gc-sections,--build-id=none,--emit-relocs
	@python3 tools/overlays.py $(B)/p1.elf $(OBJDUMP) $(B)/src $@

# ---- pass 2: LTO, functions in their overlay sections -----------------------------------
$(B)/src2/ARDU_AB.ino: $(UP)/ARDU_AB.ino $(B)/assign.json tools/patch_upstream.py
	@python3 tools/patch_upstream.py $(UP) $(B)/src2 $(B)/assign.json

P2FLAGS = -DARDU_OVERLAYS
$(B)/p2/game.o: $(B)/src2/ARDU_AB.ino $(wildcard port/*.h) Makefile
	@mkdir -p $(B)/p2
	@$(CXX) $(CXXFLAGS) $(P2FLAGS) -Dmain=arduventure_main -x c++ -include port/Arduboy2.h -c -o $@ $<
$(B)/p2/platform_cpp.o: port/platform.cpp $(wildcard port/*.h) ../common/sysx.c ../common/sysx.h Makefile
	@mkdir -p $(B)/p2
	@$(CXX) $(CXXFLAGS) $(P2FLAGS) -c -o $@ $<
$(B)/p2/libc.o: platform/libc.c Makefile
	@mkdir -p $(B)/p2
	@$(CC) $(CFLAGS) -fno-lto -c -o $@ $<
$(B)/p2/%.o: platform/%.c Makefile
	@mkdir -p $(B)/p2
	@$(CC) $(CFLAGS) $(P2FLAGS) -c -o $@ $<
P2OBJS = $(B)/p2/game.o $(B)/p2/platform_cpp.o $(patsubst platform/%.c,$(B)/p2/%.o,$(PLAT_C))

# build ID for the overlay tags: from the sources of this build
OVL_TAG := 0x$(shell cat port/*.cpp port/*.h platform/*.c platform/game.ld tools/patch_upstream.py 2>/dev/null | cksum | cut -d' ' -f1 | xargs printf '%04x' | cut -c1-4)0000
LINK2 = $(CXX) $(CXXFLAGS) -nostdlib $(P2OBJS) -T platform/game.ld -lgcc \
        -Wl,--gc-sections,--build-id=none,--emit-relocs,--defsym=OVL_TAG=$(OVL_TAG)

bin/arduventure.elf: $(P2OBJS) platform/game.ld tools/ovlpack.py
	@mkdir -p bin
	@$(LINK2) -Wl,--defsym=KBASE=0x10000,--defsym=OVL_BASE=0xC000 -o $(B)/sizing.elf  # roomy, to measure
	@ovl=$$(python3 tools/ovlpack.py window $(B)/sizing.elf $(OBJDUMP)); \
	  res=$$($(PREFIX)size -A $(B)/sizing.elf | awk '$$1 ~ /^\.(init|text|fini|data|preinit_array|init_array|fini_array|ctors|dtors)$$/ {s+=$$2} END {print s}'); \
	  echo "resident $$res B + window $$ovl B = $$(( res + ovl )) B of $$(( $(KBASE) )) B"; \
	  $(LINK2) -Wl,--defsym=KBASE=$(KBASE),--defsym=OVL_BASE=$$(( $(KBASE) - $$ovl )) -o $@
	@python3 tools/ovlpack.py check $@ $(OBJDUMP)

bin/arduventure.bin: bin/arduventure.elf tools/textindex.py tools/ovlpack.py
	@$(OBJCOPY) -O binary -R .far -R .ov1 -R .ov2 -R .ov3 -R .ov4 $< $@
	@$(OBJCOPY) -O binary -j .far $< bin/arduventure.far
	@python3 tools/textindex.py $< bin/arduventure.far $(NM)
	@python3 tools/ovlpack.py image $< $(OBJDUMP) bin/arduventure.far $(FAR_EE) bin/arduventure.eeprom

clean:
	rm -rf $(B) bin

# development build: code linked for 64 KiB (sim built with -DFLASH_SIZE=65536), far data
# in an EEPROM image at FAR_EE
$(B)/dev/game.o: $(B)/src/ARDU_AB.ino $(wildcard port/*.h) Makefile
	@mkdir -p $(B)/dev
	@$(CXX) $(CXXFLAGS) -Dmain=arduventure_main -x c++ -include port/Arduboy2.h -c -o $@ $<
$(B)/dev/platform_cpp.o: port/platform.cpp $(wildcard port/*.h) ../common/sysx.c ../common/sysx.h Makefile
	@mkdir -p $(B)/dev
	@$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/dev/libc.o: platform/libc.c Makefile
	@mkdir -p $(B)/dev
	@$(CC) $(CFLAGS) -fno-lto -c -o $@ $<
$(B)/dev/%.o: platform/%.c Makefile
	@mkdir -p $(B)/dev
	@$(CC) $(CFLAGS) -c -o $@ $<
DEVOBJS = $(B)/dev/game.o $(B)/dev/platform_cpp.o $(patsubst platform/%.c,$(B)/dev/%.o,$(PLAT_C))

# development build: everything resident, code linked for 64 KiB (sim built with
# -DFLASH_SIZE=65536), far data in an EEPROM image at FAR_EE
dev: $(DEVOBJS)
	@mkdir -p bin
	@sed 's/LENGTH = 16K/LENGTH = 64K/; $(if $(BIGRAM),s/LENGTH = 2K/LENGTH = 4K/)' platform/ch32v003.ld > $(B)/dev.ld
	@$(CXX) $(CXXFLAGS) -nostdlib -o bin/dev.elf $(DEVOBJS) -T $(B)/dev.ld -lgcc -Wl,--gc-sections,--build-id=none,--emit-relocs
	@$(OBJCOPY) -O binary -R .far bin/dev.elf bin/dev.bin
	@$(OBJCOPY) -O binary -j .far bin/dev.elf bin/dev.far
	@python3 tools/textindex.py bin/dev.elf bin/dev.far $(NM)
	@python3 tools/eeimage.py bin/dev.far $(FAR_EE) bin/dev.eeprom
	@$(PREFIX)size -A -d bin/dev.elf | grep -E "^\.(text|far|data|bss)"

.PHONY: all clean dev savecheck
