#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""samples/03-first-light — the M3 golden image, once per platform backend. M11.d task 4.3.

The M11 exit criterion in `ROADMAP.md` reads *"`samples/00-empty` and the M3 golden images run on
the native backend"*. The first half is `samples/00-empty --platform native` and it was measured
when the selector landed. THIS FILE IS THE SECOND HALF, and it exists because the obvious reading of
it names nothing:

--- WHAT `render.golden` IS, AND WHY IT IS NOT THE PLACE ------------------------------------------

`tests/render/CMakeLists.txt` links NO platform target. `render.golden` builds a `DeviceFixture`,
renders `cy::sample-first-light`'s frame into an offscreen colour target and compares it with a
committed PNG; there is no `cy::Platform`, no `cy::DisplayServer`, no surface and no swapchain
anywhere in `tests/render/`. A platform backend therefore cannot change one texel of that suite, and
"the goldens run on the native backend" is not a statement about it. Putting a window into it would
make every machine that runs the render suites need a display, which is the opposite of the argument
`tests/render/README.md` and `design.md` §1 make — that a rendering milestone must stay gated on the
machines that have no GPU at all.

--- WHAT DOES CARRY THE CLAIM ---------------------------------------------------------------------

The golden IMAGE is a different object from the golden SUITE. `tests/render/references/first_light.png`
is the picture `samples/03-first-light` draws, and `test_golden_frame.cpp` says so in its own words:

    Phase 0 rather than a phase somebody liked: it is the only phase a reader can reproduce from the
    scene alone, and `--frames 1` on the sample is the same frame.

And the sample — unlike the suite — is hosted. It owns a `cy::Platform`, a `cy::DisplayServer`, a
`cy::Runtime` and the host loop that drives its frames, and until M11.d that pair was hardwired to
SDL3. So the criterion is satisfiable exactly here: RUN THE PROGRAM THAT PRODUCES THE M3 GOLDEN
IMAGE ON EACH PLATFORM BACKEND, AND JUDGE WHAT COMES OUT AGAINST THE COMMITTED REFERENCE.

Two claims, and neither is a tolerance:

  1. every leg's capture is EXACTLY the committed reference — same 192x108, same 62208 bytes of RGB,
     no differing texel. `render.golden` compares with a neighbourhood tolerance because an image
     may legitimately be re-encoded; nothing here may differ at all, because it is the same device
     rendering the same frame with a different process owner.
  2. every leg's capture is byte-identical to every other leg's. This is the claim that has teeth if
     the reference is ever regenerated: it is the run comparing itself against itself, and it cannot
     be laundered by rewriting a committed file.

--- WHAT THIS CANNOT SAY --------------------------------------------------------------------------

It cannot say a platform backend presented the frame, because this sample does not present — it
renders offscreen and always has. The artefact that opens a swapchain on a window is
`samples/11-ship`, whose two captures through `platform/linux-native` and `platform/desktop-sdl3`
are byte-identical with manifests differing in one line. The two measurements are complementary and
neither replaces the other: 11-ship proves a presented frame does not depend on the display server,
and this file proves the M3 REFERENCE IMAGE does not depend on the platform beneath it.

A leg this machine cannot run — no display for the windowed legs, no GPU for any of them — is
reported NOT EVALUATED rather than passed, and a run that could evaluate no leg at all exits 3,
which the CTest entry maps to SKIP.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import zlib

from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from harness.artefact import Absent, Failed, Report  # noqa: E402

ROOT = Path(__file__).resolve().parents[2]

#: The reference the M3 milestone closed on, and the suite that judges it.
REFERENCE = ROOT / "tests" / "render" / "references" / "first_light.png"

#: `test_golden_frame.cpp`'s `kWidth` and `kHeight`, which are also this sample's defaults.
WIDTH = 192
HEIGHT = 108

#: Every leg, and what each one needs. The windowed two are what task 4.3 is about; `headless` is
#: the pair the sample has used since M3 and is the control; `stub` has no desktop assumption at all
#: and is here because a porting surface that is only ever exercised by desktops is not a surface.
LEGS = (
    ("headless", False),
    ("sdl3", True),
    ("native", True),
    ("stub", False),
)


# --- the reference, decoded with nothing but the standard library ---------------------------------


