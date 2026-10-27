# SPDX-License-Identifier: MIT
"""Turn `just capture-foot-placement`'s two frame sequences into the committed video and stills.

The course (samples/09b-animated-character, `--course`) is walked twice with the same program, once
with its `pose.foot_ik` node at weight zero and once at one, and this puts the two side by side:
WITHOUT on the left, WITH on the right, frame for frame. Two runs rather than one composited in the
sample because each is the engine's own frame of one character, and the comparison is only fair if
nothing but the weight differs.

The stills are close crops of the feet at two moments — on the stairs and on the ramp — because the
claim is about a few centimetres at the soles, which a full frame at document width does not show.
Recompressed, as tools/docs/collect_animated_character.py says why; the pixels are the device's. The
video's ffmpeg flags are that file's, for the reasons it gives.
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
IMAGES = REPO_ROOT / "docs" / "design" / "images"
VIDEOS = REPO_ROOT / "docs" / "design" / "videos"

CRF = "20"
FONT = pathlib.Path("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf")


def label(image: Image.Image, text: str) -> None:
    """Write `text` in the image's top-left corner, on a dark band so it reads on any frame."""
    draw = ImageDraw.Draw(image)
    size = max(14, image.height // 22)
    font = ImageFont.truetype(str(FONT), size) if FONT.is_file() else ImageFont.load_default()
    box = draw.textbbox((0, 0), text, font=font)
    pad = size // 3
    draw.rectangle((0, 0, box[2] + 2 * pad, box[3] + 2 * pad), fill=(10, 14, 24))
    draw.text((pad, pad), text, fill=(235, 240, 248), font=font)


def pair(without: Image.Image, with_: Image.Image) -> Image.Image:
    width, height = without.size
    joined = Image.new("RGB", (width * 2, height))
    left = without.convert("RGB")
    right = with_.convert("RGB")
    label(left, "foot placement OFF")
    label(right, "foot placement ON")
    joined.paste(left, (0, 0))
    joined.paste(right, (width, 0))
    return joined


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("without", help="the frames walked at weight zero")
    parser.add_argument("with_", metavar="with", help="the frames walked at weight one")
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--stairs", type=int, default=56, help="the frame for the stairs still")
    parser.add_argument("--slope", type=int, default=150, help="the frame for the ramp still")
    parser.add_argument("--name", default="foot-placement")
    arguments = parser.parse_args()

    without = pathlib.Path(arguments.without)
    with_ = pathlib.Path(arguments.with_)
    frames = sorted(without.glob("frame_*.png"))
    if not frames or len(frames) != len(sorted(with_.glob("frame_*.png"))):
        print("collect: the two runs wrote no frames, or different numbers of them",
              file=sys.stderr)
        return 1
    if shutil.which("ffmpeg") is None:
        print("collect: ffmpeg is not on the path, so no video can be encoded", file=sys.stderr)
        return 1
    IMAGES.mkdir(parents=True, exist_ok=True)
    VIDEOS.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as scratch:
        joined_dir = pathlib.Path(scratch)
        for index, frame in enumerate(frames):
            pair(Image.open(frame), Image.open(with_ / frame.name)).save(
                joined_dir / f"frame_{index:04d}.png")
        video_out = VIDEOS / f"{arguments.name}.mp4"
        command = [
            "ffmpeg", "-y", "-loglevel", "error",
            "-framerate", str(arguments.fps),
            "-start_number", "0",
            "-i", str(joined_dir / "frame_%04d.png"),
            "-c:v", "libx264", "-profile:v", "high", "-preset", "slow", "-crf", CRF,
            "-pix_fmt", "yuv420p", "-movflags", "+faststart", "-an",
            str(video_out),
        ]
        if subprocess.run(command, check=False).returncode != 0:
            print("collect: ffmpeg failed", file=sys.stderr)
            return 1
        print(f"    {video_out.name}  {video_out.stat().st_size // 1024} KiB  {len(frames)} frames")

    for moment, frame in (("stairs", arguments.stairs), ("slope", arguments.slope)):
        name = f"frame_{frame:04d}.png"
        first = Image.open(without / name)
        width, height = first.size
        # The lower middle of the frame: the legs and the ground they stand on.
        crop = (int(width * 0.26), int(height * 0.28), int(width * 0.68), height)
        still = pair(first.crop(crop), Image.open(with_ / name).crop(crop))
        out = IMAGES / f"{arguments.name}-{moment}.png"
        still.save(out, optimize=True)
        print(f"    {out.name}  {out.stat().st_size // 1024} KiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
