#!/usr/bin/env python3
"""Every requirement of a capability row, against the test, gate or exemption that answers it.

WHY THIS FILE EXISTS, AND IT IS NOT "ANOTHER LINTER".

Five criteria across four ledgers ran `just quality-requirements <rows...>`:

    m11a:network-at-complete-grade    m11b:gameplay-at-complete-grade   m11b:editor-at-complete-grade
    m11c:material-compiler-at-complete-grade / image-rows-at-complete-grade, m11d, m11e

There was no such recipe. `just` stopped at argument parsing — `error: Justfile does not contain
recipes` — having run nothing, and exited 1. The criteria were therefore RED, unmutated, in the
sandbox and in the repository, which is the exact shape `falsify._red_in_the_tree` records as a
PROOF. So a command that never executed was counted, by the falsifiability mechanism itself, as a
check that had been watched going red. That is the EIGHTH unfalsifiable-criterion defect this
project has found, and the first one the prover committed rather than caught; `falsify`'s
`absent-recipe` rule now refuses the shape outright, and this module is the recipe those five
criteria were written for.

WHAT A CELL AT `complete` IS SUPPOSED TO MEAN. `docs/roadmap/capability-matrix.md` gives every row a
`Reqs` count read from `openspec/specs/<row>/spec.md`, and `delivery-roadmap` says a Complete cell is
one whose requirements are answered rather than one whose subject exists. Nothing in this repository
ever joined the two: no tool has read a specification requirement by requirement and asked what
answers it. Twenty rows named by those five criteria carry 383 requirements between them.

    python3 tools/roadmap/requirements.py <row>...          the rows, and what answers them
    python3 tools/roadmap/requirements.py <row> --list      every requirement and its evidence

WHAT COUNTS AS AN ANSWER, and every one of the four is CHECKED rather than read:

    test:<kind>.<name>        a suite declared by `cy_add_test(NAME <name> KIND <kind>)` in a
                              COMMITTED CMakeLists.txt. Verified against the tracked tree rather
                              than against `ctest -N`, so it is answerable without a build — which
                              is what lets `just roadmap-falsify` judge it in its source-only sandbox
    gate:<id>                 a gate in tools/roadmap/gates.toml
    criterion:<ledger>:<id>   a criterion of tools/roadmap/milestones/<ledger>.toml
    exempt:<milestone>        a recorded deferral: the milestone that must close it, and a `note`
                              saying why. A milestone that is not in `record.MILESTONES` is refused,
                              so an exemption cannot be parked at a rung that does not exist

AND THE OTHER DIRECTION, WHICH IS THE ONE THAT ROTS. An entry naming a requirement that no longer
appears in the specification is a FAILURE, not a silence: requirements are renamed by archived
OpenSpec changes, and a coverage map that kept answering a question nobody asks any more is how a
green tick outlives its subject. The same holds for an entry whose test, gate or criterion has been
deleted — that is the shape `m11a`'s gate found in the record audit, one level down.

THIS TOOL DOES NOT AUTHOR THE MAP. It reads `tools/roadmap/requirements-coverage.toml` and says what
is missing. The map is written by whoever reads a specification end to end, which is the work these
criteria exist to force; a map this tool filled in itself would be the defect it was built to stop.

Standard library only, like the rest of `tools/roadmap/`: these run on every pull request on three
platforms, and a gate may not depend on a package that happens to be installed.

Governed by: delivery-roadmap (A recorded tier is evaluated by a criterion), testing-and-quality.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from record import MILESTONES, REPO_ROOT  # noqa: E402

SPECS = REPO_ROOT / "openspec" / "specs"
COVERAGE = Path(__file__).resolve().parent / "requirements-coverage.toml"
GATES = Path(__file__).resolve().parent / "gates.toml"
MILESTONE_LEDGERS = Path(__file__).resolve().parent / "milestones"

#: `### Requirement: <title>` — the heading `selftest.test_matrix_requirement_counts` already counts,
#: read here for its title as well as for the count so the two cannot disagree about what a
#: requirement is.
_REQUIREMENT = re.compile(r"^### Requirement:[ \t]*(.+?)[ \t]*$", re.MULTILINE)

#: `cy_add_test(NAME save_fuzz KIND determinism ...)`, which `tests/CMakeLists.txt` turns into the
#: ctest test `determinism.save_fuzz`. Read out of the committed CMake rather than out of a build.
_CY_ADD_TEST = re.compile(
    r"cy_add_test\s*\(\s*(?=[^)]*\bNAME\s+([A-Za-z0-9_]+))(?=[^)]*\bKIND\s+([A-Za-z0-9_]+))",
    re.MULTILINE)

KINDS = ("test", "gate", "criterion", "exempt")

#: An exemption says why in prose, and prose is accepted HERE and nowhere else in this mechanism for
#: one reason: a deferral is a human decision and there is nothing to execute. What keeps it honest
#: is the milestone beside it, which `record.MILESTONES` validates and a later rung has to close.
MINIMUM_NOTE = 40


class CoverageError(RuntimeError):
    """The map itself is unreadable or malformed. Never a row that is merely uncovered."""


def requirements(row: str) -> list[str]:
    """Every requirement title in a row's specification, in the order the specification writes them."""
    spec = SPECS / row / "spec.md"
    if not spec.is_file():
        raise CoverageError(f"{row}: no specification at {spec.relative_to(REPO_ROOT)}")
    return _REQUIREMENT.findall(spec.read_text(encoding="utf-8"))


