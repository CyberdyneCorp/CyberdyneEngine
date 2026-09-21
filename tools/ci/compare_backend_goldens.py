#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compare committed backend captures and verify the device evidence beside them."""

from __future__ import annotations

import argparse
import shlex
import struct
import sys
import tempfile
import zlib
from dataclasses import dataclass
from pathlib import Path


CHANNEL_TOLERANCE = 2
EDGE_CONTRAST = 32


@dataclass(frozen=True)
class Image:
    width: int
    height: int
    pixels: bytes

    def rgb(self, x: int, y: int) -> tuple[int, int, int]:
        offset = (y * self.width + x) * 3
        return tuple(self.pixels[offset : offset + 3])  # type: ignore[return-value]


def _paeth(left: int, above: int, upper_left: int) -> int:
    estimate = left + above - upper_left
    distances = (abs(estimate - left), abs(estimate - above), abs(estimate - upper_left))
    return (left, above, upper_left)[distances.index(min(distances))]


def read_png(path: Path) -> Image:
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path}: not a PNG")
    offset = 8
    width = height = 0
    compressed = bytearray()
    while offset + 12 <= len(data):
        length = struct.unpack(">I", data[offset : offset + 4])[0]
        kind = data[offset + 4 : offset + 8]
        body = data[offset + 8 : offset + 8 + length]
        offset += 12 + length
        if kind == b"IHDR":
            width, height, depth, colour, compression, filtering, interlace = struct.unpack(
                ">IIBBBBB", body
            )
            if (depth, colour, compression, filtering, interlace) != (8, 2, 0, 0, 0):
                raise ValueError(f"{path}: expected 8-bit non-interlaced RGB PNG")
        elif kind == b"IDAT":
            compressed.extend(body)
        elif kind == b"IEND":
            break
    if width == 0 or height == 0:
        raise ValueError(f"{path}: missing IHDR")
    raw = zlib.decompress(compressed)
    stride = width * 3
    expected = height * (stride + 1)
    if len(raw) != expected:
        raise ValueError(f"{path}: decoded {len(raw)} bytes, expected {expected}")
    pixels = bytearray(height * stride)
    source = 0
    for y in range(height):
        filter_kind = raw[source]
        source += 1
        for x in range(stride):
            value = raw[source]
            source += 1
            left = pixels[y * stride + x - 3] if x >= 3 else 0
            above = pixels[(y - 1) * stride + x] if y else 0
            upper_left = pixels[(y - 1) * stride + x - 3] if y and x >= 3 else 0
            if filter_kind == 1:
                value += left
            elif filter_kind == 2:
                value += above
            elif filter_kind == 3:
                value += (left + above) // 2
            elif filter_kind == 4:
                value += _paeth(left, above, upper_left)
            elif filter_kind != 0:
                raise ValueError(f"{path}: unsupported PNG filter {filter_kind}")
            pixels[y * stride + x] = value & 0xFF
    return Image(width, height, bytes(pixels))


def channel_delta(left: tuple[int, int, int], right: tuple[int, int, int]) -> int:
    return max(abs(a - b) for a, b in zip(left, right))


def on_edge(reference: Image, x: int, y: int) -> bool:
    here = reference.rgb(x, y)
    for nx, ny in ((x - 1, y), (x + 1, y), (x, y - 1), (x, y + 1)):
        if 0 <= nx < reference.width and 0 <= ny < reference.height:
            if channel_delta(here, reference.rgb(nx, ny)) > EDGE_CONTRAST:
                return True
    return False


def compare(reference: Image, candidate: Image) -> tuple[int, int, int, int]:
    if (reference.width, reference.height) != (candidate.width, candidate.height):
        raise ValueError(
            f"image dimensions differ: {reference.width}x{reference.height} versus "
            f"{candidate.width}x{candidate.height}"
        )
    differing = off_edge = edge_texels = maximum = 0
    for y in range(reference.height):
        for x in range(reference.width):
            edge = on_edge(reference, x, y)
            edge_texels += int(edge)
            delta = channel_delta(reference.rgb(x, y), candidate.rgb(x, y))
            maximum = max(maximum, delta)
            if delta > CHANNEL_TOLERANCE:
                differing += 1
                off_edge += int(not edge)
    return differing, off_edge, edge_texels, maximum


