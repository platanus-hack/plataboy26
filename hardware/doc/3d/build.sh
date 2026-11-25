#!/bin/sh
# 3D mockup: GLB (fixed for the browser), STEP, renders, comparison picture.
#   sh doc/3d/build.sh     (after make.sh)
set -e
cd "$(dirname "$0")"
HW=$(cd ../.. && pwd)
KICAD=${KICAD_APP:-$(ls -d /Applications/KiCad/KiCad.app $HW/../tools/kicad-dl/mnt/KiCad/KiCad.app 2>/dev/null | head -1)}/Contents
KCLI=$KICAD/MacOS/kicad-cli
export CONSOLE_FP=$(cd $HW/footprints && pwd) KICAD10_3DMODEL_DIR=$KICAD/SharedSupport/3dmodels
PCB=$HW/fab/console.kicad_pcb
# Display copies of the board: H1 (OLED module, hand-soldered after assembly) shown although
# it is DNP for the assembler; H2 stays hidden (not fitted). One per silkscreen colour.
viz() {   # viz <silk colour name> -> path of the display board
  d=$HW/build/viz/$1; mkdir -p $d; cp $HW/fab/console.kicad_pro $d/
  $HW/../tools/venv/bin/python -I - "$PCB" "$d/console.kicad_pcb" "$1" <<'PY'
import re, sys
s = open(sys.argv[1]).read()
i = s.index('(property "Reference" "H1"'); j = s.rindex('(footprint', 0, i)
k = s.index('(attr', j); e = s.index(')', k)
s = s[:k] + s[k:e].replace(' dnp', '') + s[e:]
s = re.sub(r'(Silk Screen"\) \(color ")[^"]+"', r'\g<1>' + sys.argv[3] + '"', s)
# KiCad renders its "Black" mask preset translucent (the board looks brown); real black
# mask is practically opaque
s = re.sub(r'(Solder Mask"\) \(color ")[^"]+"', r'\g<1>#0B0B0CFF"', s)
i = s.index('(property "Reference" "H2"'); j = s.rindex('(footprint', 0, i)   # H2 not fitted:
k = s.find('(model', i); e = s.find('(model', k + 1)                          # drop its 3D model
n = s.find('\n\t(footprint', i)
if 0 < k < (n if n > 0 else len(s)):
    depth, q = 0, k
    while True:
        depth += {'(': 1, ')': -1}.get(s[q], 0); q += 1
        if depth == 0: break
    s = s[:k] + s[q:]
open(sys.argv[2], 'w').write(s)
PY
  echo $d/console.kicad_pcb
}
$KCLI pcb export step --subst-models -o $HW/fab/console_v1.3.step $PCB >/dev/null 2>&1 || true
test -s $HW/fab/console_v1.3.step
for variant in white yellow; do
  if [ $variant = white ]; then B=$(viz White); sfx=""; col=""; else B=$(viz Yellow); sfx="_yellow"; col="#DEC81E"; fi
  $KCLI pcb export glb --subst-models --include-tracks --include-pads --include-zones --include-silkscreen \
    --include-soldermask -o console_v1.3$sfx.glb $B >/dev/null
  $HW/../tools/venv/bin/python -I fix_glb.py console_v1.3$sfx.glb $col
  r() { $KCLI pcb render --quality high --background opaque --use-board-stackup-colors "$@" $B >/dev/null; }
  r --side top -w 1100 -h 1690 -o front$sfx.png
  r --side bottom -w 1100 -h 1690 -o back$sfx.png
  r --rotate '-28,0,-18' --perspective -w 1400 -h 1100 -o iso_top$sfx.png
  r --side bottom --rotate '-40,0,30' --perspective -w 1400 -h 1100 -o iso_bottom$sfx.png
  if [ $variant = white ]; then
    r --side front --zoom 2 -w 1800 -h 600 -o side_front.png
    r --side right --zoom 2 -w 1800 -h 600 -o side_right.png
  fi
done
sh ../make_compare.sh >/dev/null && cp ../before_after.png .
# white vs yellow, front and back side by side
F=/System/Library/Fonts/Supplemental/Arial\ Bold.ttf
for c in "" _yellow; do magick front$c.png back$c.png -resize x900 +append /tmp/_silk$c.png; done
magick \( /tmp/_silk.png -gravity north -background white -splice 0x60 -font "/System/Library/Fonts/Supplemental/Arial Bold.ttf" -pointsize 40 -annotate +0+10 "Black + white silkscreen" \) \
       \( /tmp/_silk_yellow.png -gravity north -background white -splice 0x60 -font "/System/Library/Fonts/Supplemental/Arial Bold.ttf" -pointsize 40 -annotate +0+10 "Black + yellow silkscreen" \) \
       -bordercolor white -border 10 +append +repage silk_compare.png
# version stamp on every file the page loads, so browsers never show a stale copy
v=$(date +%s)
sed -i '' -E "s/(\.glb|\.png)(\?v=[0-9]+)?\"/\1?v=$v\"/g" index.html
echo "mockup built (v=$v)"
