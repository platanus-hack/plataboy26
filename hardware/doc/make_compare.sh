#!/bin/sh
# Before/after picture: circuit_compare.svg on top, then rev. 1.1 vs rev. 1.3, front and back.
#   sh doc/make_compare.sh     (after make.sh; needs rsvg-convert + ImageMagick)
set -e
cd "$(dirname "$0")"
T=$(mktemp -d); FB="/System/Library/Fonts/Supplemental/Arial Bold.ttf"; FR=/System/Library/Fonts/Supplemental/Arial.ttf
../../tools/venv/bin/python -W ignore -I -c "
from gerbonara import LayerStack
st = LayerStack.open('../build/rev11_gerbers')
open('$T/b11.svg','w').write(str(st.to_pretty_svg(side='bottom')))
open('$T/f11.svg','w').write(str(st.to_pretty_svg(side='top')))" 2>/dev/null
rsvg-convert -w 900 $T/b11.svg -o $T/b11full.png
rsvg-convert -w 900 $T/f11.svg -o $T/f11full.png
magick $T/f11full.png -resize x1100 $T/f11.png
rsvg-convert -w 2100 circuit_compare.svg -o $T/circ.png
magick $T/b11full.png -resize x1100 $T/b11.png
# orange boxes = new parts (pixel boxes on the 1100 px high renders)
magick 3d/back.png -resize x1100 $T/b12.png
magick 3d/front.png -resize x1100 $T/f12.png
lab() { magick -size 860x80 xc:white -font "$FB" -pointsize 34 -fill "#111827" -gravity west -annotate +10-8 "$1" \
        -font "$FR" -pointsize 24 -fill "$3" -gravity west -annotate +10+26 "$4" $2; }
lab "Before (rev. 1.1): back side" $T/l1.png "#4b5563" "MCU, resistor ladder, buzzer resistor, coin cell"
lab "After (rev. 1.3): back side" $T/l2.png "#e8590c" "EEPROM, banana Pioneer plaque, coin cell moved down"
lab "Before (rev. 1.1): front" $T/l3.png "#4b5563" "D-pad, fire button, buzzer, power switch"
lab "After (rev. 1.3): front" $T/l4.png "#e8590c" "A/B diagonal, PH26 + Platanus + Buk, 3rd M2 hole"
magick \( $T/l1.png \( $T/b11.png -gravity center -background white -extent 860x1110 \) -append \) \
       \( $T/l2.png \( $T/b12.png -gravity center -background white -extent 860x1110 \) -append \) \
       -background white +append -gravity center -extent 2100x1210 $T/boards.png
magick \( $T/l3.png \( $T/f11.png -gravity center -background white -extent 860x1110 \) -append \) \
       \( $T/l4.png \( $T/f12.png -gravity center -background white -extent 860x1110 \) -append \) \
       -background white +append -gravity center -extent 2100x1210 $T/fronts.png
magick $T/circ.png $T/fronts.png $T/boards.png -background white -append \
  \( -size 2100x90 xc:white -font "$FR" -pointsize 26 -fill "#4b5563" -gravity center \
     -annotate +0+0 "Every rev. 1.1 net is reproduced unchanged (checked against its Gerbers). Rev. 1.3 is 4 mm taller (35.56 x 54.8 mm)." \) -append \
  -bordercolor white -border 30 +repage -depth 8 before_after.png
cp before_after.png ../fab/
rm -rf $T
echo "doc/before_after.png"
