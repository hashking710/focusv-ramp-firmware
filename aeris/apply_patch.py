#!/usr/bin/env python3
"""
apply_patch.py -- applies the on-device ramp patch to your own, locally-obtained
Aeris firmware file (PROD-111224).

This script does NOT contain, download, or embed any Focus V firmware, and this
repo does not publish the compiled patch either. You build the patch's code
blob yourself from the sources in this repo with tools/build.py, which also
writes this script's patch table and blob hash and runs it end to end on your
file. The script itself contains only small "patch bytes" -- same-length
replacement instructions at specific addresses -- and the hash of the blob they
point into. See ../LEGAL.md.

Usage:
    python3 ../tools/build.py aeris --firmware your-firmware.bin
    python3 apply_patch.py --input your-firmware.bin --output patched.bin

Safeguards -- nothing is written unless ALL of these hold:
  - the input is exactly the build the patch was verified against
    (SHA-1 of the header-stripped body 7e3569fabd06..., 80684-byte body). There is deliberately no override: the blob hard-codes this
    build's RAM layout and function addresses, so on any other build it would
    act on the wrong fields even if every patch site happened to match
  - every patch site holds exactly the expected stock instruction
  - the stock image ends before the patch's code region
  - the blob is the one this patch table was generated for (SHA-256), and it
    ends before the image end

The output is a complete OTA image (header + body + fresh Telink CRC32
trailer). Keep your original file: flashing it back is a full revert.
NOT TESTED ON HARDWARE -- see the README's warning before flashing anything.
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent

# SHA-1 (first 12 hex) of the body (everything after the 40-byte header).
EXPECTED_BODY_SHA1_PREFIX = "7e3569fabd06"
EXPECTED_BODY_LEN = 80684

HEADER_LEN = 40
KNLT_OFFSET = 8
LENGTH_FIELD_OFFSET = 24

# Flash layout (physical address = file offset). The blob goes at file offset
# HEADER_LEN + CODE_INJECT_ADDR, so it runs at CODE_INJECT_ADDR + 0x28, where
# tools/build.py links it, and the image ends at IMAGE_END_ADDR. The ramp
# store is NOT in the image: it has its own sector outside both OTA banks
# (DEV_RAMP_FLASH in device.h), because stock erases the other bank at boot.
CODE_INJECT_ADDR = 0x14000
IMAGE_END_ADDR = 0x17000

CODE_BLOB_PATH = SCRIPT_DIR / "ramp_firmware_aeris_v1.bin"   # built locally, never published
BLOB_SHA256 = "cea27870e6173a402f144434809622935352128b62b391d678f77505f65eae0f"   # written by tools/build.py

# (address in the header-stripped body, expected stock bytes, replacement).
# Written by tools/build.py: each original decodes to the named stock
# instruction; each replacement is the real assembler's `tjl` to the named
# function in the blob above.
PATCHES = [
    (0x61AE, bytes.fromhex("03902d98"), bytes.fromhex("0e90279f")),  # tjl 0x920c -> ramp_led_entry
    (0x645C, bytes.fromhex("fe97449d"), bytes.fromhex("0e908e9f")),  # tjl 0x4ee8 -> ramp_event_entry
    (0x6464, bytes.fromhex("0190769e"), bytes.fromhex("0d90ac9e")),  # tjl 0x8154 -> ramp_trampoline
    (0xB066, bytes.fromhex("0390659b"), bytes.fromhex("09900d9f")),  # tjl 0xe734 -> ramp_announce_entry
    (0xB490, bytes.fromhex("35a3fb1c"), bytes.fromhex("0a903e98")),  # tmovs r3, #53 -> ramp_marker_entry
]


def telink_crc32(data: bytes) -> int:
    """Reflected CRC32, poly 0xEDB88320, init 0xFFFFFFFF, NO final XOR (the
    Telink OTA trailer). Not the same as zlib.crc32()."""
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xEDB88320 if crc & 1 else crc >> 1
    return crc & 0xFFFFFFFF


def fail(msg: str) -> int:
    print("error: " + msg, file=sys.stderr)
    print("nothing was written.", file=sys.stderr)
    return 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", required=True, type=Path, help="your own stock firmware file (header included)")
    parser.add_argument("--output", required=True, type=Path, help="where to write the patched OTA image")
    args = parser.parse_args()

    raw = args.input.read_bytes()
    if len(raw) < HEADER_LEN + 4 or raw[KNLT_OFFSET:KNLT_OFFSET + 4] != b"KNLT":
        return fail('not a Focus V firmware image (no "KNLT" header at byte 8)')

    header = bytearray(raw[:HEADER_LEN])
    body = bytearray(raw[HEADER_LEN:])   # includes the stock trailer; harmless, see below

    fingerprint = hashlib.sha1(bytes(body)).hexdigest()[:12]
    if fingerprint != EXPECTED_BODY_SHA1_PREFIX or len(body) != EXPECTED_BODY_LEN:
        return fail(f"not the verified build: fingerprint {fingerprint}, {len(body)} bytes; "
                    f"expected {EXPECTED_BODY_SHA1_PREFIX}, {EXPECTED_BODY_LEN} bytes")

    if HEADER_LEN + len(body) > HEADER_LEN + CODE_INJECT_ADDR:
        return fail("the stock image runs into the patch's code region")

    for addr, expected, _ in PATCHES:
        actual = bytes(body[addr:addr + len(expected)])
        if actual != expected:
            return fail(f"byte mismatch at {addr:#x}: expected {expected.hex()}, found {actual.hex()}")

    if not CODE_BLOB_PATH.exists():
        return fail(f"{CODE_BLOB_PATH.name} not found -- build it first: "
                    "python3 ../tools/build.py aeris --firmware <your file>")
    code_blob = CODE_BLOB_PATH.read_bytes()
    if hashlib.sha256(code_blob).hexdigest() != BLOB_SHA256:
        return fail(f"{CODE_BLOB_PATH.name} is not the blob this patch table was generated for -- "
                    "rebuild with tools/build.py")
    if HEADER_LEN + CODE_INJECT_ADDR + len(code_blob) > IMAGE_END_ADDR:
        return fail("the code blob would run past the image end")

    for addr, _, replacement in PATCHES:
        body[addr:addr + len(replacement)] = replacement
    # Grow the image with erased flash (0xFF) up to IMAGE_END_ADDR: the stock
    # trailer left at the end of `body` sits unused in this gap.
    body.extend(b"\xff" * (IMAGE_END_ADDR - HEADER_LEN - len(body)))
    body[CODE_INJECT_ADDR:CODE_INJECT_ADDR + len(code_blob)] = code_blob

    struct.pack_into("<I", header, LENGTH_FIELD_OFFSET, HEADER_LEN + len(body) + 4)
    payload = bytes(header) + bytes(body)
    final_image = payload + struct.pack("<I", telink_crc32(payload))

    args.output.write_bytes(final_image)
    print(f"wrote {args.output} ({len(final_image)} bytes)")
    print("keep your original file -- flashing it back is a full revert.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
