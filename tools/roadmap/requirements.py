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

WHAT COUNTS AS AN ANSWER, and every one of the five is CHECKED rather than read:

    test:<kind>.<name>        a suite declared by `cy_add_test(NAME <name> KIND <kind>)` in a
                              COMMITTED CMakeLists.txt, PLUS a `case` naming the test case in it
                              that answers this requirement. Verified against the tracked tree
                              rather than against `ctest -N`, so it is answerable without a build —
                              which is what lets `just roadmap-falsify` judge it in its source-only
                              sandbox
    rust:<crate>::<path>      a `#[test]` function of an editor crate, by its full path, e.g.
                              `rust:cy-editor-interface::specialised::tests::every_graph_editor...`
    gate:<id>                 a gate in tools/roadmap/gates.toml
    criterion:<ledger>:<id>   a criterion of tools/roadmap/milestones/<ledger>.toml THAT HAS BEEN
                              PROVEN ABLE TO FAIL — `tools/roadmap/falsifiability.toml` must record
                              a proof for it. See below
    exempt:<milestone>        a recorded deferral: the milestone that must close it, and a `note`
                              saying why. A milestone that is not in `record.MILESTONES` is refused,
                              so an exemption cannot be parked at a rung that does not exist

--- WHY A SUITE ALONE IS NOT AN ANSWER, AND WHY AN UNPROVEN CRITERION IS NOT ONE EITHER -------------

This module's first version accepted `test:unit.editor_documents` and asked only whether a suite of
that name was declared somewhere in the tree. **Twenty-four requirements could have been answered by
naming one suite twenty-four times**, and every one of them would have resolved. That is the ninth
instance of the defect `tools/roadmap/falsify.py` enumerates, in the tool built to count the other
eight — so it was corrected before the map had a single entry in it rather than after.

What a `test:` entry now claims is a NAMED CASE, and the case has to be in the SUITE'S OWN SOURCES:
the text is searched for in the tracked files under the directory of the `CMakeLists.txt` that
declares the suite. Renaming the case turns the entry red; pointing an entry at a case that belongs
to another suite is refused at the directory.

And a `criterion:` entry inherits its strength from the criterion, so it inherits its weakness too.
`falsifiability.toml` is the record of which criteria have been broken on purpose and watched go
red; 584 of 641 have not. An entry naming one of those would be a requirement answered by a check
nobody has shown can fail, which is the thing this ladder spent a whole phase building a prover
for. So the proof is required, and the refusal says which criterion and what would fix it.

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

KINDS = ("test", "rust", "gate", "criterion", "exempt")

FALSIFIABILITY = Path(__file__).resolve().parent / "falsifiability.toml"
EDITOR_CRATES = REPO_ROOT / "editor" / "crates"

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


#: A directory a walk must not descend into when there is no git to ask. `_deps` and any directory
#: holding a `CMakeCache.txt` is a build tree, which carries thousands of generated CMakeLists.txt;
#: the rest are checkouts of other things.
_NOT_SOURCE = frozenset({".git", "_deps", "node_modules", "target", ".venv"})


def _walked_cmake() -> list[Path]:
    """Every CMake file in a tree that is not a git checkout. See `_committed_cmake`.

    THE ONLY CALLER IS THE SANDBOX, and that is what makes a walk equivalent to `git ls-files` here
    rather than a weaker substitute: `falsify.Sandbox.materialise` writes a tar of exactly the files
    git lists, so everything present IS tracked and nothing else is. What the walk still has to
    refuse is a build tree, which a sandbox does not have and a developer running this by hand in an
    exported copy might.
    """
    found: list[Path] = []
    for path in REPO_ROOT.rglob("*"):
        if path.is_dir():
            continue
        if path.name != "CMakeLists.txt" and path.suffix != ".cmake":
            continue
        parts = set(path.relative_to(REPO_ROOT).parts)
        if parts & _NOT_SOURCE or (path.parent / "CMakeCache.txt").is_file():
            continue
        found.append(path)
    return sorted(found)


