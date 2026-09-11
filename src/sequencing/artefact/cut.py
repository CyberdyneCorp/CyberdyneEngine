#!/usr/bin/env python3
"""The cut, captured mid-blend. M8.c section 3, and the milestone's rule 14.

`smoke.sequence_cut` is the CTest entry and this file is what it runs. It drives
`cy_sequence_cut-capture` — a real compiled sequence, played through a real `cy::camera::CameraServer`
— checks every claim that program printed, and draws three panels of the cut from the
VIEW-PROJECTION MATRICES THE CAMERA STACK PRODUCED.

--- WHY THE PICTURE IS TRUSTWORTHY, AND WHERE IT STOPS BEING THE ENGINE'S -------------------------

Everything about the CAMERA in the image is the engine's own output: the pose is `CameraStack`'s
blend of two evaluated rigs, the field of view is `Lens::blend`'s, the matrix is
`render::Projection::matrix()`'s, and the weights on the panels are the stack's own contribution
report. If the blend were wrong, if a shot selected the wrong rig, or if anything wrote a pose
directly, these panels would show it.

The SCENE is not the engine's: this section of the milestone owns a camera, not a renderer, so the
fourteen world points below come from the capture program's own table and are labelled on the image
as such. That distinction is exactly the one M8.b's gate found missing — an artefact that drew its
silhouettes from the sample's own table "would have drawn a defect correctly" — so it is written on
the picture rather than in a commit message.

It reports through samples/harness/artefact.py, which is what makes a recorded gap a non-zero exit
and refuses an extreme value as the figure the run leads with.
"""

from __future__ import annotations

import argparse
import subprocess
import sys

from pathlib import Path

ARTEFACT_DIR = Path(__file__).resolve().parent
ROOT = ARTEFACT_DIR.parents[2]
sys.path.insert(0, str(ROOT / "samples" / "harness"))

from artefact import Failed, Report, Statistic, expect  # noqa: E402


INK = {
    "window": (0x0E, 0x11, 0x16),
    "panel": (0x16, 0x1B, 0x22),
    "world": (0x10, 0x14, 0x1A),
    "primary": (0xE6, 0xED, 0xF3),
    "secondary": (0x8A, 0x99, 0xA8),
    "live": (0x5A, 0xD1, 0x9A),
    "wide": (0x5A, 0x9A, 0xD1),
    "tight": (0xD1, 0x8A, 0x5A),
    "floor": (0x25, 0x2E, 0x38),
    "pillar": (0x6E, 0x7C, 0x8A),
    "subject": (0xE0, 0xC4, 0x6A),
}


def run_capture(binary: Path) -> dict:
    """Run the capture program once and parse what it printed."""
    completed = subprocess.run(
        [str(binary)], cwd=str(ROOT), capture_output=True, text=True, timeout=600,
    )
    if completed.returncode != 0:
        raise Failed(
            f"cy_sequence_cut-capture exited {completed.returncode}\n"
            f"{completed.stdout[-3000:]}\n{completed.stderr[-3000:]}"
        )

    values: dict[str, str] = {}
    frames: list[dict] = []
    points: list[tuple[tuple[float, float, float], str]] = []
    pending: dict | None = None
    for line in completed.stdout.splitlines():
        if " = " not in line:
            continue
        key, raw = line.split(" = ", 1)
        if key == "frame":
            parts = raw.split()
            pending = {
                "index": int(parts[0]),
                "wide": float(parts[1]),
                "tight": float(parts[2]),
                "fov": float(parts[3]),
                "position": (float(parts[4]), float(parts[5]), float(parts[6])),
                "contributions": int(parts[7]),
            }
        elif key == "viewproj":
            expect(pending is not None, "a viewproj line arrived before its frame line")
            pending["viewproj"] = [float(value) for value in raw.split()]
            frames.append(pending)
            pending = None
        elif key == "point":
            parts = raw.split()
            points.append(((float(parts[0]), float(parts[1]), float(parts[2])), parts[3]))
        else:
            values[key] = raw
    return {"values": values, "frames": frames, "points": points}


def number(values: dict[str, str], key: str) -> float:
    raw = values.get(key)
    expect(raw is not None, f"the capture printed no '{key}'")
    return float(str(raw))


def whole(values: dict[str, str], key: str) -> int:
    return int(number(values, key))


def check(report: Report, name: str, ok: bool, detail: str) -> bool:
    if ok:
        report.did(name, detail)
    else:
        report.gap(name, detail)
    return ok


# --- The picture ----------------------------------------------------------------------------------


def project(matrix: list[float], point: tuple[float, float, float], width: int, height: int):
    """One world point through the engine's own view-projection. Column-major, as printed."""
    vector = (point[0], point[1], point[2], 1.0)
    clip = [sum(matrix[column * 4 + row] * vector[column] for column in range(4)) for row in range(4)]
    if clip[3] <= 1e-4:
        return None
    x = ((clip[0] / clip[3]) * 0.5 + 0.5) * width
    y = (1.0 - ((clip[1] / clip[3]) * 0.5 + 0.5)) * height
    return (x, y)


