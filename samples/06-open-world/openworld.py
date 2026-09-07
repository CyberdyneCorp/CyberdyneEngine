#!/usr/bin/env python3
"""samples/06-open-world — M6's closing artefact. Section 9.

`just run-open-world` is the recipe; `smoke.open_world` is the CTest entry; this file is what both
of them run. It needs no display and no graphics device: the world is streamed, the surfaces are
paged and the save is written on the CPU, so this artefact is one continuous integration can judge.

--- WHAT IT IS ---------------------------------------------------------------------------------

Five acts, in the order the milestone claims them, over two programs and one installation:

  1. SHIP     `cy_build` runs the world's derivation graph, proves two cold builds produce identical
              artefacts, writes a package manifest, and installs it. Four nodes: two imports, a cook
              over both, a package over the cook.
  2. PLAY     `cy_sample_open-world` reads the world out of that installation BY LOGICAL NAME,
              streams six kilometres of city along a fixed route at speed, pages two virtual
              textures under the shared residency policy, records what the player changed in the
              persistence overlay, saves, and exits. The exit is the quit.
  3. RESUME   a second process loads the save, reactivates exactly the cells it holds state for, and
              checks every removal and every override against the live world.
  4. TEARDOWN the same route, torn down mid-flight at three different ticks with cells preparing and
              pages in production on worker threads. M5.5's gate found this project's first real
              engine defect that way, and M6 destroys worlds continuously.
  5. PATCH    one line of content changes. The graph is rebuilt — and the node that did not read it
              is served from the cache, which is what "a one-asset change invalidates only the
              derivations that depend on it" looks like in the outcome column. The difference is
              shipped as a patch, an INTERRUPTED application is shown to leave the previous build
              playable, the patch is then applied for real, and the world is played again: the same
              route now reports a different landmark, out of the same installation directory, with
              nothing in the game changed.

THE RUN IS THE EVIDENCE. Every claim above is checked here against what the two programs printed,
and a claim that does not hold fails the run rather than being reported as a note. `--shot <path>`
photographs the run in the editor's own palette, which is what puts M6 in docs/design/images/.
"""

from __future__ import annotations

import argparse
import math
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

SAMPLE = Path(__file__).resolve().parent
ROOT = SAMPLE.parents[1]


class Failed(Exception):
    """A claim this artefact makes that did not hold."""


# --- The transcript ---------------------------------------------------------------------------------


class Report:
    """What the run did, in the order it did it. Printed, and photographed by `--shot`."""

    def __init__(self) -> None:
        self.lines: list[tuple[str, str]] = []
        self.checks = 0

    def act(self, title: str) -> None:
        print(f"\n==> {title}")
        self.lines.append(("act", title))

    def did(self, claim: str, detail: str = "") -> None:
        self.checks += 1
        print(f"    ok    {claim}" + (f"  —  {detail}" if detail else ""))
        self.lines.append(("ok", f"{claim}" + (f"  —  {detail}" if detail else "")))

    def note(self, text: str) -> None:
        print(f"          {text}")
        self.lines.append(("note", text))


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failed(message)