def _unfilter(raw: bytes, width: int, height: int, stride: int) -> bytearray:
    """Undo PNG's per-scanline filters. Colour type 2, bit depth 8, no interlace."""
    out = bytearray(width * height * 3)
    previous = bytearray(stride)
    position = 0
    for row in range(height):
        filter_type = raw[position]
        line = bytearray(raw[position + 1 : position + 1 + stride])
        position += 1 + stride
        for i in range(stride):
            left = line[i - 3] if i >= 3 else 0
            up = previous[i]
            up_left = previous[i - 3] if i >= 3 else 0
            if filter_type == 1:
                line[i] = (line[i] + left) & 0xFF
            elif filter_type == 2:
                line[i] = (line[i] + up) & 0xFF
            elif filter_type == 3:
                line[i] = (line[i] + ((left + up) >> 1)) & 0xFF
            elif filter_type == 4:
                p = left + up - up_left
                pa, pb, pc = abs(p - left), abs(p - up), abs(p - up_left)
                nearest = left if (pa <= pb and pa <= pc) else (up if pb <= pc else up_left)
                line[i] = (line[i] + nearest) & 0xFF
            elif filter_type != 0:
                raise Failed(f"unknown PNG filter {filter_type} on row {row}")
        out[row * stride : (row + 1) * stride] = line
        previous = line
    return out


def read_reference(path: Path) -> bytes:
    """The committed reference as packed RGB triples, in the same order a PPM stores them.

    Hand-decoded rather than reached for through an image library, for the reason `write_ppm` in
    `main.cpp` gives about its own encoder: a check on a picture should not be able to fail because
    a dependency is missing on the machine running it.
    """
    data = path.read_bytes()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise Failed(f"{path} is not a PNG")
    width = height = depth = colour = None
    compressed = bytearray()
    position = 8
    while position < len(data):
        length = int.from_bytes(data[position : position + 4], "big")
        kind = data[position + 4 : position + 8]
        body = data[position + 8 : position + 8 + length]
        position += 12 + length  # length, type, body, CRC
        if kind == b"IHDR":
            width = int.from_bytes(body[0:4], "big")
            height = int.from_bytes(body[4:8], "big")
            depth, colour = body[8], body[9]
            if (depth, colour, body[12]) != (8, 2, 0):
                raise Failed(
                    f"{path}: this decoder reads 8-bit RGB without interlace; it is depth {depth}, "
                    f"colour type {colour}, interlace {body[12]}"
                )
        elif kind == b"IDAT":
            compressed += body
        elif kind == b"IEND":
            break
    if width is None:
        raise Failed(f"{path} has no IHDR")
    return bytes(_unfilter(zlib.decompress(bytes(compressed)), width, height, width * 3))


def read_capture(path: Path) -> bytes:
    """The sample's binary PPM as packed RGB triples. `write_ppm` in `main.cpp` is the encoder."""
    data = path.read_bytes()
    if not data.startswith(b"P6\n"):
        raise Failed(f"{path} is not the binary PPM this sample writes")
    # "P6\n<width> <height>\n255\n" — three newline-terminated fields, then the pixels.
    header_end = 0
    for _ in range(3):
        header_end = data.index(b"\n", header_end) + 1
    return data[header_end:]


def differing_texels(left: bytes, right: bytes) -> tuple[int, int]:
    """(texels that differ, the largest single-channel difference)."""
    if len(left) != len(right):
        raise Failed(f"images of different sizes: {len(left)} and {len(right)} bytes")
    differing = 0
    worst = 0
    for i in range(0, len(left), 3):
        a, b = left[i : i + 3], right[i : i + 3]
        if a != b:
            differing += 1
            worst = max(worst, max(abs(x - y) for x, y in zip(a, b)))
    return differing, worst


# --- the legs -------------------------------------------------------------------------------------


def run_leg(binary: Path, leg: str, capture: Path) -> subprocess.CompletedProcess:
    """One frame, at the viewport the reference was drawn at, hosted by `leg`."""
    invocation = [
        str(binary),
        "--platform", leg,
        "--frames", "1",
        "--width", str(WIDTH),
        "--height", str(HEIGHT),
        "--capture", str(capture),
    ]
    print(f"==> {' '.join(invocation)}")
    return subprocess.run(
        invocation, cwd=str(ROOT), capture_output=True, text=True, timeout=300, check=False
    )


#: The two error codes that mean "this machine does not have it" rather than "it is broken".
#: `main.cpp`'s `report()` prints the code's name in brackets, which is a far steadier thing to read
#: than SDL's or Xlib's own wording — "no X display", "No available video device" and whatever the
#: next windowing library says are all `Unavailable`, and a build with no native backend answers
#: `Unsupported` in a sentence this file must not have to match.
ABSENT_CODES = ("(Unavailable)", "(Unsupported)")


