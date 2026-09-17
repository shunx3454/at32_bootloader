#!/usr/bin/env python3
"""Create the signed 4-KiB-header firmware package used by the bootloader."""

import argparse
import hashlib
import pathlib
import struct
import subprocess
import zlib

FW_MAGIC = 0x57465441
TARGET_MCU = 0x403A0001
HEADER_SIZE = 0x1000
IMAGE_CAPACITY = 0x5F000
SLOTS = {
    "A": (0, 0x08021000),
    "B": (1, 0x08091000),
}


def integer(value: str) -> int:
    return int(value, 0)


def der_length(data: bytes, offset: int) -> tuple[int, int]:
    first = data[offset]
    offset += 1
    if first < 0x80:
        return first, offset
    count = first & 0x7F
    if count == 0 or count > 2:
        raise ValueError("unsupported DER length")
    return int.from_bytes(data[offset:offset + count], "big"), offset + count


def der_signature_to_raw(signature: bytes) -> bytes:
    offset = 0
    if signature[offset] != 0x30:
        raise ValueError("ECDSA signature is not a DER sequence")
    sequence_length, offset = der_length(signature, offset + 1)
    if offset + sequence_length != len(signature):
        raise ValueError("invalid DER sequence length")
    values = []
    for _ in range(2):
        if signature[offset] != 0x02:
            raise ValueError("ECDSA signature does not contain two integers")
        length, offset = der_length(signature, offset + 1)
        value = signature[offset:offset + length]
        offset += length
        value = value.lstrip(b"\0")
        if len(value) > 32:
            raise ValueError("ECDSA integer exceeds P-256 size")
        values.append(value.rjust(32, b"\0"))
    return b"".join(values)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=pathlib.Path,
                        help="App body .bin linked at the selected slot vector")
    parser.add_argument("--slot", required=True, choices=SLOTS)
    parser.add_argument("--private-key", required=True, type=pathlib.Path)
    parser.add_argument("--public-key", type=pathlib.Path,
                        help="optional product public PEM; rejects a mismatched private key")
    parser.add_argument("--firmware-version", required=True, type=integer)
    parser.add_argument("--security-version", required=True, type=integer)
    parser.add_argument("--build-id", help="optional 16-byte hexadecimal build id")
    parser.add_argument("--output", required=True, type=pathlib.Path)
    args = parser.parse_args()

    if args.public_key:
        private_public = subprocess.run(
            ["openssl", "pkey", "-in", str(args.private_key),
             "-pubout", "-outform", "DER"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        installed_public = subprocess.run(
            ["openssl", "pkey", "-pubin", "-in", str(args.public_key),
             "-pubout", "-outform", "DER"],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        if private_public.returncode != 0:
            raise SystemExit(private_public.stderr.decode(errors="replace"))
        if installed_public.returncode != 0:
            raise SystemExit(installed_public.stderr.decode(errors="replace"))
        if private_public.stdout != installed_public.stdout:
            raise SystemExit("private key does not match --public-key")

    body = args.input.read_bytes()
    if not 8 <= len(body) <= IMAGE_CAPACITY:
        raise SystemExit(f"invalid image size {len(body)} (capacity {IMAGE_CAPACITY})")

    slot_id, vector_address = SLOTS[args.slot]
    msp, entry_address = struct.unpack_from("<II", body)
    if not (0x20000000 <= msp <= 0x20018000 and msp % 8 == 0):
        raise SystemExit(f"invalid initial MSP 0x{msp:08x}; is this an App {args.slot} binary?")
    if not (entry_address & 1 and
            vector_address <= entry_address < vector_address + len(body)):
        raise SystemExit(
            f"reset handler 0x{entry_address:08x} is outside App {args.slot}")

    image_hash = hashlib.sha256(body).digest()
    if args.build_id:
        build_id = bytes.fromhex(args.build_id)
        if len(build_id) != 16:
            raise SystemExit("--build-id must contain exactly 16 bytes")
    else:
        build_id = image_hash[:16]

    manifest = struct.pack(
        "<IHH11I32s16s",
        FW_MAGIC, 1, HEADER_SIZE, TARGET_MCU, slot_id,
        args.firmware_version, args.security_version, len(body),
        vector_address, vector_address, entry_address,
        1, 1, 1, image_hash, build_id,
    )
    assert len(manifest) == 100
    result = subprocess.run(
        ["openssl", "dgst", "-sha256", "-sign", str(args.private_key)],
        input=manifest, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        check=False,
    )
    if result.returncode != 0:
        raise SystemExit(result.stderr.decode(errors="replace"))
    signature = der_signature_to_raw(result.stdout)

    reserved_size = HEADER_SIZE - len(manifest) - len(signature) - 4
    header_without_crc = manifest + signature + bytes(reserved_size)
    assert len(header_without_crc) == HEADER_SIZE - 4
    header = header_without_crc + struct.pack("<I", zlib.crc32(header_without_crc))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(header + body)
    print(f"package: {args.output}")
    print(f"slot: App {args.slot}, vector=0x{vector_address:08x}, entry=0x{entry_address:08x}")
    print(f"body: {len(body)} bytes, SHA-256={image_hash.hex()}")
    print(f"firmware_version={args.firmware_version}, security_version={args.security_version}")


if __name__ == "__main__":
    main()
