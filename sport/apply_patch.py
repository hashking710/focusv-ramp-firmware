#!/usr/bin/env python3
"""
apply_patch.py -- applies the phone-independent ramp patch to your own,
locally-obtained Carta Sport firmware dump.

This script does NOT contain, download, or embed any Focus V firmware. You
supply your own firmware file (the full file as downloaded from Focus V's own
update infrastructure, header included). The script only contains our own
compiled code (ramp_firmware_v1.bin, built from ramp_tick.c/ramp_save.c/
ramp_led.c in this same directory) and a handful of small "patch bytes" --
replacement machine-code instructions, the same size as what they replace,
at specific addresses. See ../LEGAL.md.

Usage:
    python3 apply_patch.py --input your-firmware.bin --output patched.bin

Like the Aeris patch, this has NOT been fingerprinted against a complete
header-included download -- only the stripped firmware body (header
excluded) has a confirmed SHA-1. See this folder's README for exactly
what that gap means.
"""

import argparse
import hashlib
import struct
import sys
from pathlib import Path

SCRIPT_DIR = Path(__file__).resolve().parent

EXPECTED_BODY_SHA1_PREFIX = "4b57f086a175"
EXPECTED_BODY_LEN = 90860

HEADER_LEN = 40
KNLT_OFFSET = 8
LENGTH_FIELD_OFFSET = 24

# Flash region this patch's own code + waypoint storage live in. NOT
# independently flash-verified -- see ramp_tick.c's header and this
# folder's README for the full reasoning.
CODE_INJECT_ADDR = 0x18000
IMAGE_END_ADDR = 0x20000

CODE_BLOB_PATH = SCRIPT_DIR / "ramp_firmware_v1.bin"

# Each entry: (patch address, expected original bytes, replacement bytes).
# Both same-length `tjl <addr>` instruction swaps (4 bytes each).
PATCHES = [
    # Per-tick orchestrator call site, inside the main scheduler loop ->
    # ramp_trampoline, which runs the original tick unmodified, then the
    # ramp sequencer, then the LED progress indicator.
    (0x58b0, bytes.fromhex("0290a699"), bytes.fromhex("1290a69b")),
    # Marker-byte load, just before the stock A5/AF/66 compare chain in the
    # 0xCC handler's marker dispatch -> ramp_marker_entry, same design as
    # the Carta 2 and Aeris patches' equivalent fix.
    (0xb002, bytes.fromhex("35a3fb1c"), bytes.fromhex("0d905199")),
]


def telink_crc32(data: bytes) -> int:
    """Reflected CRC32, poly 0xEDB88320, init 0xFFFFFFFF, NO final XOR --
    same algorithm confirmed against official Focus V images project-wide.
    Not the same as zlib.crc32()."""
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xEDB88320 if crc & 1 else crc >> 1
    return crc & 0xFFFFFFFF


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", required=True, type=Path, help="your own stock firmware file (header included)")
    parser.add_argument("--output", required=True, type=Path, help="where to write the patched, OTA-ready image")
    parser.add_argument(
        "--force",
        action="store_true",
        help="apply the patch even if the input doesn't match the confirmed build fingerprint",
    )
    args = parser.parse_args()

    raw = args.input.read_bytes()
    if len(raw) < HEADER_LEN + 4:
        print("error: that file is too small to be a real firmware image", file=sys.stderr)
        return 1

    magic = raw[KNLT_OFFSET : KNLT_OFFSET + 4]
    if magic != b"KNLT":
        print(f'error: no "KNLT" header at byte 8 (found {magic!r}) -- is this the right file?', file=sys.stderr)
        return 1

    header = bytearray(raw[:HEADER_LEN])
    body = bytearray(raw[HEADER_LEN:])

    fingerprint = hashlib.sha1(bytes(body)).hexdigest()[:12]
    if fingerprint != EXPECTED_BODY_SHA1_PREFIX or len(body) != EXPECTED_BODY_LEN:
        print(
            "warning: this doesn't match the exact build these patch addresses were confirmed\n"
            f"against (fingerprint {EXPECTED_BODY_SHA1_PREFIX}, {EXPECTED_BODY_LEN} bytes).\n"
            f"got fingerprint {fingerprint}, {len(body)} bytes. a different build likely has these\n"
            "call sites at different addresses -- applying this patch anyway will probably corrupt\n"
            "the image rather than patch it cleanly.",
            file=sys.stderr,
        )
        if not args.force:
            print("refusing to continue without --force.", file=sys.stderr)
            return 1
        print("--force given, continuing anyway.", file=sys.stderr)

    for addr, expected, replacement in PATCHES:
        n = len(expected)
        actual = bytes(body[addr : addr + n])
        if actual != expected:
            print(
                f"error: byte mismatch at {addr:#x} -- expected {expected.hex()}, found {actual.hex()}.\n"
                "this firmware doesn't match what this patch was built for; refusing to touch it.",
                file=sys.stderr,
            )
            return 1

    for addr, _expected, replacement in PATCHES:
        body[addr : addr + len(replacement)] = replacement

    if len(body) < IMAGE_END_ADDR:
        body.extend(b"\xff" * (IMAGE_END_ADDR - len(body)))

    code_blob = CODE_BLOB_PATH.read_bytes()
    if CODE_INJECT_ADDR + len(code_blob) > IMAGE_END_ADDR:
        print("error: injected code blob no longer fits its reserved region", file=sys.stderr)
        return 1
    body[CODE_INJECT_ADDR : CODE_INJECT_ADDR + len(code_blob)] = code_blob

    total_len = HEADER_LEN + len(body) + 4
    struct.pack_into("<I", header, LENGTH_FIELD_OFFSET, total_len)

    payload = bytes(header) + bytes(body)
    trailer = struct.pack("<I", telink_crc32(payload))
    final_image = payload + trailer

    args.output.write_bytes(final_image)
    print(f"wrote {args.output} ({len(final_image)} bytes)")
    print("keep your original input file -- push it back any time to revert.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
