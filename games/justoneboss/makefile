# ===================================================================================
# Just One Boss (ayla~nonsense, PICO-8) for the CH32V003 game console
# ===================================================================================
#   make            build bin/justoneboss.bin (src/sprites.h and src/sfx.h from tools/)
#   make sim        build, then start the simulator with live reload
#   make test       build, run headless in the simulator, save screenshots
#   make clean
#
# src/logic.py is the game logic (compiled by tools/jbc.py to bytecode: src/bytecode.h
# in flash, bin/cold.bin for the EEPROM); tools/sprites.txt holds the 1-bit sprites
# (converted from the cart by tools/extract.py, then edited by hand); bin/sfx.bin is
# made from the cart's sound effects, which fetch.sh downloads (the cart is not kept in
# this repository). bin/justoneboss.region goes to the EEPROM at JOB_COLD.
# ===================================================================================

TARGET   = justoneboss
SOURCE   = src
BIN      = bin
F_CPU    = 48000000
LDSCRIPT = ld/ch32v003.ld
CPUARCH  = -march=rv32ec_zicsr -mabi=ilp32e

ROOT     := $(abspath $(CURDIR)/../..)
XPACK    := $(wildcard $(ROOT)/tools/xpack-riscv-none-elf-gcc-*/bin)
PREFIX   ?= $(if $(XPACK),$(firstword $(XPACK))/,)riscv-none-elf-
CC       = $(PREFIX)gcc
OBJCOPY  = $(PREFIX)objcopy
OBJDUMP  = $(PREFIX)objdump
OBJSIZE  = $(PREFIX)size
SIM      = $(ROOT)/sim
PORT     ?= 8088
CART     = upstream/just_one_boss.p8
JOB_COLD ?= 0xE4C0

CFLAGS   = -g -Os -flto -ffunction-sections -fdata-sections -fno-builtin -nostdlib -msave-restore -fno-inline-functions-called-once
CFLAGS  += $(EXTRA) $(CPUARCH) -DF_CPU=$(F_CPU) -DJOB_COLD=$(JOB_COLD) -I$(SOURCE) -I../common -DSYSX_NO_BATTERY -Wall
LDFLAGS  = -T$(LDSCRIPT) -lgcc -Wl,--gc-sections,--build-id=none,--no-warn-rwx-segments
CFILES   = $(filter-out $(SOURCE)/libc.c,$(wildcard $(SOURCE)/*.c)) ../common/sysx.c
HFILES   = $(wildcard $(SOURCE)/*.h) $(SOURCE)/sprites.h $(SOURCE)/bytecode.h

all: $(BIN)/$(TARGET).bin $(BIN)/$(TARGET).eeprom size

$(CART): fetch.sh
	@sh fetch.sh

$(SOURCE)/sprites.h: tools/sprites.txt tools/sprites.py
	@python3 tools/sprites.py tools/sprites.txt $@

$(BIN)/sfx.bin: tools/sfx.py | $(CART)
	@mkdir -p $(BIN)
	@python3 tools/sfx.py $(CART) $@

$(SOURCE)/bytecode.h $(BIN)/cold.bin: $(SOURCE)/logic.py tools/jbc.py $(SOURCE)/sprites.h
	@mkdir -p $(BIN)
	@python3 tools/jbc.py $(SOURCE)/logic.py $(SOURCE)/bytecode.h $(BIN)/cold.bin $(SOURCE)/sprites.h

# memset/memcpy the compiler emits calls to: outside LTO, or they are dropped
$(BIN)/libc.o: $(SOURCE)/libc.c makefile
	@mkdir -p $(BIN)
	@$(CC) -c -o $@ $< $(CFLAGS) -fno-lto

$(BIN)/$(TARGET).elf: $(CFILES) $(HFILES) ../common/sysx.h ../common/joycal.h $(BIN)/libc.o $(LDSCRIPT) makefile
	@mkdir -p $(BIN)
	@$(CC) -o $@ $(CFILES) $(BIN)/libc.o $(CFLAGS) $(LDFLAGS)

$(BIN)/$(TARGET).bin: $(BIN)/$(TARGET).elf
	@$(OBJCOPY) -O binary $< $@

# the game's EEPROM region (JOB_COLD): the bytecode that runs rarely, then the sound
# effects; .eeprom: a 128 KiB image with the region (simulator)
$(BIN)/$(TARGET).region $(BIN)/$(TARGET).eeprom: $(BIN)/cold.bin $(BIN)/sfx.bin makefile
	@python3 -c "import sys; c=open('$(BIN)/cold.bin','rb').read(); c+=b'\xff'*(-len(c)%32); \
	  r=c+open('$(BIN)/sfx.bin','rb').read(); open('$(BIN)/$(TARGET).region','wb').write(r); \
	  a=$(JOB_COLD); e=bytearray(b'\xff'*131072); e[a:a+len(r)]=r; open('$(BIN)/$(TARGET).eeprom','wb').write(e); \
	  print('EEPROM: %d bytes at 0x%X' % (len(r), a))"

# a build with state on USART1 and room for it (16 KiB): the simulator prints it
debug:
	@$(MAKE) -s EXTRA=-DDEBUG LDSCRIPT=ld/debug.ld BIN=bin/debug

asm: $(BIN)/$(TARGET).elf
	@$(OBJDUMP) -d -S $< > $(BIN)/$(TARGET).lst
	@echo "Disassembly in $(BIN)/$(TARGET).lst"

size: $(BIN)/$(TARGET).elf
	@$(OBJSIZE) -d $< | awk '/[0-9]/ {printf "FLASH: %5d / 15424 bytes   SRAM: %4d / 2048 bytes (static, stack not included)\\n", $$1+$$2, $$2+$$3}'

sim: all $(SIM)/chsim
	@echo "Open http://localhost:$(PORT)/ - run 'make' in another terminal to reload"
	@node $(SIM)/server.mjs --port $(PORT) --watch $(BIN)/$(TARGET).elf --name "$(TARGET)"

test: all $(SIM)/chsim
	@$(SIM)/chsim $(BIN)/$(TARGET).bin -t 8000 -p right@1000-1100 -s 900:$(BIN)/title.pgm -s 7999:$(BIN)/screen.pgm
	@python3 -I $(SIM)/tools/pgm2png.py $(BIN)/title.pgm && python3 -I $(SIM)/tools/pgm2png.py $(BIN)/screen.pgm && rm $(BIN)/*.pgm && echo "Screenshots: $(BIN)/title.png $(BIN)/screen.png"

$(SIM)/chsim:
	@$(MAKE) -s -C $(SIM) chsim

flash: all
	rvprog -f $(BIN)/$(TARGET).bin

clean:
	@rm -rf $(BIN) $(SOURCE)/sprites.h $(SOURCE)/bytecode.h

.PHONY: all asm size sim test flash clean debug
