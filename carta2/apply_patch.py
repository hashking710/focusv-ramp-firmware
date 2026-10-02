#!/usr/bin/env python3
"""
apply_patch.py -- applies the on-device ramp patch to your own, locally-obtained
Carta 2 (Quantum) firmware file (PROD-111224).

This script does NOT contain, download, or embed any Focus V firmware, and this
repo does not publish the compiled patch either. You build the patch's code
blob yourself from the sources in this repo with tools/build.py, which also
writes this script's patch table and blob hash and runs it end to end on your
file. The script itself contains only small "patch bytes" -- same-length
replacement instructions at specific addresses -- and the hash of the blob they
point into. See ../LEGAL.md.

Usage:
    python3 ../tools/build.py carta2 --firmware your-firmware.bin
    python3 apply_patch.py --input your-firmware.bin --output patched.bin

Safeguards -- nothing is written unless ALL of these hold:
  - the input is exactly the build the patch was verified against
    (SHA-1 of the whole file a6b741dc0508..., 143564-byte body). There is deliberately no override: the blob hard-codes this
    build's RAM layout and function addresses, so on any other build it would
    act on the wrong fields even if every patch site happened to match
  - every patch site holds exactly the expected stock instruction
  - the stock image ends before the patch's code region
  - the blob is the one this patch table was generated for (SHA-256), and it
    ends before the waypoint sector

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

# SHA-1 (first 12 hex) of the complete file as downloaded, header included.
EXPECTED_SHA1_PREFIX = "a6b741dc0508"
EXPECTED_BODY_LEN = 143564  # bytes, header excluded

HEADER_LEN = 40
KNLT_OFFSET = 8
LENGTH_FIELD_OFFSET = 24

# Flash layout (physical address = file offset). The blob goes at file offset
# HEADER_LEN + CODE_INJECT_ADDR, so it runs at CODE_INJECT_ADDR + 0x28, where
# tools/build.py links it; the waypoint store has its own sector after it.
CODE_INJECT_ADDR = 0x30000
WAYPOINT_SECTOR = 0x32000
IMAGE_END_ADDR = 0x33000

CODE_BLOB_PATH = SCRIPT_DIR / "ramp_firmware_v1.bin"   # built locally, never published
BLOB_SHA256 = "985ca392836bbaa895a5261fc254552d48adcd277a6b014f1cff7ba7355ca38c"   # written by tools/build.py

# (address in the header-stripped body, expected stock bytes, replacement).
# Written by tools/build.py: each original decodes to the named stock
# instruction; each replacement is the real assembler's `tjl` to the named
# function in the blob above.
PATCHES = [
    (0x6D0C, bytes.fromhex("fe97849c"), bytes.fromhex("2a908099")),  # tjl 0x5618 -> ramp_event_entry
    (0x6E2E, bytes.fromhex("04907d98"), bytes.fromhex("29906e99")),  # tjl 0xaf2c -> ramp_trampoline
    (0xE706, bytes.fromhex("fe97b39b"), bytes.fromhex("22905b9c")),  # tjl 0xce70 -> ramp_ce70_hide
    (0xE70A, bytes.fromhex("fe97259c"), bytes.fromhex("2290659c")),  # tjl 0xcf58 -> ramp_cf58_hide
    (0xE9B0, bytes.fromhex("fe975e9a"), bytes.fromhex("2290069b")),  # tjl 0xce70 -> ramp_ce70_hide
    (0xE9B6, bytes.fromhex("fe97cf9a"), bytes.fromhex("22900f9b")),  # tjl 0xcf58 -> ramp_cf58_hide
    (0xEB42, bytes.fromhex("fe979599"), bytes.fromhex("22903d9a")),  # tjl 0xce70 -> ramp_ce70_hide
    (0xEB46, bytes.fromhex("fe97079a"), bytes.fromhex("2290479a")),  # tjl 0xcf58 -> ramp_cf58_hide
    (0xF36C, bytes.fromhex("fd97809d"), bytes.fromhex("2190289e")),  # tjl 0xce70 -> ramp_ce70_hide
    (0xF370, bytes.fromhex("fd97f29d"), bytes.fromhex("2190329e")),  # tjl 0xcf58 -> ramp_cf58_hide
    (0xF39C, bytes.fromhex("fe97869c"), bytes.fromhex("2190029e")),  # tjl 0xdcac -> ramp_dcac_view
    (0xF4A8, bytes.fromhex("fe974a9b"), bytes.fromhex("2190a29d")),  # tjl 0xdb40 -> ramp_db40_hide
    (0xF4AC, bytes.fromhex("fd97e09c"), bytes.fromhex("2190889d")),  # tjl 0xce70 -> ramp_ce70_hide
    (0xF4B0, bytes.fromhex("fd97529d"), bytes.fromhex("2190929d")),  # tjl 0xcf58 -> ramp_cf58_hide
    (0xF546, bytes.fromhex("fd973b9f"), bytes.fromhex("2190139d")),  # tjl 0xd3c0 -> ramp_d3c0_view
    (0xF54C, bytes.fromhex("fe97f89a"), bytes.fromhex("2190509d")),  # tjl 0xdb40 -> ramp_db40_hide
    (0xF558, bytes.fromhex("fd978a9c"), bytes.fromhex("2190329d")),  # tjl 0xce70 -> ramp_ce70_hide
    (0xF55C, bytes.fromhex("fd97fc9c"), bytes.fromhex("21903c9d")),  # tjl 0xcf58 -> ramp_cf58_hide
    (0xFA30, bytes.fromhex("fd971e9a"), bytes.fromhex("2190c69a")),  # tjl 0xce70 -> ramp_ce70_hide
    (0xFA34, bytes.fromhex("fd97909a"), bytes.fromhex("2190d09a")),  # tjl 0xcf58 -> ramp_cf58_hide
    (0xFA38, bytes.fromhex("fd97c29c"), bytes.fromhex("2190869a")),  # tjl 0xd3c0 -> ramp_d3c0_full
    (0xFA3C, bytes.fromhex("fe973699"), bytes.fromhex("2190a69a")),  # tjl 0xdcac -> ramp_dcac_full
    (0xFA40, bytes.fromhex("fe977e98"), bytes.fromhex("2190d69a")),  # tjl 0xdb40 -> ramp_db40_hide
    (0x11D96, bytes.fromhex("28a3eb1c"), bytes.fromhex("1f908799")),  # tmovs r3, #40 -> ramp_marker_entry
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

    fingerprint = hashlib.sha1(raw).hexdigest()[:12]
    if fingerprint != EXPECTED_SHA1_PREFIX or len(body) != EXPECTED_BODY_LEN:
        return fail(f"not the verified build: fingerprint {fingerprint}, {len(body)} bytes; "
                    f"expected {EXPECTED_SHA1_PREFIX}, {EXPECTED_BODY_LEN} bytes")

    if HEADER_LEN + len(body) > HEADER_LEN + CODE_INJECT_ADDR:
        return fail("the stock image runs into the patch's code region")

    for addr, expected, _ in PATCHES:
        actual = bytes(body[addr:addr + len(expected)])
        if actual != expected:
            return fail(f"byte mismatch at {addr:#x}: expected {expected.hex()}, found {actual.hex()}")

    if not CODE_BLOB_PATH.exists():
        return fail(f"{CODE_BLOB_PATH.name} not found -- build it first: "
                    "python3 ../tools/build.py carta2 --firmware <your file>")
    code_blob = CODE_BLOB_PATH.read_bytes()
    if hashlib.sha256(code_blob).hexdigest() != BLOB_SHA256:
        return fail(f"{CODE_BLOB_PATH.name} is not the blob this patch table was generated for -- "
                    "rebuild with tools/build.py")
    if HEADER_LEN + CODE_INJECT_ADDR + len(code_blob) > WAYPOINT_SECTOR:
        return fail("the code blob would run into the waypoint sector")

    for addr, _, replacement in PATCHES:
        body[addr:addr + len(replacement)] = replacement
    # Grow the image with erased flash (0xFF) up to IMAGE_END_ADDR: the stock
    # trailer left at the end of `body` sits unused in this gap, and the
    # waypoint sector ships erased, so a flash always starts with an empty store.
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