def _committed_cmake() -> list[Path]:
    """Every CMake file git TRACKS. Asked of git rather than of the filesystem, for two reasons: a
    build directory carries thousands of generated CMakeLists.txt that a walk would read, and an
    untracked one is not evidence anybody else's checkout has.

    AND ASKED OF THE FILESYSTEM WHERE THERE IS NO GIT TO ASK, which is not a softening of that rule
    but the only place it does not apply. `falsify.Sandbox` materialises a tar of the tracked tree
    and is deliberately NOT a checkout, so this raised `git could not list this tree's CMake files`
    there — and five criteria across four ledgers run this module, every one of them reported "red
    in the sandbox and GREEN in the repository", which is `not provable here`: the prover could not
    judge the checks that ask whether a capability row is at Complete grade. The fallback is taken
    ONLY when there is no repository at all; a git that is present and fails still raises, because
    that is a broken checkout rather than a copy of one, and silently walking it would read a build
    tree's generated CMake as though somebody had committed it.
    """
    if not (REPO_ROOT / ".git").exists():
        return _walked_cmake()
    listed = subprocess.run(["git", "ls-files", "-z", "*CMakeLists.txt", "*.cmake"], cwd=REPO_ROOT,
                            capture_output=True, text=True, check=False)
    if listed.returncode != 0:
        raise CoverageError(f"git could not list this tree's CMake files: {listed.stderr.strip()}")
    return [REPO_ROOT / name for name in listed.stdout.split("\0") if name]


def declared_tests() -> dict[str, Path]:
    """`<kind>.<name>` -> the directory of the CMakeLists.txt that declares it.

    The directory is what makes a `case` checkable. `cy_add_test`'s SOURCES are written through
    CMake variables (`"${tests_dir}/test_editor_play.cpp"`), so resolving them would mean evaluating
    CMake; the directory holding the declaration is exact, needs no evaluation, and is enough to
    refuse an entry that names a case belonging to a different suite's module.
    """
    found: dict[str, Path] = {}
    for path in _committed_cmake():
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        for name, kind in _CY_ADD_TEST.findall(text):
            found[f"{kind}.{name}"] = path.parent
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


def proven_criteria() -> set[str]:
    """`<ledger>:<id>` for every criterion `falsifiability.toml` records a PROOF for.

    A verdict of "red in the tree" or "red against a built tree" is a criterion that is currently
    failing, not one that has been shown able to fail on purpose; only a verdict beginning "proven"
    counts. The file is generated by `just roadmap-falsify --record` and re-earned by
    `just roadmap-test`, so this reads a record something else keeps honest.
    """
    if not FALSIFIABILITY.is_file():
        raise CoverageError(f"no falsifiability record at {FALSIFIABILITY}; a `criterion:` entry "
                            "cannot be judged without it")
    loaded = tomllib.loads(FALSIFIABILITY.read_text(encoding="utf-8"))
    proofs = loaded.get("proof", [])
    if not proofs:
        raise CoverageError(f"{FALSIFIABILITY.name} records no proofs at all. It is not the file "
                            "this module reads.")
    return {f"{proof.get('ledger', '')}:{proof.get('criterion', '')}" for proof in proofs
            if str(proof.get("verdict", "")).startswith("proven")}


def _tracked_sources(directory: Path, suffixes: tuple[str, ...]) -> list[Path]:
    """Every file git tracks under `directory` with one of these suffixes.

    Walked instead where there is no repository to ask, for the reason `_committed_cmake` gives:
    the prover's sandbox is a tar of the tracked tree rather than a checkout of it.
    """
    if not (REPO_ROOT / ".git").exists():
        return sorted(path for path in directory.rglob("*")
                      if path.is_file() and path.name.endswith(suffixes)
                      and not set(path.relative_to(REPO_ROOT).parts) & _NOT_SOURCE)
    listed = subprocess.run(["git", "ls-files", "-z", "--", str(directory)], cwd=REPO_ROOT,
                            capture_output=True, text=True, check=False)
    if listed.returncode != 0:
        raise CoverageError(f"git could not list {directory}: {listed.stderr.strip()}")
    return [REPO_ROOT / name for name in listed.stdout.split("\0")
            if name and name.endswith(suffixes)]


#: A C++ adjacent-string-literal join: a closing quote, whitespace (a line break included), an
#: opening quote. The compiler concatenates those into one string and so does this, because the
#: name of a test case is what the PROGRAM sees and not how the source happens to be wrapped.
_ADJACENT_LITERALS = re.compile(r'"\s*"')


def _contains(paths: list[Path], needle: str) -> bool:
    """Is this case name in one of these sources — as written, or as the compiler reads it?

    THE SECOND ATTEMPT IS NOT A LOOSENING AND IT WAS ADDED OVER TWO ENTRIES THAT WERE WRONGLY RED.
    A `CY_TEST_CASE("...")` whose name is longer than the line limit is wrapped by clang-format into
    adjacent string literals, and a plain substring search over the bytes then cannot find the name
    the case actually has. `vfx-system` / *Data interfaces* and *VFX does not influence gameplay
    state* were both reported "renamed, deleted, or it belongs to another suite" while the cases sat
    in `test_vfx_compiler.cpp:509` and `test_firewall.cpp:448` under exactly those names — measured,
    not assumed: `git log -S` over the unwrapped spelling returns nothing for either file, so neither
    entry had ever resolved and no run had ever said so, because no criterion runs
    `quality-requirements` over those two rows.

    What the join does is what the compiler does and nothing more: it removes a quote-whitespace-
    quote boundary. `{"foo", "bar"}` is untouched, because a comma is not whitespace — so this cannot
    fuse two unrelated strings into a name nobody wrote. An entry naming a case that is not in these
    sources is still red, which is the claim the whole `case` key exists to make.
    """
    for path in paths:
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        if needle in text or needle in _ADJACENT_LITERALS.sub("", text):
            return True
    return False


