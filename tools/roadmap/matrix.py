#!/usr/bin/env python3
"""The ledger's build matrix: one tree per distinct build CONFIGURATION, shared by every criterion
that needs it.

FIVE CRITERIA WERE THE MOST EXPENSIVE THING ON THE LADDER. `m8c:feature-options-off`,
`m8c:ml-option-off`, `m9:networking-defaults-on`, `m9:networking-option-off` and
`m9:multiplayer-profiles-agree` cost 5916.7 s — 98.6 minutes — in one evaluation measured on this
workstation, against the 1.77 hours a whole 435-criterion run took at 2ca9e15. Each of them
RECONFIGURES AND REBUILDS THE WHOLE ENGINE with a different option set, and between the five of them
they did it seven times. Over the matrix the same five are 4760.6 s on a machine that has never
evaluated the ledger and 98.8 s on one that has; tools/roadmap/README.md has the row-by-row figures
and the five mutations that were watched turning each of them red.

THE COST WAS NEVER THE BUILDS THEMSELVES. It was three things around them:

  * SEVEN BUILDS FOR SIX CONFIGURATIONS. `networking-defaults-on` configures the tree with no `-D`
    at all; so does the Development half of `multiplayer-profiles-agree`. That is one configuration
    compiled twice into two directories, because the two criteria live in two ledger files and
    neither could see the other's directory.
  * A DIFFERENT DIRECTORY EVERY RUN. Each body spelled its tree as
    `"${CY_BUILD_DIR:+${CY_BUILD_DIR}/off-ml}"`, which hangs the matrix underneath whatever the
    caller pointed `CY_BUILD_DIR` at. A ledger run under `build/m11c-ci` and the next one under
    `build/m11d-crit` therefore share nothing, and EVERY run is a cold build of six trees. With
    `CY_BUILD_DIR` unset it is worse than cold: the expression collapses to the empty string, so
    `just build-engine` falls back to `build/dev` and the option-off criteria reconfigure the
    developer's own tree with CY_VFX, then CY_ML, then CY_NETWORKING off — each one forcing a full
    rebuild of the previous one's work, and the last one leaving `build/dev` in a state nobody
    asked for.
  * `rm -rf` ON A TREE THAT ONLY NEEDED A CLEAN CACHE. `networking-defaults-on` deleted its whole
    build tree before every run, because its claim is about what a configure with no `-D` produces
    and a remembered cache cannot answer that. Deleting the cache answers it; deleting the object
    files answers nothing and costs a full compile of the engine every single evaluation.

WHAT SHARING MAY NOT MEAN HERE, AND IT IS THE WHOLE DIFFICULTY. Each of these criteria asserts THAT
THE ENGINE STILL CONFIGURES AND BUILDS WITH AN OPTION OFF. That claim IS the build. So this module
does not cache an answer, does not skip a configuration because a similar one passed, and does not
let a criterion read a manifest in place of a compiler: `ensure` runs `just build-engine` for every
configuration it is asked for, every time it is asked, and CMake and Ninja decide what is out of
date — the same rule just/build.just states for the recipe itself. A source file that only one
configuration compiles, broken, still breaks that configuration's build here.

What the matrix changes is WHERE the build happens: one stable directory per distinct configuration,
under `build/ledger-matrix/`, so that a criterion asking for a configuration a sibling has already
built finds the work done and Ninja has nothing left to do. That is the difference between paying
for a configuration and paying for it again.

AND IT IS ALSO A CORRECTNESS FIX. A shared tree needs ONE owner of its option set: `dev-default` is
read by `networking-defaults-on` as the answer to "what does a configure with no `-D` produce", so
nothing else may ever configure that directory with a `-D`. Here that is a property of the table
below rather than of two shell strings in two files that have to agree by hand.

Governed by: delivery-roadmap (Milestone exit criteria are executable), developer-workflow-and-just.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

#: Where the matrix lives. Deliberately NOT under `CY_BUILD_DIR`: that variable carries the caller's
#: own tree and it is different for every agent and every continuous-integration job, which is what
#: made every evaluation a cold build. `CY_LEDGER_MATRIX` is the override for a caller that really
#: does want an isolated matrix; two ledgers sharing one matrix is the intended case, and
#: `just build-engine` already takes a per-directory lock so that two of them cannot configure the
#: same tree at the same time.
DEFAULT_ROOT = "build/ledger-matrix"

#: What `just build-engine` gets as `CY_JOBS` when the matrix builds several configurations at once,
#: so that two concurrent builds do not each ask for every core on the machine. One configuration on
#: its own is left alone: Ninja's own default is right for it.
CPU_COUNT = os.cpu_count() or 8


@dataclass(frozen=True)
class Configuration:
    """One option set, the tree it is built in, and the criteria that need it."""

    id: str
    profile: str
    options: tuple[str, ...]
    why: str
    needed_by: tuple[str, ...]
    #: Delete `CMakeCache.txt` before configuring. Declared by the ONE configuration whose claim is
    #: about what CMake computes when nothing is remembered — see `dev-default` below.
    fresh_cache: bool = False

    @property
    def directory(self) -> Path:
        return root() / self.id

    @property
    def cmake_arguments(self) -> list[str]:
        return [argument for option in self.options for argument in ("-D", option)]

    @property
    def described(self) -> str:
        return " ".join(f"-D {option}" for option in self.options) or "no -D at all"


#: THE MATRIX. One row per distinct configuration; a row is shared by every criterion in `needed_by`.
#:
#: Adding a criterion that needs an option set already here costs nothing — it names the row and the
#: build is already done. Adding one that needs a new option set adds a row, and the cost of that row
#: is one build per ledger run rather than one per criterion.
CONFIGURATIONS: tuple[Configuration, ...] = (
    Configuration(
        id="dev-default",
        profile="dev",
        options=(),
        fresh_cache=True,
        why="the tree as a configure with no `-D` at all leaves it: the defaults CMake computes, "
            "which is what `networking-defaults-on` reads out of the cache, and the Development "
            "half of the two builds `multiplayer-profiles-agree` compares. THE CACHE IS DELETED "
            "FIRST and the object files are not: an option's default can only be observed where "
            "nothing is remembered, and no object file remembers anything about it. Deleting the "
            "tree — which is what this criterion did until the matrix — answers the same question "
            "and pays a full compile of the engine for it every evaluation.",
        needed_by=("m9:networking-defaults-on", "m9:multiplayer-profiles-agree"),
    ),
    Configuration(
        id="debug-default",
        profile="debug",
        options=(),
        why="the same tree at -O0 with CY_UNOPTIMISED, which is the other half of the pair "
            "`multiplayer-profiles-agree` requires to produce identical bytes.",
        needed_by=("m9:multiplayer-profiles-agree",),
    ),
    Configuration(
        id="off-vfx",
        profile="dev",
        options=("CY_VFX=OFF",),
        why="M8.c's vfx-system removed at build time. A capability that cannot be removed is not "
            "behind an option, whatever cmake/features.cmake says.",
        needed_by=("m8c:feature-options-off",),
    ),
    Configuration(
        id="off-vulkan",
        profile="dev",
        options=("CY_RENDERER_VULKAN=OFF", "CY_VIRTUAL_GEOMETRY=OFF"),
        why="the Vulkan backend removed, together with the CY_VIRTUAL_GEOMETRY that "
            "cmake/features.cmake requires with it.",
        needed_by=("m8c:feature-options-off",),
    ),
    Configuration(
        id="off-ml",
        profile="dev",
        options=("CY_ML=OFF",),
        why="src/ml/ removed entirely — the direction M8.b's gate found nobody had checked when "
            "CY_UI defaulted OFF while gating nothing.",
        needed_by=("m8c:ml-option-off",),
    ),
    Configuration(
        id="off-networking",
        profile="dev",
        options=("CY_NETWORKING=OFF",),
        why="the TRANSPORT removed and not the RECORD: `ReplicationInputCursor` stays in "
            "src/replay/ and outside the option, so a log is still read five ways with no network "
            "in the build.",
        needed_by=("m9:networking-option-off",),
    ),
)

BY_ID = {configuration.id: configuration for configuration in CONFIGURATIONS}


class MatrixError(Exception):
    """A configuration nothing declares, or a build that could not be started at all."""


def root() -> Path:
    override = os.environ.get("CY_LEDGER_MATRIX", "").strip()
    return Path(override) if override else Path(DEFAULT_ROOT)


def configuration(identifier: str) -> Configuration:
    try:
        return BY_ID[identifier]
    except KeyError:
        known = ", ".join(sorted(BY_ID))
        raise MatrixError(f"no configuration '{identifier}' in the matrix. Declared: {known}") from None


@dataclass
class Outcome:
    configuration: Configuration
    code: int
    output: str

    @property
    def ok(self) -> bool:
        return self.code == 0


def ensure(identifiers: list[str], jobs: int | None = None) -> list[Outcome]:
    """Configure and build each named configuration. Nothing is skipped and nothing is cached.

    Several are built AT THE SAME TIME, each in its own tree with its own share of the machine.
    That is worth doing because a build is not one long compile: it opens with a CMake configure
    that fetches and checks dependencies nearly single-threaded, and ends with links that are
    serial too, and those phases of one configuration overlap the compiles of another.
    """
    # NAMED TWICE IS ASKED FOR ONCE. Two builds of one tree would serialise on the lock
    # `just build-engine` takes and the second would find nothing to do, which is merely wasteful —
    # but it would also read as two builds in the output, and this module exists to stop one
    # configuration being paid for twice.
    wanted = list(dict.fromkeys(configuration(identifier) for identifier in identifiers))
    if not wanted:
        raise MatrixError("name at least one configuration to build")
    if len(wanted) == 1:
        return [_build(wanted[0], jobs)]
    share = jobs or max(2, CPU_COUNT // len(wanted))
    with ThreadPoolExecutor(max_workers=len(wanted)) as pool:
        return list(pool.map(lambda entry: _build(entry, share), wanted))


def _build(entry: Configuration, jobs: int | None) -> Outcome:
    directory = entry.directory
    directory.mkdir(parents=True, exist_ok=True)

    # THE ONE THING DELETED, AND ONLY WHERE A CLAIM NEEDS IT. `dev-default` answers "what does a
    # configure with no `-D` produce", and a cache written by an earlier configure — by this matrix,
    # by a developer, by anything — would answer with what it was told rather than with the default.
    # CMakeFiles/ is left alone deliberately: `cmake --fresh` would remove it, and for the Ninja
    # generator that directory holds the object files of every top-level target, so the "fresh"
    # would be a full recompile of the engine to learn something only the cache knows.
    if entry.fresh_cache:
        cache = directory / "CMakeCache.txt"
        if cache.exists():
            cache.unlink()

    command = ["just", "build-engine", "--profile", entry.profile, *entry.cmake_arguments]
    environment = dict(os.environ, CY_BUILD_DIR=str(directory))
    if jobs:
        environment["CY_JOBS"] = str(jobs)
    try:
        completed = subprocess.run(  # noqa: S603 — the command is this module's own table, not input
            command, cwd=REPO_ROOT, env=environment, capture_output=True, text=True, check=False)
    except OSError as error:
        raise MatrixError(f"could not run `{' '.join(command)}`: {error}") from None
    return Outcome(entry, completed.returncode, (completed.stdout or "") + (completed.stderr or ""))


# --- The command line -----------------------------------------------------------------------------


def command_build(arguments: argparse.Namespace) -> int:
    outcomes = ensure(arguments.configuration, arguments.jobs)
    failed = [outcome for outcome in outcomes if not outcome.ok]
    # Printed in the order they were ASKED FOR rather than the order they finished, so that a
    # ledger's output reads the same whether or not the builds overlapped.
    for outcome in outcomes:
        entry = outcome.configuration
        print(f"==> {entry.id:<16} {entry.directory}  ({entry.profile}, {entry.described})")
        if not outcome.ok:
            for line in outcome.output.splitlines():
                print(f"    | {line}")
    for outcome in outcomes:
        entry = outcome.configuration
        state = "built" if outcome.ok else f"FAILED (exit {outcome.code})"
        print(f"{entry.id:<16} {state}")
    if failed:
        names = ", ".join(outcome.configuration.id for outcome in failed)
        print(f"the matrix could not build: {names}", file=sys.stderr)
        return 1
    return 0


def command_path(arguments: argparse.Namespace) -> int:
    print(configuration(arguments.configuration).directory)
    return 0


def command_list(arguments: argparse.Namespace) -> int:
    print(f"the ledger's build matrix — {len(CONFIGURATIONS)} configurations under {root()}")
    for entry in CONFIGURATIONS:
        print()
        print(f"  {entry.id:<16} {entry.profile:<6} {entry.described}")
        print(f"  {'':<16} {entry.why}")
        print(f"  {'':<16} needed by: {', '.join(entry.needed_by)}")
    return 0


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="matrix", description=__doc__.splitlines()[0])
    subcommands = parser.add_subparsers(dest="command", required=True)

    build = subcommands.add_parser("build", help="configure and build the named configurations")
    build.add_argument("configuration", nargs="+")
    build.add_argument("--jobs", type=int, default=None,
                       help="compile jobs per configuration (default: the machine, shared)")
    build.set_defaults(handler=command_build)

    path = subcommands.add_parser("path", help="print one configuration's build directory")
    path.add_argument("configuration")
    path.set_defaults(handler=command_path)

    listing = subcommands.add_parser("list", help="the matrix, and which criteria need each row")
    listing.set_defaults(handler=command_list)
    return parser


def main(argv: list[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    os.chdir(REPO_ROOT)
    try:
        return arguments.handler(arguments)
    except MatrixError as error:
        print(f"matrix {arguments.command}: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
