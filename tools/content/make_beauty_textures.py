#!/usr/bin/env python3
"""Generate the source textures for M11.c's beauty shot. Task 6.1c and 6.2.

--- WHY A GENERATOR AND NOT A DOWNLOAD -------------------------------------------------------------

This is the first content this repository has ever shipped, and `thirdparty-dependencies`' governance
applies to content the project ships as much as to code it links: every file needs a licence and a
provenance record, and "a texture somebody found" has neither that anyone can verify years later.

So the textures are the project's own, generated from a seed by this file. The provenance is then
exactly reproducible rather than asserted — `just capture-beauty-shot --regenerate-textures` runs
this, and `content/beauty/textures/PROVENANCE.md` records the seed, this file's digest and the
dimensions. A texture whose bytes stop matching what this generator produces is a texture somebody
edited by hand, and the check says so.

--- WHAT IT WRITES --------------------------------------------------------------------------------

PNG, deliberately, and with the stdlib's `zlib` rather than an image library:

  * PNG because that is what content arrives as, and because M11.c task 6.1b put a DEFLATE decoder
    and a PNG reader into `tools/import/`. A Targa here would leave that decoder untested by any
    content in the tree.
  * the stdlib because a generator that needed Pillow would be a build dependency for a file that
    runs once, and `deps/manifest.toml` is a governed list.

Three materials, each an albedo, a tangent-space normal map and a packed data map:

  | file | usage | what it is |
  |---|---|---|
  | `<name>_albedo.png` | Colour, sRGB | base colour |
  | `<name>_normal.png` | NormalMap, linear | tangent-space normal, +Y up (OpenGL convention, which is `mesh.h`'s) |
  | `<name>_data.png` | Data, linear | R = roughness, G = ambient occlusion, B = metallic, A = cavity |

**The data map has four channels and that is a format decision, not a convenience.**
`select_format(Data, has_alpha, Desktop)` answers BC4 — one channel — when the image has no
meaningful alpha, and BC7 when it has. A three-channel data map would therefore cook to BC4 and lose
occlusion and metalness silently. The fourth channel is a cavity mask, it is real, and it is what
keeps all three channels.

--- THE NOISE IS DETERMINISTIC AND MACHINE-INDEPENDENT ----------------------------------------------

Integer hashing and float arithmetic in double precision, with no library random and no iteration
over a set. Two runs on two machines produce identical bytes, which is what makes the provenance
check above meaningful rather than decorative.
"""

from __future__ import annotations

import argparse
import hashlib
import math
import pathlib
import struct
import zlib

SIZE = 256
SEED = 0x5EED_B107


# --- Noise -----------------------------------------------------------------------------------------


def _hash2(x: int, y: int, seed: int) -> int:
    """A 32-bit integer hash of a lattice point. Integer-only, so it is machine-independent."""
    h = (x * 374761393 + y * 668265263 + seed * 362437) & 0xFFFFFFFF
    h = (h ^ (h >> 13)) * 1274126177 & 0xFFFFFFFF
    return h ^ (h >> 16)


def _lattice(x: int, y: int, seed: int, period: int) -> float:
    """A value in [0, 1) at a lattice point, wrapped so the texture tiles."""
    return _hash2(x % period, y % period, seed) / 4294967296.0


def _smooth(t: float) -> float:
    return t * t * (3.0 - 2.0 * t)


def value_noise(u: float, v: float, frequency: int, seed: int) -> float:
    """Tiling value noise at one frequency."""
    x = u * frequency
    y = v * frequency
    x0, y0 = math.floor(x), math.floor(y)
    fx, fy = _smooth(x - x0), _smooth(y - y0)
    a = _lattice(x0, y0, seed, frequency)
    b = _lattice(x0 + 1, y0, seed, frequency)
    c = _lattice(x0, y0 + 1, seed, frequency)
    d = _lattice(x0 + 1, y0 + 1, seed, frequency)
    return (a * (1 - fx) + b * fx) * (1 - fy) + (c * (1 - fx) + d * fx) * fy


def fbm(u: float, v: float, octaves: int, base: int, seed: int) -> float:
    """Fractional Brownian motion: octaves of value noise at halving amplitude."""
    total = 0.0
    amplitude = 1.0
    normal = 0.0
    frequency = base
    for octave in range(octaves):
        total += amplitude * value_noise(u, v, frequency, seed + octave * 7919)
        normal += amplitude
        amplitude *= 0.5
        frequency *= 2
    return total / normal


def ridged(u: float, v: float, octaves: int, base: int, seed: int) -> float:
    """Ridged noise: the creases a weathered surface has and smooth noise does not."""
    total = 0.0
    amplitude = 1.0
    normal = 0.0
    frequency = base
    for octave in range(octaves):
        value = 1.0 - abs((value_noise(u, v, frequency, seed + octave * 104729) * 2.0) - 1.0)
        total += amplitude * value * value
        normal += amplitude
        amplitude *= 0.5
        frequency *= 2
    return total / normal