@dataclass
class Entry:
    row: str
    requirement: str
    evidence: str
    note: str
    case: str = ""

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
        unknown = set(table) - {"row", "requirement", "evidence", "note", "case"}
        if unknown:
            raise CoverageError(f"{path.name} entry {index}: unknown key(s) {', '.join(sorted(unknown))}")
        entry = Entry(row=str(table.get("row", "")), requirement=str(table.get("requirement", "")),
                      evidence=str(table.get("evidence", "")), note=str(table.get("note", "")),
                      case=str(table.get("case", "")))
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

    tests: dict = field(default_factory=declared_tests)
    gates: set[str] = field(default_factory=declared_gates)
    criteria: set[str] = field(default_factory=declared_criteria)
    proven: set[str] = field(default_factory=proven_criteria)

    def unresolved(self, entry: Entry) -> str:
        """Why this entry's evidence does not answer this requirement in this tree, or ``""``."""
        resolve = {"test": self._test, "rust": self._rust, "gate": self._gate,
                   "criterion": self._criterion, "exempt": self._exempt}
        return resolve[entry.kind](entry)

    def _test(self, entry: Entry) -> str:
        """A suite, AND the case in it that answers this requirement.

        The case is what stops one suite from answering twenty-four requirements: it is searched for
        in the tracked sources beside the `cy_add_test` that declares the suite, so an entry cannot
        borrow a case from somewhere else, and renaming the case turns the entry red.
        """
        directory = self.tests.get(entry.subject)
        if directory is None:
            return (f"no suite `{entry.subject}` is declared by any committed cy_add_test() — "
                    "renamed, deleted, or never written")
        if not entry.case.strip():
            return ("a `test:` entry needs a `case` naming the test case that answers this "
                    "requirement; a suite alone is satisfied by any suite")
        sources = _tracked_sources(directory, (".cpp", ".h", ".hpp", ".cc"))
        if not sources:
            return f"{directory.relative_to(REPO_ROOT)} holds no tracked sources to find a case in"
        if not _contains(sources, entry.case):
            return (f"no case named {entry.case!r} in "
                    f"{directory.relative_to(REPO_ROOT)} — renamed, deleted, or it belongs to "
                    "another suite")
        return ""

    def _rust(self, entry: Entry) -> str:
        """A `#[test]` of an editor crate, by crate and full path.

        The same claim as `_test` in the workspace where most of the editor's evidence lives: the
        function has to be declared in THAT crate, not merely named somewhere.
        """
        crate, _, path = entry.subject.partition("::")
        directory = EDITOR_CRATES / crate
        if not directory.is_dir():
            return f"no crate `{crate}` under editor/crates/"
        function = path.rsplit("::", 1)[-1]
        if not function:
            return ("a `rust:` entry is `<crate>::<module path>::<test function>`; this one names "
                    "no function")
        sources = _tracked_sources(directory, (".rs",))
        if not sources:
            return f"editor/crates/{crate} holds no tracked Rust sources"
        if not _contains(sources, f"fn {function}("):
            return f"no `fn {function}(` in editor/crates/{crate} — renamed, deleted, or moved"
        return ""

    def _gate(self, entry: Entry) -> str:
        return "" if entry.subject in self.gates else f"no gate `{entry.subject}` in gates.toml"

    def _criterion(self, entry: Entry) -> str:
        """A criterion, AND a proof that it can fail.

        `falsifiability.toml` is the ladder's record of which criteria have been broken on purpose
        and watched go red. An entry naming one that has not been is a requirement answered by a
        check nobody knows works, which is the shape this whole mechanism exists to refuse.
        """
        if entry.subject not in self.criteria:
            return f"no criterion `{entry.subject}` in tools/roadmap/milestones/"
        if entry.subject not in self.proven:
            return (f"`{entry.subject}` has never been shown able to fail — falsifiability.toml "
                    "records no proof for it, so it cannot answer a requirement. Prove it with "
                    "`just roadmap-falsify --record`, or answer this requirement with a case")
        return ""

    def _exempt(self, entry: Entry) -> str:
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
