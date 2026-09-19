#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The licence-header gate — `testing-and-quality`, "Static analysis and formatting".

    The repository SHALL enforce, via pre-commit hooks and CI: … a licence header check on every
    source file

The repository is MIT (`LICENSE`), every dependency's licence is recorded in `THIRD_PARTY.md`, and
until M11.d **not one source file in the tree said so**: a search for `SPDX-License-Identifier`
across `src/`, `platform/`, `bindings/` and `editor/` returned zero. A distributed source tree whose
files carry no licence marking is the thing this requirement is about.

--- THE HEADER --------------------------------------------------------------------------------------

One line, in the file's own comment syntax, within the first five lines:

    // SPDX-License-Identifier: MIT

SPDX rather than a copyright paragraph, for a reason that is not taste: it is machine-readable, so
this gate is an exact match rather than a fuzzy one, and a licence scanner run against a shipped
source drop gets the same answer this gate does.

--- WHY THERE IS A BACKLOG, AND WHY IT IS NOT AN ALLOWLIST -------------------------------------------

2 632 source files predate the gate. Prepending a line to all of them in the change that introduces
the gate would be a 2 632-file diff landing beside eleven other agents' work in one tree, and the
first merge conflict would silently drop the line from some unknown subset — which is a licence
defect introduced by a licence gate.

So the shape is `tools/quality/doc_gate.py`'s, which is `src/abi/abi_baseline.json`'s:
`licence_baseline.txt` lists the files that had no header when the gate landed, **by path**, and

  * a source file **not** in the baseline and **not** carrying the header fails the gate — so no new
    file can arrive without one;
  * a baseline path that has since gained the header, or been deleted or renamed, **also** fails,
    telling the author to drop the line — so the backlog can only shrink and a stale baseline is a
    red build rather than a quiet exemption.

The count is printed on every run. It is a debt with a number on it, which is the difference between
a backlog and an excuse.

--- WHAT IS NOT A SOURCE FILE OF THIS PROJECT --------------------------------------------------------

Fetched dependencies (`external/`, `deps/`, `_deps/`), build trees, generated output whose generator
owns the bytes, and `tools/layercheck/fixtures/` — which `just quality-format-check` excludes for the
same reason: those files are inputs to a checker and several are deliberately malformed.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]

#: The exact text a source file must carry. Matched as a substring so the comment marker may differ.
MARKER = "SPDX-License-Identifier: MIT"

#: How far into the file the marker may sit. Five lines allows a shebang, an include guard's comment
#: or a generated-file banner above it without allowing it to be buried.
WINDOW = 5

#: The roots this project's own sources live under.
ROOTS = ("src", "platform", "modules", "tests", "benchmarks", "samples", "bindings", "editor",
         "tools", "cmake")

SUFFIXES = (".h", ".hpp", ".inl", ".c", ".cc", ".cpp", ".cxx", ".m", ".mm", ".swift", ".rs", ".py",
            ".slang")

#: Directory names that are never this project's source, wherever they appear.
PRUNED = {".git", ".build", "_deps", "__pycache__", "node_modules", "target", "Generated"}

#: Paths excluded with a reason, rather than by a pattern that would quietly widen.
EXCLUDED = (
    # Deliberately malformed trees a checker reads. `just quality-format-check` excludes them too.
    "tools/layercheck/fixtures/",
    "tools/project/fixtures/",
    # Generated: the generator owns every byte, and a line added here is erased by the next run.
    # `just generate-check` is what holds these to their generator.
    "bindings/swift/Sources/CyberdyneCore/Generated/",
    # A Swift package manifest is read by SwiftPM before the first line of it is compiled, and its
    # `// swift-tools-version:` comment must be the first line of the file.
    "bindings/swift/Package.swift",
)

BASELINE = "tools/quality/licence_baseline.txt"


def _pruned(path: Path, root: Path) -> bool:
    parts = path.relative_to(root).parts
    if any(part in PRUNED for part in parts):
        return True
    # A build tree is a directory with a CMakeCache.txt in it, not a directory called `build`.
    for index in range(1, len(parts)):
        if parts[index - 1].startswith("build") and (root.joinpath(*parts[:index]) / "CMakeCache.txt").is_file():
            return True
    return False


