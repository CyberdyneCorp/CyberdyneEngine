#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prove M11.d's four quality gates by breaking what they check, on purpose.

`tools/abi/selftest.py` states the rule: **a gate that has never failed is a gate nobody knows
works.** This repository has shipped nine checks unable to go red — `tools/roadmap/falsify.py`
enumerates them — and M9's closing gate found one that had passed 44 of 44 runs with its enforcement
point deleted. Four new gates landing without this file would be the tenth through thirteenth.

--- THE SANDBOX IS DERIVED FROM THE LIVE TREE, NEVER COPIED INTO A FIXTURES DIRECTORY ----------------

Each case copies **the real files the gate reads, at their real repository-relative paths**, into a
temporary tree, applies exactly one edit, and runs the gate there with `--root`. Nothing is stored
under `fixtures/`, for the two reasons the ABI and editor selftests give: a copied fixture goes
stale, and a copied fixture can pass while the gate is broken — if the reader stopped recognising
the tree, a hand-written "broken" fixture and a hand-written "correct" one would both describe
nothing, and comparing nothing to nothing succeeds.

A *slice* rather than the whole tree, because the licence gate reads 2 584 files and a selftest that
copies them for each of eleven cases is a selftest nobody runs. The slice is real files chosen by
pattern, so it tracks the tree without being the tree.

**Case 0 is load-bearing**: the UNEDITED slice must pass every gate. Every other case is only
meaningful because it does.

--- THE THREE KINDS OF CASE --------------------------------------------------------------------------

1. **The gate catches the defect it exists for** — a new file with no licence header, a new public
   symbol with no documentation, a misspelling in prose, Swift that does not match `.swift-format`.
2. **The gate's own backlog cannot rot** — a baseline entry that has been fixed must ALSO fail, so
   the declared debt can only shrink. Without this case a baseline is an allowlist.
3. **The gate refuses to read nothing** — pointed at a tree with no input, or with its configuration
   removed, each gate must FAIL rather than report a clean result. This is the exact defect class
   this project has paid for nine times, and it is the case that is most often missing.

--- WHAT THIS HOST CANNOT RUN ------------------------------------------------------------------------

`swift-format-break` needs a Swift toolchain and there is none on the Linux machine M11.d was worked
on. By default it is reported **NOT EVALUATED**, which is not a pass; `--strict`, which is what CI
runs on a leg that installs Swift 6, refuses to exit 0 with any case unrun.

Run through `just quality-gates-selftest`.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

REPOSITORY = Path(__file__).resolve().parents[2]
GATES = REPOSITORY / "tools/quality"

#: The files each gate's sandbox holds. Globs against the live tree, so the slice follows the tree.
#: Enough of each kind that a gate reading zero of something is distinguishable from one reading it.
SLICES: dict[str, tuple[str, ...]] = {
    "licence": (
        "src/core/base/include/cy/core/base/*.h",
        "src/core/memory/include/cy/core/memory/*.h",
        "platform/host/src/*.cpp",
        "tools/quality/licence_gate.py",
    ),
    "doc": (
        "src/core/base/include/cy/core/base/*.h",
        "src/ecs/include/cy/ecs/*.h",
        "src/abi/include/cy/abi/cy_abi.h",
    ),
    "spelling": (
        "docs/*.md",
        "tools/quality/spelling-ignore.txt",
        "tools/quality/spelling-exclude.txt",
        "src/text/src/shaping.cpp",
    ),
    "swift": (
        "bindings/swift/Sources/CyberdyneKit/*.swift",
        ".swift-format",
    ),
}


@dataclass
class Case:
    """One edit, the gate it should break, and the words the gate must say about it."""

    name: str
    gate: str
    #: Why this case exists — printed on failure, because "case 4 failed" tells a reader nothing.
    about: str
    #: The exit status the gate must produce. 0 only for case 0.
    expect: int = 1
    #: Text the gate's output must contain, so a gate that fails for an unrelated reason is caught.
    says: tuple[str, ...] = ()
    #: `(path, find, replace)` — an edit in the sandbox. `find` empty appends, None replaces the
    #: whole file, `path` absent creates it.
    edit: tuple[str, str, str] | None = None
    #: Paths to delete from the sandbox before running.
    remove: tuple[str, ...] = ()
    #: Run the gate against an empty directory instead of the slice.
    empty: bool = False
    #: Extra arguments.
    arguments: list[str] = field(default_factory=list)


