#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""samples/11-ship — M11.d's closing artefact. Section 8.

`just run-ship` is the recipe, `smoke.ship` is the CTest entry, and this file is what both of them
run. It is the SINGLE RECIPE task 8.1 asks for: one project built, cooked, packaged, installed and
launched, with every claim checked against what the two programs printed.

--- THE ACTS ---------------------------------------------------------------------------------

  1. SHIP        `cy_build` runs the card's derivation graph cold, then warm, proves the two
                 manifests are byte-identical, installs the result, and the installation VERIFIES —
                 every chunk present and digesting, which is what "installed" has to mean before
                 "launched" means anything.
  2. PROVENANCE  the installed manifest carries the build identity, the project, the revision, the
                 platform, the profile, the toolchain fingerprint and the content version, and the
                 LAUNCH READS THEM BACK OUT OF IT. Task 8.4: the launch reproduces from the
                 package, not from this driver's memory of what it asked for.
  3. LAUNCH      `cy_sample_ship` opens the installation, reads the card by logical name, composes
                 it, opens a window and presents it. What it could not do it reports as NOT
                 EVALUATED, which this driver records as not evaluated too — never as a pass.
  4. THE CONTENT DECIDES THE PIXELS. One line of the card changes; the palette's import is served
                 from the cache and the card's is not; the package is rebuilt and reinstalled; and
                 the program reports a card of a different size out of the same installation
                 directory with nothing in the program changed. A packaged project whose content
                 could not be changed without a recompile would be a program with a resource file.

--- WHAT THIS ARTEFACT IS NOT ------------------------------------------------------------------

IT IS NOT A GAME, and it must not be read as one. `m11b:the-game-exists` is red — `ls -d
samples/*game*` matches nothing — and a packaging proof does not close it. README.md says so at
length, the card says so on the screen, and this docstring says so to whoever greps here first.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import stat
import subprocess
import sys
from pathlib import Path

SAMPLE = Path(__file__).resolve().parent
ROOT = SAMPLE.parents[1]
sys.path.insert(0, str(SAMPLE.parent / "harness"))

import artefact  # noqa: E402  — the path above is what makes it importable
from artefact import Failed, Report, expect  # noqa: E402

OUTCOME = re.compile(r"^(ran|cached|rebuilt|skipped|failed)\s+(\S+)\s+([0-9a-f]{16})")
NODES = {"import:palette", "import:card", "cook:card", "package:card"}


def run(command: list[str], timeout: int = 900) -> subprocess.CompletedProcess:
    return subprocess.run([str(part) for part in command], cwd=str(ROOT),
                          capture_output=True, text=True, timeout=timeout)


def outcomes(text: str) -> dict[str, str]:
    """Every node `cy_build build` reported, by name.

    Matched on the derivation key that follows the name so the report's own summary line — `ran 4
    cached 0 ...` — is not read as a node called "4". samples/06-open-world's driver explains why
    that pattern is here rather than a looser one.
    """
    return {match.group(2): match.group(1)
            for match in (OUTCOME.match(line) for line in text.splitlines())
            if match is not None}


def revision() -> str:
    """The engine revision, for the package's provenance. `unknown` where git cannot answer — a
    provenance field that lied would be worse than one that says it does not know."""
    result = run(["git", "rev-parse", "HEAD"], timeout=30)
    return result.stdout.strip() if result.returncode == 0 else "unknown"


class Tools:
    def __init__(self, sample: Path, cy_build: Path, work: Path) -> None:
        self.sample = sample
        self.cy_build = cy_build
        self.work = work
        self.project = work / "project"
        self.artefacts = work / "artefacts"
        self.cache = work / "cache"
        self.install = work / "install"
        self.revision = revision()

    def build(self, package: Path) -> subprocess.CompletedProcess:
        return run([
            self.cy_build, "build",
            "--description", self.project / "build" / "ship.cybuild",
            "--project", self.project,
            "--out", self.artefacts,
            "--cache", self.cache,
            "--package", package,
            "--platform", "host",
            "--profile", "client",
            "--revision", self.revision,
        ])

    def install_package(self, package: Path) -> subprocess.CompletedProcess:
        return run([
            self.cy_build, "install",
            "--package", package,
            "--artefacts", self.artefacts,
            "--install", self.install,
        ])

    def launch(self, platform: str, frames: int, shot: Path | None,
               coverage: Path | None, require_draw: bool) -> subprocess.CompletedProcess:
        command = [self.sample, "--install", self.install,
                   "--platform", platform, "--frames", str(frames)]
        if shot is not None:
            command += ["--shot", shot]
        if coverage is not None:
            command += ["--coverage", coverage]
        if require_draw:
            command.append("--require-draw")
        return run(command)


