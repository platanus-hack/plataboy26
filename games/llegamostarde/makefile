# ===================================================================================
# Llegamos tarde a la hackathon (port) - CH32V003 game console
# ===================================================================================
#   make            build bin/$(TARGET).elf/.bin/.hex
#   make sim        build, then start the simulator with live reload
#   make test       build, run 5 s headless in the simulator, save a screenshot
#   make flash      upload with a WCH-LinkE (needs: pip install rvprog)
#   make clean
# ===================================================================================

TARGET   = $(notdir $(CURDIR))
SOURCE   = src
BIN      = bin
F_CPU    = 48000000
LDSCRIPT = ld/ch32v003.ld
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
PORT     ?= 8088

CFLAGS   = -g -Os -flto -ffunction-sections -fdata-sections -fno-builtin -nostdlib -msave-restore
CFLAGS  += $(CPUARCH) -DF_CPU=$(F_CPU) -I$(SOURCE) -I../common -DSYSX_SELFCAL -Wall
LDFLAGS  = -T$(LDSCRIPT) -lgcc -Wl,--gc-sections,--build-id=none
CFILES   = $(wildcard $(SOURCE)/*.c) $(wildcard $(SOURCE)/*.S) ../common/sysx.c
HFILES   = $(wildcard $(SOURCE)/*.h) ../common/sysx.h ../common/joycal.h

# the sprites and texts: python3 tools/gen_data.py upstream/game.js src/game_data.h (after ./fetch.sh)

all: $(BIN)/$(TARGET).bin $(BIN)/$(TARGET).hex size

$(BIN)/$(TARGET).elf: $(CFILES) $(HFILES) $(LDSCRIPT) makefile
	@mkdir -p $(BIN)
	@$(CC) -o $@ $(CFILES) $(CFLAGS) $(LDFLAGS)

$(BIN)/$(TARGET).bin: $(BIN)/$(TARGET).elf
	@$(OBJCOPY) -O binary $< $@

$(BIN)/$(TARGET).hex: $(BIN)/$(TARGET).elf
	@$(OBJCOPY) -O ihex $< $@

asm: $(BIN)/$(TARGET).elf
	@$(OBJDUMP) -d -S $< > $(BIN)/$(TARGET).lst
	@echo "Disassembly in $(BIN)/$(TARGET).lst"

size: $(BIN)/$(TARGET).elf
	@$(OBJSIZE) -d $< | awk '/[0-9]/ {printf "FLASH: %5d / 16384 bytes   SRAM: %4d / 2048 bytes (static, stack not included)\n", $$1+$$2, $$2+$$3}'

# Simulator: rebuilds of the .elf are picked up by the open browser tab automatically
sim: all $(SIM)/chsim
	@echo "Open http://localhost:$(PORT)/ - run 'make' in another terminal to reload"
	@node $(SIM)/server.mjs --port $(PORT) --watch $(BIN)/$(TARGET).elf --name "$(TARGET)"

test: all $(SIM)/chsim
	@$(SIM)/chsim $(BIN)/$(TARGET).bin -t 5000 -p act@1000-1100 -p left@2000-2600 -p right@3000-3800 -s 4999:$(BIN)/screen.pgm
	@python3 -I $(SIM)/tools/pgm2png.py $(BIN)/screen.pgm && rm $(BIN)/screen.pgm && echo "Screenshot: $(BIN)/screen.png"

$(SIM)/chsim:
	@$(MAKE) -s -C $(SIM) chsim

flash: all
	rvprog -f $(BIN)/$(TARGET).bin

clean:
	@rm -rf $(BIN)

.PHONY: all asm size sim test flash clean
