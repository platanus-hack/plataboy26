# ===================================================================================
# MicroCity (James Howard) for the CH32V003 game console
# ===================================================================================
#   make         fetch the game (fetch.sh), build bin/microcity.bin (flash) and
#                bin/microcity.eeprom (the game's EEPROM region: terrains, code overlays)
#   make test    play a scripted game in the simulator and compare every frame with the
#                original game's (tools/reftest.py)
#   make clean
#
# The game's own files are fetched from James Howard's repository and patched
# (fetch.sh, microcity.patch); this directory holds only the port: port/ (the platform
# functions MicroCity draws, reads buttons and saves through) and platform/ (this
# console's drivers).

ROOT     := $(abspath $(CURDIR)/../..)
XPACK    := $(wildcard $(ROOT)/tools/xpack-riscv-none-elf-gcc-*/bin)
PREFIX   ?= $(if $(XPACK),$(firstword $(XPACK))/,)riscv-none-elf-
CC       = $(PREFIX)gcc
CXX      = $(PREFIX)g++
OBJCOPY  = $(PREFIX)objcopy
SIZE     = $(PREFIX)size
F_CPU    = 48000000
# EEPROM address of the game's data (EEDATA section); its region header is 32 bytes before
EE_DATA  ?= 0x179C0
ARCH     = -march=rv32ec_zicsr -mabi=ilp32e
COMMON   = -Os -g -flto -msave-restore -msmall-data-limit=0 -ffunction-sections -fdata-sections \
           -fno-builtin -fno-jump-tables -fno-tree-switch-conversion $(ARCH) -DF_CPU=$(F_CPU) -DMC_EE_DATA=$(EE_DATA) $(EXTRA) \
           -Iport -Iplatform -I$(UP) -I$(B) -Wall -Wno-unused
CXXFLAGS = $(COMMON) -std=gnu++17 -fno-exceptions -fno-rtti -fno-threadsafe-statics \
           -fno-use-cxa-atexit -Wno-sign-compare
CFLAGS   = $(COMMON)
B        ?= build
OUT      ?= bin/microcity
UP       = upstream/Source/MicroCity

GAME     = Building Connectivity Draw Font Game Interface Simulation Strings Terrain
PLAT_C   = $(wildcard platform/*.c)
OBJS     = $(GAME:%=$(B)/%.o) $(B)/platform_cpp.o $(PLAT_C:platform/%.c=$(B)/%.o) $(B)/kunpack.o
# the overlays are LZ streams, unpacked by the library kernel (games/common/kunpack.S)

all: savecheck $(OUT).bin

# a saved city fits its EEPROM slot (tools/save_check.cpp): compile-time only
savecheck: tools/save_check.cpp
	@$(CXX) $(filter-out -flto,$(CXXFLAGS)) -fsyntax-only tools/save_check.cpp

upstream/.patched: fetch.sh microcity.patch
	@sh fetch.sh
	@touch $@
$(UP)/%.cpp $(UP)/%.h: upstream/.patched ;
.PRECIOUS: $(UP)/%.cpp $(UP)/%.h

# upstream's CalculatePowerConnectivity is wrapped by the port's (port/platform.cpp)
$(B)/Connectivity.o: DEFS = -DCalculatePowerConnectivity=CalculatePowerConnectivity_body
$(B)/TileSet.h: upstream/.patched tools/tiles.py
	@mkdir -p $(B)
	@python3 tools/tiles.py $(UP)/TileData.h $@
$(B)/Draw.o: $(B)/TileSet.h
$(B)/%.o: $(UP)/%.cpp upstream/.patched $(wildcard port/*.h port/avr/*.h) Makefile
	@mkdir -p $(B)
	@$(CXX) $(CXXFLAGS) $(DEFS) -c -o $@ $<
$(B)/platform_cpp.o: port/platform.cpp upstream/.patched ../common/sysx.c ../common/sysx.h Makefile
	@mkdir -p $(B)
	@$(CXX) $(CXXFLAGS) -c -o $@ $<
$(B)/libc.o: platform/libc.c Makefile
	@mkdir -p $(B)
	@$(CC) $(CFLAGS) -fno-lto -c -o $@ $<
$(B)/kunpack.o: ../common/kunpack.S
	@mkdir -p $(B)
	@$(CC) $(ARCH) -c -o $@ $<
$(B)/%.o: platform/%.c Makefile
	@mkdir -p $(B)
	@$(CC) $(CFLAGS) -c -o $@ $<

# ---- link: vectors, the overlay window, resident code, below the kernel (KBASE) ---------
KBASE    ?= 0x3C40
OBJDUMP  = $(PREFIX)objdump
# build ID for the overlay tags: from the sources of this build
OVL_TAG := 0x$(shell cat port/*.cpp port/avr/*.h platform/*.c platform/game.ld microcity.patch 2>/dev/null | cksum | cut -d' ' -f1 | xargs printf '%04x' | cut -c1-4)0000
LINK     = $(CXX) $(CXXFLAGS) -nostdlib $(OBJS) -T platform/game.ld -lgcc \
           -Wl,--gc-sections,--build-id=none,--no-warn-rwx-segments,--emit-relocs,--defsym=OVL_TAG=$(OVL_TAG)
$(B)/at24.o: CFLAGS += -D'EE_WRITE_ATTR=__attribute__((section(".ov3.text"),noinline))'

$(OUT).elf: $(OBJS) platform/game.ld tools/ovl.py tools/stack.py
	@mkdir -p $(dir $(OUT))
	@$(LINK) -Wl,--defsym=KBASE=$(KBASE) -o $@
	@python3 tools/ovl.py check $@ $(OBJDUMP)
	@python3 tools/stack.py $@ $(OBJDUMP)
	@$(SIZE) -A $@ | awk '$$1 ~ /^\.(data|bss)$$/ {r+=$$2} END {printf "SRAM: %d / 2048 bytes (static, stack not included)\n", r}'

$(OUT).bin: $(OUT).elf
	@$(OBJCOPY) -O binary -R .eedata -R .ov1 -R .ov2 -R .ov3 $< $@
	@echo "FLASH: $$(wc -c < $@) / $$(( $(KBASE) )) bytes (vectors, overlay window, resident code)"
	@$(OBJCOPY) -O binary -j .eedata $< $(B)/eedata.bin
	@python3 tools/ovl.py image $< $(OBJDUMP) $(B)/eedata.bin $(EE_DATA) $(OUT)

test:
	@python3 tools/reftest.py

clean:
	rm -rf $(B) bin

.PHONY: all test clean savecheck