# --- PNG -------------------------------------------------------------------------------------------


def write_png(path: pathlib.Path, width: int, height: int, channels: int, pixels: bytearray) -> int:
    """Write an 8-bit PNG. Filter 1 (sub) on every row, which is what a tiling texture compresses on.

    Deliberately one filter rather than an adaptive choice: the output has to be byte-identical
    across runs and across Python versions, and an adaptive filter is a heuristic that could change.
    """
    colour_type = {1: 0, 3: 2, 4: 6}[channels]
    raw = bytearray()
    for row in range(height):
        start = row * width * channels
        line = pixels[start : start + (width * channels)]
        raw.append(1)
        for index in range(len(line)):
            left = line[index - channels] if index >= channels else 0
            raw.append((line[index] - left) & 0xFF)

    def chunk(kind: bytes, body: bytes) -> bytes:
        return (
            struct.pack(">I", len(body))
            + kind
            + body
            + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
        )

    header = struct.pack(">IIBBBBB", width, height, 8, colour_type, 0, 0, 0)
    data = (
        b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", header)
        + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
        + chunk(b"IEND", b"")
    )
    path.write_bytes(data)
    return len(data)


# --- The materials ---------------------------------------------------------------------------------


def clamp_byte(value: float) -> int:
    scaled = int(round(value * 255.0))
    return 0 if scaled < 0 else (255 if scaled > 255 else scaled)


def height_to_normal(height: list[float], strength: float) -> bytearray:
    """A tangent-space normal map from a height field, by central differences.

    +Y UP, which is `mesh.h`'s convention and the one the importer's `convention = opengl` option
    leaves alone. A DirectX-orientation map has its green channel inverted and the importer inverts
    it back; generating one here so the importer could invert it would be theatre.
    """
    pixels = bytearray(SIZE * SIZE * 3)
    for y in range(SIZE):
        for x in range(SIZE):
            left = height[(y * SIZE) + ((x - 1) % SIZE)]
            right = height[(y * SIZE) + ((x + 1) % SIZE)]
            down = height[(((y - 1) % SIZE) * SIZE) + x]
            up = height[(((y + 1) % SIZE) * SIZE) + x]
            nx = (left - right) * strength
            ny = (down - up) * strength
            nz = 1.0
            length = math.sqrt((nx * nx) + (ny * ny) + (nz * nz))
            index = ((y * SIZE) + x) * 3
            pixels[index + 0] = clamp_byte(((nx / length) * 0.5) + 0.5)
            pixels[index + 1] = clamp_byte(((ny / length) * 0.5) + 0.5)
            pixels[index + 2] = clamp_byte(((nz / length) * 0.5) + 0.5)
    return pixels


def make_stone(seed: int):
    """Weathered limestone: large blocks, a mortar line, and rain streaks down the faces."""
    albedo = bytearray(SIZE * SIZE * 3)
    data = bytearray(SIZE * SIZE * 4)
    height = [0.0] * (SIZE * SIZE)
    for y in range(SIZE):
        for x in range(SIZE):
            u, v = x / SIZE, y / SIZE
            # Courses of blocks, offset every other row, with a recessed joint.
            course = int(v * 4.0)
            offset = 0.5 * (course % 2)
            block_u = (u + offset) % 1.0
            joint_u = min(abs((block_u * 4.0) % 1.0), 1.0 - ((block_u * 4.0) % 1.0))
            joint_v = min(abs((v * 4.0) % 1.0), 1.0 - ((v * 4.0) % 1.0))
            joint = min(joint_u, joint_v)
            mortar = 1.0 - _smooth(min(joint / 0.035, 1.0))

            grain = fbm(u, v, 5, 8, seed)
            pits = ridged(u, v, 4, 24, seed + 11)
            streak = fbm(u * 0.35, v * 2.4, 3, 6, seed + 29)

            tone = 0.62 + (grain * 0.22) - (pits * 0.10) - (streak * 0.12)
            tone = tone * (1.0 - (mortar * 0.38))
            index = ((y * SIZE) + x) * 3
            albedo[index + 0] = clamp_byte(tone * 1.00)
            albedo[index + 1] = clamp_byte(tone * 0.96)
            albedo[index + 2] = clamp_byte(tone * 0.88)

            roughness = 0.58 + (pits * 0.26) + (mortar * 0.18) - (grain * 0.08)
            occlusion = 1.0 - (mortar * 0.55) - (pits * 0.12)
            packed = ((y * SIZE) + x) * 4
            data[packed + 0] = clamp_byte(roughness)
            data[packed + 1] = clamp_byte(occlusion)
            data[packed + 2] = 0
            data[packed + 3] = clamp_byte(1.0 - (pits * 0.45) - (mortar * 0.3))
            height[(y * SIZE) + x] = (grain * 0.5) + (pits * 0.35) - (mortar * 1.0)
    return albedo, height_to_normal(height, 2.6), data