def read_manifest(path: Path, backend: str) -> dict[str, str]:
    lines = path.read_text(encoding="utf-8").splitlines()
    if not lines or lines[0] != "cygolden 1":
        raise ValueError(f"{path}: unsupported or missing cygolden header")
    for line in lines:
        if not line.startswith("row "):
            continue
        fields = dict(token.split("=", 1) for token in shlex.split(line)[1:])
        if fields.get("backend") == backend:
            return fields
    raise ValueError(f"{path}: no row for backend={backend}")


def validate_evidence(backend: str, evidence: dict[str, str]) -> None:
    if evidence.get("outcome") != "matched":
        raise ValueError(f"{backend}: ledger outcome is {evidence.get('outcome', 'missing')}")
    if evidence.get("class") in (None, "unknown") or not evidence.get("device"):
        raise ValueError(f"{backend}: ledger does not identify the answering device")


def validate(root: Path, backends: list[str]) -> int:
    if len(backends) < 2 or len(set(backends)) != len(backends):
        raise ValueError("name at least two distinct backends")
    reference = read_png(root / "tests/render/references/first_light.png")
    captures: dict[str, Image] = {}
    for backend in backends:
        stem = root / f"docs/design/images/m11d5-three-backends-{backend}"
        image_path = stem.with_suffix(".png")
        manifest_path = stem.with_suffix(".manifest")
        if not image_path.is_file() or not manifest_path.is_file():
            raise ValueError(f"{backend}: image and manifest must both be committed")
        evidence = read_manifest(manifest_path, backend)
        validate_evidence(backend, evidence)
        capture = read_png(image_path)
        differing, off_edge, edges, maximum = compare(reference, capture)
        if off_edge or differing > edges:
            raise ValueError(
                f"{backend}: differs from the committed scene: {differing} texels, "
                f"{off_edge} off-edge, budget {edges}, max channel {maximum}"
            )
        captures[backend] = capture
        print(
            f'{backend}: matched on "{evidence["device"]}" '
            f'({evidence.get("class")}, {evidence.get("vendor")}); max channel delta {maximum}'
        )
    baseline = backends[0]
    for backend in backends[1:]:
        differing, off_edge, edges, maximum = compare(captures[baseline], captures[backend])
        if off_edge or differing > edges:
            raise ValueError(
                f"{baseline} versus {backend}: {differing} texels differ, "
                f"{off_edge} off-edge, budget {edges}, max channel {maximum}"
            )
        print(f"{baseline} versus {backend}: within tolerance; max channel delta {maximum}")
    return 0


def selftest() -> int:
    clean = Image(2, 1, bytes((10, 20, 30, 40, 50, 60)))
    changed = Image(2, 1, bytes((10, 20, 30, 40, 50, 64)))
    assert compare(clean, clean) == (0, 0, 0, 0)
    assert compare(clean, changed) == (1, 1, 0, 4)
    with tempfile.TemporaryDirectory() as directory:
        bad = Path(directory) / "bad.manifest"
        bad.write_text('cygolden 1\nrow backend=metal outcome=matched class=unknown device=""\n')
        evidence = read_manifest(bad, "metal")
        try:
            validate_evidence("metal", evidence)
        except ValueError:
            pass
        else:
            raise AssertionError("an unidentified device was accepted")
    print("compare_backend_goldens.py selftest: comparison changes and missing identity go red")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("backends", nargs="*")
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--selftest", action="store_true")
    args = parser.parse_args()
    try:
        if args.selftest:
            return selftest()
        return validate(args.root, args.backends)
    except (OSError, ValueError, zlib.error) as error:
        print(f"backend golden comparison: FAILED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
