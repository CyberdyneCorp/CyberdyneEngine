#!/usr/bin/env python3
"""Source-level layering checks for CyberdyneEngine.

Task 1.3.3. The second half of the layer enforcement: `cmake/module.cmake` sees every link a target
declares, and this sees every `#include` a translation unit writes, including one that reaches
upward through an include path CMake was never told about.

Three checks, all of them cheap enough to run on every pull request:

  includes      a file may include a header at its own layer or below, never above
  sdl           no SDL header appears outside platform/ — design.md §4
  targets       no bare add_library or add_executable, because that is a target that opted out of
                the configure-time check (task 1.3.2)

Exit status is 0 when the tree is clean and 1 when it is not. Violations are printed one per line as
`path:line: <check>: <message>`, which is both readable and what CI annotation tooling expects.

Governed by: engine-architecture (Layered architecture), project-and-plugins (Architectural layering
is enforced), build-system-and-platforms (Platform porting surface).
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

# --- The layer table ------------------------------------------------------------------------------
#
# design.md §1 and the table in engine-architecture. Kept identical to cmake/module.cmake by hand;
# there are ten rows and they have not changed since the specification was written.

LAYER_OF_NAME = {
    "core": 0,
    "ecs": 1,
    "servers": 2,
    "backends": 3,
    "platform": 3,
    "scene": 4,
    "runtime": 5,
    "abi": 6,
    "editor": 7,
    "tools": 7,
}

# Repository-relative directory prefixes, matched longest first. tests/, benchmarks/ and samples/ are
# consumers of the whole engine rather than parts of the stack, so they sit at the top and may
# include anything.
DIRECTORY_LAYERS = {
    "src/core": 0,
    "src/ecs": 1,
    "src/servers": 2,
    "src/backends": 3,
    "platform": 3,
    "src/scene": 4,
    "src/runtime": 5,
    "src/abi": 6,
    "editor": 7,
    "tools": 7,
    "tests": 7,
    "benchmarks": 7,
    "samples": 7,
}

LAYER_NAMES = {0: "core", 1: "ecs", 2: "servers", 3: "backends", 4: "scene",
               5: "runtime", 6: "abi", 7: "editor/tools"}

SOURCE_SUFFIXES = {".h", ".hpp", ".hh", ".hxx", ".inl", ".ipp", ".c", ".cc", ".cpp", ".cxx",
                   ".m", ".mm"}

# Anchored to the start of a line so that a `//`-commented include is not a finding. A #include
# inside a /* block comment */ still is; that is a known and acceptable false positive, and the fix
# is to delete the dead include rather than to teach this a C++ lexer.
INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*[<"]([^>"\n]+)[>"]', re.MULTILINE)

BARE_TARGET_RE = re.compile(r'^[ \t]*(add_library|add_executable)[ \t]*\(', re.MULTILINE)

# Where the bare-target check applies: the engine tree, plus the top-level CMakeLists.txt. cmake/ is
# out of scope because it *is* the build system — it declares cy_add_module() itself, the
# cy_compile_options interface every module links, and the cy::dep::<name> shims that wrap targets
# this project does not own. A target declared there is build machinery, not an engine module.
TARGET_CHECK_ROOTS = ("src", "platform", "modules", "editor", "tools", "tests", "benchmarks",
                      "samples")

# Files inside that scope permitted to declare a target directly, each with the reason. Paths are
# relative to the scanned root. This table is the only opt-out, and adding a row to it is a reviewed
# change to this file — which is the point.
BARE_TARGET_EXEMPT = {
    "CMakeLists.txt":
        "the task 0.2 profile spike, which predates cy_add_module(); remove both at task 3.6.1",
}

# Directories that are not the engine tree: build output, version control, editor caches, and the
# layer checker's own fixtures, which contain deliberate violations.
EXCLUDED_DIRS = {".git", ".cache", "node_modules", "target", "__pycache__"}
EXCLUDED_PATHS = {"tools/layercheck/fixtures"}


@dataclass(frozen=True)
class Violation:
    path: str
    line: int
    check: str
    message: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.check}: {self.message}"


def directory_layer(relative: str) -> int | None:
    """The layer a repository-relative path belongs to, or None if it is outside the stack."""
    parts = PurePosixPath(relative).parts
    for depth in (2, 1):
        prefix = "/".join(parts[:depth])
        if prefix in DIRECTORY_LAYERS:
            return DIRECTORY_LAYERS[prefix]
    return None


def include_layer(include: str) -> int | None:
    """The layer an include path names, by its leading directory component.

    Textual rather than filesystem-based on purpose: it gives the right answer for a header that has
    not been written yet, and does not depend on how the include directories happen to be set up.
    `cy/` and `src/` are stripped so that all three spellings of the same header agree.
    """
    parts = PurePosixPath(include.replace("\\", "/")).parts
    while parts and parts[0] in ("cy", "src"):
        parts = parts[1:]
    return LAYER_OF_NAME.get(parts[0]) if parts else None


def is_sdl_include(include: str) -> bool:
    path = PurePosixPath(include.replace("\\", "/"))
    return path.parts[0].startswith("SDL") or path.name.startswith("SDL")


def line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def walk(root: Path, names: set[str], suffixes: set[str]):
    """Yield (absolute path, repository-relative path) for the tree's own files.

    Excluded directories are pruned rather than filtered, so a build tree next to the sources costs
    nothing to skip however large it has grown. That is most of what keeps this cheap enough to run
    on every pull request.
    """
    for parent, directories, files in os.walk(root):
        prefix = PurePosixPath(Path(parent).relative_to(root).as_posix())
        directories[:] = sorted(d for d in directories if not is_excluded(prefix / d))
        for name in sorted(files):
            if name in names or Path(name).suffix in suffixes:
                yield Path(parent) / name, (prefix / name).as_posix().removeprefix("./")


def is_excluded(directory: PurePosixPath) -> bool:
    relative = directory.as_posix().removeprefix("./")
    if directory.name in EXCLUDED_DIRS or directory.name.startswith("build"):
        return True
    return relative in EXCLUDED_PATHS


def check_source_file(relative: str, text: str) -> list[Violation]:
    """Both source-level rules for one file: the layer of each include, and the SDL rule."""
    own_layer = directory_layer(relative)
    in_platform = relative.startswith("platform/")
    violations = []

    for match in INCLUDE_RE.finditer(text):
        include = match.group(1)
        line = line_of(text, match.start())

        if is_sdl_include(include) and not in_platform:
            violations.append(Violation(relative, line, "sdl",
                f"includes '{include}'. No SDL header appears outside platform/ — SDL sits beneath "
                f"the engine-owned Platform and DisplayServer interfaces, and a second backend "
                f"lands at M11 (design.md §4)."))
            continue

        target_layer = include_layer(include)
        if own_layer is None or target_layer is None or target_layer <= own_layer:
            continue
        violations.append(Violation(relative, line, "includes",
            f"a file at layer {own_layer} ({LAYER_NAMES[own_layer]}) includes '{include}' at "
            f"layer {target_layer} ({LAYER_NAMES[target_layer]}). A lower layer never depends on a "
            f"higher one."))

    return violations


def in_target_check_scope(relative: str) -> bool:
    return relative == "CMakeLists.txt" or relative.startswith(
        tuple(root + "/" for root in TARGET_CHECK_ROOTS))


def check_cmake_file(relative: str, text: str) -> list[Violation]:
    """Task 1.3.2: no target is declared outside cy_add_module()."""
    if relative in BARE_TARGET_EXEMPT or not in_target_check_scope(relative):
        return []
    return [
        Violation(relative, line_of(text, match.start()), "targets",
            f"bare {match.group(1)}(). Every engine target is declared through cy_add_module(NAME "
            f"<n> LAYER <layer> ...), which records its layer and refuses at configure time to link "
            f"a target above it. A bare target opts out of that check.")
        for match in BARE_TARGET_RE.finditer(text)
    ]


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def run(root: Path, checks: set[str]) -> tuple[list[Violation], int]:
    violations: list[Violation] = []
    scanned = 0

    if {"includes", "sdl"} & checks:
        for path, relative in walk(root, names=set(), suffixes=SOURCE_SUFFIXES):
            scanned += 1
            violations += [v for v in check_source_file(relative, read(path)) if v.check in checks]

    if "targets" in checks:
        for path, relative in walk(root, names={"CMakeLists.txt"}, suffixes={".cmake"}):
            scanned += 1
            violations += check_cmake_file(relative, read(path))

    violations.sort(key=lambda v: (v.path, v.line))
    return violations, scanned


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2],
                        help="tree to check (default: the repository root)")
    parser.add_argument("--check", action="append", choices=["includes", "sdl", "targets", "all"],
                        help="run only this check; repeatable (default: all)")
    parser.add_argument("-q", "--quiet", action="store_true", help="print nothing when clean")
    args = parser.parse_args(argv)

    selected = set(args.check or ["all"])
    checks = {"includes", "sdl", "targets"} if "all" in selected else selected

    root = args.root.resolve()
    if not root.is_dir():
        print(f"layercheck: not a directory: {root}", file=sys.stderr)
        return 2

    started = time.monotonic()
    violations, scanned = run(root, checks)
    elapsed_ms = (time.monotonic() - started) * 1000

    for violation in violations:
        print(violation, file=sys.stderr)

    if violations:
        print(f"\nlayercheck: {len(violations)} violation(s) in {scanned} files "
              f"({elapsed_ms:.0f} ms)", file=sys.stderr)
        return 1
    if not args.quiet:
        print(f"layercheck: clean — {scanned} files, {', '.join(sorted(checks))} "
              f"({elapsed_ms:.0f} ms)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
