#!/bin/sh
# Rev. 1.2 hardware pipeline: circuit -> checks -> PCB -> routing -> DRC -> fab outputs.
#   sh make.sh            (needs tools/venv, KiCad 10, Java 21; see README.md)
#   FAB_ONLY=1 sh make.sh  keep the routed board, regenerate the fab outputs and checks
set -e
cd "$(dirname "$0")"
ROOT=..
PY=$ROOT/tools/venv/bin/python
KICAD=${KICAD_APP:-$(ls -d /Applications/KiCad/KiCad.app $ROOT/tools/kicad-dl/mnt/KiCad/KiCad.app 2>/dev/null | head -1)}/Contents
KPY=$KICAD/Frameworks/Python.framework/Versions/Current/bin/python3
KCLI=$KICAD/MacOS/kicad-cli
JAVA=${JAVA:-$(ls /opt/homebrew/opt/openjdk@21/bin/java 2>/dev/null || echo java)}
FR=$ROOT/tools/freerouting/freerouting-2.1.0.jar
REV11=${REV11_GERBERS:-build/rev11_gerbers}
export KICAD_FOOTPRINTS=$KICAD/SharedSupport/footprints KICAD10_3DMODEL_DIR=$KICAD/SharedSupport/3dmodels CONSOLE_FP=$PWD/footprints

mkdir -p build fab
[ -d "$REV11" ] || { mkdir -p "$REV11"; unzip -oq $ROOT/ref/CH32V003-GameConsole/hardware/GameConsole_v1.1_gerber.zip -d "$REV11"; }

echo "== circuit + ERC";              $PY footprints.py >/dev/null
$PY console.py 2>&1 | grep -E "^ERC|errors found"
echo "== netlist vs rev. 1.1 copper"; $PY -I verify/check_netlist.py "$REV11" | tail -1
echo "== footprints / pin meaning vs LCSC"; $PY -I verify/check_footprints.py | tail -1
echo "== buttons, worst case";       $PY -I verify/check_buttons.py | tail -1
echo "== artwork -> silkscreen";      $PY -I art_to_silk.py 30; $PY -I logos_to_silk.py >/dev/null
if [ -z "$FAB_ONLY" ]; then        # FAB_ONLY=1: keep build/console.kicad_pcb, redo outputs only
echo "== placement";                  $KPY pcb.py place 2>&1 | grep -v wxApp
# Freerouting is not deterministic: retry until DRC finds no violation and nothing unrouted
for attempt in 1 2 3 4 5 6 7 8; do
  echo "== autorouting (Freerouting), attempt $attempt"
  (cd build && "$JAVA" -Djava.awt.headless=true -jar ../$FR --gui.enabled=false \
     --usage_and_diagnostic_data.disable_analytics=true -de console.dsn -do console.ses -mp 150 >freerouting.log 2>&1)
  $KPY pcb.py finish 2>&1 | grep -v wxApp
  echo "== DRC"
  $KCLI pcb drc --severity-all --refill-zones --save-board -o build/drc.rpt build/console.kicad_pcb >/dev/null
  if grep -q "track_dangling" build/drc.rpt; then
    $KPY pcb.py cleanup 2>&1 | grep -v wxApp
    $KCLI pcb drc --severity-all --refill-zones --save-board -o build/drc.rpt build/console.kicad_pcb >/dev/null
  fi
  grep "^\*\* Found" build/drc.rpt
  grep -q "Found 0 DRC violations" build/drc.rpt && grep -q "Found 0 unconnected" build/drc.rpt && break
  [ $attempt = 8 ] && { echo "DRC not clean, see build/drc.rpt"; exit 1; }
done
fi
$KPY pcb.py dnp 2>&1 | grep -v wxApp
$KPY pcb.py colours 2>&1 | grep -v wxApp
echo "== fab outputs"
rm -rf build/fab && mkdir -p build/fab/gerbers
$KCLI pcb export gerbers --use-drill-file-origin -l F.Cu,B.Cu,F.Paste,B.Paste,F.Silkscreen,B.Silkscreen,F.Mask,B.Mask,Edge.Cuts \
  --subtract-soldermask -o build/fab/gerbers/ build/console.kicad_pcb >/dev/null
$KCLI pcb export drill --drill-origin plot --format excellon --excellon-separate-th --generate-map --map-format pdf \
  -o build/fab/gerbers/ build/console.kicad_pcb >/dev/null
$KCLI pcb export pos --format csv --units mm --side both --exclude-dnp --use-drill-file-origin -o build/fab/pos.csv build/console.kicad_pcb >/dev/null
$PY -I fab.py build/fab/pos.csv build/bom.csv fab
echo "== fab Gerbers vs netlist"
$KPY pcb.py pads 2>&1 | grep -v wxApp
$PY -I verify/check_fab.py build/fab/gerbers build/rev12_pads.json | tail -1
(cd build/fab/gerbers && rm -f ../../../fab/console_v1.3_gerbers.zip && zip -q ../../../fab/console_v1.3_gerbers.zip *)
for side in top bottom; do
  $KCLI pcb render --side $side --width 900 --height 1390 --quality high --zoom 1 --background opaque --use-board-stackup-colors \
    -o fab/console_v1.3_$side.png build/console.kicad_pcb >/dev/null
done
cp build/console.kicad_pcb build/console.kicad_pro fab/
$KCLI pcb export pdf -l F.Cu,F.Silkscreen,F.Mask,Edge.Cuts --mode-single -o fab/console_v1.3_top.pdf build/console.kicad_pcb >/dev/null 2>&1 || true
$KCLI pcb export pdf -l B.Cu,B.Silkscreen,B.Mask,Edge.Cuts --mirror --mode-single -o fab/console_v1.3_bottom.pdf build/console.kicad_pcb >/dev/null 2>&1 || true
ls fab
