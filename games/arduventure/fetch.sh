#!/bin/sh
# Fetches TEAM a.r.g.'s Arduventure (game code MIT; story, characters, sprites, tiles,
# design and art copyright TEAM a.r.g., used with their permission) at a fixed commit.
# Their files are not kept in this repository: the build takes them from here.
set -e
cd "$(dirname "$0")"
REPO=https://github.com/Team-ARG-Museum/ID-46-Arduventure.git
COMMIT=938fae770f04369ae4e4dd29c1039f7b43e7d96c
if [ ! -d upstream/.git ]; then
  git clone -q "$REPO" upstream
fi
git -C upstream fetch -q origin "$COMMIT" 2>/dev/null || true
git -C upstream checkout -q "$COMMIT"
echo "upstream: Arduventure @ ${COMMIT%${COMMIT#????????}}"
