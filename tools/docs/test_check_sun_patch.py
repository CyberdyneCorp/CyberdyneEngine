#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for tools/docs/check_sun_patch.py, which needs no device.

    python3 tools/docs/test_check_sun_patch.py

The sunlit room's frame is replaced by a synthetic one: dark, with the floor lit where the check
predicts the window's light, or lit somewhere else. The device run that proved the check red on
the mirrored shadow lookup is recorded in openspec/changes/add-gi-probe-space-denoiser/evidence.
"""

import pathlib
import sys
import unittest

from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import check_sun_patch as check  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[2]
SHOT = check.read_shot(ROOT / "content" / "beauty" / "interior.cyshot")
WINDOW = [-0.9, 1.0, 6.0, 0.9, 2.4, 6.5]
FLOOR = [-3.0, -1.0, 3.0, 6.0]


def frame_lit_where(predicate) -> Image.Image:
    """A dark 480x270 frame, its floor lit at every (x, z) the predicate accepts."""
    image = Image.new("RGB", (480, 270), (10, 10, 10))
    sun = check.to_sun(SHOT)
    quads = [check.opening(WINDOW, WINDOW[2], sun), check.opening(WINDOW, WINDOW[5], sun)]
    steps = 400
    for i in range(steps + 1):
        for j in range(steps + 1):
            x = FLOOR[0] + ((FLOOR[2] - FLOOR[0]) * i / steps)
            z = FLOOR[1] + ((FLOOR[3] - FLOOR[1]) * j / steps)
            if not predicate(x, z, check.patch_distance((x, z), quads)):
                continue
            pixel = check.project(SHOT, image.width, image.height, (x, 0.0, z))
            if pixel is not None:
                image.putpixel(pixel, (240, 240, 240))
    return image


class SunPatchTest(unittest.TestCase):
    def test_the_patch_where_the_window_puts_it_passes(self) -> None:
        image = frame_lit_where(lambda x, z, distance: distance >= 0.0)
        lit, dark, _, _ = check.measure(image, SHOT, WINDOW, FLOOR)
        self.assertGreaterEqual(lit, check.MIN_RATIO * dark)

    def test_a_patch_moved_across_the_room_fails(self) -> None:
        # The patch mirrored across the room's centre line, as a mirrored map lookup moves it.
        image = frame_lit_where(lambda x, z, distance: x > 0.6 and 2.5 < z < 4.8)
        lit, dark, _, _ = check.measure(image, SHOT, WINDOW, FLOOR)
        self.assertLess(lit, check.MIN_RATIO * dark)

    def test_the_sun_comes_through_the_far_wall(self) -> None:
        # The shot's sun is behind the far wall and above it: the window's light falls toward the
        # camera, on the floor between the camera and the window.
        sun = check.to_sun(SHOT)
        self.assertGreater(sun[1], 0.0)
        self.assertGreater(sun[2], 0.0)
        for x, z in check.opening(WINDOW, WINDOW[2], sun):
            self.assertLess(z, WINDOW[2])
            self.assertGreater(z, FLOOR[1])


if __name__ == "__main__":
    unittest.main()
