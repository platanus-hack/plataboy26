# ===================================================================================
# Plataboy 26 - developer entry points
# ===================================================================================
#   make setup            RISC-V toolchain into tools/ + the simulator CLI (once)
#   make play             web simulator with the published games -> http://localhost:8088/
#   make new NAME=mygame  start a game from the template (then: cd games/mygame && make sim)
#   make pick             choose the games of the EEPROM library in the terminal -> firmware/build-custom/
#   make flash            flash boards from a built library (B=build-full ...), no rebuild
#   make dist             folder + zip to flash from any computer, Windows included
#   make check            simulator regression check + firmware end-to-end tests
#
# Needs: a C compiler, Node 18+, python3, curl. Nothing is installed outside this folder.
# ===================================================================================

PORT ?= 8088

setup:
	@sh tools/setup.sh
	@$(MAKE) -s -C sim chsim
	@echo "ready: 'make play' for the simulator, 'make new NAME=mygame' to start a game"

play:
	@echo "Open http://localhost:$(PORT)/"
	@node sim/server.mjs --port $(PORT)

new:
	@test -n "$(NAME)" || { echo "usage: make new NAME=mygame"; exit 1; }
	@test ! -e games/$(NAME) || { echo "games/$(NAME) already exists"; exit 1; }
	@cp -r games/template games/$(NAME) && rm -rf games/$(NAME)/bin
	@echo "games/$(NAME) created: cd games/$(NAME) && make sim"

pick:
	@$(MAKE) -s -C firmware pick SELECT="$(SELECT)"

flash dist:
	@$(MAKE) -s -C firmware $@ $(if $(B),B=$(B))

check:
	@$(MAKE) -s -C sim check
	@$(MAKE) -s -C firmware test

.PHONY: setup play new pick flash dist check
