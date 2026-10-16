#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Is the sun through a window where the window and the sun put it? `just capture-gi-interior` runs
it over the sunlit room's frame.

THE CHECK, AND THE DEFECT IT WAS WRITTEN FOR. `samples/12-beauty`'s shadow lookup once read the sun's
map with its rows the wrong way up (`v = ndc.y / 2 + 1/2`, where the RHI rasterises the light's +Y
into row 0), so every lookup took the texel mirrored across the map's centre line. The Colonnade
looked plausible; a room did not — its side wall, under the ceiling, was lit by the sun. This
computes from the shot's own numbers where the window's light falls on the floor — the window's
inner and outer openings each projected down the sun's direction, the patch their intersection —
and where that lands in the frame through the shot's camera, and requires the frame to be bright
there and dark on the floor around it.

Geometry is the engine's: y up, the sun at (cos e cos a, sin e, cos e sin a) for elevation e and
azimuth a (`stage.cpp`'s `build_sky`), a camera looking down `target - position` whose right is
`forward x up` and whose field of view is horizontal.
"""

from __future__ import annotations

import argparse
import math
import pathlib
import sys

from PIL import Image

# The floor is y = 0. A point is "inside" the patch when it is this far inside both projected
# openings, and "around" it when it is between these two distances outside the patch — margins
# that keep the soft edge of the filtered shadow out of both measures.
INSIDE_MARGIN = 0.15
AROUND_NEAR = 0.35
AROUND_FAR = 0.9
# The patch must be this many times as bright as the floor around it, in mean 8-bit luma. Read on the
# frame WITH the GI, where the floor out of the sun is lit by the room's bounce alone: 17 times as
# bright with the map read the right way up, 1.7 times when it was mirrored. (Without the GI, the
# flat sky term lights the whole floor and the patch is 1.4 times its surroundings.)
MIN_RATIO = 4.0


def read_shot(path: pathlib.Path) -> dict[str, list[float]]:
    shot: dict[str, list[float]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        words = line.split()
        if len(words) < 3 or words[0] not in ("camera", "sun"):
            continue
        shot[f"{words[0]} {words[1]}"] = [float(value) for value in words[2:]]
    return shot


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def dot(a, b):
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2])


def cross(a, b):
    return ((a[1] * b[2]) - (a[2] * b[1]), (a[2] * b[0]) - (a[0] * b[2]),
            (a[0] * b[1]) - (a[1] * b[0]))


def normalise(a):
    length = math.sqrt(dot(a, a))
    return (a[0] / length, a[1] / length, a[2] / length)


def to_sun(shot: dict[str, list[float]]):
    elevation = math.radians(shot["sun elevation"][0])
    azimuth = math.radians(shot["sun azimuth"][0])
    return (math.cos(elevation) * math.cos(azimuth), math.sin(elevation),
            math.cos(elevation) * math.sin(azimuth))


def on_floor(point, sun):
    """Where the sun's light through `point` meets the floor, as (x, z)."""
    travel = point[1] / sun[1]
    return (point[0] - (sun[0] * travel), point[2] - (sun[2] * travel))


def opening(window: list[float], z: float, sun):
    """The floor quad one opening of the window throws, its corners in order."""
    x0, y0, x1, y1 = window[0], window[1], window[3], window[4]
    corners = [(x0, y0, z), (x1, y0, z), (x1, y1, z), (x0, y1, z)]
    return [on_floor(corner, sun) for corner in corners]


def signed_distance(point, quad) -> float:
    """Inside a convex quad is positive: the least distance to its edges, signed."""
    area = 0.0
    for index, a in enumerate(quad):
        b = quad[(index + 1) % 4]
        area += (a[0] * b[1]) - (b[0] * a[1])
    orientation = 1.0 if area > 0 else -1.0
    least = math.inf
    for index, a in enumerate(quad):
        b = quad[(index + 1) % 4]
        edge = (b[0] - a[0], b[1] - a[1])
        length = math.hypot(*edge)
        side = ((edge[0] * (point[1] - a[1])) - (edge[1] * (point[0] - a[0]))) / length
        least = min(least, side * orientation)
    return least


def patch_distance(point, quads) -> float:
    return min(signed_distance(point, quad) for quad in quads)


def project(shot: dict[str, list[float]], width: int, height: int, point):
    eye = tuple(shot["camera position"])
    forward = normalise(sub(tuple(shot["camera target"]), eye))
    right = normalise(cross(forward, (0.0, 1.0, 0.0)))
    up = cross(right, forward)
    offset = sub(point, eye)
    depth = dot(offset, forward)
    if depth <= 0.0:
        return None
    tan_x = math.tan(math.radians(shot["camera fov"][0]) / 2.0)
    tan_y = tan_x * height / width
    x = dot(offset, right) / (depth * tan_x)
    y = dot(offset, up) / (depth * tan_y)
    column = int((x + 1.0) * 0.5 * width)
    row = int((1.0 - y) * 0.5 * height)
    if 0 <= column < width and 0 <= row < height:
        return column, row
    return None


def measure(image: Image.Image, shot: dict[str, list[float]], window: list[float],
            bounds: list[float]) -> tuple[float, float, int, int]:
    """Mean luma inside the predicted patch and on the floor around it, and how many pixels each."""
    sun = to_sun(shot)
    quads = [opening(window, window[2], sun), opening(window, window[5], sun)]
    luma = image.convert("L")
    width, height = image.size
    inside: list[int] = []
    around: list[int] = []
    steps = 160
    for i in range(steps + 1):
        for j in range(steps + 1):
            x = bounds[0] + ((bounds[2] - bounds[0]) * i / steps)
            z = bounds[1] + ((bounds[3] - bounds[1]) * j / steps)
            distance = patch_distance((x, z), quads)
            pixel = project(shot, width, height, (x, 0.0, z))
            if pixel is None:
                continue
            if distance >= INSIDE_MARGIN:
                inside.append(luma.getpixel(pixel))
            elif -AROUND_FAR <= distance <= -AROUND_NEAR:
                around.append(luma.getpixel(pixel))
    if not inside or not around:
        sys.exit("check_sun_patch: the patch or the floor around it is not in the frame")
    return sum(inside) / len(inside), sum(around) / len(around), len(inside), len(around)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shot", required=True, type=pathlib.Path)
    parser.add_argument("--still", required=True, type=pathlib.Path)
    parser.add_argument("--window", required=True, type=float, nargs=6,
                        metavar=("X0", "Y0", "Z_INNER", "X1", "Y1", "Z_OUTER"),
                        help="the opening: its corners' x and y, and the wall's two faces' z")
    parser.add_argument("--floor", required=True, type=float, nargs=4,
                        metavar=("X0", "Z0", "X1", "Z1"), help="the room's floor, inside")
    args = parser.parse_args()
    with Image.open(args.still) as opened:
        image = opened.convert("RGB")
    lit, dark, lit_count, dark_count = measure(image, read_shot(args.shot), args.window,
                                               args.floor)
    print(f"{args.still}: the sun's patch {lit:.1f} over {lit_count} pixels, the floor around it "
          f"{dark:.1f} over {dark_count}")
    if lit < MIN_RATIO * dark:
        print("check_sun_patch: the floor is not lit where the window and the sun put the light — "
              "is the shadow map read the wrong way up?", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
