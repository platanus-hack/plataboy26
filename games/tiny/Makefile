# ===================================================================================
# The original Tiny games (ref/CH32V003-GameConsole, Stefan Wagner, CC BY-SA 3.0),
# rebuilt from their unmodified sources for the plataboy26 library with the framework
# extensions of games/common/sysx.c: UP + DOWN held 2 s -> back to the menu, low-battery
# beep. The source changes: gpio.h's inline ADC_read() is renamed so that sysx.c's
# (which calls SYS_poll on every joypad reading) is used, and system.c gets SYS_trap
# (games/common/systrap.py: a CPU exception goes back to the menu, buzzer released), and
# driver.h's JOY_sound keeps the buzzer pin low for at most BUZ_PULSE_US per period
# (games/common/buzpatch.py, buzzer.h: same pitch, less current from the coin cell).
#
#   make            bin/<game>.bin for every game
#   make clean
# ===================================================================================

ROOT    := $(abspath $(CURDIR)/../..)
UP      := $(ROOT)/ref/CH32V003-GameConsole/software
XPACK   := $(wildcard $(ROOT)/tools/xpack-riscv-none-elf-gcc-*/bin)
PREFIX  ?= $(if $(XPACK),$(firstword $(XPACK))/,)riscv-none-elf-
CC       = $(PREFIX)gcc
OBJCOPY  = $(PREFIX)objcopy
COMMON   = $(ROOT)/games/common
GAMES    = tiny_arkanoid tiny_boulder tiny_bubble tiny_conway tiny_doom tiny_invaders \
           tiny_karate tiny_lander tiny_pacman tiny_run tiny_shooter tiny_tris
CFLAGS   = -g -Os -flto -ffunction-sections -fdata-sections -fno-builtin -nostdlib \
           -march=rv32ec_zicsr -mabi=ilp32e -Wall -Wno-unused-function -DSYSX_ADC_READ

all: $(GAMES:%=bin/%.bin)

$(UP)/%/src/gpio.h:
	@test -d $(UP) || git clone --depth 1 https://github.com/wagiminator/CH32V003-GameConsole $(ROOT)/ref/CH32V003-GameConsole

build/%/src/gpio.h: $(UP)/%/src/gpio.h Makefile $(COMMON)/systrap.py $(COMMON)/buzpatch.py
	@mkdir -p $(@D)
	@cp $(UP)/$*/src/* $(@D)/
	@sed -i '' 's/^static inline uint16_t ADC_read(void) {/uint16_t ADC_read(void);\
static inline uint16_t ADC_read_inline(void) {/' $@
	@grep -q 'ADC_read_inline' $@ || (echo "$*: gpio.h patch failed"; exit 1)
	@python3 $(COMMON)/systrap.py $(@D)/system.c > /dev/null
	@if [ -f $(@D)/driver.h ]; then python3 $(COMMON)/buzpatch.py $(@D)/driver.h > /dev/null; fi

bin/%.elf: build/%/src/gpio.h $(COMMON)/sysx.c $(COMMON)/sysx.h $(COMMON)/joycal.h $(COMMON)/buzzer.h
	@mkdir -p bin
	@$(CC) -o $@ build/$*/src/*.c $(COMMON)/sysx.c $(CFLAGS) \
	  -DF_CPU=$$(awk '/^F_CPU/ {print $$3}' $(UP)/$*/makefile) -Ibuild/$*/src -I$(COMMON) \
	  -T $(UP)/$*/ld/ch32v003.ld -lgcc -Wl,--gc-sections,--build-id=none

bin/%.bin: bin/%.elf
	@$(OBJCOPY) -O binary $< $@
	@printf '%-15s %6d bytes\n' $* $$(stat -f%z $@)

clean:
	rm -rf build bin

.PRECIOUS: bin/%.elf build/%/src/gpio.h
.PHONY: all clean
