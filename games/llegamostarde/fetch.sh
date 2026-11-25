#!/bin/sh
# Downloads the original game (Paolo / paoloose, "Llegamos tarde a la hackathon", Platanus
# Hack 25 arcade, github.com/paoloose/platanus-hack-25-arcade) at a fixed commit: the build
# takes the characters' sprites from it (tools/gen_data.py); and its arcade cover (the
# Platanus Hack site's arcade, arcade_game_versions.cover_url) for the menu card
# (art/cards/llegamostarde.py). Not kept in this repository.
set -e
cd "$(dirname "$0")"
COMMIT=86eb9e0ef516874607eb7846a253e0f485ce3ecc
mkdir -p upstream
curl -fsSL -o upstream/game.js \
  "https://raw.githubusercontent.com/paoloose/platanus-hack-25-arcade/$COMMIT/game.js"
curl -fsSL -o upstream/cover.png \
  "https://hack-user-assets.s3.amazonaws.com/arcade-covers/llegamos-tarde-a-la-hackathon.png"
echo "upstream: game.js @ ${COMMIT%${COMMIT#????????}}, cover.png"
