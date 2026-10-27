# SPDX-License-Identifier: MIT
"""Turn `just capture-first-person`'s frames into the committed video and stills.

samples/15-first-person draws its scripted session with `--video`, every frame, and with its stills at
the moments the session is about (render.h's `kStills`): the walk to the beam, the first shot on a
target, the crate a shot throws, the wall and the floor struck, a target hit down the sights, and the
aftermath. The video is the whole session at the frame rate it was simulated at; the stills are the
device's pixels, recompressed — the engine's PNG writer stores rather than deflates, as
tools/docs/collect_animated_character.py says — and nothing else is done to them.
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

CRF = "22"
#: The stills the docs show, by the frame render.h writes them at, and the name each is committed as.
STILLS = {
    "fps-318.png": "first-person-shot",
    "fps-396.png": "first-person-crate",
    "fps-428.png": "first-person-floor-sparks",
    "fps-450.png": "first-person-sights",
    "fps-600.png": "first-person-aftermath",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("video", help="the directory of frame-NNNN.png the run wrote")
    parser.add_argument("stills", help="the directory of fps-NNN.png the run wrote")
    parser.add_argument("--fps", type=int, default=60)
    parser.add_argument("--name", default="first-person-demo")
    arguments = parser.parse_args()

    frames = sorted(pathlib.Path(arguments.video).glob("frame-*.png"))
    if not frames:
        print("collect: the run wrote no frames", file=sys.stderr)
        return 1
    if shutil.which("ffmpeg") is None:
        print("collect: ffmpeg is not on the path, so no video can be encoded", file=sys.stderr)
        return 1
    IMAGES.mkdir(parents=True, exist_ok=True)
    VIDEOS.mkdir(parents=True, exist_ok=True)
    video_out = VIDEOS / f"{arguments.name}.mp4"
    command = [
        "ffmpeg", "-y", "-loglevel", "error",
        "-framerate", str(arguments.fps),
        "-pattern_type", "glob", "-i", str(pathlib.Path(arguments.video) / "frame-*.png"),
        "-c:v", "libx264", "-profile:v", "high", "-preset", "slow", "-crf", CRF,
        "-pix_fmt", "yuv420p", "-movflags", "+faststart", "-an",
        str(video_out),
    ]
    if subprocess.run(command, check=False).returncode != 0:
        print("collect: ffmpeg failed", file=sys.stderr)
        return 1
    print(f"    {video_out.name}  {video_out.stat().st_size // 1024} KiB  {len(frames)} frames")

    stills = pathlib.Path(arguments.stills)
    for source, name in STILLS.items():
        path = stills / source
        if not path.is_file():
            print(f"collect: the run wrote no {source}", file=sys.stderr)
            return 1
        out = IMAGES / f"{name}.png"
        Image.open(path).convert("RGB").save(out, optimize=True)
        print(f"    {out.name}  {out.stat().st_size // 1024} KiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
