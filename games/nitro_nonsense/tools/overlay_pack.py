#!/usr/bin/env python3
"""Extract bounded overlay resources and patch a fixed resident metadata table.

The linked ELF retains every group for inspection. The emitted resident ELF/bin
contain no high-address load segments. Overlay debug sections remain in the ELF
without SHF_ALLOC, retaining symbols while keeping them out of the boot image.
Runtime EEPROM access is read-only.
This tool does not install or rewrite an existing console library.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import sys
import zlib

# plataboy26 library build: the groups are stored as LZ streams of the library format,
# unpacked by the library kernel (src/nn_unpack.S)
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "firmware" / "tools"))
from lz import compress, decompress  # noqa: E402


def compress_cached(data):
    """compress(), remembered in firmware/tools/.lzcache/ (shared with pack.py)"""
    d = Path(__file__).resolve().parents[3] / "firmware" / "tools" / ".lzcache"
    f = d / (hashlib.sha1(data).hexdigest() + ".lz")
    if f.exists():
        return f.read_bytes()
    c = compress(data)
    assert decompress(c, len(data)) == data
    d.mkdir(exist_ok=True)
    f.write_bytes(c)
    return c

MAGIC = 0x564F4E4E
META_SIZE = 112
PAGE = 64
KBASE = 0x3C40
EE_SIZE = 0x20000
RESERVED = ((0, 0x800), (0xFF00, 0x10100), (0x1F000, 0x1F800))


class FormatError(ValueError):
    """Reject an image before emitting any installable artifact."""


class Elf:
    def __init__(self, raw):
        self.raw = bytes(raw)
        if len(raw) < 52 or raw[:7] != b"\x7fELF\x01\x01\x01":
            raise FormatError("Expected a little-endian ELF32 image")
        header = struct.unpack_from("<16sHHIIIIIHHHHHH", raw)
        if header[2] != 243:
            raise FormatError("Expected the RISC-V machine type")
        self.sections = {}
        self.symbols = {}
        self.loads = []
        phoff, shoff, phsize, phcount, shsize, shcount, shstrings = (
            header[5], header[6], header[9], header[10], header[11], header[12], header[13]
        )
        if phsize != 32 or shsize != 40 or shstrings >= shcount:
            raise FormatError("Unsupported or malformed ELF tables")
        if phoff + phsize * phcount > len(raw) or shoff + shsize * shcount > len(raw):
            raise FormatError("Truncated ELF tables")
        sections = [struct.unpack_from("<10I", raw, shoff + i * shsize) for i in range(shcount)]

        def contents(section):
            if section[1] == 8:
                return b""
            offset, size = section[4:6]
            if offset > len(raw) or size > len(raw) - offset:
                raise FormatError("Truncated ELF section")
            return raw[offset:offset + size]

        strings = contents(sections[shstrings])

        def string(table, index):
            if index >= len(table):
                raise FormatError("Invalid ELF string reference")
            end = table.find(b"\0", index)
            if end < 0:
                raise FormatError("Unterminated ELF string")
            return table[index:end].decode("utf-8")

        self.header = header
        for index, section in enumerate(sections):
            name = string(strings, section[0])
            self.sections[name] = {"address": section[3], "offset": section[4],
                                   "size": section[5], "data": contents(section),
                                   "flags": section[2], "headerOffset": shoff + index * shsize}
            if section[1] == 2:
                if section[6] >= shcount or section[9] != 16 or section[5] % 16:
                    raise FormatError("Invalid ELF symbol table")
                names = contents(sections[section[6]])
                for offset in range(section[4], section[4] + section[5], 16):
                    symbol = struct.unpack_from("<IIIBBH", raw, offset)
                    if symbol[0]:
                        self.symbols[string(names, symbol[0])] = symbol[1]
        for i in range(phcount):
            segment = struct.unpack_from("<8I", raw, phoff + i * phsize)
            if segment[0] == 1 and segment[4]:
                offset, length = segment[1], segment[4]
                if offset > len(raw) or length > len(raw) - offset:
                    raise FormatError("Truncated ELF load segment")
                self.loads.append((segment[3], raw[offset:offset + length]))

    def resident(self, base):
        loads = [(address, data) for address, data in self.loads if address < base]
        if not loads or any(address + len(data) > base for address, data in loads):
            raise FormatError("Resident image extends into the overlay window")
        image = bytearray(b"\xff" * max(address + len(data) for address, data in loads))
        owned = set()
        for address, data in loads:
            for i, byte in enumerate(data, address):
                if i in owned and image[i] != byte:
                    raise FormatError("Conflicting resident load segments")
                image[i] = byte
                owned.add(i)
        for address, _ in self.loads:
            if base <= address < 0x40000000:
                raise FormatError("Unexpected nonresident load address")
        if len(image) < 8 or struct.unpack_from("<I", image)[0] & 0xFFF != 0x06F:
            raise FormatError("Resident word zero must jump to reset")
        return image

    def resident_elf(self, patched):
        """Disable resource loads without objcopy reconstructing RAM program headers."""
        result = bytearray(patched)
        for name in [*(f".ov{group}" for group in range(1, 5)), ".ram_loader"]:
            section = self.sections.get(name)
            if not section:
                continue
            struct.pack_into("<I", result, section["headerOffset"] + 8, section["flags"] & ~2)
        phoff, phsize, phcount = self.header[5], self.header[9], self.header[10]
        for index in range(phcount):
            at = phoff + index * phsize
            segment = struct.unpack_from("<8I", result, at)
            if segment[0] == 1 and segment[3] >= 0x40000000 and segment[4]:
                struct.pack_into("<I", result, at, 0)  # PT_NULL; preserve all other offsets.
        return bytes(result)


def align(size):
    return (size + PAGE - 1) & -PAGE


def source_inputs(game, extra):
    paths = sorted({*game.joinpath("src").glob("*.c"), *game.joinpath("src").glob("*.h"),
                    *game.joinpath("src").glob("*.inc"), game / "makefile",
                    game / "ld/overlay.ld", Path(__file__).resolve(),
                    *(Path(p).resolve() for p in extra)})
    return [p for p in paths if p.is_file()]


def build(elf_path, eeprom_base, identity_inputs=(), test_build_id=None):
    elf_path = Path(elf_path)
    elf = Elf(elf_path.read_bytes())
    try:
        base = elf.symbols["__nn_overlay_base"]
        size = elf.symbols["__nn_overlay_size"]
        metadata_address = elf.symbols["__nn_overlay_meta"]
        metadata_section = elf.sections[".nn_overlay_meta"]
    except KeyError as error:
        raise FormatError(f"Missing required overlay link symbol/section: {error}") from error
    if base <= 0 or (base | size) & (PAGE - 1) or base + size != KBASE:
        raise FormatError("Invalid declared flash window")
    if metadata_section["size"] != META_SIZE or metadata_section["address"] != metadata_address:
        raise FormatError("Metadata reservation must be exactly 112 bytes")
    if metadata_address + META_SIZE > base:
        raise FormatError("Metadata must remain resident")
    resident = elf.resident(base)
    resident[metadata_address:metadata_address + META_SIZE] = bytes(META_SIZE)
    region = bytearray(b"\xff" * align(META_SIZE))
    groups = []
    full_images = []
    for group in range(1, 5):
        section = elf.sections.get(f".ov{group}")
        if not section or section["address"] != base:
            raise FormatError(f"Group {group} does not use the declared window")
        data = section["data"]
        if len(data) < 4 or len(data) > size or struct.unpack_from("<I", data)[0] != group:
            raise FormatError(f"Group {group} has an invalid tag or size")
        full = data.ljust(size, b"\xff")
        stored = compress_cached(full)
        if len(stored) > 0xFFFF:
            raise FormatError(f"Group {group} stream too large")
        offset = len(region)
        region.extend(stored)
        full_images.append(full)
        groups.append({"id": group, "section": f".ov{group}", "offset": offset,
                       "actualBytes": len(data), "storedBytes": len(stored),
                       "crc32": zlib.crc32(full), "windowSha256": hashlib.sha256(full).hexdigest()})
    ram_loader = {"offset": 0, "actualBytes": 0, "storedBytes": 0, "crc32": 0,
                  "note": "none: the library kernel unpacks the groups"}
    if eeprom_base < 0 or eeprom_base >= EE_SIZE or len(region) > EE_SIZE - eeprom_base:
        raise FormatError("Resource allocation exceeds the physical EEPROM")
    end = eeprom_base + len(region)
    if eeprom_base < 0x10000 < end or any(eeprom_base < hi and end > lo for lo, hi in RESERVED):
        raise FormatError("Resource allocation crosses a bank, metadata, test or save area")
    game = Path(__file__).resolve().parents[1]
    inputs = source_inputs(game, identity_inputs)
    digest = hashlib.sha256(b"Nitro Nonsense overlays v3\0")
    digest.update(resident)
    for full in full_images:
        digest.update(full)
    digest.update(struct.pack("<III", base, size, eeprom_base))
    input_hashes = {}
    for path in inputs:
        data = path.read_bytes()
        try:
            name = str(path.relative_to(game))
        except ValueError:
            name = path.name
        digest.update(name.encode() + b"\0" + hashlib.sha256(data).digest())
        input_hashes[name] = hashlib.sha256(data).hexdigest()
    build_id = digest.digest()
    if test_build_id is not None:
        if "native_bus_high" not in elf.symbols:
            raise FormatError("Identity overrides require the diagnostic native_bus_high symbol")
        try:
            build_id = bytes.fromhex(test_build_id)
        except ValueError as error:
            raise FormatError("Diagnostic identity must be hexadecimal") from error
        if len(build_id) != 32:
            raise FormatError("Diagnostic identity must contain exactly 32 bytes")
    metadata = struct.pack("<IBBBBIIHH32s", MAGIC, 3, 4, 6, 0, eeprom_base, len(region), base, size, build_id)
    for group in [*groups, ram_loader]:
        metadata += struct.pack("<IHHI", group["offset"], group["actualBytes"],
                                group["storedBytes"], group["crc32"])
    assert len(metadata) == META_SIZE
    region[:META_SIZE] = metadata
    resident[metadata_address:metadata_address + META_SIZE] = metadata
    patched = bytearray(elf.raw)
    offset = metadata_section["offset"]
    patched[offset:offset + META_SIZE] = metadata
    policy = {"format": "nn-overlays-v3-lz", "ramLoader": ram_loader,
              "testInstrumentation": "native_bus_high" in elf.symbols, "buildId": build_id.hex(),
              "testInstrumentationIdentityOverride": test_build_id is not None,
              "residentLoadBytes": len(resident), "residentCeiling": base,
              "residentHeadroomBytes": base - len(resident),
              "reservedWindowBytes": size, "occupiedFlashBytes": len(resident) + size,
              "applicationCeiling": KBASE, "window": {"start": base, "size": size, "end": base + size},
              "sourceRegion": {"address": eeprom_base, "length": len(region),
                               "sha256": hashlib.sha256(region).hexdigest()},
              "groups": groups, "metadataAddress": metadata_address,
              "metadataSize": META_SIZE, "sourceSha256": input_hashes,
              "residentSha256": hashlib.sha256(resident).hexdigest(),
              "runtimeEepromWrites": "forbidden", "deployment": "resource image only; no existing library is rewritten"}
    return bytes(patched), bytes(resident), bytes(region), policy


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--elf", required=True, type=Path, help="Full linked ELF, including all overlay sections")
    parser.add_argument("--resident-elf", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--region", required=True, type=Path)
    parser.add_argument("--policy", required=True, type=Path)
    parser.add_argument("--eeprom-base", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--objcopy", required=True)
    parser.add_argument("--identity-input", action="append", default=[])
    parser.add_argument("--test-build-id", help="Diagnostic-only production identity; requires native_bus_high")
    args = parser.parse_args()
    try:
        patched, resident, region, policy = build(args.elf, args.eeprom_base,
                                                  args.identity_input, args.test_build_id)
        for path in (args.resident_elf, args.output, args.region, args.policy):
            path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="nn-overlay-", dir=args.output.parent) as temporary:
            temporary = Path(temporary)
            result = temporary / "resident.elf"
            original = Elf(patched)
            result.write_bytes(original.resident_elf(patched))
            converted = temporary / "resident.bin"
            subprocess.run([args.objcopy, "-O", "binary", str(result), str(converted)], check=True)
            if converted.read_bytes() != resident or bytes(Elf(result.read_bytes()).resident(policy["window"]["start"])) != resident:
                raise FormatError("Resident ELF extraction changed the linked image")
            os.replace(result, args.resident_elf)
        args.output.write_bytes(resident)
        args.region.write_bytes(region)
        args.policy.write_text(json.dumps(policy, indent=2) + "\n")
        print(f"Resident {len(resident)}/{policy['residentCeiling']} bytes; "
              f"window {policy['reservedWindowBytes']}; headroom {policy['residentHeadroomBytes']}; "
              f"EEPROM resource {len(region)} bytes")
    except (FormatError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"overlay_pack: {error}\n")


if __name__ == "__main__":
    main()
