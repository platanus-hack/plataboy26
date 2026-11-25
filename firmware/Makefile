# ===================================================================================
# plataboy26 game library firmware
# ===================================================================================
#   make            kernel + menu + EEPROM image + flash image  (build/)
#   make test       end-to-end tests in the simulator (tools/test.py)
#   make full       the full library for rev. 1.3/1.3.1 (library.full.json) -> build-full/
#                   (PH26 edition: PH-arrow-26 boot animation)
#   make platanus   the same library, Platanus edition (full Platanus logo) -> build-platanus/
#   make pick       choose the games in the terminal, with the EEPROM use shown -> build-custom/
#   make dist       a folder + zip with a built library and the flash scripts (B=build-full ...)
#   make flash      flash boards from a built library (B=build-full ...): tools/flash.sh
#   make sim-roms   copy the firmware into the web simulator's ROM list (plataboy26)
#   make clean
#
# Outputs:
#   build/flash.bin    16 KiB: menu installed + kernel + config  -> MCU (WCH-LinkE)
#   build/eeprom.bin   128 KiB: the compressed library           -> AT24CM01
#   build/library.txt  what is in the library and how much space is left
#   build/writer/      writer_NN.bin: the EEPROM contents in chunks, for programming
#                      boards with a WCH-LinkE only (tools/flash.sh, flash.bat)
# ===================================================================================

# kernel start = the largest program size: 0x3FC0 (config page) - 14 pages of kernel
KBASE    = 0x3C40
ROOT     := $(abspath $(CURDIR)/..)
XPACK    := $(wildcard $(ROOT)/tools/xpack-riscv-none-elf-gcc-*/bin)
PREFIX   ?= $(if $(XPACK),$(firstword $(XPACK))/,)riscv-none-elf-
CC       = $(PREFIX)gcc
OBJCOPY  = $(PREFIX)objcopy
ARCH     = -march=rv32ec_zicsr -mabi=ilp32e
B        = build
SIM      = $(ROOT)/sim

