#!/usr/bin/env python3
"""Export the production title renderer as the game's monochrome library card.

This executes only the host-compiled pure game renderer. It does not execute a
console image, bypass a firmware size check or establish hardware/input behavior.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import struct
import tempfile
import zlib

GAME = Path(__file__).resolve().parents[1]
INPUTS = (
    "src/game.c", "src/minimal_game.inc", "src/game.h", "src/power_catalog.h",
    "src/overlay.h", "src/track.c", "tools/build_card.py",
)
HARNESS = r'''#include "game.h"
#include <stdio.h>
#include <string.h>
int main(void) {
    game_boot();
    if(game.phase != TITLE) return 2;
    Game before = game;
    game_draw();
    if(memcmp(&before, &game, sizeof(game))) return 3;
    return fwrite(framebuffer, 1, sizeof(framebuffer), stdout) == sizeof(framebuffer) ? 0 : 4;
}
'''


def source_hashes():
    return {name: hashlib.sha256((GAME / name).read_bytes()).hexdigest() for name in INPUTS}


def render(compiler):
    before = source_hashes()
    with tempfile.TemporaryDirectory(prefix="nn-card-") as scratch:
        scratch = Path(scratch)
        source, binary = scratch / "title.c", scratch / "title"
        source.write_text(HARNESS)
        command = [*shlex.split(compiler), "-std=c11", "-DNN_MINIMAL", "-Wall",
                   "-Wextra", "-Werror", "-O2", "-I", str(GAME / "src"),
                   str(source), str(GAME / "src/game.c"), str(GAME / "src/track.c"),
                   "-o", str(binary)]
        subprocess.run(command, check=True)
        framebuffer = subprocess.check_output([str(binary)])
    if len(framebuffer) != 1024 or source_hashes() != before:
        raise RuntimeError("Invalid framebuffer or renderer changed during capture")
    pixels = bytes(255 if framebuffer[(y >> 3) * 128 + x] & (1 << (y & 7)) else 0
                   for y in range(64) for x in range(128))
    return pixels, before


def card_png(pixels, width, height):
    """Use the SDK converter's dependency-free PNG framing with binary pixels."""
    if len(pixels) != width * height or set(pixels) - {0, 255}:
        raise RuntimeError("A menu card must contain exactly two monochrome values")
    rows = []
    for y in range(height):
        row = pixels[y * width:(y + 1) * width]
        packed = bytes(sum((row[x + bit] != 0) << (7 - bit) for bit in range(8))
                       for x in range(0, width, 8))
        rows.append(b"\x00" + packed)
    def chunk(kind, payload):
        return (struct.pack(">I", len(payload)) + kind + payload +
                struct.pack(">I", zlib.crc32(kind + payload)))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 1, 0, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(b"".join(rows))) + chunk(b"IEND", b""))


def card_art(title_pixels):
    # The chooser owns its bottom-right position badge. Gameplay prompts belong
    # to the game title screen, so reserve the entire footer in this resource.
    return title_pixels[:52 * 128] + bytes(12 * 128)


def compare_title_capture(actual, title_pixels):
    """Compare all OLED bits while retaining actual display photometry."""
    header = b"P5\n128 64\n255\n"
    if not actual.startswith(header) or len(actual) != len(header) + 8192:
        raise RuntimeError("Actual title capture must be an exact 128x64 P5 frame")
    pixels = actual[len(header):]
    lit = sorted(set(pixels) - {0})
    # The simulator maps contrast 0..255 to one lit intensity 70..255.
    # Host rendering exports binary assets; a physical display's contrast
    # changes brightness, not any of its 8192 on/off pixel positions.
    if len(lit) != 1 or not 70 <= lit[0] <= 255:
        raise RuntimeError("Actual title capture must have one valid OLED lit intensity")
    difference = sum(bool(a) != bool(b) for a, b in zip(pixels, title_pixels))
    if len(title_pixels) != 8192 or difference:
        raise RuntimeError(f"Actual-console title differs in {difference} pixel positions")
    return {"bitmapPixelsCompared": 8192, "bitmapDifferences": 0,
            "actualLitIntensity": lit[0], "referenceLitIntensity": 255,
            "captureSha256": hashlib.sha256(actual).hexdigest()}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--output", type=Path, default=GAME / "assets")
    parser.add_argument("--check", action="store_true", help="Compare existing assets without rewriting them")
    parser.add_argument("--compare-pgm", type=Path, help="Require exact pixels from a later actual-console title capture")
    args = parser.parse_args()
    title_pixels, hashes = render(args.cc)
    pixels = card_art(title_pixels)
    preview = b"".join(bytes(value for pixel in pixels[y * 128:(y + 1) * 128]
                             for value in (pixel,) * 4) * 4 for y in range(64))
    paths = [(args.output / "menu-card.png", card_png(pixels, 128, 64), 128, 64),
             (args.output / "menu-card-preview.png", card_png(preview, 512, 256), 512, 256)]
    if args.compare_pgm:
        comparison = compare_title_capture(args.compare_pgm.read_bytes(), title_pixels)
        print("PASS supplied full title-capture bitmap matches the production renderer: " +
              json.dumps(comparison, sort_keys=True))
    for path, expected, width, height in paths:
        if args.check:
            if path.read_bytes() != expected:
                raise RuntimeError(f"Stale production title asset: {path}")
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(expected)
    receipt = {
        "title": "NANO NITRO NONSENSE",
        "gameIdentity": "nitro_nonsense",
        "libraryEntryName": "Nitro Nonsense",
        "method": "Host-compiled production game_boot and game_draw, unmodified canonical Game",
        "consoleExecution": "Not claimed; later actual-MCU title pixel comparison remains pending",
        "sourceSha256": hashes,
        "pixelSha256": hashlib.sha256(pixels).hexdigest(),
        "productionTitlePixelSha256": hashlib.sha256(title_pixels).hexdigest(),
        "composition": "Production title/car artwork rows 0..51; black footer rows 52..63 reserved for chooser badge",
        "harnessSha256": hashlib.sha256(HARNESS.encode()).hexdigest(),
        "images": [{"file": path.name, "width": width, "height": height,
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
                   for path, expected, width, height in paths],
        "libraryCardField": "../games/nitro_nonsense/assets/menu-card.png",
        "preservation": "Card reference only; no existing entry, save, resource or published ROM changed",
    }
    if not args.check:
        (args.output / "menu-card-source.json").write_text(json.dumps(receipt, indent=2) + "\n")
    else:
        previous = json.loads((args.output / "menu-card-source.json").read_text())
        if previous["sourceSha256"] != hashes or previous["harnessSha256"] != receipt["harnessSha256"]:
            raise RuntimeError("Stale renderer provenance; regenerate the card and source receipt")
    print("PASS production title card: 128x64 monochrome; 512x256 nearest-neighbor preview")


if __name__ == "__main__":
    main()