def why_absent(result: subprocess.CompletedProcess) -> str | None:
    """The reason this MACHINE could not judge the leg, or None when it could."""
    output = result.stdout + result.stderr
    if "nothing to write" in output or "backend=null" in output:
        return "no graphics device on this host, so the frame has no texels to compare"
    for line in output.splitlines():
        if "platform failed:" in line and any(code in line for code in ABSENT_CODES):
            return line.split("platform failed:", 1)[1].strip()
    return None


def judge(report: Report, binary: Path, work: Path, reference: bytes) -> dict[str, bytes]:
    """Run every leg, judge each against the reference, and hand back what each one drew."""
    captured: dict[str, bytes] = {}
    for leg, needs_display in LEGS:
        capture = work / f"first_light_{leg}.ppm"
        result = run_leg(binary, leg, capture)
        absent = why_absent(result)
        if absent is not None:
            report.not_evaluated(f"the M3 frame on the {leg} platform backend", absent)
            continue
        if result.returncode != 0 or not capture.exists():
            tail = (result.stdout + result.stderr).strip().splitlines()[-3:]
            report.gap(
                f"the M3 frame on the {leg} platform backend",
                f"exit {result.returncode}: {' | '.join(tail) if tail else 'no output'}",
            )
            continue
        pixels = read_capture(capture)
        differing, worst = differing_texels(reference, pixels)
        where = "with a window" if needs_display else "with no window"
        if differing == 0:
            report.did(
                f"the M3 golden image on the {leg} platform backend",
                f"{WIDTH}x{HEIGHT}, {len(pixels)} bytes, EXACTLY "
                f"tests/render/references/first_light.png ({where})",
            )
            captured[leg] = pixels
        else:
            report.gap(
                f"the M3 golden image on the {leg} platform backend",
                f"{differing} texel(s) differ from the committed reference, worst channel {worst}",
            )
            captured[leg] = pixels
    return captured


def judge_against_each_other(report: Report, captured: dict[str, bytes]) -> None:
    """The claim that survives a regenerated reference: the legs agree with one another."""
    legs = sorted(captured)
    if len(legs) < 2:
        report.not_evaluated(
            "one platform backend against another",
            f"only {len(legs)} leg(s) could be run on this machine, and agreement needs two",
        )
        return
    first = legs[0]
    for other in legs[1:]:
        differing, worst = differing_texels(captured[first], captured[other])
        if differing == 0:
            report.did(
                f"{other} draws the same image as {first}",
                f"byte-identical, {len(captured[other])} bytes — the platform backend is not in "
                "the picture",
            )
        else:
            report.gap(
                f"{other} draws the same image as {first}",
                f"{differing} texel(s) differ, worst channel {worst}",
            )
    if "native" not in captured:
        report.not_evaluated(
            "the M11 exit criterion's own leg",
            "platform/linux-native could not be run here, so this run does not carry the criterion",
        )


def find_binary(arguments: argparse.Namespace) -> Path:
    if arguments.sample:
        return Path(arguments.sample)
    build = Path(arguments.build_dir or f"build/{arguments.profile}")
    candidate = ROOT / build / "samples" / "03-first-light" / "cy_sample_first-light"
    if not candidate.exists():
        raise Absent(f"no cy_sample_first-light at {candidate}; build it with `just build-engine`")
    return candidate


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sample", help="the cy_sample_first-light binary to drive")
    parser.add_argument("--build-dir", help="where the engine was built")
    parser.add_argument("--profile", default="dev")
    parser.add_argument("--work", help="a scratch directory for the captures")
    arguments = parser.parse_args(argv)

    report = Report()
    try:
        binary = find_binary(arguments)
        work = Path(arguments.work) if arguments.work else ROOT / "build" / "first-light-legs"
        work.mkdir(parents=True, exist_ok=True)
        if not REFERENCE.exists():
            raise Absent(f"no committed reference at {REFERENCE}")
        reference = read_reference(REFERENCE)
        if len(reference) != WIDTH * HEIGHT * 3:
            raise Failed(
                f"{REFERENCE} is not {WIDTH}x{HEIGHT}: it decoded to {len(reference)} bytes"
            )
        print(f"==> reference {REFERENCE} — {WIDTH}x{HEIGHT}, {len(reference)} bytes of RGB")

        captured = judge(report, binary, work, reference)
        judge_against_each_other(report, captured)
        if not captured:
            raise Absent(
                "no platform backend could draw the M3 frame on this machine — no graphics device, "
                "or no display"
            )
    except Absent as absent:
        print(f"\n--- not evaluated ---\n    {absent}")
        return 3
    except Failed as failed:
        report.failed(str(failed))
    return report.summarise()


if __name__ == "__main__":
    sys.exit(main())
