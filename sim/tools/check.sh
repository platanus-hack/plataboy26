#!/bin/sh
# Regression check: run every bundled game headless with scripted input and compare
# a hash of the final OLED frame with tools/golden.txt.  UPDATE=1 rewrites the file.
# A ROM with an EEPROM image next to it (the library firmware, MicroCity, Just One Boss) gets
# it, as in the web UI: its data and code are there.
cd "$(dirname "$0")/.." || exit 1
tmp=$(mktemp -d); fail=0; out=""
for rom in web/roms/*.bin; do
  id=$(basename "$rom" .bin)
  ee=""; [ -f "web/roms/$id.eeprom" ] && ee="-e web/roms/$id.eeprom"
  ./chsim "$rom" $ee -q -t 6000 -p act@1500-1650 -p act@2500-2650 -p right@3000-3600 -p up@3800-4300 \
          -p left@4400-4800 -p act@5000-5100 -s 5999:"$tmp/$id.pgm" > "$tmp/$id.log" 2>&1
  status=$?
  h=$(shasum "$tmp/$id.pgm" | cut -c1-16)
  out="$out$id $h
"
  want=$(grep "^$id " tools/golden.txt 2>/dev/null | cut -d' ' -f2)
  if [ "$status" != 0 ]; then echo "FAIL $id (exit $status)"; cat "$tmp/$id.log"; fail=1
  elif [ -z "$UPDATE" ] && [ "$h" != "$want" ]; then echo "DIFF $id ($h != $want)"; fail=1
  else echo "ok   $id"; fi
done
# Rev. 1.3 compatibility: B shares the fire pin. Games that only know the fire button must
# not see B, so holding B for the whole run must not change the final frame.
for rom in web/roms/*.bin; do
  id=$(basename "$rom" .bin)
  case $id in board_test|eeprom_test|hopper|microcity|justoneboss) continue;; esac   # these use B
  ee=""; [ -f "web/roms/$id.eeprom" ] && ee="-e web/roms/$id.eeprom"
  ./chsim "$rom" $ee -q -t 6000 -p act@1500-1650 -p act@2500-2650 -p right@3000-3600 -p up@3800-4300 \
          -p left@4400-4800 -p act@5000-5100 -p b@0-6000 -s 5999:"$tmp/$id.b.pgm" > /dev/null 2>&1
  [ "$(shasum "$tmp/$id.b.pgm" | cut -c1-16)" = "$(grep "^$id " tools/golden.txt | cut -d' ' -f2)" ] \
    && echo "ok   $id with B held" || { echo "DIFF $id with B held"; fail=1; }
done
[ -n "$UPDATE" ] && printf "%s" "$out" > tools/golden.txt && echo "golden hashes updated"
rm -rf "$tmp"
exit $fail
