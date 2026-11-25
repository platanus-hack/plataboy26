#!/bin/sh
# Fetches James Howard's MicroCity (GPL-3.0, github.com/jhhoward/MicroCity) at a fixed
# commit and applies this port's changes (microcity.patch). The game's files are not
# kept in this repository: the build takes them from upstream/.
set -e
cd "$(dirname "$0")"
REPO=https://github.com/jhhoward/MicroCity.git
COMMIT=ae265f1923e9ac9789df601bb438faa29b7ae339
if [ ! -d upstream/.git ]; then
  git clone -q "$REPO" upstream
fi
git -C upstream fetch -q origin "$COMMIT" 2>/dev/null || true
git -C upstream checkout -q -f "$COMMIT"
git -C upstream apply ../microcity.patch
echo "upstream: MicroCity @ ${COMMIT%${COMMIT#????????}} + microcity.patch"