def run(command: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess:
    """Run one process and hand back everything it said. Never checked here; the acts check."""
    return subprocess.run(
        [str(part) for part in command],
        cwd=str(cwd or ROOT),
        capture_output=True,
        text=True,
        timeout=900,
    )


def number(text: str, pattern: str, what: str) -> float:
    match = re.search(pattern, text)
    expect(match is not None, f"{what}: the output does not carry it\n{text}")
    return float(match.group(1))


OUTCOME = re.compile(r"^(ran|cached|rebuilt|skipped|failed)\s+(\S+)\s+([0-9a-f]{16})")


def nodes(text: str) -> dict[str, tuple[str, str]]:
    """Every node `cy_build build` reported: its outcome and its derivation key, by name.

    Matched on the key that follows the name, so the report's own summary line — `ran 4 cached 0
    ...` — is not read as a node called "4". The KEY is what act 5 compares: whether a node ran is a
    fact about the cache's contents, and whether its key moved is the fact about invalidation.
    """
    return {match.group(2): (match.group(1), match.group(3))
            for match in (OUTCOME.match(line) for line in text.splitlines())
            if match is not None}


def outcomes(text: str) -> dict[str, str]:
    return {name: outcome for name, (outcome, _) in nodes(text).items()}


# --- Act 1: ship ------------------------------------------------------------------------------------


class Tools:
    def __init__(self, sample: Path, cy_build: Path, work: Path) -> None:
        self.sample = sample
        self.cy_build = cy_build
        self.work = work
        self.project = work / "project"
        self.artefacts = work / "artefacts"
        self.cache = work / "cache"
        self.install = work / "install"
        self.save = work / "save"

    def build(self, package: Path) -> subprocess.CompletedProcess:
        return run([
            self.cy_build, "build",
            "--description", self.project / "build" / "openworld.cybuild",
            "--project", self.project,
            "--out", self.artefacts,
            "--cache", self.cache,
            "--package", package,
            "--platform", "host",
            "--profile", "client",
        ])

    def play(self, act: str, ticks: int, extra: list[str] | None = None):
        command = [
            self.sample, "--act", act,
            "--install", self.install,
            "--save", self.save,
            "--ticks", str(ticks),
        ]
        return run(command + (extra or []))


def act_ship(tools: Tools, report: Report) -> dict[str, tuple[str, str]]:
    report.act("Act 1 — the content is a graph: cooked, proven deterministic, packaged, installed")

    # A COPY, so a run never edits the committed project: act 5 changes a line of content, and an
    # artefact that wrote into its own source directory would make the second run start from the
    # first run's result. samples/05b-agent-authoring states the same rule.
    #
    # AND A COLD CACHE, which is not tidiness either: the first build below has to be a build that
    # RAN, or "a cold build and a cache-warm build produce byte-identical artefacts" is being checked
    # against two warm builds and says nothing. A previous run of this driver left both directories
    # full, and the first draft of this act duly reported four cache hits as a cold build.
    for directory in (tools.project, tools.cache, tools.artefacts, tools.install):
        if directory.exists():
            shutil.rmtree(directory)
    shutil.copytree(SAMPLE / "project", tools.project)

    built = tools.build(tools.work / "base.cypackage")
    expect(built.returncode == 0, f"the graph did not build:\n{built.stdout}{built.stderr}")
    cold = nodes(built.stdout)
    expect(set(cold) == {"import:palette", "import:city", "cook:city", "package:world"},
           f"the graph is not the four nodes the description declares:\n{built.stdout}")
    expect(all(outcome == "ran" for outcome, _ in cold.values()),
           f"a cold build did not run every node:\n{built.stdout}")
    report.did("the world's graph built cold",
               " ".join(f"{name}={outcome}" for name, (outcome, _) in sorted(cold.items())))

    warm = tools.build(tools.work / "warm.cypackage")
    expect(warm.returncode == 0, f"the warm build failed:\n{warm.stdout}{warm.stderr}")
    expect(set(outcomes(warm.stdout).values()) == {"cached"},
           f"a second build of unchanged content ran a node:\n{warm.stdout}")
    base_manifest = (tools.work / "base.cypackage").read_text()
    warm_manifest = (tools.work / "warm.cypackage").read_text()
    expect(base_manifest == warm_manifest,
           "the cold build and the cache-warm build produced different manifests")
    report.did("a cold build and a cache-warm build produce byte-identical artefacts",
               f"{len(cold)} nodes, all cached the second time")

    determinism = run([
        tools.cy_build, "determinism",
        "--description", tools.project / "build" / "openworld.cybuild",
        "--project", tools.project,
        "--out", tools.work / "determinism",
    ])
    expect(determinism.returncode == 0,
           f"two cold builds differ:\n{determinism.stdout}{determinism.stderr}")
    report.did("two cold builds into two roots agree, and embed no absolute path",
               determinism.stdout.strip().splitlines()[-1])

    installed = run([
        tools.cy_build, "install",
        "--package", tools.work / "base.cypackage",
        "--artefacts", tools.artefacts,
        "--install", tools.install,
    ])
    expect(installed.returncode == 0, f"the build did not install:\n{installed.stderr}")
    report.did("the build is installed", installed.stdout.strip())
    return cold


# --- Act 2 and 3: play, quit, resume ----------------------------------------------------------------


def act_play(tools: Tools, report: Report, ticks: int) -> dict[str, float]:
    report.act("Act 2 — a world larger than memory, traversed continuously at speed")
    if tools.save.exists():
        shutil.rmtree(tools.save)

    started = time.monotonic()
    played = tools.play("traverse", ticks)
    elapsed = time.monotonic() - started
    expect(played.returncode == 0,
           f"the traversal reported a failure:\n{played.stdout}{played.stderr}")
    text = played.stdout
    print(text)

    figures = {
        "activated": number(text, r"cells\s+activated (\d+)", "cells activated"),
        "deactivated": number(text, r"deactivated (\d+)", "cells deactivated"),
        "evicted": number(text, r"evicted (\d+)", "cells evicted"),
        "published": number(text, r"peak\s+(\d+) entities", "peak entities"),
        "entities": number(text, r"of (\d+) in the world", "the world's entity count"),
        "worst_us": number(text, r"worst ([\d.]+) us", "the worst tick"),
        "hitches": number(text, r"hitches (\d+)", "hitches"),
        "missing": number(text, r"missing samples (\d+)", "missing samples"),
        "produced": number(text, r"produced (\d+)", "pages produced"),
        "resident_idle": number(text, r"(\d+) cells resident with nothing simulating",
                                "cells resident with nothing simulating"),
        "landmarks": number(text, r"landmarks passed (\d+)", "landmarks passed"),
        "removed": number(text, r"entities removed (\d+)", "entities removed"),
        "modified": number(text, r"modified (\d+)", "entities modified"),
        "digest": float(int(re.search(r"content digest ([0-9a-f]+)", text).group(1), 16)),
    }

    expect(figures["activated"] > 20 and figures["deactivated"] > 10,
           "cells did not stream in and out along the route")
    expect(figures["evicted"] > 0, "nothing was ever evicted: the world fits in the budget")
    expect(figures["published"] < figures["entities"] * 0.25,
           "most of the world was resident at once, so it is not larger than memory")
    report.did("cells streamed in and out continuously",
               f"{figures['activated']:.0f} activated, {figures['deactivated']:.0f} withdrawn, "
               f"{figures['evicted']:.0f} evicted; at most "
               f"{100 * figures['published'] / figures['entities']:.1f}% of the world resident")

    expect(figures["hitches"] == 0, "a tick cost more than the threshold")
    report.did("the frame budget held over the whole route",
               f"worst tick {figures['worst_us']:.0f} us of CPU time, no hitch")

    expect(figures["produced"] > 0, "no texture page was ever produced")
    expect(figures["missing"] == 0, "a sample resolved to nothing; the mip tail is not resident")
    report.did("textures paged under traversal, and no frame was ever missing",
               f"{figures['produced']:.0f} pages produced, 0 missing samples")

    expect(figures["resident_idle"] > 0,
           "no cell was resident without being activated: residency and activation are not separate")
    report.did("residency and activation are separate",
               f"{figures['resident_idle']:.0f} cells resident with nothing simulating in them")

    expect(figures["landmarks"] >= 1, "the route passed no landmark, so nothing was changed")
    report.did("the player changed the world",
               f"{figures['landmarks']:.0f} landmarks passed, {figures['removed']:.0f} entities "
               f"removed, {figures['modified']:.0f} modified")
    report.note(f"the route took {elapsed:.1f} s of wall clock, {ticks} ticks")
    return figures


def act_resume(tools: Tools, report: Report, ticks: int, played: dict[str, float]) -> None:
    report.act("Act 3 — quit and reloaded: the world resumes in the state that was saved")
    resumed = tools.play("resume", ticks)
    expect(resumed.returncode == 0,
           f"the save did not resume:\n{resumed.stdout}{resumed.stderr}")
    print(resumed.stdout)

    honoured = number(resumed.stdout, r"(\d+) removals honoured", "removals honoured")
    matched = number(resumed.stdout, r"(\d+) overrides matched", "overrides matched")
    mismatches = number(resumed.stdout, r"(\d+) mismatches", "mismatches")
    expect(mismatches == 0, "the resumed world differs from the save")
    expect(honoured == played["removed"], "an entity the save destroyed came back")
    expect(matched == played["modified"], "a component the save changed came back unchanged")
    report.did("every change survived the quit",
               f"{honoured:.0f} removals honoured, {matched:.0f} overrides matched, 0 mismatches")

    saved = re.search(r"saved ([0-9a-f]+)\s+installed ([0-9a-f]+)", resumed.stdout)
    expect(saved is not None and saved.group(1) == saved.group(2),
           "the save was written against different content from the one installed")
    report.did("the save names the content it was written against", f"digest {saved.group(1)}")


# --- Act 4: teardown under load ---------------------------------------------------------------------


def act_teardown(tools: Tools, report: Report, ticks: int) -> None:
    report.act("Act 4 — torn down mid-flight, with cells preparing and pages in production")
    for fraction in (0.15, 0.5, 0.85):
        at = max(1, int(ticks * fraction))
        torn = tools.play("traverse", ticks, ["--abort-at", str(at), "--workers", "4"])
        expect(torn.returncode == 0,
               f"a session destroyed at tick {at} did not exit cleanly "
               f"(rc={torn.returncode}):\n{torn.stdout}{torn.stderr}")
    report.did("three sessions destroyed under load, each cleanly",
               f"at ticks {int(ticks * 0.15)}, {int(ticks * 0.5)}, {int(ticks * 0.85)} "
               "with four production workers")


# --- Act 5: the patch -------------------------------------------------------------------------------


def change_content(tools: Tools) -> tuple[str, str]:
    """One line of the world's description changes: a landmark is rebuilt as something else."""
    source = tools.project / "world" / "city.cyopenworld"
    text = source.read_text()
    # THE FIRST LANDMARK PAST THE START, so that a shortened route still reaches it: the smoke entry
    # runs 1,800 ticks and this one is 1.6 km along a 7.0 km route.
    before = 'landmark 14 12 "reservoir"   kind 12'
    after = 'landmark 14 12 "reservoir-drained" kind 21'
    expect(before in text, f"the line act 5 changes is not in {source}")
    source.write_text(text.replace(before, after))
    return before, after


def act_patch(tools: Tools, report: Report, ticks: int,
              cold: dict[str, tuple[str, str]]) -> None:
    report.act("Act 5 — one line of content changes, and is shipped as a patch")
    before, after = change_content(tools)
    report.note(f"{before}   ->   {after}")

    rebuilt = tools.build(tools.work / "next.cypackage")
    expect(rebuilt.returncode == 0, f"the patched world did not build:\n{rebuilt.stdout}")
    now = nodes(rebuilt.stdout)
    # THE KEY IS THE CLAIM, AND THE OUTCOME IS THE CONSEQUENCE. A node whose derivation key did not
    # move cannot be invalidated by this change however the cache happens to be populated; a node
    # whose key moved must not be served the old artefact. Checking the outcome alone would make
    # this act depend on what a previous run left in the cache, which is what it did at first.
    expect(now["import:palette"][1] == cold["import:palette"][1],
           "the palette's derivation key moved for a change it does not read")
    for node in ("import:city", "cook:city", "package:world"):
        expect(now[node][1] != cold[node][1],
               f"{node}'s derivation key did not move for a change it reads")
    expect(now["import:palette"][0] == "cached",
           f"the palette was rebuilt by a change it does not read:\n{rebuilt.stdout}")
    expect(all(now[node][0] in ("ran", "rebuilt")
               for node in ("import:city", "cook:city", "package:world")),
           f"a node whose key moved was served from the cache:\n{rebuilt.stdout}")
    report.did("a one-asset change invalidated only what depends on it",
               " ".join(f"{name}={outcome}" for name, (outcome, _) in sorted(now.items())))

    patched = run([
        tools.cy_build, "patch",
        "--from", tools.work / "base.cypackage",
        "--to", tools.work / "next.cypackage",
        "--out", tools.work / "world.cypatch",
    ])
    expect(patched.returncode == 0, f"the patch could not be produced:\n{patched.stderr}")
    report.did("the difference between the two builds is a patch", patched.stdout.strip())

    installed_before = (tools.install / "current").read_text().strip()
    interrupted = run([
        tools.cy_build, "apply",
        "--patch", tools.work / "world.cypatch",
        "--install", tools.install,
        "--artefacts", tools.artefacts,
        "--stop-at", "commit",
    ])
    expect(interrupted.returncode != 0,
           f"an interrupted patch reported success:\n{interrupted.stdout}")
    verified = run([tools.cy_build, "verify", "--install", tools.install])
    expect(verified.returncode == 0,
           f"an interrupted patch left the installation damaged:\n{verified.stderr}")
    expect((tools.install / "current").read_text().strip() == installed_before,
           "an interrupted patch switched the build in force")
    report.did("an interrupted patch leaves the previous build playable",
               f"stopped at commit; {verified.stdout.strip()}")

    applied = run([
        tools.cy_build, "apply",
        "--patch", tools.work / "world.cypatch",
        "--install", tools.install,
        "--artefacts", tools.artefacts,
    ])
    expect(applied.returncode == 0, f"the patch did not apply:\n{applied.stdout}{applied.stderr}")
    expect((tools.install / "current").read_text().strip() != installed_before,
           "the patch applied and the build in force did not change")
    report.did("the patch applied atomically", applied.stdout.strip())

    replayed = tools.play("traverse", ticks)
    expect(replayed.returncode == 0,
           f"the patched world did not run:\n{replayed.stdout}{replayed.stderr}")
    expect("reservoir-drained" in replayed.stdout and "kind 21" in replayed.stdout,
           f"the same route did not report the changed landmark:\n{replayed.stdout}")
    report.did("the same route, out of the same directory, reports the changed world",
               [line.strip() for line in replayed.stdout.splitlines()
                if "reservoir-drained" in line][0])


# --- The committed screenshot -----------------------------------------------------------------------
#
# THIS PROJECT WENT SIX MILESTONES WITH ONE SCREENSHOT. What there is to photograph here is not a
# window — nothing in this artefact draws — it is the world and the route through it, so what is
# rendered is a MAP: every cell of the city, the fixed route across it, the landmarks it passes, and
# the run's own figures beside it, in the editor's palette so that the images in docs/design/images/
# read as one product.
#
# Drawn by the artefact rather than captured by hand, for the reason M5.5's two images give: an image
# nobody can regenerate is an image that decays. Re-running with `--shot` refreshes it.

INK = {
    "window": (0x0E, 0x10, 0x12),
    "panel": (0x16, 0x19, 0x1C),
    "raised": (0x1E, 0x22, 0x27),
    "primary": (0xE6, 0xE9, 0xEC),
    "secondary": (0xA2, 0xAB, 0xB4),
    "live": (0x35, 0xC0, 0x7C),
    "warning": (0xF0, 0x91, 0x3A),
    "selection": (0xE5, 0xB9, 0x5C),
    "active": (0x4C, 0x9A, 0xFF),
}


def _monospace(size: int):
    from PIL import ImageFont

    for candidate in (
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
    ):
        if Path(candidate).is_file():
            return ImageFont.truetype(candidate, size)
    return ImageFont.load_default()


def render_shot(report: Report, figures: dict[str, float], path: Path) -> None:
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        print(f"    (no Pillow, so no screenshot at {path})", file=sys.stderr)
        return

    extent, cell_pixels = 48, 12
    map_size = extent * cell_pixels
    margin, top = 28, 74
    width = 1560
    lines = [line for line in report.lines if line[0] in ("act", "ok")]
    height = max(top + map_size + 90, top + 26 * len(lines) + 90)

    image = Image.new("RGB", (width, height), INK["window"])
    draw = ImageDraw.Draw(image)
    font = _monospace(14)
    small = _monospace(12)

    draw.rectangle([0, 0, width, 46], fill=INK["panel"])
    draw.text((margin, 14), "CyberEngine", font=_monospace(15), fill=INK["primary"])
    draw.text((margin + 128, 15),
              "samples/06-open-world — six kilometres of city, streamed, paged, saved and patched",
              font=font, fill=INK["secondary"])

    # The map: one square per cell, the route over it, the landmarks on it.
    draw.rectangle([margin - 8, top - 8, margin + map_size + 8, top + map_size + 8],
                   fill=INK["panel"])
    # A cell is drawn lighter the closer it is to the route, so the picture says what was streamed:
    # the corridor the traveller opened, against the world it never touched.
    for z in range(extent):
        for x in range(extent):
            along = min(1.0, max(0.0, ((x - 4.5) * 39 + (z - 3.5) * 38) / (39 * 39 + 38 * 38)))
            offset = math.hypot(x - 4.5 - along * 39, z - 3.5 - along * 38)
            near = max(0.0, 1.0 - offset / 3.0)
            shade = int(0x1C + 0x30 * near)
            draw.rectangle(
                [margin + x * cell_pixels, top + z * cell_pixels,
                 margin + (x + 1) * cell_pixels - 2, top + (z + 1) * cell_pixels - 2],
                fill=(shade, shade + 3, shade + 6))
    draw.line([margin + 4.5 * cell_pixels, top + 3.5 * cell_pixels,
               margin + 43.5 * cell_pixels, top + 41.5 * cell_pixels],
              fill=INK["active"], width=3)
    for x, z, name in ((4, 3, "north-gate"), (14, 12, "reservoir"), (24, 22, "relay"),
                       (34, 32, "foundry"), (43, 41, "south-yard")):
        centre = (margin + x * cell_pixels, top + z * cell_pixels)
        draw.ellipse([centre[0] - 5, centre[1] - 5, centre[0] + 7, centre[1] + 7],
                     fill=INK["selection"])
        draw.text((centre[0] + 12, centre[1] - 6), name, font=small, fill=INK["secondary"])

    # The transcript beside it.
    left = margin + map_size + 34
    y = top
    for kind, text in lines:
        colour = INK["selection"] if kind == "act" else INK["live"]
        clipped = text if len(text) <= 96 else text[:95] + "…"
        draw.text((left, y), clipped, font=font, fill=colour)
        y += 26

    footer = (f"{figures.get('activated', 0):.0f} cells activated · "
              f"{figures.get('evicted', 0):.0f} evicted · "
              f"{figures.get('produced', 0):.0f} pages produced · "
              f"worst tick {figures.get('worst_us', 0):.0f} us · "
              f"{report.checks} claims checked")
    draw.text((margin, height - 40), footer, font=font, fill=INK["active"])
    path.parent.mkdir(parents=True, exist_ok=True)
    image.save(path)
    print(f"    shot  {path}")


# --- Running it ---------------------------------------------------------------------------------------


def binaries(arguments) -> tuple[Path, Path]:
    """The two programs, from explicit paths or from a build directory."""
    if arguments.sample and arguments.cy_build:
        return Path(arguments.sample), Path(arguments.cy_build)
    build_dir = Path(arguments.build_dir or os.environ.get("CY_BUILD_DIR")
                     or ROOT / "build" / arguments.profile)
    sample = build_dir / "samples" / "06-open-world" / "cy_sample_open-world"
    cy_build = build_dir / "tools" / "build" / "cy_build"
    for binary in (sample, cy_build):
        if not binary.is_file():
            raise Failed(
                f"no {binary.name} at {binary}. Build it with: "
                f"just build-engine --profile {arguments.profile}")
    return sample, cy_build


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", default="dev")
    parser.add_argument("--build-dir")
    parser.add_argument("--sample")
    parser.add_argument("--cy-build")
    parser.add_argument("--work", default=str(ROOT / "build" / "open-world"))
    parser.add_argument("--ticks", type=int, default=6000)
    parser.add_argument("--shot", help="write the run's picture here")
    parser.add_argument("--only", help="run one act: ship, play, resume, teardown or patch")
    arguments = parser.parse_args()

    report = Report()
    try:
        sample, cy_build = binaries(arguments)
        work = Path(arguments.work)
        work.mkdir(parents=True, exist_ok=True)
        tools = Tools(sample, cy_build, work)

        acts = ("ship", "play", "resume", "teardown", "patch")
        wanted = (arguments.only,) if arguments.only else acts
        expect(all(act in acts for act in wanted), f"an act must be one of {', '.join(acts)}")

        figures: dict[str, float] = {}
        cold: dict[str, tuple[str, str]] = {}
        if "ship" in wanted:
            cold = act_ship(tools, report)
        if "play" in wanted:
            figures = act_play(tools, report, arguments.ticks)
        if "resume" in wanted:
            expect(bool(figures),
                   "act 3 checks the save against what act 2 reported, so it needs act 2 as well")
            act_resume(tools, report, arguments.ticks, figures)
        if "teardown" in wanted:
            act_teardown(tools, report, arguments.ticks)
        if "patch" in wanted:
            expect(bool(cold), "act 5 compares derivation keys against act 1's, so it needs it too")
            act_patch(tools, report, arguments.ticks, cold)

        if arguments.shot:
            render_shot(report, figures, Path(arguments.shot))
    except Failed as failure:
        print(f"\nopen-world: {failure}", file=sys.stderr)
        return 1
    except subprocess.TimeoutExpired as timeout:
        print(f"\nopen-world: {timeout.cmd} did not finish", file=sys.stderr)
        return 1

    print(f"\nopen-world: {report.checks} claim(s) checked, all of them held")
    return 0


if __name__ == "__main__":
    sys.exit(main())
