#!/usr/bin/env python3
"""Verify the new card in the unchanged shipped selector using an additive copy.

The resident may be the stable merged game while the powers firmware is still
under development. This preview never writes hardware or published ROM files.
"""
import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

GAME=Path(__file__).resolve().parents[1]
SDK=GAME.parents[1]


def module(name,path):
    spec=importlib.util.spec_from_file_location(name,path)
    result=importlib.util.module_from_spec(spec);spec.loader.exec_module(result)
    return result


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image',type=Path,required=True)
    parser.add_argument('--inventory',type=Path,required=True)
    parser.add_argument('--resident',type=Path,required=True)
    parser.add_argument('--region',type=Path,
                        help='Use the final executable overlay resource instead of the raw-card fixture')
    parser.add_argument('--region-base',type=lambda value:int(value,0),default=0xc000)
    parser.add_argument('--policy',type=Path,help='Bind final resident and overlay resource hashes')
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--right-presses',type=int,default=3)
    parser.add_argument('--scope',default='Stable merged resident game; powers firmware is not exercised')
    args=parser.parse_args()
    if not 0<=args.right_presses<=46:raise ValueError('Right press count must fit the library')
    if args.output.exists():raise ValueError('Use a new output directory; prior evidence is retained')
    planner=module('card_resource_plan',GAME/'tools/resource_plan.py')
    card_builder=module('production_card',GAME/'tools/build_card.py')
    title_pixels,hashes=card_builder.render('cc')
    pixels=card_builder.card_art(title_pixels)
    card=bytes(sum((pixels[(page*8+bit)*128+x]>128)<<bit for bit in range(8))
               for x in range(128) for page in range(8))
    if (GAME/'assets/menu-card.png').read_bytes()!=card_builder.card_png(pixels,128,64):
        raise ValueError('Regenerate and inspect the current production title card first')
    image=args.image.read_bytes();inventory=json.loads(args.inventory.read_text())
    # Without --region the historical preview reserves a raw card fixture only.
    region=args.region.read_bytes() if args.region else card
    target,next_inventory,plan=planner.build_plan(image,inventory,args.inventory,region,args.region_base,
                                                 resident=args.resident.read_bytes(),card=card,
                                                 policy=json.loads(args.policy.read_text()) if args.policy else None)
    args.output.mkdir(parents=True,exist_ok=False)
    (args.output/'original.eeprom').write_bytes(image)
    (args.output/'candidate.eeprom').write_bytes(target)
    (args.output/'expected-title.pixels').write_bytes(pixels)
    (args.output/'inventory.next.json').write_text(json.dumps(next_inventory,indent=2)+'\n')
    for operation in plan['operations']:operation.pop('data')
    (args.output/'resource-plan.json').write_text(json.dumps(plan,indent=2)+'\n')
    runner=args.output/'menu-card-selector'
    command=['cc','-std=c11','-O2','-I'+str(SDK/'sim/core'),str(GAME/'tests/menu_card_selector.c'),
             str(SDK/'sim/core/emu.c'),'-lm','-o',str(runner)]
    subprocess.run(command,check=True)
    screenshot=args.output/'selector-card.pgm'
    run=[str(runner),str(SDK/'sim/web/roms/plataboy26.bin'),str(args.output/'candidate.eeprom'),
         str(args.output/'expected-title.pixels'),str(screenshot),str(args.right_presses)]
    result=subprocess.run(run,text=True,capture_output=True,check=True)
    (args.output/'selector.log').write_text(result.stdout+result.stderr)
    # Preserve the actual SSD1306 contrast instead of whitening its pixels.
    converter_path=SDK/'sim/tools/pgm2png.py'
    saved_arguments=sys.argv
    try:
        sys.argv=[str(converter_path)]
        converter=module('selector_png_converter',converter_path)
    finally:
        sys.argv=saved_arguments
    native=args.output/'selector-card.png'
    preview=args.output/'selector-card-preview.png'
    converter.conv(str(screenshot),str(native),1)
    converter.conv(str(screenshot),str(preview),4)
    old=planner.inventory_image(image,inventory,args.inventory)
    new=planner.inventory_image(target,next_inventory,args.inventory)
    assert old['entries']==new['entries'][:old['count']]
    assert image[0x1f000:0x1f800]==target[0x1f000:0x1f800]
    assert args.image.read_bytes()==image
    report={'scope':args.scope,'method':'Actual unchanged shipped menu and emulated MCU, public Right/ADC input',
            'originalEntries':old['count'],'candidateEntries':new['count'],
            'originalIdentitiesAndContentPreserved':True,'entireStandardSaveAreaPreserved':True,
            'sourceFileUntouched':True,'newEntry':plan['newEntry'],
            'sourceImageSha256':planner.digest(image),'candidateImageSha256':planner.digest(target),
            'menuFirmwareSha256':planner.digest((SDK/'sim/web/roms/plataboy26.bin').read_bytes()),
            'residentSha256':planner.digest(args.resident.read_bytes()),
            'resourceSha256':planner.digest(region),'resourceBytes':len(region),
            'resourceKind':'Executable overlay region' if args.region else 'Raw card preview fixture',
            'overlayPolicySha256':planner.digest(args.policy.read_bytes()) if args.policy else None,
            'productionTitleSourceSha256':hashes,'nativeResult':json.loads(result.stdout),
            'commands':[command,run],
            'images':[{'file':path.name,'sha256':planner.digest(path.read_bytes())} for path in (native,preview)],
            'hardwareDeploymentBlocked':True,
            'hardwareDeploymentReason':plan['hardwareDeploymentReason'],
            'humanVisualReview':'Pending individual native and 4x inspection'}
    (args.output/'report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({'result':'PASS','sourceEntries':old['count'],'candidateEntries':new['count'],
                      'elapsedUs':report['nativeResult']['elapsedUs'],'nativeImage':str(native)},indent=2))


if __name__=='__main__':main()