def _committed_cmake() -> list[Path]:
    """Every CMake file git TRACKS. Asked of git rather than of the filesystem, for two reasons: a
    build directory carries thousands of generated CMakeLists.txt that a walk would read, and an
    untracked one is not evidence anybody else's checkout has."""
    listed = subprocess.run(["git", "ls-files", "-z", "*CMakeLists.txt", "*.cmake"], cwd=REPO_ROOT,
                            capture_output=True, text=True, check=False)
    if listed.returncode != 0:
        raise CoverageError(f"git could not list this tree's CMake files: {listed.stderr.strip()}")
    return [REPO_ROOT / name for name in listed.stdout.split("\0") if name]


def declared_tests() -> set[str]:
    """`<kind>.<name>` for every suite a committed CMakeLists.txt declares."""
    found: set[str] = set()
    for path in _committed_cmake():
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        for name, kind in _CY_ADD_TEST.findall(text):
            found.add(f"{kind}.{name}")
    return found


def declared_gates() -> set[str]:
    gates = tomllib.loads(GATES.read_text(encoding="utf-8"))
    return {str(gate.get("id", "")) for gate in gates.get("gate", [])}


def declared_criteria() -> set[str]:
    """`<ledger>:<id>` over every milestone ledger, read as TOML rather than through `criteria.load`.

    `criteria.load` validates a whole ledger and raises on the first fault in it. This module is run
    by five criteria of four ledgers and must be able to say "that criterion is not there" without
    being taken down by an unrelated fault in a ledger it was not asked about.
    """
    found: set[str] = set()
    for ledger in MILESTONE_LEDGERS.glob("*.toml"):
        try:
            loaded = tomllib.loads(ledger.read_text(encoding="utf-8"))
        except (OSError, tomllib.TOMLDecodeError):
            continue
        for criterion in loaded.get("criterion", []):
            found.add(f"{ledger.stem}:{criterion.get('id', '')}")
    return found


@dataclass
class Entry:
    row: str
    requirement: str
    evidence: str
    note: str

    @property
    def kind(self) -> str:
        return self.evidence.split(":", 1)[0]

    @property
    def subject(self) -> str:
        return self.evidence.split(":", 1)[1] if ":" in self.evidence else ""


def coverage(path: Path = COVERAGE) -> list[Entry]:
    """The committed map. A malformed entry raises: half a map is not a smaller map."""
    if not path.is_file():
        raise CoverageError(f"no coverage map at {path}")
    loaded = tomllib.loads(path.read_text(encoding="utf-8"))
    entries = []
    seen: set[tuple[str, str]] = set()
    for index, table in enumerate(loaded.get("coverage", []), start=1):
        unknown = set(table) - {"row", "requirement", "evidence", "note"}
        if unknown:
            raise CoverageError(f"{path.name} entry {index}: unknown key(s) {', '.join(sorted(unknown))}")
        entry = Entry(row=str(table.get("row", "")), requirement=str(table.get("requirement", "")),
                      evidence=str(table.get("evidence", "")), note=str(table.get("note", "")))
        if not entry.row or not entry.requirement or not entry.evidence:
            raise CoverageError(f"{path.name} entry {index}: 'row', 'requirement' and 'evidence' "
                                "are all required")
        if entry.kind not in KINDS:
            raise CoverageError(f"{path.name} entry {index}: evidence {entry.evidence!r} does not "
                                f"begin with one of {', '.join(k + ':' for k in KINDS)}")
        key = (entry.row, entry.requirement)
        if key in seen:
            raise CoverageError(f"{path.name} entry {index}: {entry.row} / {entry.requirement!r} is "
                                "answered twice; one requirement, one answer")
        seen.add(key)
        entries.append(entry)
    return entries


