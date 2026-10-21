#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Tests for tools/docs/compare_upscale.py's log reading and error measure.

    python3 tools/docs/test_compare_upscale.py
"""

import math
import pathlib
import sys
import unittest

from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import compare_upscale as compare  # noqa: E402


class LogTest(unittest.TestCase):
    def test_both_programs_timings_are_read(self) -> None:
        beauty = "frame timing  60 frames: min 4.581, median 4.639, mean 4.643 ms\n"
        world = "  device time: median 3.278 ms, min 3.232 ms, max 3.339 ms\n"
        self.assertEqual(compare.median_ms(beauty), 4.639)
        self.assertEqual(compare.median_ms(world), 3.278)
        self.assertIsNone(compare.median_ms("no timing here"))

    def test_validation_errors_are_the_largest_count(self) -> None:
        self.assertEqual(compare.validation_errors("validation    0 error(s)\n"), 0)
        self.assertEqual(
            compare.validation_errors("RHI validation errors: 0\nRHI validation errors: 26\n"), 26)


class ErrorTest(unittest.TestCase):
    def test_identical_pictures_have_no_error(self) -> None:
        picture = Image.new("RGB", (8, 4), (10, 200, 30))
        mae, psnr = compare.error(picture, picture)
        self.assertEqual(mae, 0.0)
        self.assertTrue(math.isinf(psnr))

    def test_one_level_everywhere_is_one_level_of_error(self) -> None:
        a = Image.new("RGB", (8, 4), (10, 10, 10))
        b = Image.new("RGB", (8, 4), (11, 9, 11))
        mae, psnr = compare.error(a, b)
        self.assertEqual(mae, 1.0)
        self.assertAlmostEqual(psnr, 10.0 * math.log10(255.0 * 255.0), places=6)

    def test_a_crop_strip_keeps_the_device_pixels(self) -> None:
        picture = Image.new("RGB", (16, 16), (0, 0, 0))
        picture.putpixel((5, 6), (255, 0, 0))
        strip = compare.crop_strip({"native": picture}, (4, 4, 4, 4), 2)
        # The red texel lands at ((5-4)*2, (6-4)*2 + the label bar), twice in each direction.
        self.assertEqual(strip.getpixel((2, 22 + 4)), (255, 0, 0))
        self.assertEqual(strip.getpixel((3, 22 + 5)), (255, 0, 0))


if __name__ == "__main__":
    unittest.main()
