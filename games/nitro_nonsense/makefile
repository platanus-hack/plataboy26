# ===================================================================================
# Game Makefile for the CH32V003 Mini Game Console
# ===================================================================================
#   make            build bin/$(TARGET).elf/.bin/.hex
#   make sim        build, then start the simulator with live reload
#   make test       build, run 5 s headless in the simulator, save a screenshot
#   make check      build, then check the limits of the console and the game library
#                   (tools/check_game.py: size, RAM/stack, CPU faults, EEPROM/flash writes, install)
#   make flash      upload with a WCH-LinkE (needs: pip install rvprog)
#   make clean
# ===================================================================================

TARGET   = $(notdir $(CURDIR))
SOURCE   = src
BIN      = bin
F_CPU    = 48000000
LDSCRIPT = ld/overlay.ld
OVL_BASE ?= 0x2B80
DATA_BASE ?= 0x00800
CPUARCH  = -march=rv32ec_zicsr -mabi=ilp32e

# Toolchain: the xPack GCC inside this repo, else riscv-none-elf-gcc from PATH
ROOT     := $(abspath $(CURDIR)/../..)
XPACK    := $(wildcard $(ROOT)/tools/xpack-riscv-none-elf-gcc-*/bin)
PREFIX   ?= $(if $(XPACK),$(firstword $(XPACK))/,)riscv-none-elf-
CC       = $(PREFIX)gcc
OBJCOPY  = $(PREFIX)objcopy
OBJDUMP  = $(PREFIX)objdump
OBJSIZE  = $(PREFIX)size
SIM      = $(ROOT)/sim
PORT     ?= 8100

CFLAGS   = -g -Os -flto -malign-data=natural -fno-tree-pre -ffunction-sections -fdata-sections -fno-builtin -nostdlib
CFLAGS  += $(CPUARCH) -DF_CPU=$(F_CPU) -I$(SOURCE) -Wall -Wextra -DNN_MINIMAL=1 -DNN_OVERLAYS=1 -mstrict-align -msave-restore
LDFLAGS  = -T$(LDSCRIPT) -lgcc -Wl,--gc-sections,--build-id=none,--defsym=OVL_BASE=$(OVL_BASE)
CFILES   = $(wildcard $(SOURCE)/*.c) $(wildcard $(SOURCE)/*.S)
# plataboy26 library: UP + DOWN -> menu (games/common/sysx.c). No low-battery beep: its
# ~0.2 s busy wait would stall a linked race (the menu's Settings shows the battery)
CFILES  += ../common/sysx.c ../common/kunpack.S
CFLAGS  += -I../common -DSYSX_NO_BATTERY
HFILES   = $(wildcard $(SOURCE)/*.h) $(wildcard $(SOURCE)/*.inc)

all: $(BIN)/$(TARGET).bin $(BIN)/$(TARGET).hex size

$(BIN)/$(TARGET).link.elf: $(CFILES) $(HFILES) $(LDSCRIPT) makefile
	@mkdir -p $(BIN)
	@$(CC) -o $@ $(CFILES) $(CFLAGS) $(LDFLAGS)

$(BIN)/$(TARGET).elf: $(BIN)/$(TARGET).link.elf tools/overlay_pack.py
	@python3 tools/overlay_pack.py --elf $< --resident-elf $@ --output $(BIN)/$(TARGET).bin --region $(BIN)/$(TARGET).region --policy $(BIN)/$(TARGET).policy.json --eeprom-base $(DATA_BASE) --objcopy $(OBJCOPY) --identity-input makefile

$(BIN)/$(TARGET).bin $(BIN)/$(TARGET).region $(BIN)/$(TARGET).policy.json: $(BIN)/$(TARGET).elf
	@test -f $@

$(BIN)/$(TARGET).hex: $(BIN)/$(TARGET).elf
	@$(OBJCOPY) -O ihex $< $@

asm: $(BIN)/$(TARGET).elf
	@$(OBJDUMP) -d -S $< > $(BIN)/$(TARGET).lst
	@echo "Disassembly in $(BIN)/$(TARGET).lst"

size: $(BIN)/$(TARGET).elf
	@python3 -c 'import json; p=json.load(open("$(BIN)/$(TARGET).policy.json")); print("RESIDENT: %d / %d bytes; WINDOW: %d bytes; RESIDENT HEADROOM: %d bytes" % (p["residentLoadBytes"],p["residentCeiling"],p["window"]["size"],p["residentHeadroomBytes"]))'

# Each preview freezes a validated image/resource pair; restart after a rebuild.
sim: all $(SIM)/chsim
	@node tools/preview_server.mjs --port $(PORT)

test: all $(SIM)/chsim
	@python3 -c 'import json,pathlib; p=json.load(open("$(BIN)/$(TARGET).policy.json")); r=pathlib.Path("$(BIN)/$(TARGET).region").read_bytes(); e=bytearray([255])*131072; a=p["sourceRegion"]["address"]; e[a:a+len(r)]=r; pathlib.Path("$(BIN)/test.eeprom").write_bytes(e)'
	@$(SIM)/chsim $(BIN)/$(TARGET).bin -e $(BIN)/test.eeprom -t 6000 -p act@2000-4000 -p left@4200-4400 -p right@4600-4800 -s 5999:$(BIN)/screen.pgm
	@python3 -I $(SIM)/tools/pgm2png.py $(BIN)/screen.pgm && rm $(BIN)/screen.pgm && echo "Screenshot: $(BIN)/screen.png"

$(SIM)/chsim: $(SIM)/core/emu.c $(SIM)/core/cli.c $(SIM)/core/emu.h
	@$(MAKE) -s -C $(SIM) chsim

check: all $(SIM)/chsim
	@python3 $(ROOT)/tools/check_game.py $(BIN)/$(TARGET).elf --overlay-policy $(BIN)/$(TARGET).policy.json --data $(BIN)/$(TARGET).region@$(DATA_BASE) $(CHECK_ARGS)

flash: all
	@echo "Hardware deployment requires a verified additive resource plan; see docs/RESOURCE_PLAN.md."
	@echo "No hardware writer is provided while the existing directory publication is not recoverable."
	@false

clean:
	@rm -rf $(BIN)

verify: all
	@python3 tests/verify.py

.PHONY: all asm size sim test check flash clean verify