def make_copper(seed: int):
    """Oxidised copper: metal where it is worn, verdigris where the rain sits."""
    albedo = bytearray(SIZE * SIZE * 3)
    data = bytearray(SIZE * SIZE * 4)
    height = [0.0] * (SIZE * SIZE)
    for y in range(SIZE):
        for x in range(SIZE):
            u, v = x / SIZE, y / SIZE
            # A SOFTER TRANSITION THAN THE FIRST ATTEMPT, and the reason is that the first one
            # looked like a map. Real verdigris creeps: the mask is widened and its edge smoothed so
            # the two states meet over centimetres rather than over a texel, and the green is pulled
            # towards the metal rather than sitting at full chroma beside it.
            patina = _smooth(min(max((fbm(u, v, 5, 4, seed) - 0.40) * 1.7, 0.0), 1.0))
            grain = fbm(u * 3.0, v * 3.0, 4, 16, seed + 3)
            speckle = ridged(u, v, 3, 40, seed + 61)
            # Bare copper, and the carbonate green it turns into.
            bare = (0.58 + (grain * 0.14) + (speckle * 0.06),
                    0.31 + (grain * 0.09),
                    0.20 + (grain * 0.05))
            green = (0.27 + (grain * 0.07), 0.40 + (grain * 0.09), 0.34 + (grain * 0.08))
            index = ((y * SIZE) + x) * 3
            for channel in range(3):
                albedo[index + channel] = clamp_byte(
                    (bare[channel] * (1.0 - patina)) + (green[channel] * patina)
                )
            # THE ONE THING A CONSTANT MATERIAL CANNOT DO: metalness varies across the surface.
            # Bare metal is metallic and smooth; the patina is a dielectric crust and is neither.
            packed = ((y * SIZE) + x) * 4
            data[packed + 0] = clamp_byte(0.16 + (patina * 0.62) + (grain * 0.08))
            data[packed + 1] = clamp_byte(1.0 - (patina * 0.25))
            data[packed + 2] = clamp_byte(1.0 - patina)
            data[packed + 3] = clamp_byte(1.0 - (patina * 0.35))
            height[(y * SIZE) + x] = (patina * 0.6) + (grain * 0.3)
    return albedo, height_to_normal(height, 1.4), data


def make_gravel(seed: int):
    """The courtyard floor: packed sand with gravel in it."""
    albedo = bytearray(SIZE * SIZE * 3)
    data = bytearray(SIZE * SIZE * 4)
    height = [0.0] * (SIZE * SIZE)
    for y in range(SIZE):
        for x in range(SIZE):
            u, v = x / SIZE, y / SIZE
            stones = ridged(u, v, 3, 32, seed + 7)
            sand = fbm(u, v, 5, 12, seed + 19)
            damp = fbm(u * 0.5, v * 0.5, 3, 3, seed + 41)
            tone = 0.52 + (sand * 0.20) + (stones * 0.14) - (damp * 0.16)
            index = ((y * SIZE) + x) * 3
            albedo[index + 0] = clamp_byte(tone * 1.00)
            albedo[index + 1] = clamp_byte(tone * 0.90)
            albedo[index + 2] = clamp_byte(tone * 0.74)
            packed = ((y * SIZE) + x) * 4
            data[packed + 0] = clamp_byte(0.74 + (sand * 0.18) - (stones * 0.14))
            data[packed + 1] = clamp_byte(1.0 - (stones * 0.22))
            data[packed + 2] = 0
            data[packed + 3] = clamp_byte(1.0 - (stones * 0.4))
            height[(y * SIZE) + x] = (stones * 0.7) + (sand * 0.3)
    return albedo, height_to_normal(height, 3.2), data


MATERIALS = {"stone": make_stone, "copper": make_copper, "gravel": make_gravel}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default="content/beauty/textures", type=pathlib.Path)
    parser.add_argument("--seed", default=SEED, type=lambda text: int(text, 0))
    arguments = parser.parse_args()
    arguments.out.mkdir(parents=True, exist_ok=True)

    digest = hashlib.blake2b(pathlib.Path(__file__).read_bytes(), digest_size=8).hexdigest()
    written = []
    for name, build in MATERIALS.items():
        albedo, normal, data = build(arguments.seed)
        for suffix, pixels, channels in (
            ("albedo", albedo, 3),
            ("normal", normal, 3),
            ("data", data, 4),
        ):
            path = arguments.out / f"{name}_{suffix}.png"
            size = write_png(path, SIZE, SIZE, channels, pixels)
            written.append((path.name, size))
            print(f"{path}  {SIZE}x{SIZE}  {size} bytes")

    total = sum(size for _, size in written)
    print(f"{len(written)} textures, {total} bytes, seed 0x{arguments.seed:08X}, generator {digest}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
