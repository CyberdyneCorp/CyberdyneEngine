#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Regression tests for tools/docs/compare_depth_of_field.py's detail measure.

    python3 tools/docs/test_compare_depth_of_field.py

The measure once filtered the CROPPED box, and PIL's kernel filter copies an image's edge pixels
through unfiltered: each border pixel then counted its raw luminance, offset from 128, as detail.
On the copper sphere that border was most of the measure, and a visibly blurred sphere "kept" 92 %
of its detail.
"""

import pathlib
import sys
import unittest

from PIL import Image, ImageFilter

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import compare_depth_of_field as compare  # noqa: E402


class DetailTest(unittest.TestCase):
    def test_a_flat_frame_has_no_detail(self) -> None:
        # Dark, so a border counted as raw luminance would be (20 - 128)^2 a pixel.
        flat = Image.new("RGBA", (400, 300), (20, 20, 20, 255))
        for name in compare.BOXES:
            self.assertEqual(compare.detail(flat, name), 0.0)

    def test_a_blur_removes_detail(self) -> None:
        # A one-pixel checkerboard, then the same under a box blur: the measure must fall.
        sharp = Image.new("L", (400, 300))
        sharp.putdata([255 if (x + y) % 2 else 0 for y in range(300) for x in range(400)])
        blurred = sharp.filter(ImageFilter.BoxBlur(3))
        for name in compare.BOXES:
            kept = compare.detail(blurred.convert("RGBA"), name) / compare.detail(
                sharp.convert("RGBA"), name)
            self.assertLess(kept, 0.05)


if __name__ == "__main__":
    unittest.main()
