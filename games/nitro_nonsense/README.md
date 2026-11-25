# Nitro Nonsense

A native four-car racer with a rear camera, automatic acceleration, a wide seeded
closed circuit, ordered off-road lap gates and first-to-three matches. The current
powers build keeps 3D and gives every car Nitro on A and Cannon on B from the
first race. There is no power-selection screen. Track editing and traps remain
deferred. The current development delivery adds phase-based external code
resources, improved cars/roadside presentation, a title/menu card and a melody.

## Build and run

From the devkit root, after the usual `sh tools/setup.sh`:

```sh
make -C games/nitro_nonsense
make -C games/nitro_nonsense check CHECK_ARGS="--seed 3 --seconds 120"
make -C games/nitro_nonsense test
make -C games/nitro_nonsense sim PORT=8095
```

The game uses 48 MHz, the original RV32EC startup/linker and normal SDK checker.
Generated binaries and test outputs stay under ignored `bin/`. No separate
repository checkout or generated SDK staging tree is needed. The powers build loads verified phase code into a declared flash window from a
read-only external resource. It does not write runtime EEPROM or claim a save
slot. Installing resources into an existing library requires the preservation
inventory and checks described in [the memory plan](docs/POWER_MEMORY_PLAN.md);
do not replace an installed EEPROM with a fresh blank image.

## Play

After initialization, NANO NITRO NONSENSE appears before the lobby. Release any
button held while opening the game. A opens LINK LOBBY; B opens multiplayer
HELP, where B returns to the title and A opens the lobby directly. Directional
buttons do not launch the game. The start press is consumed: release it and press
A separately in the lobby to join and become ready. Title and HELP do not
advance the simulation. HELP explains common GND and SWIO buses for two to four
independently powered consoles; all joined players ready automatically starts
the race, and bots fill the remaining four-car slots.

Left/Right steer relative to your car. Acceleration is automatic; hold Down to
brake and reverse. A fires Nitro and B fires Cannon during races; each has its
own cooldown. Active Nitro uses rear flames without a center-screen NITRO
banner; cannon impacts retain the HIT banner. The upper HUD shows both powers and their cooldowns directly
below LAP/player/wins. The first stripe crossing arms the lap counter; four forward gates in
order complete each lap. Leaving the road does not erase progress, while reverse
crossings and skipped gates do not count. Three laps win a round.

The winning human presses A to continue; if a bot wins, the first human does so.
The other consoles show WAIT FOR PLAYER. The round podium follows the actual
race order and shows each player's accumulated wins. Continue directly into a
different seeded circuit with the same players, scores and fixed powers. First
to three wins the match; the champion screen labels its rows LAST RACE rather
than presenting them as a separate overall ranking. A explicitly starts a new
match. [Round-course rules](docs/ROUND_COURSES.md) describe deterministic course
selection and the retained legacy policy.

Hold B while powering on to use the solo/debug route without claiming the SWIO
pin. Release B, clear the title with a fresh button, then press A separately in
the local one-player lobby. A normal solo start also restores that pin after
discovery. Physical flashing
recovery remains untested.

## Linked lobby

Use the separate common GND and SWIO/PD1 conductors shown in
[the two/three/four-console diagrams](docs/WIRING.md). Each console uses its own
normal power. VCC is not connected. Hardware/electrical behavior is unverified.

Open the game on up to four connected consoles. Consoles may open the game later
while the pre-race lobby remains open; the roster refreshes to include them.
The roster marks your console with YOU and each participant with WAIT or READY.
Player numbers follow UID arbitration rather than cable position and may change
when the roster grows. Press A once to join and become ready. Readiness belongs
to the physical console identity and survives roster changes. New consoles wait
for their own A input. There is no configuration screen or separate roster action.

While a discovered player is WAIT, the lobby stays open. When everyone is READY,
the lobby makes its final admission check and confirms that chosen group before
countdown. A console arriving after that check is outside the closing race.
Humans plus bots always total four cars; later consoles do not replace racers.
A lone console can start solo with three bots. To keep a lobby waiting for more
players, leave a discovered participant at WAIT until the intended group appears.

The ordinary browser emulator models one console, so it runs solo with three
bots. Game-local native tests supply an explicit external-line latch to separate
CPUs and exercise actual GPIO/ADC and OLED behavior. They do not establish
physical cable operation or final human handling/readability acceptance.

## Verification

```sh
make -C games/nitro_nonsense verify
# Extended four-human round and complete linked championship:
cd games/nitro_nonsense
python3 tests/verify.py --extended
```

The merged linked-lobby baseline matrix runs three original 120-second checker seeds, sanitized rule
fixtures, one-console first-to-five/boot recovery and actual two/three/four-CPU
lobby, input, OLED and line-fault cases. The extended case also drives all four
humans through a complete round and verifies the logical transition into the
next countdown. Its final stored OLED still shows RESULTS, as documented in the
test matrix. See the
[test matrix and provenance](docs/TEST_MATRIX.md) for observed results and limits.

The initial evidence/screenshots migrate the already merged base from
[the standalone integration PR](https://github.com/aberguecio/nitro-nonsense-plataboy/pull/9).
The fixed-cohort baseline remains historical evidence. Late-entry verification
retains its own source, observer and binary identities; it is not represented as
a rerun of the older implementation. The corrected observer binds READY
acceptance to cached completed-frame identities and actual OLED glyphs. Existing SDK games,
published ROMs, saves and reference checker must remain intact. The preserved
late-lobby receipts describe the merged baseline; they are not final powers-build
validation. The current scope and final-source validation gates are tracked in
[the memory plan](docs/POWER_MEMORY_PLAN.md).

Current follow-up validation and preview-cover details: [championship-only delivery](docs/evidence/finale-polish/champion-only/README.md).

The extended inventory keeps the original one-round regression and also runs `tests/minimal_online_match.mjs` through three wins and a deliberate new-match restart. Its additional inter-round countdown probe requires advancing committed ticks at four seconds and completion within six seconds; the original entry/retry timing assertions remain unchanged. The harness drives public controls only and validates the complete Game state, cohort, scores, ordered finishes, powers and new courses.
