#!/bin/sh
# Downloads the PICO-8 cart of Just One Boss (ayla~nonsense, github.com/aylanonsense/
# just-one-boss) at a fixed commit: the build takes the sound effects from it
# (tools/sfx.py). The cart is not kept in this repository.
set -e
cd "$(dirname "$0")"
COMMIT=1dcb5810ba87861c063786f07a899be75d3af5ad
mkdir -p upstream
curl -fsSL -o upstream/just_one_boss.p8 \
  "https://raw.githubusercontent.com/aylanonsense/just-one-boss/$COMMIT/just_one_boss.p8"
echo "upstream: just_one_boss.p8 @ ${COMMIT%${COMMIT#????????}}"
