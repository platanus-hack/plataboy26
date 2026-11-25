# Resident overlay loader contract

The loader replaces one declared flash window from a read-only, Nitro-owned
EEPROM resource. It never writes EEPROM. The original SDK startup and linker
remain intact; the overlay build explicitly selects `ld/overlay.ld`.

## Caller and lifetime rules

`nn_overlay_load(group)` returns zero only after the full executable window,
including its unused tail, matches the selected group's CRC32. Group IDs are
1 Catalog, 2 the initial identity/START handshake and race setup, 3 the complete
pinned race/power domain, and 4 menus. Setup reuses the already verified startup
bank, avoiding a redundant first-START flash exchange. There is no player-facing
power selection. Each cold helper returns to resident code before a new bank loads.
`nn_overlay_current()` starts at zero on every reset. No persistent group tag
authorizes entry after a reset or interrupted replacement.

The caller must already have a quiescent cohort and a resident stack. It must
not retain an overlay return address, callback or pointer that replacement can
invalidate. All interrupt code and data reachable during a load remain resident.
No individual A/B power activation swaps executable groups during a race.

The loader borrows the complete 1024-byte framebuffer: the first 64 bytes are
page scratch, and the remaining 960 bytes hold verified replacement instructions.
The linker explicitly overlaps the `.ram_loader` and `.nn_framebuffer` lifetimes
without allocating another buffer. No drawing, OLED transfer or other
framebuffer reader may run during the call. The OLED retains its previous image;
the next display must follow a complete framebuffer clear/redraw. Game, Car,
effects and live transport storage are never scratch buffers. The only dedicated
mutable loader state is the one-byte verified group; stack use still requires
measurement in the complete game, including interrupts.

## Resource and executable validation

The fixed resident `.nn_overlay_meta` reservation is 112 bytes. The packer patches
it after final linking, avoiding a second link that could change code addresses.
The external resource begins with the identical metadata, then padding to 128
bytes, four individually 64-byte-padded flash images and one padded RAM loader.

| Metadata offset | Type | Meaning |
| --- | --- | --- |
| 0 | u32 little endian | `NNOV` magic |
| 4–7 | four u8 | Version 2, group count 4, page shift 6, reserved zero |
| 8 | u32 | Complete external region base |
| 12 | u32 | Complete external region length |
| 16, 18 | two u16 | Declared flash window base and size |
| 20 | 32 bytes | SHA-256 build identity |
| 52 + 12 × flash group index | u32, u16, u16, u32 | Resource-relative source offset, actual size, stored size, full-window CRC32 |
| 100 | u32, u16, u16, u32 | RAM loader source offset, actual size, stored size, full 960-byte RAM-code CRC32 |

Before flash unlock, the loader validates format, full-width addresses,
overflow-safe containment, bank/reserved-area boundaries, fixed window identity,
per-image sizes/alignment and pairwise source disjointness. It compares all
external metadata against the expected resident bytes and streams the complete
selected flash image through CRC32 before any erase. Before executing the RAM
body, the resident bootstrap copies its checked source into framebuffer bytes
64–1023, verifies the full destination CRC including FF padding, and fences
instruction fetch. A typed indirect call enters that verified RAM body; it returns
to resident code before rendering can reuse the buffer. A corrupt RAM source or
destination cannot reach flash unlock. The RAM body only uses the first scratch
page and resident helpers; it cannot call either evictable flash bank or rendering. The digest includes the unused window tail, defined as FF. CRC32 detects accidental corruption; it is not a
malicious-content authentication claim.

Immediately before replacement, the verified group becomes zero. Every EEPROM
read and flash wait/program result is checked. Only differing pages are rewritten,
but every changed page is fully programmed: the CH32V003's erased value is
`0xE339E339`, not FF. Flash is locked on all exits after unlock. Full-window
readback integrity and an instruction-fetch fence precede verified entry.

A source-validation failure preserves the previous verified image because no
replacement has begun. A failure during programming/readback leaves group zero;
the caller must remain in resident loading/error handling. It must not call a
missing group or silently resume simulation.

## Build artifacts

```sh
python3 tools/overlay_pack.py \
  --elf bin/nitro_nonsense.link.elf \
  --resident-elf bin/nitro_nonsense.elf \
  --output bin/nitro_nonsense.bin \
  --region bin/nitro_nonsense.region \
  --policy bin/nitro_nonsense.overlay.json \
  --eeprom-base 0xADDRESS --objcopy /path/to/riscv-none-elf-objcopy
```

The address is a build parameter selected from a validated complete-library
inventory; `0xADDRESS` is deliberately not a proposed allocation. The packer
rejects overlap with directory, scratch and save areas or a bank boundary, but
does not inspect a user's existing library. Its output is a resource, not an
installer or permission to replace an EEPROM image. Installation must separately
preserve all existing library identities, resources, saves and associated values.

The resident ELF retains overlay symbols/debug contents as non-allocating
sections and disables their high-address load segments. Its RAM program headers
remain valid. The packer verifies that ordinary objcopy produces exactly the
same resident binary; it never constructs a huge LMA-gap binary.

Build identity covers the normalized resident code, full flash and RAM-loader images, address
layout, source/linker/makefile/tool inputs and any extra `--identity-input` files.
For test-only per-node UID instrumentation, `--test-build-id` requires the
`native_bus_high` symbol and marks `testInstrumentationIdentityOverride=true`.
Every native-instrumented ELF also sets `testInstrumentation=true`, including
fixtures that use a single common binary without an identity override. Production
installation must reject both diagnostic flags. Distinct production payloads
receive distinct identities; the multiplayer gate must reject mixed identities.

Budget reporting must count `residentLoadBytes + reservedWindowBytes`, rather
than presenting the short resident binary length as total flash use. The usable
resident margin is `residentCeiling - residentLoadBytes`. Moving code does not
increase the 2048-byte physical RAM capacity.

## Reproducible loader checks

```sh
cc -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DNN_OVERLAYS=1 -DNN_OVERLAY_TEST=1 -Isrc \
  tests/overlay_loader.c src/overlay.c -o bin/overlay-loader-tests
ASAN_OPTIONS=halt_on_error=1 UBSAN_OPTIONS=halt_on_error=1 bin/overlay-loader-tests
python3 tests/overlay_cpu.py
python3 tests/overlay_pack_test.py
```

The host fixture injects metadata/read/program/integrity failures and interrupted
replacement, checking verified state, range containment and framebuffer lifetime. The original
64-byte scratch-only contract is still asserted directly for the replacement body;
bootstrap tests separately assert its intentional full-framebuffer lifetime and
reject corruption injected after copying RAM instructions.
The CPU fixture compiles and actually executes four different RV32EC functions
from the same window using the unchanged flash/EEPROM emulator. It rejects a
corrupted flash or RAM source before flash writes, confirms all external storage unchanged,
and automatically finds a genuinely partial window before testing reset recovery.
Its scheduled power cycles are additional checks, not a claim of exhaustive
physical power-loss coverage.

These isolated checks do not establish the complete game's fit, multiplayer
loading protocol, worst-case stack, real flash latency, electrical bus behavior,
low-voltage recovery or endurance. Those remain integration/hardware gates.