@dataclass
class Resolver:
    """What this tree actually declares, read once for every row a run is asked about."""

    tests: set[str] = field(default_factory=declared_tests)
    gates: set[str] = field(default_factory=declared_gates)
    criteria: set[str] = field(default_factory=declared_criteria)

    def unresolved(self, entry: Entry) -> str:
        """Why this entry's evidence does not exist in this tree, or an empty string."""
        if entry.kind == "test":
            if entry.subject not in self.tests:
                return (f"no suite `{entry.subject}` is declared by any committed cy_add_test() — "
                        "renamed, deleted, or never written")
            return ""
        if entry.kind == "gate":
            return "" if entry.subject in self.gates else f"no gate `{entry.subject}` in gates.toml"
        if entry.kind == "criterion":
            if entry.subject not in self.criteria:
                return f"no criterion `{entry.subject}` in tools/roadmap/milestones/"
            return ""
        if entry.subject.lower() not in MILESTONES:
            return (f"exempt until `{entry.subject}`, which is not a milestone; they are "
                    f"{', '.join(MILESTONES)}")
        if len(entry.note.strip()) < MINIMUM_NOTE:
            return (f"an exemption needs a 'note' of at least {MINIMUM_NOTE} characters saying why; "
                    f"this one has {len(entry.note.strip())}")
        return ""


@dataclass
class Report:
    row: str
    total: int = 0
    answered: int = 0
    uncovered: list[str] = field(default_factory=list)
    broken: list[str] = field(default_factory=list)
    stale: list[str] = field(default_factory=list)

    @property
    def shortfalls(self) -> int:
        return len(self.uncovered) + len(self.broken) + len(self.stale)


def audit(row: str, entries: list[Entry], resolver: Resolver, verbose: bool = False) -> Report:
    """One row: every requirement against the map, and every entry of the map against the row."""
    titles = requirements(row)
    report = Report(row=row, total=len(titles))
    answers = {entry.requirement: entry for entry in entries if entry.row == row}

    print(f"\n{row}: {len(titles)} requirement(s) in openspec/specs/{row}/spec.md")
    for title in titles:
        entry = answers.get(title)
        if entry is None:
            report.uncovered.append(title)
            if verbose:
                print(f"  UNANSWERED  {title}")
            continue
        broken = resolver.unresolved(entry)
        if broken:
            report.broken.append(f"{title} -> {entry.evidence}: {broken}")
            print(f"  BROKEN      {title}")
            print(f"              {entry.evidence}: {broken}")
            continue
        report.answered += 1
        if verbose:
            print(f"  ok          {title}  <- {entry.evidence}")

    for title in sorted(set(answers) - set(titles)):
        report.stale.append(f"{title} -> {answers[title].evidence}")
        print(f"  STALE       the map answers {title!r}, which this specification no longer asks")

    print(f"  {report.answered} answered, {len(report.uncovered)} unanswered, "
          f"{len(report.broken)} answered by something that is not there, "
          f"{len(report.stale)} stale entr(ies)")
    if report.uncovered and not verbose:
        print("  unanswered:")
        for title in report.uncovered:
            print(f"    {title}")
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("rows", nargs="+", help="capability rows, as named in openspec/specs/")
    parser.add_argument("--list", action="store_true", dest="verbose",
                        help="print every requirement and the evidence that answers it")
    parser.add_argument("--map", default=str(COVERAGE), help="the coverage map to read")
    arguments = parser.parse_args(argv)

    try:
        entries = coverage(Path(arguments.map))
        resolver = Resolver()
        reports = [audit(row, entries, resolver, arguments.verbose) for row in arguments.rows]
    except CoverageError as error:
        print(f"\nrequirements: {error}", file=sys.stderr)
        return 2

    total = sum(report.total for report in reports)
    answered = sum(report.answered for report in reports)
    print(f"\n{answered} of {total} requirement(s) across {len(reports)} row(s) map to a test, a "
          f"gate or a recorded exemption")

    failing = [report for report in reports if report.shortfalls]
    if not failing:
        return 0
    print("\nTHESE ROWS ARE NOT AT COMPLETE GRADE:", file=sys.stderr)
    for report in failing:
        print(f"  {report.row}: {len(report.uncovered)} requirement(s) answered by nothing, "
              f"{len(report.broken)} by something that is not there, {len(report.stale)} stale",
              file=sys.stderr)
    print("\nEach one needs an entry in tools/roadmap/requirements-coverage.toml naming the suite, "
          "the gate, the criterion or the deferral that answers it.", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