# --- Act 1: ship ------------------------------------------------------------------------------------


def act_ship(tools: Tools, report: Report) -> None:
    print("\n==> Act 1 — one project: built, cooked, packaged, installed, verified")

    # A COPY, and a cold cache. Act 4 edits a line of content, so a run that built out of the
    # committed project would edit the repository; and a first build that was served from a previous
    # run's cache would make "a cold build ran every node" a statement about nothing.
    #
    # Windows: rmtree fails with ERROR_ACCESS_DENIED on files with the read-only attribute set.
    # Historical runs before the artefact store's Windows fix left such files behind, so clear the
    # attribute on every entry we walk before deleting. `onexc` is Python 3.12+ (`onerror` in 3.11).
    def _force_delete(func, path, _exc):
        os.chmod(path, stat.S_IWRITE)
        func(path)
    for directory in (tools.project, tools.cache, tools.artefacts, tools.install):
        if directory.exists():
            shutil.rmtree(directory, onexc=_force_delete)
    tools.work.mkdir(parents=True, exist_ok=True)
    shutil.copytree(SAMPLE / "project", tools.project)

    cold = tools.build(tools.work / "base.cypackage")
    expect(cold.returncode == 0, f"the card's graph did not build:\n{cold.stdout}{cold.stderr}")
    nodes = outcomes(cold.stdout)
    expect(set(nodes) == NODES,
           f"the graph is not the four nodes the description declares: {sorted(nodes)}")
    expect(set(nodes.values()) == {"ran"}, f"a cold build did not run every node:\n{cold.stdout}")
    report.did("the card's graph built cold",
               " ".join(f"{name}={outcome}" for name, outcome in sorted(nodes.items())))

    warm = tools.build(tools.work / "warm.cypackage")
    expect(warm.returncode == 0, f"the warm build failed:\n{warm.stdout}{warm.stderr}")
    expect(set(outcomes(warm.stdout).values()) == {"cached"},
           f"a second build of unchanged content ran a node:\n{warm.stdout}")
    expect((tools.work / "base.cypackage").read_text() == (tools.work / "warm.cypackage").read_text(),
           "the cold build and the cache-warm build produced different manifests")
    report.did("a cold build and a cache-warm build produce byte-identical manifests",
               f"{len(nodes)} nodes, all cached the second time")

    installed = tools.install_package(tools.work / "base.cypackage")
    expect(installed.returncode == 0, f"the build did not install:\n{installed.stderr}")
    report.did("the build is installed", installed.stdout.strip().splitlines()[-1])


# --- Act 2 and 3: provenance, and the launch --------------------------------------------------------


PROVENANCE = re.compile(r"provenance\s+project=(\S*) revision=(\S*) platform=(\S*) profile=(\S*)")
TOOLCHAIN = re.compile(r"toolchain=(\S*) content-version=(\d+)")
CARD = re.compile(r"card\s+(\S+) (\d+)x(\d+), (\d+) directives, (\d+) bytes")
PRESENTED = re.compile(r"frames presented\s+(\d+)")
# Anchored, because the coverage table also carries a `device class` line two rows down and an
# unanchored `device\s+(.+)` would read whichever came first if the order ever changed.
DEVICE = re.compile(r"^\s+device\s{2,}(.+)$", re.MULTILINE)


def leg_path(path: Path | None, leg: str) -> Path | None:
    """`out.png` for one leg becomes `out-sdl3.png`. Two legs writing one file would leave whichever
    ran last, and the artefact's claim is that BOTH drew."""
    return None if path is None else path.with_name(f"{path.stem}-{leg}{path.suffix}")