MENU_SRC = $(wildcard menu/src/*.c)
LIB_SRC  = $(filter-out menu/src/main.c,$(MENU_SRC))
MENU_H   = $(wildcard menu/src/*.h)

all: $(B)/flash.bin

$(B)/kernel.elf: kernel/kernel.S kernel/kernel.ld Makefile
	@mkdir -p $(B)
	@$(CC) $(ARCH) -nostdlib -Wl,--defsym=KBASE=$(KBASE) -T kernel/kernel.ld -Wl,--build-id=none -o $@ $<

$(B)/menu.elf: $(MENU_SRC) $(MENU_H) $(ROOT)/games/common/sysx.h $(ROOT)/games/common/joycal.h menu/ld/ch32v003.ld Makefile
	@mkdir -p $(B)
	@$(CC) -g -Os -flto -ffunction-sections -fdata-sections -fno-builtin -nostdlib $(ARCH) \
	  -DF_CPU=24000000 -DKBASE=$(KBASE) -Imenu/src -I$(ROOT)/games/common -Wall -o $@ $(MENU_SRC) \
	  -T menu/ld/ch32v003.ld -lgcc -Wl,--gc-sections,--build-id=none

# the Platanus edition's menu: the boot animation shows the full Platanus logo
# (art/boot/platanus.py -> menu/src/bootanim_platanus.h) instead of PH-arrow-26
$(B)/menu_platanus.elf: $(MENU_SRC) $(MENU_H) $(ROOT)/games/common/sysx.h $(ROOT)/games/common/joycal.h menu/ld/ch32v003.ld Makefile
	@mkdir -p $(B)
	@$(CC) -g -Os -flto -ffunction-sections -fdata-sections -fno-builtin -nostdlib $(ARCH) \
	  -DF_CPU=24000000 -DKBASE=$(KBASE) -DEDITION_PLATANUS -Imenu/src -I$(ROOT)/games/common -Wall -o $@ $(MENU_SRC) \
	  -T menu/ld/ch32v003.ld -lgcc -Wl,--gc-sections,--build-id=none

$(B)/writer.elf: writer/main.c $(LIB_SRC) $(MENU_H) $(ROOT)/games/common/joycal.h menu/ld/ch32v003.ld Makefile
	@mkdir -p $(B)
	@$(CC) -g -Os -flto -ffunction-sections -fdata-sections -fno-builtin -nostdlib $(ARCH) \
	  -DF_CPU=24000000 -Imenu/src -I$(ROOT)/games/common -Wall -o $@ writer/main.c $(LIB_SRC) \
	  -T menu/ld/ch32v003.ld -lgcc -Wl,--gc-sections,--build-id=none

$(B)/%.bin: $(B)/%.elf
	@$(OBJCOPY) -O binary $< $@

CARDS = $(wildcard $(SIM)/web/roms/*.png $(SIM)/web/roms/*.json)   # menu cards: the thumbnails
$(B)/flash.bin: $(B)/kernel.bin $(B)/menu.bin $(B)/writer.bin library.json tools/pack.py tools/lz.py \
  $(CARDS)
	@python3 tools/pack.py --kbase $(KBASE) --kernel $(B)/kernel.bin --menu $(B)/menu.bin \
	  --writer $(B)/writer.bin --library library.json --out $(B)

test: all $(SIM)/chsim
	@python3 tools/test.py

# a library with locally built games that may not be shared (library.local.json, not in
# the repository): build-local/, and the web simulator's local entry "plataboy26-local"
local: $(B)/kernel.bin $(B)/menu.bin $(B)/writer.bin library.local.json tools/pack.py tools/lz.py \
  $(CARDS)
	@python3 tools/pack.py --kbase $(KBASE) --kernel $(B)/kernel.bin --menu $(B)/menu.bin \
	  --writer $(B)/writer.bin --library library.local.json --out build-local
	@mkdir -p $(SIM)/web/roms/local
	@cp build-local/flash.bin $(SIM)/web/roms/local/plataboy26.bin
	@cp build-local/eeprom.bin $(SIM)/web/roms/local/plataboy26.eeprom

# the Platanus Hack 26 library (library.hack26.json, not in the repository): the full one
# with Llegamos tarde in place of Tiny Bubble and Tiny Lander -> build-hack26/, and the web
# simulator's local entry "plataboy26-hack26"
hack26: $(B)/kernel.bin $(B)/menu.bin $(B)/writer.bin library.hack26.json tools/pack.py tools/lz.py \
  $(CARDS)
	@python3 tools/pack.py --kbase $(KBASE) --kernel $(B)/kernel.bin --menu $(B)/menu.bin \
	  --writer $(B)/writer.bin --library library.hack26.json --out build-hack26
	@mkdir -p $(SIM)/web/roms/local
	@cp build-hack26/flash.bin $(SIM)/web/roms/local/plataboy26-hack26.bin
	@cp build-hack26/eeprom.bin $(SIM)/web/roms/local/plataboy26-hack26.eeprom

# the library with MicroCity (library.microcity.json): build-microcity/, and the web
# simulator's entry "plataboy26-microcity"
microcity: $(B)/kernel.bin $(B)/menu.bin $(B)/writer.bin library.microcity.json tools/pack.py tools/lz.py \
  $(CARDS) $(SIM)/web/roms/microcity.region
	@python3 tools/pack.py --kbase $(KBASE) --kernel $(B)/kernel.bin --menu $(B)/menu.bin \
	  --writer $(B)/writer.bin --library library.microcity.json --out build-microcity
	@cp build-microcity/flash.bin $(SIM)/web/roms/plataboy26-microcity.bin
	@cp build-microcity/eeprom.bin $(SIM)/web/roms/plataboy26-microcity.eeprom

# the full library for board rev. 1.3 / 1.3.1 (library.full.json): every port plus as many
# Tiny games as fit, all with the UP + DOWN exit and the low-battery beep (games/common/sysx.c).
# Rebuild the games first: make -C ../games/tiny, and make in each port's directory.
# -> build-full/ and the web simulator's local entry "plataboy26-full"
full: $(B)/kernel.bin $(B)/menu.bin $(B)/writer.bin library.full.json tools/pack.py tools/lz.py $(CARDS)
	@python3 tools/pack.py --kbase $(KBASE) --kernel $(B)/kernel.bin --menu $(B)/menu.bin \
	  --writer $(B)/writer.bin --library library.full.json --out build-full
	@mkdir -p $(SIM)/web/roms/local
	@cp build-full/flash.bin $(SIM)/web/roms/local/plataboy26-full.bin
	@cp build-full/eeprom.bin $(SIM)/web/roms/local/plataboy26-full.eeprom

# the same library, Platanus edition (full Platanus logo at power-on) -> build-platanus/
# and the web simulator's local entry "plataboy26-platanus"
platanus: $(B)/kernel.bin $(B)/menu_platanus.bin $(B)/writer.bin library.full.json tools/pack.py tools/lz.py $(CARDS)
	@python3 tools/pack.py --kbase $(KBASE) --kernel $(B)/kernel.bin --menu $(B)/menu_platanus.bin \
	  --writer $(B)/writer.bin --library library.full.json --out build-platanus
	@mkdir -p $(SIM)/web/roms/local
	@cp build-platanus/flash.bin $(SIM)/web/roms/local/plataboy26-platanus.bin
	@cp build-platanus/eeprom.bin $(SIM)/web/roms/local/plataboy26-platanus.eeprom

# choose the games in the terminal, with the EEPROM use shown (tools/pick.py, catalog.json):
# writes library.custom.json when the selection fits, then builds it -> build-custom/
#   make pick                              interactive
#   make pick SELECT="Hopper,MicroCity"    no terminal UI
#   make custom                            build library.custom.json again
pick: $(B)/kernel.bin $(B)/menu.bin $(B)/writer.bin
	@if python3 tools/pick.py --kbase $(KBASE) --menu $(B)/menu.bin --out library.custom.json \
	  $(if $(SELECT),--select "$(SELECT)"); then $(MAKE) -s custom; fi

custom: $(B)/kernel.bin $(B)/menu.bin $(B)/writer.bin library.custom.json tools/pack.py tools/lz.py $(CARDS)
	@python3 tools/pack.py --kbase $(KBASE) --kernel $(B)/kernel.bin --menu $(B)/menu.bin \
	  --writer $(B)/writer.bin --library library.custom.json --out build-custom
	@echo "build-custom/: make flash B=build-custom   |   ../sim/chsim build-custom/flash.bin -e build-custom/eeprom.bin"

# Flashing is a separate step from building: it needs only the built files and a flashing
# tool (tools/flash.sh on Linux / macOS, tools/flash.bat on Windows).
#   make flash [B=build-full]    flash boards from a build directory
#   make dist  [B=build-full]    dist/plataboy26-<name>/ and .zip: the built library with
#                                flash.sh, flash.bat and FLASHING.txt, to flash on any computer
DIST = dist/plataboy26-$(if $(patsubst build-%,%,$(B:build=default)),$(patsubst build-%,%,$(B:build=default)),default)
flash:
	@sh tools/flash.sh $(B)

dist:
	@test -f $(B)/flash.bin || { echo "$(B)/flash.bin is missing: build the library first"; exit 1; }
	@rm -rf $(DIST) $(DIST).zip && mkdir -p $(DIST)/writer
	@cp $(B)/flash.bin $(B)/eeprom.bin $(B)/library.txt tools/flash.sh tools/flash.bat $(DIST)/
	@cp tools/FLASHING.txt $(DIST)/README.txt
	@cp $(B)/writer/writer_*.bin $(DIST)/writer/
	@cd dist && zip -qr $(notdir $(DIST)).zip $(notdir $(DIST))
	@echo "$(DIST)/ and $(DIST).zip"

sim-roms: all
	@cp $(B)/flash.bin $(SIM)/web/roms/plataboy26.bin
	@cp $(B)/eeprom.bin $(SIM)/web/roms/plataboy26.eeprom
	@echo "web simulator: entry plataboy26 updated"

$(SIM)/chsim:
	@$(MAKE) -C $(SIM) chsim

clean:
	rm -rf $(B)

.PHONY: hack26 all test clean sim-roms local microcity full platanus pick custom flash dist