CASES: tuple[Case, ...] = (
    # --- Case 0: the unedited slice. Every case below is meaningless without this one. -------------
    Case("licence-clean", "licence", "the unedited slice passes, so an edit is what fails", expect=0,
         says=("declared backlog",)),
    Case("doc-clean", "doc", "the unedited slice passes, so an edit is what fails", expect=0,
         says=("public symbols documented",)),
    Case("spelling-clean", "spelling", "the unedited slice passes, so an edit is what fails",
         expect=0, says=("no misspellings",)),

    # --- 1. Each gate catches the defect it exists for ---------------------------------------------
    Case("licence-new-file", "licence",
         "a source file arriving with no SPDX header is what this gate is for",
         says=("carry no licence header", "src/core/base/include/cy/core/base/m11d_probe.h"),
         edit=("src/core/base/include/cy/core/base/m11d_probe.h", "",
               "#pragma once\nnamespace cy { struct Probe {}; }\n")),
    Case("doc-new-symbol", "doc",
         "a newly exported function with no documentation comment — the specification's own scenario",
         says=("without a documentation comment", "m11d_probe"),
         edit=("src/ecs/include/cy/ecs/system.h", "\nnamespace cy::ecs {",
               "\nnamespace cy::ecs {\n\n[[nodiscard]] int m11d_probe(int value) noexcept;\n")),
    Case("spelling-typo", "spelling",
         "a misspelling in documentation, which is the requirement in one word",
         says=("misspelling", "recieve"),
         edit=("docs/README.md", "", "\nThe gate must recieve this line and object to it.\n")),
    Case("swift-format-break", "swift",
         "Swift that does not match the committed .swift-format",
         says=("not what these files are formatted to",),
         edit=("bindings/swift/Sources/CyberdyneKit/Diagnostics.swift", "public enum",
               "public   enum")),

    # --- 2. The declared backlog cannot rot --------------------------------------------------------
    Case("licence-stale-baseline", "licence",
         "a baseline file that has since gained a header must fail, or the backlog is an allowlist",
         says=("no longer name a file without a header",),
         arguments=["--header-a-baseline-file"]),
    Case("doc-stale-baseline", "doc",
         "a baseline symbol that has since been documented must fail, for the same reason",
         says=("no longer name an undocumented symbol",),
         # Filled in at run time: the first entry of the sandbox's own baseline.
         edit=None, arguments=["--document-a-baseline-symbol"]),

    # --- 3. Each gate refuses to read nothing ------------------------------------------------------
    Case("licence-reads-nothing", "licence",
         "pointed at a tree with no sources, the gate must fail rather than report it clean",
         says=("read nothing",), empty=True),
    Case("spelling-no-configuration", "spelling",
         "without its ignore list the gate would report this project's vocabulary as errors — and "
         "without its exclude list it would be red forever, so both are required rather than optional",
         says=("is missing",), remove=("tools/quality/spelling-ignore.txt",)),
    Case("spelling-ignore-list-is-load-bearing", "spelling",
         "removing one word from the ignore list must turn the gate red, which is how we know the "
         "list is being read at all rather than silently dropped",
         says=("misspelling",),
         edit=("tools/quality/spelling-ignore.txt", "lod            #", "#")),
    Case("doc-parser-blind", "doc",
         "a public header that declares a cy namespace and parses to no symbol is the gate failing "
         "to read the tree, not the tree being clean — this project's most-repeated defect",
         says=("parsed to no symbol",),
         edit=("src/ecs/include/cy/ecs/system.h", None, "#pragma once\nnamespace cy::ecs {\n}\n")),
    Case("swift-no-configuration", "swift",
         "without .swift-format the tool would enforce whatever its default is on the machine it "
         "ran on, which is not a gate — this case needs no toolchain and runs everywhere",
         says=("is missing",), remove=(".swift-format",)),
)

COMMANDS = {
    "licence": ("licence_gate.py", "licence_baseline.txt"),
    "doc": ("doc_gate.py", "doc_baseline.txt"),
    "spelling": ("spell_gate.py", None),
    "swift": ("swift_format_gate.py", None),
}


def build_slice(gate: str, sandbox: Path) -> int:
    """Copy the live files this gate reads into `sandbox`, at their repository-relative paths."""
    copied = 0
    for pattern in SLICES[gate]:
        for path in sorted(REPOSITORY.glob(pattern)):
            if not path.is_file():
                continue
            target = sandbox / path.relative_to(REPOSITORY)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
            copied += 1
    return copied


def prepare_baseline(gate: str, sandbox: Path) -> None:
    """Record the slice's own backlog, so a case's edit is the only thing the gate can object to."""
    script, baseline = COMMANDS[gate]
    if not baseline:
        return
    subprocess.run([sys.executable, str(GATES / script), "--root", str(sandbox),
                    "--baseline", str(sandbox / baseline), "--update"],
                   check=True, capture_output=True, text=True)


def document_a_baseline_symbol(sandbox: Path) -> bool:
    """Write a doc comment above the first symbol the sandbox's baseline names. True if it worked."""
    baseline = sandbox / "doc_baseline.txt"
    entries = [line.strip() for line in baseline.read_text(encoding="utf-8").splitlines()
               if line.strip() and not line.startswith("#")]
    if not entries:
        return False
    relative, _, name = entries[0].partition("::")
    path = sandbox / relative
    lines = path.read_text(encoding="utf-8").splitlines(keepends=True)
    for index, line in enumerate(lines):
        if name in line and not line.strip().startswith("//"):
            indent = line[: len(line) - len(line.lstrip())]
            lines.insert(index, f"{indent}/// Documented by the selftest, so the baseline is stale.\n")
            path.write_text("".join(lines), encoding="utf-8")
            return True
    return False


