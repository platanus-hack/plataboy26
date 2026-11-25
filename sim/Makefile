# ===================================================================================
# CH32V003 Game Console Simulator - build
# ===================================================================================
#   make          native headless runner (chsim) + WebAssembly core (web/chsim.wasm)
#   make serve    start the web UI on http://localhost:8088/
#   make check    run every bundled game headless and compare against golden hashes

LLVM    ?= $(firstword $(wildcard /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin /usr/lib/llvm-*/bin))
LLD     ?= $(firstword $(wildcard /opt/homebrew/opt/lld/bin /usr/local/opt/lld/bin))
WASMCC  = PATH="$(LLVM):$(LLD):$$PATH" $(LLVM)/clang
CORE    = core/emu.c
HDRS    = core/emu.h

all: chsim web/chsim.wasm

chsim: $(CORE) core/cli.c $(HDRS)
	cc -O2 -Wall -Wextra -Wno-unused-parameter -o $@ $(CORE) core/cli.c

web/chsim.wasm: $(CORE) core/wasm_api.c $(HDRS)
	$(WASMCC) --target=wasm32 -O3 -nostdlib -ffreestanding -Wall \
	  -Wl,--no-entry -Wl,-z,stack-size=65536 -o $@ $(CORE) core/wasm_api.c

serve:
	node server.mjs --port 8088

check: chsim
	@sh tools/check.sh

.PHONY: all serve check