def act_launch(tools: Tools, report: Report, platform: str, frames: int,
               shot: Path | None, coverage: Path | None, require_draw: bool) -> tuple[str, int]:
    print(f"\n==> Act 2 [{platform}] — the launch reads its provenance and its content out of the "
          "installation")

    launched = tools.launch(platform, frames, shot, coverage, require_draw)
    text = launched.stdout

    # EXIT 3 IS "IT DREW AND TRIPPED VALIDATION", which is a gap rather than a fallen-over run: the
    # frame is correct and the render graph's two barriers at the swapchain boundary are not. A gap
    # cannot be turned back into a pass — `Report.exit_code` is derived — so recording it here is
    # what makes this run non-zero for as long as the defect stands.
    if launched.returncode == 3:
        errors = re.search(r"validation errors\s+(\d+)", text)
        report.gap(f"the frame through '{platform}' trips backend validation",
                   f"{errors.group(1) if errors else '?'} error(s): SYNC-HAZARD-WRITE-AFTER-READ "
                   "against PRESENT_ACQUIRE_READ and SYNC-HAZARD-PRESENT-AFTER-WRITE. The render "
                   "graph spells both swapchain-boundary barriers with Stage::None on the side "
                   "facing the presentation engine — see samples/11-ship/README.md")
    elif launched.returncode != 0 and require_draw:
        raise Failed(f"the launch was asked to draw and did not:\n{text}{launched.stderr}")
    else:
        expect(launched.returncode == 0, f"the launch failed:\n{text}{launched.stderr}")

    provenance = PROVENANCE.search(text)
    expect(provenance is not None, f"the launch printed no provenance:\n{text}")
    expect(provenance.group(2) == tools.revision,
           f"the package's revision is {provenance.group(2)!r}, not the one it was built with")
    toolchain = TOOLCHAIN.search(text)
    expect(toolchain is not None and toolchain.group(1) != "",
           f"the package carries no toolchain fingerprint:\n{text}")
    report.did("the launch read its provenance out of the package it was installed from",
               f"revision={provenance.group(2)[:12]} platform={provenance.group(3)} "
               f"profile={provenance.group(4)} toolchain={toolchain.group(1)[:12]} "
               f"content-version={toolchain.group(2)}")

    card = CARD.search(text)
    expect(card is not None, f"the launch printed no card line:\n{text}")
    report.did("the card came out of the manifest in force, by logical name",
               f"{card.group(1)} {card.group(2)}x{card.group(3)}, "
               f"{card.group(4)} directives, {card.group(5)} bytes")

    presented = PRESENTED.search(text)
    expect(presented is not None, f"the launch printed no frame count:\n{text}")
    frames_presented = int(presented.group(1))
    device = DEVICE.search(text)
    device_name = device.group(1).strip() if device else "(none)"

    if frames_presented > 0:
        report.did(f"the packaged project DREW through '{platform}': a swapchain frame, presented",
                   f"{frames_presented} frame(s) through {device_name}")
    else:
        reason = "no reason recorded"
        for line in text.splitlines():
            if "NOT EVALUATED" in line:
                reason = line.split("NOT EVALUATED", 1)[1].strip()
        # NOT EVALUATED is never a pass and never a failure to be hidden: the harness counts it
        # where a reader cannot miss it and does not move the exit status in either direction.
        report.not_evaluated(f"presentation through '{platform}' on this machine", reason)

    return text, int(card.group(5))


# --- Act 4: the content decides the pixels ----------------------------------------------------------