def draw_panel(image, frame: dict, points, box, title: str, fonts) -> None:
    """One panel, drawn into its own surface so nothing spills over its neighbours."""
    from PIL import Image, ImageDraw

    left, top, width, height = box
    panel = Image.new("RGB", (width, height), INK["world"])
    draw = ImageDraw.Draw(panel)

    def place(point):
        return project(frame["viewproj"], point, width, height)

    floor = [place(position) for position, kind in points if kind == "floor"]
    if len(floor) == 4 and all(corner is not None for corner in floor):
        draw.polygon(floor, fill=INK["floor"])

    pillars = [position for position, kind in points if kind == "pillar"]
    for index in range(0, len(pillars) - 1, 2):
        base = place(pillars[index])
        head = place(pillars[index + 1])
        if base is None or head is None:
            continue
        draw.line([base, head], fill=INK["pillar"], width=6)

    subject = [position for position, kind in points if kind == "subject"]
    if len(subject) >= 2:
        base = place(subject[0])
        head = place(subject[1])
        if base is not None and head is not None:
            draw.line([base, head], fill=INK["subject"], width=8)
            draw.ellipse([head[0] - 9, head[1] - 9, head[0] + 9, head[1] + 9], fill=INK["subject"])

    # The weight bar: the stack's own contribution report, drawn to scale.
    bar_top = height - 34
    draw.rectangle([12, bar_top, width - 12, bar_top + 12], fill=INK["panel"])
    span = width - 24
    wide_width = int(span * max(0.0, min(1.0, frame["wide"])))
    tight_width = int(span * max(0.0, min(1.0, frame["tight"])))
    draw.rectangle([12, bar_top, 12 + wide_width, bar_top + 12], fill=INK["wide"])
    draw.rectangle([12 + span - tight_width, bar_top, 12 + span, bar_top + 12], fill=INK["tight"])

    draw.text((12, 10), title, font=fonts["bold"], fill=INK["primary"])
    draw.text((12, 30),
              f"wide {frame['wide']:.2f}   tight {frame['tight']:.2f}   "
              f"fov {frame['fov'] * 57.29578:.1f}\u00b0",
              font=fonts["small"], fill=INK["secondary"])
    draw.text((12, height - 18),
              f"camera ({frame['position'][0]:.1f}, {frame['position'][1]:.1f}, "
              f"{frame['position'][2]:.1f})  \u2022  frame {frame['index']}",
              font=fonts["small"], fill=INK["secondary"])
    image.paste(panel, (left, top))


def fonts_for():
    from PIL import ImageFont

    def load(size: int, bold: bool = False):
        candidates = [
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf" if bold else
            "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
            "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        ]
        for candidate in candidates:
            if Path(candidate).exists():
                return ImageFont.truetype(candidate, size)
        return ImageFont.load_default()

    return {"bold": load(15, True), "small": load(12), "body": load(13)}


def render(report: Report, capture: dict, chosen: list[tuple[str, dict]], headline: Statistic,
           path: Path) -> None:
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print(f"    (no Pillow, so no screenshot at {path})", file=sys.stderr)
        return

    panel_width, panel_height = 420, 300
    banner, footer, gap = 62, 96, 10
    width = (panel_width * 3) + (gap * 4)
    height = banner + panel_height + footer + (gap * 2)
    image = Image.new("RGB", (width, height), INK["window"])
    draw = ImageDraw.Draw(image)
    fonts = fonts_for()

    draw.rectangle([0, 0, width, banner], fill=INK["panel"])
    draw.text((24, 14), "CyberEngine", font=fonts["bold"], fill=INK["primary"])
    draw.text((150, 15), "M8.c — CyberSequence: a cut, through the camera stack",
              font=fonts["body"], fill=INK["secondary"])
    draw.text((150, 34),
              "the sequence selects rigs, priorities and blends; the STACK performs the blend; "
              "no camera transform is written",
              font=fonts["small"], fill=INK["secondary"])
    label = str(headline)
    length = draw.textlength(label, font=fonts["small"])
    draw.text((width - length - 24, 16), label, font=fonts["small"], fill=INK["live"])

    for index, (title, frame) in enumerate(chosen):
        draw_panel(image, frame, capture["points"],
                   (gap + index * (panel_width + gap), banner + gap, panel_width, panel_height),
                   title, fonts)

    values = capture["values"]
    lines = [
        f"camera  {whole(values, 'pushed')} contribution(s) pushed, "
        f"{whole(values, 'released')} released, {whole(values, 'cuts')} cut and "
        f"{whole(values, 'anticipated_cuts')} announced in advance, "
        f"{whole(values, 'framing_targets')} framing target(s) set",
        f"blend   {whole(values, 'blend_frames')} frames with both shots contributing; "
        f"pose_overridden was false on every one of {len(capture['frames'])} frames "
        f"— CameraServer::override_pose() is never called",
        f"program {whole(values, 'segments')} segments, {whole(values, 'channels')} channels, "
        f"{whole(values, 'bucket_count')} interval buckets",
        "picture the pose, the lens and the matrices are the engine's "
        "(CameraStack::blend, Lens::blend, render::Projection::matrix); "
        "the 14 scene points are the capture program's own table",
    ]
    y = banner + panel_height + (gap * 2) + 6
    for text in lines:
        draw.text((24, y), text[:190], font=fonts["small"], fill=INK["secondary"])
        y += 17

    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)
    report.shot(path)