def header_a_baseline_file(sandbox: Path) -> bool:
    """Put the SPDX line at the top of the first file the sandbox's baseline names. True if it worked.

    The file is chosen from the baseline rather than named here, so the case cannot go stale when a
    source file is renamed — which is the failure mode a hand-written fixture path has.
    """
    baseline = sandbox / "licence_baseline.txt"
    entries = [line.strip() for line in baseline.read_text(encoding="utf-8").splitlines()
               if line.strip() and not line.startswith("#")]
    if not entries:
        return False
    path = sandbox / entries[0]
    path.write_text("// SPDX-License-Identifier: MIT\n" + path.read_text(encoding="utf-8"),
                    encoding="utf-8")
    return True


def run(case: Case, sandbox: Path) -> tuple[int, str]:
    script, baseline = COMMANDS[case.gate]
    command = [sys.executable, str(GATES / script), "--root", str(sandbox)]
    if baseline:
        command += ["--baseline", str(sandbox / baseline)]
    finished = subprocess.run(command, capture_output=True, text=True)
    return finished.returncode, finished.stdout + finished.stderr


def apply(case: Case, sandbox: Path) -> bool:
    for relative in case.remove:
        target = sandbox / relative
        if target.exists():
            target.unlink()
    if "--header-a-baseline-file" in case.arguments:
        return header_a_baseline_file(sandbox)
    if "--document-a-baseline-symbol" in case.arguments:
        return document_a_baseline_symbol(sandbox)
    if case.edit is None:
        return True
    relative, find, replace = case.edit
    path = sandbox / relative
    if find is None:  # replace the file wholesale
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(replace, encoding="utf-8")
        return True
    if find == "":  # append, or create
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("a", encoding="utf-8") as handle:
            handle.write(replace)
        return True
    text = path.read_text(encoding="utf-8")
    if find not in text:
        return False
    path.write_text(text.replace(find, replace, 1), encoding="utf-8")
    return True


def execute(case: Case) -> tuple[bool, str]:
    with tempfile.TemporaryDirectory(prefix="cy-quality-selftest-") as directory:
        sandbox = Path(directory)
        if case.empty:
            (sandbox / "src").mkdir()
        else:
            if build_slice(case.gate, sandbox) == 0:
                return False, "the slice copied no files — the patterns no longer match the tree"
            prepare_baseline(case.gate, sandbox)
            if not apply(case, sandbox):
                return False, "the edit did not apply — the text it looks for has left the tree"

        status, output = run(case, sandbox)
        if status == 2:
            return False, "UNRUN"
        if status != case.expect:
            return False, f"exit {status}, expected {case.expect}\n{_indent(output)}"
        for phrase in case.says:
            if phrase not in output:
                return False, f"output does not mention {phrase!r}\n{_indent(output)}"
        return True, ""


def _indent(text: str) -> str:
    return "\n".join(f"      | {line}" for line in text.splitlines()[:20])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--strict", action="store_true",
                        help="refuse to pass with any case unrun — what CI runs")
    parser.add_argument("-k", dest="only", default="", help="run only cases whose name contains this")
    arguments = parser.parse_args()

    cases = [case for case in CASES if arguments.only in case.name]
    failed: list[str] = []
    unrun: list[str] = []

    print(f"==> quality-gates  {len(cases)} case(s) over four gates")
    for case in cases:
        passed, detail = execute(case)
        if passed:
            print(f"    ok       {case.name}")
        elif detail == "UNRUN":
            print(f"    UNRUN    {case.name}  — no toolchain on this machine")
            unrun.append(case.name)
        else:
            print(f"    FAILED   {case.name}")
            print(f"      why the case exists: {case.about}")
            print(f"      {detail}")
            failed.append(case.name)

    sys.stdout.flush()
    if failed:
        print(f"\nquality-gates selftest: {len(failed)} case(s) failed: {', '.join(failed)}",
              file=sys.stderr)
        print("  A gate that does not go red on these is a gate that cannot go red.", file=sys.stderr)
        return 1

    if unrun:
        print(f"\n  NOT EVALUATED: {', '.join(unrun)}")
        print("  These need a Swift toolchain, which this machine does not have. NOT EVALUATED is")
        print("  not a pass — CI runs `--strict` on a leg that installs Swift 6, and that is where")
        print("  the swift-format gate's red is watched.")
        if arguments.strict:
            print("\nquality-gates selftest: --strict, and a case did not run.", file=sys.stderr)
            return 1

    print(f"\nquality-gates selftest: {len(cases) - len(unrun)} of {len(cases)} cases proved a red")
    return 0


if __name__ == "__main__":
    sys.exit(main())