def sources(root: Path) -> list[str]:
    """Every file this project licenses, as repository-relative paths, sorted."""
    found: list[str] = []
    for name in ROOTS:
        base = root / name
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if not path.is_file() or path.suffix not in SUFFIXES or _pruned(path, root):
                continue
            relative = str(path.relative_to(root))
            if any(relative.startswith(prefix) for prefix in EXCLUDED):
                continue
            found.append(relative)
    return sorted(found)


def carries_header(path: Path) -> bool:
    try:
        with path.open(encoding="utf-8", errors="replace") as handle:
            for index, line in enumerate(handle):
                if index >= WINDOW:
                    return False
                if MARKER in line:
                    return True
    except OSError:
        return False
    return False


def read_baseline(path: Path) -> set[str]:
    if not path.is_file():
        return set()
    return {
        line.strip()
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip() and not line.startswith("#")
    }


def write_baseline(path: Path, missing: set[str], total: int) -> None:
    header = (
        "# The source files that carried no SPDX licence header when the licence gate landed, at\n"
        "# M11.d. tools/quality/licence_gate.py explains why this file exists and why it can only\n"
        "# shrink: a path here that has since gained the header, or left the tree, FAILS the gate.\n"
        "#\n"
        "# Add `// SPDX-License-Identifier: MIT` to a file and delete its line here. Regenerate\n"
        f"# with `just quality-licence --update`. Backlog at last write: {len(missing)} of {total}.\n"
    )
    path.write_text(header + "\n".join(sorted(missing)) + "\n", encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=REPOSITORY)
    parser.add_argument("--baseline", type=Path, default=None)
    parser.add_argument("--summary", action="store_true", help="counts only, for the selftest")
    parser.add_argument("--update", action="store_true", help="record today's backlog as the baseline")
    arguments = parser.parse_args()

    root = arguments.root.resolve()
    baseline_path = arguments.baseline or (root / BASELINE)

    files = sources(root)
    missing = {relative for relative in files if not carries_header(root / relative)}

    if arguments.summary:
        print(f"files {len(files)}")
        print(f"missing {len(missing)}")
        return 0

    if arguments.update:
        write_baseline(baseline_path, missing, len(files))
        print(f"licence-gate: baseline written — {len(missing)} of {len(files)} without a header")
        return 0

    # A tree that yields no files at all is the gate failing to read it, not a clean tree. This is
    # the shape of defect `tools/roadmap/falsify.py` enumerates, and it is refused here rather than
    # reported as a pass.
    if not files:
        print(f"licence-gate: no source files found under {root}. The gate read nothing, so it",
              file=sys.stderr)
        print("  cannot say the tree is clean. Check --root.", file=sys.stderr)
        return 1

    baseline = read_baseline(baseline_path)
    new = sorted(missing - baseline)
    stale = sorted(baseline - missing)

    if new:
        print(f"licence-gate: {len(new)} source file(s) carry no licence header.", file=sys.stderr)
        print(f"  Add `// SPDX-License-Identifier: MIT` within the first {WINDOW} lines of each:",
              file=sys.stderr)
        for relative in new:
            print(f"    {relative}", file=sys.stderr)
        return 1

    if stale:
        print(f"licence-gate: {len(stale)} baseline entr{'y' if len(stale) == 1 else 'ies'} "
              "no longer name a file without a header.", file=sys.stderr)
        print("  The backlog only shrinks, so this is a red build rather than a quiet exemption.",
              file=sys.stderr)
        for relative in stale:
            print(f"    {relative}", file=sys.stderr)
        print("  Remove those lines: `just quality-licence --update`.", file=sys.stderr)
        return 1

    print(f"licence-gate: {len(files) - len(missing)} of {len(files)} source files carry "
          f"`{MARKER}`; {len(baseline)} in the declared backlog, 0 new")
    return 0


if __name__ == "__main__":
    sys.exit(main())
