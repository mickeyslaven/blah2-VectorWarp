#!/usr/bin/env python3
"""Generate Raspberry Pi Imager metadata for an existing Pi 4 image.

This does not build, download, flash, or publish an image. Imager's manifest
format is documented at https://github.com/raspberrypi/rpi-imager/tree/main/doc.
"""

import argparse
from datetime import date
import hashlib
import json
import lzma
import os
from pathlib import Path
import tempfile
from urllib.parse import urlsplit


CHUNK = 1024 * 1024


def https_url(value, label):
    if any(ord(c) <= 32 or ord(c) == 127 for c in value):
        raise ValueError(f"{label} must not contain whitespace or ASCII controls")
    parsed = urlsplit(value)
    if (parsed.scheme != "https" or not parsed.hostname or parsed.username is not None
            or parsed.password is not None or parsed.query or parsed.fragment
            or not parsed.path.startswith("/") or parsed.path.endswith("/")):
        raise ValueError(f"{label} must be an HTTPS file URL without credentials, query, or fragment")
    try:
        _ = parsed.port
    except ValueError as error:
        raise ValueError(f"{label} has an invalid port") from error
    return value


def image_digests(path):
    compressed = hashlib.sha256()
    extracted = hashlib.sha256()
    compressed_size = extracted_size = 0
    decoder = lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
    with path.open("rb") as source:
        for block in iter(lambda: source.read(CHUNK), b""):
            compressed.update(block)
            compressed_size += len(block)
            if decoder.eof:
                raise ValueError("XZ has trailing data or another stream")
            try:
                data = decoder.decompress(block, max_length=CHUNK)
                extracted.update(data)
                extracted_size += len(data)
                while not decoder.needs_input and not decoder.eof:
                    data = decoder.decompress(b"", max_length=CHUNK)
                    extracted.update(data)
                    extracted_size += len(data)
            except lzma.LZMAError as error:
                raise ValueError("invalid XZ image") from error
            if decoder.unused_data:
                raise ValueError("XZ has trailing data or another stream")
    if not decoder.eof or not compressed_size or not extracted_size:
        raise ValueError("XZ image is truncated or empty")
    return compressed.hexdigest(), compressed_size, extracted.hexdigest(), extracted_size


def generate(image, url, icon_url, version, release_date, output):
    image = Path(image)
    output = Path(output)
    if image.suffixes[-2:] != [".img", ".xz"] or not image.is_file():
        raise ValueError("--image must be an existing .img.xz file")
    if output.suffix != ".rpi-imager-manifest":
        raise ValueError("--output must end in .rpi-imager-manifest")
    https_url(url, "--url")
    https_url(icon_url, "--icon-url")
    if urlsplit(url).path.rsplit("/", 1)[-1] != image.name:
        raise ValueError("image URL filename must match the supplied .img.xz file")
    if not version or any(c not in "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz._-" for c in version):
        raise ValueError("--version must be a nonempty release identifier")
    try:
        if date.fromisoformat(release_date).isoformat() != release_date:
            raise ValueError
    except ValueError as error:
        raise ValueError("--release-date must be YYYY-MM-DD") from error
    download_sha, download_size, extract_sha, extract_size = image_digests(image)
    manifest = {"imager": {"devices": [{
        "name": "Raspberry Pi 4 Model B",
        "description": "Raspberry Pi 4 Model B",
        "tags": ["pi4"],
        "matching_type": "exclusive",
        "architecture": "armv8",
    }]}, "os_list": [{
        "name": f"VectorWarp Pi 4 ({version})",
        "description": "Headless VectorWarp for Raspberry Pi 4, 64-bit Bookworm Lite",
        "icon": icon_url,
        "url": url,
        "extract_size": extract_size,
        "extract_sha256": extract_sha,
        "image_download_size": download_size,
        "image_download_sha256": download_sha,
        "release_date": release_date,
        "devices": ["pi4"],
        "architecture": "armv8",
        "init_format": "systemd",
    }]}
    # Build and verify all metadata before creating the temporary output. An
    # invalid image never replaces an existing manifest or leaves a partial one.
    payload = (json.dumps(manifest, indent=2) + "\n").encode("utf-8")
    fd, temporary = tempfile.mkstemp(prefix=f".{output.name}.", dir=output.parent)
    try:
        with os.fdopen(fd, "wb") as destination:
            destination.write(payload)
            destination.flush()
            os.fsync(destination.fileno())
        os.replace(temporary, output)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)
    return manifest


def main():
    parser = argparse.ArgumentParser(description="Generate Imager metadata for an existing Pi 4 .img.xz; does not build, flash, download, or publish an image.")
    parser.add_argument("--image", required=True, help="existing compressed .img.xz file")
    parser.add_argument("--url", required=True, help="immutable HTTPS download URL for that exact image filename")
    parser.add_argument("--icon-url", required=True, help="HTTPS URL for the OS icon")
    parser.add_argument("--version", required=True, help="image release identifier shown in Imager")
    parser.add_argument("--release-date", required=True, help="image release date, YYYY-MM-DD")
    parser.add_argument("--output", required=True, help="output .rpi-imager-manifest path")
    args = parser.parse_args()
    try:
        generate(args.image, args.url, args.icon_url, args.version, args.release_date, args.output)
    except (OSError, ValueError) as error:
        parser.exit(1, f"generate-imager-manifest: {error}\n")


if __name__ == "__main__":
    main()
