#!/usr/bin/env python3
"""Turn `just capture-world`'s frame sequence into the committed video, still and budget figure.

THREE ARTEFACTS, AND THEY ANSWER THREE DIFFERENT QUESTIONS.

The STILL is what a reader sees in a document that does not play video — every markdown viewer that
is not github.com, and every printed page — so it has to stand alone.

The VIDEO is the only thing that can show a CYCLE: a single frame of a world is a single frame of a
world, and what M10's artefact claims is that the sun crosses it, a front moves through it, snow
settles and thaws, the sea's spectrum follows the wind and the trees lean with it. No still carries
any of that.

The BUDGET FIGURE is M10 tasks.md 7.3's own wording: the environment demo's cost "measured as a
CURVE across the cycle rather than asserted at one time of day". The program writes every frame's
per-producer cost as a CSV and this plots it — a maximum in a log would have been the assertion the
task is written to refuse.

WHY THE STILL IS RECOMPRESSED. The capture writes through the golden-image suite's PNG writer, which
stores rather than compresses — the right trade for a test comparing bytes, and the wrong one for a
file that lives in the tree, where it costs about 2.7 MiB per image against about 400 KiB. This only
recompresses; the pixels are the ones the device produced. That is the same argument, and the same
two lines of PIL, as tools/docs/collect_animated_character.py and collect_virtual_geometry.py.

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
import csv
import pathlib
import shutil
import subprocess
import sys

from PIL import Image

REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
IMAGES = REPO_ROOT / "docs" / "design" / "images"
VIDEOS = REPO_ROOT / "docs" / "design" / "videos"

#: Constant rate factor. Two higher than the animated character's 20, because a forest of a hundred
#: thousand small triangles is the hardest thing x264 is ever asked to carry and 20 spent ten
#: megabytes on twenty seconds of it. 22 is about two and a half, which is the size the other video
#: in this tree is, and the difference is visible only on the wave crests under a low sun. Dropping
#: to 24 halves it again and starts to smear the forest canopy into a texture.
CRF = "22"

#: The per-producer columns of the budget CSV, in the order they are stacked in the figure and in
#: the order the frame runs them. The labels are the modules, because "where did the frame go" is
#: a question about modules and not about function names.
PRODUCERS = [
    ("weather_ms", "cy::weather — climate, cells, wind, fields"),
    ("water_ms", "cy::water — spectrum, foam, clock"),
    ("ocean_ms", "cy::water — the camera-relative surface patch"),
    ("sky_ms", "cy::rendering::sky — atmosphere and the cloud march"),
    ("terrain_shade_ms", "the substrate re-sampled for every terrain vertex"),
    ("foliage_ms", "cy::foliage — wind response over every plant"),
    ("stage_build_ms", "the sample's own vertex streams"),
    ("stage_submit_ms", "submit and wait for the device"),
]


def plot_budget(csv_path: pathlib.Path, name: str) -> bool:
    """Stack the per-producer cost against the time of day, with the sun's elevation over it."""
    import matplotlib

    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    with csv_path.open(newline="") as handle:
        rows = list(csv.DictReader(handle))
    if not rows:
        print(f"collect: {csv_path} has no rows", file=sys.stderr)
        return False

    # The take starts partway through the day and wraps through midnight, so the fraction the
    # program reports is not monotonic. Unwrapping it is what makes the x axis a timeline rather
    # than a sawtooth, and it is done here rather than in the program because the program's own
    # number is the CLOCK and should stay one.
    hours = []
    wraps = 0.0
    previous = None
    for row in rows:
        fraction = float(row["day_fraction"])
        if previous is not None and fraction < previous - 0.5:
            wraps += 1.0
        previous = fraction
        hours.append((fraction + wraps) * 24.0)
    series = [[float(row[column]) for row in rows] for column, _ in PRODUCERS]
    sun = [float(row["sun_elevation_degrees"]) for row in rows]
    rain = [float(row["precipitation_mm_per_hour"]) for row in rows]

    figure, (cost_axes, sky_axes) = plt.subplots(
        2, 1, figsize=(11, 7), height_ratios=[3, 1], sharex=True
    )
    cost_axes.stackplot(hours, *series, labels=[label for _, label in PRODUCERS], linewidth=0)
    total = [sum(values) for values in zip(*series)]
    cost_axes.plot(hours, total, color="black", linewidth=1.0, label="frame total")
    cost_axes.set_ylabel("milliseconds per frame")
    cost_axes.set_title(
        "samples/10-world — where one frame goes, across a full day/night cycle\n"
        "every producer on the processor; the device's share is the bottom band"
    )
    cost_axes.legend(loc="upper left", fontsize=8, framealpha=0.9)
    cost_axes.grid(alpha=0.25)
    cost_axes.set_ylim(bottom=0.0)

    sky_axes.plot(hours, sun, color="#b8860b", label="sun elevation (deg)")
    sky_axes.axhline(0.0, color="#888888", linewidth=0.8)
    sky_axes.fill_between(hours, 0.0, rain, color="#4682b4", alpha=0.5, step="mid",
                          label="precipitation (mm/h)")
    sky_axes.set_xlabel("hour of the simulated day (the take starts in the small hours and wraps "
                        "through midnight)")
    sky_axes.set_ylabel("sun / rain")
    sky_axes.legend(loc="upper left", fontsize=8, framealpha=0.9)
    sky_axes.grid(alpha=0.25)

    figure.tight_layout()
    out = IMAGES / f"{name}-budget.png"
    figure.savefig(out, dpi=110)
    plt.close(figure)
    print(f"    {out.name}  {out.stat().st_size // 1024} KiB  {len(rows)} frames")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("frames", help="the directory holding frame_%04d.png")
    parser.add_argument("--still", required=True, help="the representative frame, as written")
    parser.add_argument("--budget", help="the per-frame budget CSV the sample wrote")
    parser.add_argument("--fps", type=int, default=30)
    parser.add_argument("--name", default="m10-world")
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

    if arguments.budget:
        budget = pathlib.Path(arguments.budget)
        if not budget.is_file():
            print(f"collect: {budget} is missing; the capture wrote no budget curve",
                  file=sys.stderr)
            return 1
        if not plot_budget(budget, arguments.name):
            return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
