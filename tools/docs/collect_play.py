# SPDX-License-Identifier: MIT
"""Turn a played capture's frames into the committed video and stills.

`just capture-play-fps` and `just capture-play-tps` play a sample under the desktop play host with
an input script for hands (samples/<n>/play/capture.input), one step a frame, and the presenter
copies every n-th presented frame — the swapchain image, the HUD and the pause menu on it — to
`frame-NNNNN.png`. This encodes them, at the rate they were kept, into
docs/design/videos/<name>.mp4, and recompresses the frames the docs show into
docs/design/images/<name>-<label>.png: the engine's PNG writer stores rather than deflates, as
tools/docs/collect_first_person.py says, and nothing else is done to them.

    python3 tools/docs/collect_play.py <frames> --name first-person-play --fps 30 \\
        --still 120=aim --still 330=menu
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
CRF = "23"


def parse_still(text: str) -> tuple[int, str]:
    frame, _, label = text.partition("=")
    if not frame.isdigit() or not label:
        raise argparse.ArgumentTypeError(f"a still is FRAME=LABEL, not '{text}'")
    return int(frame), label


def encode(frames: pathlib.Path, out: pathlib.Path, fps: int) -> bool:
    command = [
        "ffmpeg", "-y", "-loglevel", "error",
        "-framerate", str(fps),
        "-pattern_type", "glob", "-i", str(frames / "frame-*.png"),
        "-c:v", "libx264", "-profile:v", "high", "-preset", "slow", "-crf", CRF,
        "-pix_fmt", "yuv420p", "-movflags", "+faststart", "-an",
        str(out),
    ]
    return subprocess.run(command, check=False).returncode == 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("frames", type=pathlib.Path, help="the directory of frame-NNNNN.png")
    parser.add_argument("--name", required=True)
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--still", type=parse_still, action="append", default=[])
    arguments = parser.parse_args()

    frames = sorted(arguments.frames.glob("frame-*.png"))
    if not frames:
        print("collect: the run wrote no frames", file=sys.stderr)
        return 1
    if shutil.which("ffmpeg") is None:
        print("collect: ffmpeg is not on the path, so no video can be encoded", file=sys.stderr)
        return 1
    IMAGES.mkdir(parents=True, exist_ok=True)
    VIDEOS.mkdir(parents=True, exist_ok=True)
    video = VIDEOS / f"{arguments.name}.mp4"
    if not encode(arguments.frames, video, arguments.fps):
        print("collect: ffmpeg failed", file=sys.stderr)
        return 1
    print(f"    {video.name}  {video.stat().st_size // 1024} KiB  {len(frames)} frames")
    for frame, label in arguments.still:
        source = arguments.frames / f"frame-{frame:05d}.png"
        if not source.is_file():
            print(f"collect: no frame {frame} was captured", file=sys.stderr)
            return 1
        out = IMAGES / f"{arguments.name}-{label}.png"
        Image.open(source).convert("RGB").save(out, optimize=True)
        print(f"    {out.name}  {out.stat().st_size // 1024} KiB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