def act_content(tools: Tools, report: Report, platform: str, bytes_before: int) -> None:
    print("\n==> Act 3 — one line of content changes, and the window says something else")

    card_source = tools.project / "card" / "ship.cycard"
    card_source.write_text(card_source.read_text().replace(
        'text  48 232    2 dim    "THIS FRAME WAS READ OUT OF THE INSTALLATION BY LOGICAL NAME"',
        'text  48 232    2 dim    "THIS FRAME WAS READ OUT OF THE INSTALLATION BY LOGICAL NAME"\n'
        'text  48 256    2 dim    "AND THIS LINE WAS ADDED TO THE CONTENT, NOT TO THE PROGRAM"'))

    rebuilt = tools.build(tools.work / "next.cypackage")
    expect(rebuilt.returncode == 0, f"the rebuild failed:\n{rebuilt.stdout}{rebuilt.stderr}")
    nodes = outcomes(rebuilt.stdout)
    expect(nodes.get("import:palette") == "cached",
           f"the palette's import was not served from the cache:\n{rebuilt.stdout}")
    expect(nodes.get("import:card") in {"ran", "rebuilt"},
           f"the card's import did not re-run after its source changed:\n{rebuilt.stdout}")
    report.did("a one-asset change invalidated only the derivations that depend on it",
               " ".join(f"{name}={outcome}" for name, outcome in sorted(nodes.items())))

    installed = tools.install_package(tools.work / "next.cypackage")
    expect(installed.returncode == 0, f"the rebuilt package did not install:\n{installed.stderr}")

    relaunched = tools.launch(platform, 1, None, None, False)
    # 3 is "it drew and tripped validation", which act 2 has already recorded as a gap. This act is
    # about whether the CONTENT reached the installation, and it must not re-report the same defect
    # as a second, different failure.
    expect(relaunched.returncode in (0, 3), f"the relaunch failed:\n{relaunched.stdout}")
    card = CARD.search(relaunched.stdout)
    expect(card is not None, f"the relaunch printed no card line:\n{relaunched.stdout}")
    expect(int(card.group(5)) > bytes_before,
           "the same card came back out of a rebuilt package: the change did not reach the install")
    report.did("the same binary drew a different card out of the same installation directory",
               f"{bytes_before} bytes before, {card.group(5)} after — no recompile")


# --- The run ----------------------------------------------------------------------------------------


def binaries(arguments) -> tuple[Path, Path]:
    if arguments.binaries:
        expect(len(arguments.binaries) == 2,
               "--binaries takes the sample and cy_build, in that order")
        return Path(arguments.binaries[0]), Path(arguments.binaries[1])
    build_dir = Path(arguments.build_dir or os.environ.get("CY_BUILD_DIR", "build/dev"))
    if not build_dir.is_absolute():
        build_dir = ROOT / build_dir
    sample = build_dir / "samples" / "11-ship" / "cy_sample_ship"
    cy_build = build_dir / "tools" / "build" / "cy_build"
    for binary in (sample, cy_build):
        expect(binary.exists(),
               f"{binary} does not exist; build with `just build-engine -D CY_BUILD_TOOLS=ON`")
    return sample, cy_build


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binaries", nargs="+", help="cy_sample_ship and cy_build")
    parser.add_argument("--build-dir")
    parser.add_argument("--work", default=None, help="where the project, cache and install go")
    parser.add_argument("--platform", default="auto",
                        help="sdl3 | native | headless | auto | both")
    parser.add_argument("--frames", type=int, default=120)
    parser.add_argument("--shot", default=None, help="write the presented frame here")
    parser.add_argument("--coverage", default=None, help="write the coverage table here")
    parser.add_argument("--require-draw", action="store_true",
                        help="fail the run if nothing was presented")
    arguments = parser.parse_args(argv)

    report = Report()
    try:
        sample, cy_build = binaries(arguments)
        work = Path(arguments.work) if arguments.work else ROOT / "build" / "ship"
        if not work.is_absolute():
            work = ROOT / work
        tools = Tools(sample, cy_build, work)

        act_ship(tools, report)

        # `--platform both` runs the SAME BINARY twice, once per display server, which is the only
        # form in which task 8.2's claim — that it draws through the native backend AND through
        # SDL3 — is one run's evidence rather than two people's memories of two runs.
        legs = ["sdl3", "native"] if arguments.platform == "both" else [arguments.platform]
        shot = Path(arguments.shot) if arguments.shot else None
        coverage = Path(arguments.coverage) if arguments.coverage else None
        card_bytes = 0
        for leg in legs:
            _, card_bytes = act_launch(tools, report, leg, arguments.frames,
                                       leg_path(shot, leg) if len(legs) > 1 else shot,
                                       leg_path(coverage, leg) if len(legs) > 1 else coverage,
                                       arguments.require_draw)
            written = leg_path(shot, leg) if len(legs) > 1 else shot
            if written is not None and written.is_file():
                report.shot(written)
        act_content(tools, report, legs[-1], card_bytes)
    except Failed as failure:
        report.failed(str(failure))
    except artefact.Absent as absent:
        report.not_evaluated("the artefact", str(absent))

    return report.summarise()


if __name__ == "__main__":
    sys.exit(main())