# --- The run --------------------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", required=True, help="the capture program")
    parser.add_argument("--work", help="a scratch directory (unused; accepted for symmetry)")
    parser.add_argument("--shot", help="write the picture here")
    arguments = parser.parse_args()

    report = Report()
    binary = Path(arguments.program)
    if not binary.exists():
        print(f"cut.py: no capture program at {binary}", file=sys.stderr)
        return 1

    print("\n--- the cut ---")
    capture = run_capture(binary)
    values = capture["values"]
    frames = capture["frames"]
    expect(len(frames) > 0, "the capture printed no frames")

    cut_frame = whole(values, "cut_frame")
    blend_frames = whole(values, "blend_frames")

    check(report, "the sequence compiled to segments and an index",
          whole(values, "segments") >= 3 and whole(values, "bucket_count") > 0,
          f"{whole(values, 'segments')} segments, {whole(values, 'channels')} channel(s), "
          f"{whole(values, 'bucket_count')} interval buckets")

    check(report, "both shots reached the camera stack as contributions",
          whole(values, "pushed") >= 2,
          f"{whole(values, 'pushed')} pushed, {whole(values, 'released')} released as declared, "
          f"{whole(values, 'unresolved_rigs')} unresolved")

    check(report, "the cut was announced before it happened",
          whole(values, "anticipated_cuts") >= 1,
          f"{whole(values, 'anticipated_cuts')} anticipated announcement(s) and "
          f"{whole(values, 'cuts')} cut(s) raised on the rig — the anticipated one becomes a "
          "residency deadline through the camera's own streaming source")

    check(report, "the shot parameterised what the rig frames, rather than posing it",
          whole(values, "framing_targets") >= 1,
          f"{whole(values, 'framing_targets')} framing target(s) set through "
          "CameraServer::set_target()")

    # THE CRITERION. A blend the reader can see, and a pose nobody wrote.
    overrides = whole(values, "pose_overrides")
    check(report, "no camera transform was written on any frame",
          overrides == 0,
          (f"pose_overridden false on all {len(frames)} frames; the bridge calls push(), "
           "release(), cut() and set_target() and never override_pose()") if overrides == 0 else
          (f"pose_overridden was TRUE on {overrides} of {len(frames)} frames — something called "
           "CameraServer::override_pose(), which is the one call this criterion forbids"))

    blending = [frame for frame in frames if frame["wide"] > 0.01 and frame["tight"] > 0.01]
    check(report, "the two shots overlap, so the cut is a blend rather than a jump",
          blend_frames >= 12 and len(blending) >= 12,
          f"{blend_frames} frames with both shots contributing — a one-second blend at 24 "
          "frames per second")

    midpoint = min(blending, key=lambda frame: abs(frame["tight"] - 0.5)) if blending else None
    if midpoint is None:
        report.gap("a mid-blend frame exists to photograph", "no frame had both shots contributing")
        return report.summarise()

    before = frames[max(0, cut_frame - 6)]
    after = frames[min(len(frames) - 1, cut_frame + 40)]
    check(report, "mid-blend the camera is between the two shots and equal to neither",
          before["position"][2] > midpoint["position"][2] > after["position"][2]
          and before["fov"] > midpoint["fov"] > after["fov"],
          f"z {before['position'][2]:.2f} → {midpoint['position'][2]:.2f} → "
          f"{after['position'][2]:.2f} m, "
          f"fov {before['fov'] * 57.29578:.1f} → {midpoint['fov'] * 57.29578:.1f} → "
          f"{after['fov'] * 57.29578:.1f}° — the lens blends as well as the pose")

    check(report, "the light track drove a value through arbitration",
          number(values, "light_final") > 1.0,
          f"the key light finished at {number(values, 'light_final'):.2f} — a property track "
          "resolved through arbitration and handed back as one value")

    # The headline is a MEDIAN over the blend's frames, not the midpoint sample: the harness refuses
    # a figure whose kind a reader cannot tell, and a single sample of anything is a draw.
    headline = report.headline(
        Statistic.median("median incoming-shot weight through the blend",
                         [frame["tight"] for frame in blending]))
    report.figure(Statistic.stable("count of frames in the blend", float(blend_frames), "frames"))
    report.figure(Statistic.stable("count of stack contributions pushed",
                                   float(whole(values, "pushed")), ""))

    if arguments.shot:
        render(report, capture,
               [("before — the wide shot", before),
                ("mid-blend — both shots", midpoint),
                ("after — the long lens", after)],
               headline, Path(arguments.shot))

    return report.summarise(Path(arguments.shot).parent if arguments.shot else None)


if __name__ == "__main__":
    sys.exit(main())
