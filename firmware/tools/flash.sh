#!/bin/sh
# Flashes plataboy26 boards with a WCH-LinkE (Linux, macOS): the EEPROM library in chunks
# (writer/writer_NN.bin), then the firmware (flash.bin). Building is a separate step: this
# script only needs the built files and a flashing tool, no toolchain.
#
#   sh flash.sh [dir] [--once]
#
#   dir      the folder with flash.bin and writer/: by default the folder of this script
#            (a package made by "make dist"), or firmware/build when run from the repository
#   --once   one board, then exit (0 = programmed); default: board after board
#
# Flashing tool, the first one found: $FLASHER ("rvprog" or the path of wlink), rvprog
# (pip install rvprog), wlink (in the PATH or next to this script;
# https://github.com/ch32-rs/wlink/releases).
# Linux, "access denied" / no probe found: the WCH-LinkE needs a udev rule, see README.txt.
#
# Each chunk shows "CHUNK n/N ... OK" on the OLED and beeps shortly; "FAILED" and a long
# beep mean the EEPROM write failed: program that board again.
HERE=$(cd "$(dirname "$0")" && pwd)
DIR=""; ONCE=""
for a in "$@"; do
  case $a in
    --once) ONCE=1 ;;
    -h|--help) sed -n '2,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) DIR=$a ;;
  esac
done
if [ -z "$DIR" ]; then
  if [ -f "$HERE/flash.bin" ]; then DIR=$HERE; else DIR=$HERE/../build; fi
fi
DIR=${DIR%/}
[ -f "$DIR/flash.bin" ] || { echo "$DIR/flash.bin is missing: build first (make -C firmware)"; exit 1; }
ls "$DIR"/writer/writer_*.bin >/dev/null 2>&1 || { echo "$DIR/writer/ has no writer_NN.bin: build first"; exit 1; }

TOOL=${FLASHER:-}
if [ -z "$TOOL" ]; then
  if command -v rvprog >/dev/null 2>&1; then TOOL=rvprog
  elif command -v wlink >/dev/null 2>&1; then TOOL=wlink
  elif [ -x "$HERE/wlink" ]; then TOOL=$HERE/wlink
  else
    echo "no flashing tool found. Install one of:"
    echo "  rvprog:  pip install rvprog"
    echo "  wlink:   https://github.com/ch32-rs/wlink/releases (put wlink in the PATH or next to this script)"
    exit 1
  fi
fi
case $(basename "$TOOL") in
  rvprog*) write() { "$TOOL" -f "$1"; } ;;
  *)       write() { "$TOOL" flash -e -a 0x08000000 "$1"; } ;;
esac
echo "flashing from $DIR with $TOOL"

board() {
  for w in "$DIR"/writer/writer_*.bin "$DIR/flash.bin"; do
    echo "  $(basename "$w")"
    write "$w" || { echo "  *** FAILED at $(basename "$w") ***  check the WCH-LinkE cable and the board's power"; return 1; }
    [ "$w" = "$DIR/flash.bin" ] || sleep 2      # the writer puts its chunk into the EEPROM
  done
}

if [ -n "$ONCE" ]; then
  board || exit 1
  echo "  done: the menu should be on the screen"; exit 0
fi
n=0
while true; do
  printf "board %d: connect it and press Enter (Ctrl-C to stop) " $((n + 1)); read -r _ || exit 0
  if board; then
    n=$((n + 1)); echo "  done: the menu should be on the screen ($n programmed so far)"
  else
    echo "  press Enter to program this board again from the start"
  fi
done
