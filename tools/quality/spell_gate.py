#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The spelling gate — `testing-and-quality`, "Static analysis and formatting".

    The repository SHALL enforce, via pre-commit hooks and CI: … spelling checks on documentation
    and comments

--- WHY codespell AND NOT A DICTIONARY ---------------------------------------------------------------

A dictionary speller over a game engine produces thousands of hits — `navmesh`, `bitmask`, `swizzle`,
`Slang`, every type name — and a gate whose output is thousands of false positives is a gate that is
switched off in its first week. codespell is the other shape: a curated list of *known misspellings*
of real words. It says nothing about `navmesh`; it says the commonest transposition of "the" is a
misspelling of it.

Measured over this tree on 2026-09-19 before any configuration: 889 hits, of which **all but three
were domain vocabulary** — `lod`, `ser`, `thirdparty`, `inout`, `paeth`, `PerView`, `OutIn`. That
ratio is what the two committed configuration files are for, and it is why the gate is green today
rather than red-and-ignored.

--- THE TWO FILES, AND THE DIFFERENCE BETWEEN THEM ---------------------------------------------------

`spelling-ignore.txt`   a word this project spells deliberately, suppressed EVERYWHERE. Each line
                        carries its reason, because every line narrows the gate.
`spelling-exclude.txt`  ONE LINE of ONE FILE, suppressed exactly. Its entries are literal source
                        lines; codespell skips a line that matches one of them. This is where a real
                        misspelling that must stay goes — the Arabic letter whose Unicode name is a
                        transposition of "the", in the shaper's joining table, and the settings test
                        that feeds a deliberately misspelled `--renderer.<...>` option and requires
                        it to be refused.

The split matters: putting that transposition in the ignore list would make a genuine one invisible
across the whole tree, which is the gate quietly ceasing to check the most common typo in English.

--- WHAT IS READ -------------------------------------------------------------------------------------

The engine's own trees, their comments and their documentation, plus `openspec/`, `docs/`, the
workflows and the recipes. Fetched dependencies, build trees, generated output and binary captures
are not this project's prose.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]

#: What the gate reads. `docs/` and `openspec/` are here because the requirement says
#: "documentation and comments", and the specifications are the documentation with the most readers.
ROOTS = ("src", "platform", "modules", "tests", "benchmarks", "samples", "bindings", "editor/src",
         "tools", "cmake", "docs", "openspec", "just", ".github", "README.md", "CONTRIBUTING.md",
         "justfile", "THIRD_PARTY.md")

#: Not this project's prose: fetched sources, build trees, generator output, recorded captures.
SKIP = ("*/build/*", "./build*", "*/target/*", "*/.build/*", "*/_deps/*", "*/__pycache__/*",
        "*/node_modules/*", "*.cytrace", "*/external/*", "*/Generated/*",
        "*/layercheck/fixtures/*", "*/project/fixtures/*")

IGNORE_WORDS = "tools/quality/spelling-ignore.txt"
EXCLUDE_LINES = "tools/quality/spelling-exclude.txt"

INSTALL = "pip install codespell==2.4.3"


def ignored_words(path: Path) -> list[str]:
    """The ignore list, with its reasons stripped. Comments are this file's whole point."""
    words = []
    for line in path.read_text(encoding="utf-8").splitlines():
        word = line.split("#", 1)[0].strip()
        if word:
            words.append(word)
    return words


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=REPOSITORY)
    parser.add_argument("paths", nargs="*", help="limit the check to these paths")
    arguments = parser.parse_args()

    root = arguments.root.resolve()
    tool = shutil.which("codespell")
    if not tool:
        print("spelling-gate: codespell is not on PATH.", file=sys.stderr)
        print("  It is the tool that reads the misspelling dictionary, and a spelling gate with no", file=sys.stderr)
        print(f"  dictionary checks nothing. Install it with:\n      {INSTALL}", file=sys.stderr)
        return 1

    ignore = root / IGNORE_WORDS
    exclude = root / EXCLUDE_LINES
    for path in (ignore, exclude):
        if not path.is_file():
            print(f"spelling-gate: {path.relative_to(root)} is missing.", file=sys.stderr)
            print("  Without it the gate would report this project's own vocabulary as errors.",
                  file=sys.stderr)
            return 1

    targets = [str(path) for path in (arguments.paths or ROOTS) if (root / path).exists()]
    if not targets:
        print("spelling-gate: none of the roots exist under --root. The gate read nothing, so it",
              file=sys.stderr)
        print("  cannot say the tree is clean.", file=sys.stderr)
        return 1

    command = [
        tool,
        "--quiet-level=2",
        f"--ignore-words-list={','.join(ignored_words(ignore))}",
        f"--exclude-file={exclude}",
        f"--skip={','.join(SKIP)}",
        *targets,
    ]
    finished = subprocess.run(command, cwd=root, capture_output=True, text=True)
    findings = [line for line in finished.stdout.splitlines() if line.strip()]

    if findings:
        print(f"spelling-gate: {len(findings)} misspelling(s) in documentation or comments.",
              file=sys.stderr)
        for line in findings:
            print(f"    {line}", file=sys.stderr)
        print("  Fix them, or — if the word is this project's own — add it to", file=sys.stderr)
        print(f"  {IGNORE_WORDS} WITH THE REASON, which is what that file is reviewed for.",
              file=sys.stderr)
        return 1

    if finished.returncode not in (0, 65):  # 65 is codespell's "found something", handled above
        print(f"spelling-gate: codespell exited {finished.returncode}.", file=sys.stderr)
        print(finished.stderr, file=sys.stderr)
        return 1

    print(f"spelling-gate: no misspellings over {len(targets)} root(s); "
          f"{len(ignored_words(ignore))} declared words, "
          f"{len(exclude.read_text(encoding='utf-8').splitlines())} excluded lines")
    return 0


if __name__ == "__main__":
    sys.exit(main())
