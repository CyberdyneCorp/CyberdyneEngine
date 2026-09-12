#!/usr/bin/env python3
"""Turn `just capture-animated-character`'s frame sequence into the committed video and still.

Two jobs, and they are separate because the two artefacts answer different questions. The STILL is
what a reader sees in a document that does not play video — every markdown viewer that is not
github.com, and every printed page — so it has to stand alone. The VIDEO is the only thing that can
show a DEFORMATION: a single frame of a skinned character is indistinguishable from a single frame of
a posed one, and the whole claim of this artefact is that the geometry moves.

WHY THE STILL IS RECOMPRESSED. The capture writes through the golden-image suite's PNG writer, which
stores rather than compresses — the right trade for a test comparing bytes, and the wrong one for a
file that lives in the tree, where it costs about 1.5 MiB per image against about 200 KiB. This only
recompresses; the pixels are the ones the device produced. That is the same argument, and the same
two lines of PIL, as tools/docs/collect_virtual_geometry.py.

WHY EACH ffmpeg FLAG IS THERE, since every one of them is a way to produce a file that plays on the
machine that made it and nowhere else:

  -framerate before -i   sets the INPUT rate for the image demuxer. `-r` after `-i` sets the output
                         rate and leaves the input at the demuxer's default 25, which makes ffmpeg
                         duplicate and drop frames to reconcile them.
  -start_number 0        the image demuxer starts at 1 by default, so a sequence written from frame
                         zero loses its first frame or fails to open at all.
  -pix_fmt yuv420p       x264 defaults to yuv444p from RGB input, and Safari, QuickTime and GitHub's
                         player refuse or mis-render anything but 8-bit 4:2:0.
  -profile:v high        the universally decodable profile at 8-bit 4:2:0.
  -movflags +faststart   moves the moov atom to the front so the file starts playing before it has
                         finished downloading, which is what a reader clicking a link needs.
  -an                    explicit: there is no audio track and there was never meant to be one.

The even-dimension filter every recipe of this shape carries is NOT here, because the sample refuses
an odd width or height at its own argument parsing — one place to enforce it, and the earlier of the
two.
"""

from __future__ import annotations

import argparse
import pathlib
import shutil
import subprocess
import sys

from PIL import Image

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
IMAGES = REPO_ROOT / "docs" / "design" / "images"
VIDEOS = REPO_ROOT / "docs" / "design" / "videos"

#: Constant rate factor. 20 is the figure this artefact's committed file was judged at: visually
#: clean on the character's silhouette, which is the part of the picture that carries the claim,
#: against about 1 MiB for ten seconds at 960x540. Raising it to 23 halves the file and mushes the
#: chequerboard; lowering it to 18 doubles it and changes nothing a reader would notice.
CRF = "20"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("frames", help="the directory holding frame_%04d.png")
    parser.add_argument("--still", required=True, help="the representative frame, as written")
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--name", default="animated-character")
    arguments = parser.parse_args()

    frames = pathlib.Path(arguments.frames)
    still = pathlib.Path(arguments.still)
    pattern = frames / "frame_%04d.png"
    written = sorted(frames.glob("frame_*.png"))
    if not written:
        print(f"collect: {frames} holds no frames; the capture wrote nothing", file=sys.stderr)
        return 1
    if not still.is_file():
        print(f"collect: {still} is missing; the capture did not write the still", file=sys.stderr)
        return 1
    if shutil.which("ffmpeg") is None:
        print("collect: ffmpeg is not on the path, so no video can be encoded", file=sys.stderr)
        return 1

    IMAGES.mkdir(parents=True, exist_ok=True)
    VIDEOS.mkdir(parents=True, exist_ok=True)

    image_out = IMAGES / f"{arguments.name}.png"
    Image.open(still).save(image_out, optimize=True)
    print(f"    {image_out.name}  {image_out.stat().st_size // 1024} KiB")

    video_out = VIDEOS / f"{arguments.name}.mp4"
    command = [
        "ffmpeg", "-y", "-loglevel", "error",
        "-framerate", str(arguments.fps),
        "-start_number", "0",
        "-i", str(pattern),
        "-c:v", "libx264",
        "-profile:v", "high",
        "-preset", "slow",
        "-crf", CRF,
        "-pix_fmt", "yuv420p",
        "-movflags", "+faststart",
        "-an",
        str(video_out),
    ]
    completed = subprocess.run(command, check=False)
    if completed.returncode != 0:
        print(f"collect: ffmpeg exited {completed.returncode}", file=sys.stderr)
        return 1
    seconds = len(written) / float(arguments.fps)
    print(f"    {video_out.name}  {video_out.stat().st_size // 1024} KiB  "
          f"{len(written)} frames, {seconds:.1f} s at {arguments.fps} fps")
    return 0


if __name__ == "__main__":
    sys.exit(main())
