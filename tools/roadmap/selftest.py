#!/usr/bin/env python3
"""Tests for the roadmap tooling: drift detection, the criteria loader, and the override rules.

Task 4.3.2 requires `roadmap-status` to fail when the record and `openspec/specs/` disagree. That
requirement is only worth something if the failure still happens, so the three ways a record can
drift are exercised here rather than described:

  a capability gains a specification and no entry     — a capability was added
  an entry is removed while the specification stays   — an entry was deleted
  an entry names a capability that has no spec        — a capability was renamed

Each case runs against a temporary copy of the record and a temporary specification tree, so the
tests never touch docs/roadmap/status.yaml or openspec/specs/ — a test that edits the repository to
prove a point is a test that fails halfway through and leaves it edited.

The rest cover the data files these gates read: a criterion that names no CI job, a milestone that
cannot be closed, an override with a missing field or a past expiry — and the ladder itself: every
milestone's ledger loads, every criterion in it names a gate that exists, and every milestone with a
ledger has a gate its criteria join on close. That last group is here rather than only inside
`just roadmap-milestone <id>` because `just roadmap-test` runs on every pull request and the
milestone recipes take a working session each: a ledger that no longer loads should fail in minutes,
not the next time somebody tries to close a milestone.

Run directly, or through `just roadmap-test`.
"""

from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import criteria as criteria_module  # noqa: E402
import gates as gates_module  # noqa: E402
import record as record_module  # noqa: E402
import roadmap as roadmap_module  # noqa: E402

HERE = Path(__file__).resolve().parent
ROADMAP = HERE / "roadmap.py"
GATE_EXIT, DATA_EXIT = 1, 2

_cases: list[str] = []
_failures: list[str] = []


def check(name: str, condition: bool, detail: str = "") -> None:
    print(f"{'ok  ' if condition else 'FAIL'} {name}")
    _cases.append(name)
    if not condition:
        _failures.append(name)
        for line in detail.strip().splitlines():
            print(f"     | {line}", file=sys.stderr)


# --- Drift ----------------------------------------------------------------------------------------


def specs_tree(root: Path, capabilities) -> Path:
    """A specification directory holding the named capabilities, and nothing else."""
    specs = root / "specs"
    for capability in capabilities:
        (specs / capability).mkdir(parents=True)
        (specs / capability / "spec.md").write_text(f"# {capability}\n", encoding="utf-8")
    return specs


def status(record: Path, specs: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(ROADMAP), "status", "--record", str(record), "--specs", str(specs)],
        capture_output=True, text=True, check=False,
    )


def drop_entry(text: str, capability: str) -> str:
    """Remove one capability's block from the record, as deleting an entry by hand would."""
    lines, kept, dropping = text.splitlines(keepends=True), [], False
    for line in lines:
        if line.startswith(f"  {capability}:"):
            dropping = True
            continue
        if dropping and line.startswith("    "):
            continue
        dropping = False
        kept.append(line)
    return "".join(kept)


def test_drift(root: Path) -> None:
    record = record_module.DEFAULT_RECORD
    capabilities = record_module.specified()
    intact = specs_tree(root / "intact", capabilities)

    result = status(record, intact)
    check("the record and the specification set agree", result.returncode == 0,
          result.stdout + result.stderr)

    added = specs_tree(root / "added", (*capabilities, "fake-capability"))
    result = status(record, added)
    check("a capability with a specification and no entry fails, naming it",
          result.returncode == GATE_EXIT and "fake-capability" in result.stderr,
          f"exit {result.returncode}\n{result.stderr}")

    shortened = root / "shortened.yaml"
    shortened.write_text(drop_entry(record.read_text(encoding="utf-8"), "core-math"),
                         encoding="utf-8")
    result = status(shortened, intact)
    check("an entry removed from the record fails, naming the capability",
          result.returncode == GATE_EXIT and "core-math" in result.stderr,
          f"exit {result.returncode}\n{result.stderr}")

    renamed = root / "renamed.yaml"
    renamed.write_text(record.read_text(encoding="utf-8").replace("  core-math:", "  core-maths:"),
                       encoding="utf-8")
    result = status(renamed, intact)
    check("a renamed entry fails as both an unrecorded capability and an unspecified entry",
          result.returncode == GATE_EXIT
          and "core-math" in result.stderr and "core-maths" in result.stderr,
          f"exit {result.returncode}\n{result.stderr}")

    result = status(record, intact)
    check("the restored record passes again", result.returncode == 0, result.stdout + result.stderr)


# --- The record's own rules -----------------------------------------------------------------------


def expect_error(name: str, error_type, action) -> None:
    try:
        action()
    except error_type as error:
        check(name, True)
        print(f"     {error}")
        return
    except Exception as error:  # noqa: BLE001 — the wrong error type is as much a failure as none
        check(name, False, f"raised {type(error).__name__}: {error}")
        return
    check(name, False, "no error was raised")


def write(path: Path, text: str) -> Path:
    path.write_text(text, encoding="utf-8")
    return path


def test_record_rules(root: Path) -> None:
    header = "schema: 1\ncapabilities:\n"
    expect_error(
        "a tier that names no change is rejected", record_module.RecordError,
        lambda: record_module.load(write(root / "untraceable.yaml",
                                         header + "  a:\n    tier: seed\n    milestone: m0\n"
                                         "    change: null\n")))
    expect_error(
        "an unknown tier is rejected", record_module.RecordError,
        lambda: record_module.load(write(root / "tier.yaml",
                                         header + "  a:\n    tier: nearly\n    milestone: null\n"
                                         "    change: null\n")))
    expect_error(
        "a milestone that is not on the ladder is rejected", record_module.RecordError,
        lambda: record_module.load(write(root / "ladder.yaml",
                                         header + "  a:\n    tier: seed\n    milestone: m99\n"
                                         "    change: c\n")))
    expect_error(
        "a malformed line is rejected with its line number", record_module.RecordError,
        lambda: record_module.load(write(root / "malformed.yaml", header + "  a\n")))

    entries = record_module.load(write(root / "valid.yaml",
                                       header + "  a:\n    tier: working\n    milestone: M0\n"
                                       "    change: implement-m0-ground\n"))
    check("a well-formed entry loads with its milestone and change",
          entries[0].tier == "working" and entries[0].change == "implement-m0-ground")


# --- The criteria and the gate set ------------------------------------------------------------------


def milestone_file(root: Path, name: str, body: str) -> Path:
    directory = root / name
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "m0.toml").write_text(body, encoding="utf-8")
    return directory


def test_criteria(root: Path) -> None:
    milestone = criteria_module.load("m0")
    check("M0's criteria load, and every one names a source and a CI job",
          all(criterion.source and criterion.ci_job for criterion in milestone.criteria))
    check("M0 has a criterion for each of the milestone's exit conditions",
          len(milestone.criteria) >= 10, f"{len(milestone.criteria)} criteria")
    check("a criterion that this host cannot evaluate carries a reason",
          all(criterion.reason for criterion in milestone.criteria
              if criterion.where == "ci" or criterion.requires))

    head = 'schema = 1\nid = "m0"\nname = "Ground"\n'
    expect_error(
        "a criterion that names no CI job is rejected", criteria_module.CriteriaError,
        lambda: criteria_module.load("m0", milestone_file(
            root, "no-job",
            head + '[[criterion]]\nid = "x"\ndescribe = "d"\nsource = "s"\nkind = "recipe"\n'
            'run = "just quality-layers"\n')))
    expect_error(
        "a CI-only criterion with no reason is rejected", criteria_module.CriteriaError,
        lambda: criteria_module.load("m0", milestone_file(
            root, "no-reason",
            head + '[[criterion]]\nid = "x"\ndescribe = "d"\nsource = "s"\nkind = "recipe"\n'
            'run = "just quality-layers"\nci_job = "layering"\nwhere = "ci"\n')))
    expect_error(
        "a milestone with no criteria cannot be closed", criteria_module.CriteriaError,
        lambda: criteria_module.load("m0", milestone_file(root, "empty", head)))
    expect_error(
        "an unknown milestone is rejected, naming the ones that exist", criteria_module.CriteriaError,
        lambda: criteria_module.load("m99"))


def test_exit_tiers(root: Path) -> None:
    """An exit tier is a floor, so a closed milestone stays closed when a later one advances past it.

    REGRESSION. `_check_tiers` compared for equality, so M1 raising `project-and-plugins` from Seed
    to Working made `just roadmap-milestone m0` fail on a criterion M0 had satisfied. `milestone-m0`
    is a permanent merge gate, so that turned an advance into a red build with no correct fix.
    """
    del root
    criterion = criteria_module.Criterion(
        id="tiers", describe="exit tiers", source="selftest", kind="tiers", ci_job="milestone-m0",
        expect_tiers={"alpha": "seed"})

    def entry(tier: str) -> tuple[record_module.Entry, ...]:
        return (record_module.Entry(capability="alpha", tier=tier, milestone="M0", change="c"),)

    at, _, _ = criteria_module._check_tiers(criterion, entry("seed"))
    check("a capability exactly at the exit tier passes", at == criteria_module.OK)
    above, _, _ = criteria_module._check_tiers(criterion, entry("working"))
    check("a capability a later milestone advanced past the exit tier still passes",
          above == criteria_module.OK)
    complete, _, _ = criteria_module._check_tiers(criterion, entry("complete"))
    check("a completed capability still satisfies an earlier milestone's exit tier",
          complete == criteria_module.OK)
    below, _, detail = criteria_module._check_tiers(criterion, entry("none"))
    check("a capability below the exit tier fails, naming both tiers",
          below == criteria_module.FAILED and "none" in detail and "seed" in detail, detail)
    missing, _, detail = criteria_module._check_tiers(criterion, ())
    check("a capability missing from the record fails",
          missing == criteria_module.FAILED and "not in the record" in detail, detail)


# --- The milestone ladder -------------------------------------------------------------------------


def test_milestone_ladder(root: Path) -> None:
    """Every ledger under milestones/ loads, is gated, and has a gate of its own.

    `roadmap.py` already refuses to run a milestone whose criterion names a CI job that is not a
    declared gate. Calling that same function here rather than restating the rule means the two
    cannot disagree, and it moves the failure from "the day someone closes a milestone" to "every
    pull request", which is the only place a data file that has stopped loading is cheap to fix.
    """
    milestones = criteria_module.available()
    check("every milestone on the ladder so far has a ledger",
          {"m0", "m1", "m2", "m3"} <= set(milestones), f"found: {', '.join(milestones) or 'none'}")

    gate_set = gates_module.load()
    milestone_gates = {gate.milestone for gate in gate_set.gates if gate.klass == "milestone"}
    for identifier in milestones:
        milestone = criteria_module.load(identifier)
        check(f"{identifier.upper()}'s criteria load, and every one names a source and a CI job",
              all(criterion.source and criterion.ci_job for criterion in milestone.criteria))
        check(f"{identifier.upper()}: every criterion names a gate that is declared",
              _gated(milestone), f"{identifier}.toml names a CI job that is not in gates.toml")
        check(f"{identifier.upper()}'s criteria have a gate to join on close",
              identifier in milestone_gates)
        check(f"{identifier.upper()}: a criterion this host cannot evaluate carries a reason",
              all(criterion.reason for criterion in milestone.criteria
                  if criterion.where == "ci" or criterion.requires))

    # THE OMISSION THAT HAPPENED THREE TIMES, AS A CHECK. M0's gate was still `joins-on-close`
    # after M0 closed, M1's after M1 closed, and M2's after M2 closed — each caught by the next
    # milestone's author noticing, which is not a mechanism. gates.toml records the pattern and
    # says what would work: the ARCHIVE PATH is the fact a tool can read. A change under
    # openspec/changes/archive/ whose directory names the milestone means that milestone closed,
    # and a closed milestone's gate that is still waiting to join is a ledger nothing runs.
    #
    # It does not fire for the milestone being closed right now, whose change is archived after
    # its own recipe passes — which is correct: that gate is flipped by the change that closes it,
    # and this check is what catches the flip being forgotten one milestone later.
    # record_module.REPO_ROOT, not this function's `root`: `root` is a scratch directory the
    # fixtures are written into, and the archive being read here is the repository's own.
    archive = record_module.REPO_ROOT / "openspec" / "changes" / "archive"
    archived = {
        identifier
        for identifier in milestones
        if any(directory.is_dir() and identifier in directory.name.split("-")
               for directory in (archive.iterdir() if archive.is_dir() else ()))
    }
    states = {gate.milestone: gate.state for gate in gate_set.gates if gate.klass == "milestone"}
    for identifier in sorted(archived):
        check(f"{identifier.upper()} is archived, so its gate is green rather than joins-on-close",
              states.get(identifier) == "green",
              f"gates.toml records milestone-{identifier} as "
              f"'{states.get(identifier, '(no gate)')}'")

    m1 = criteria_module.load("m1")
    check("M1 has a criterion for each of the milestone's exit conditions",
          len(m1.criteria) >= 15, f"{len(m1.criteria)} criteria")
    m2 = criteria_module.load("m2")
    check("M2 has a criterion for each of the milestone's exit conditions",
          len(m2.criteria) >= 20, f"{len(m2.criteria)} criteria")
    m3 = criteria_module.load("m3")
    check("M3 has a criterion for each of the milestone's exit conditions",
          len(m3.criteria) >= 20, f"{len(m3.criteria)} criteria")
    # M1 broke M0's static analysis gate before this ledger existed. `delivery-roadmap` forbids that
    # outright, so the rule is a criterion rather than a paragraph, and this is the check that it
    # stays one. Every ledger from M1 on carries the previous milestone's recipe, so the whole ladder
    # runs from whichever rung is being closed — checked here rather than left to the next author to
    # notice, because the omission is invisible until the day it matters.
    # DERIVED FROM THE LEDGERS RATHER THAN LISTED. The pairs used to be written out here, which
    # made this check itself something the next author had to remember to extend — the same class of
    # omission it exists to catch. `available()` is sorted, so zipping it against its own tail is
    # every rung, and a ledger added under milestones/ is checked the moment it is added.
    rungs = criteria_module.available()
    for later, earlier in zip(rungs[1:], rungs[:-1]):
        ledger = criteria_module.load(later)
        check(f"{later.upper()}'s ledger runs {earlier.upper()}'s, so a milestone that breaks an "
              f"earlier one cannot close",
              any(criterion.run == f"just roadmap-milestone {earlier}"
                  for criterion in ledger.criteria))


def test_requirements(root: Path) -> None:
    """A criterion this host cannot evaluate is reported as unevaluated, never as passed.

    M3 is the first milestone with criteria that need a graphics device, so `gpu` joins `display` as
    a requirement a host can fail to meet. The failure mode this guards against is the expensive one:
    a milestone recipe that silently skipped its rendering criteria would report M3 green on exactly
    the machines least able to judge it.
    """
    criterion = criteria_module.Criterion(
        id="needs-a-gpu", describe="a criterion that needs a device", source="selftest",
        kind="recipe", run="just test-render", ci_job="render-device", requires="gpu",
        reason="this host has no graphics device")
    previous = os.environ.get("CY_HAS_GPU")
    try:
        os.environ["CY_HAS_GPU"] = "0"
        check("a criterion that needs a GPU is not evaluated on a host without one",
              criteria_module.unmet_requirement(criterion) == criterion.reason)
        os.environ["CY_HAS_GPU"] = "1"
        check("and is evaluated on a host with one",
              criteria_module.unmet_requirement(criterion) == "")
    finally:
        if previous is None:
            del os.environ["CY_HAS_GPU"]
        else:
            os.environ["CY_HAS_GPU"] = previous

    check("'gpu' and 'display' are the requirements a criterion may declare",
          set(criteria_module.REQUIREMENTS) == {"display", "gpu"})
    expect_error(
        "a requirement this tool does not know is rejected", criteria_module.CriteriaError,
        lambda: criteria_module.load("m0", milestone_file(
            root, "unknown-requirement",
            'schema = 1\nid = "m0"\n[[criterion]]\nid = "c"\ndescribe = "d"\nsource = "s"\n'
            'kind = "recipe"\nrun = "just x"\nci_job = "build-and-test"\n'
            'requires = "quantum-computer"\nreason = "r"\n')))


def _gated(milestone: criteria_module.Milestone) -> bool:
    try:
        roadmap_module._check_criteria_are_gated(milestone)
    except criteria_module.CriteriaError:
        return False
    return True


def test_gates(root: Path) -> None:
    gate_set = gates_module.load()
    ids = {gate.id for gate in gate_set.gates}
    required = {"build-and-test", "format", "lint", "layering", "generated-code", "specs",
                "roadmap-status"}
    check("the permanent gate set covers what testing-and-quality requires",
          required <= ids, f"missing: {sorted(required - ids)}")
    check("M0's criteria join the gate set on close",
          any(gate.klass == "milestone" and gate.milestone == "m0" for gate in gate_set.gates))

    with gates_module.GATES.open("rb") as handle:
        document = tomllib.load(handle)
    check("no override is recorded today", not document.get("override"))

    body = gates_module.GATES.read_text(encoding="utf-8")
    expect_error(
        "an override missing a field is rejected", gates_module.GateError,
        lambda: gates_module.load(write(root / "partial.toml", body + '\n[[override]]\n'
                                        'gate = "lint"\nreason = "r"\napproved_by = "a"\n'
                                        'change = "c"\nexpires = ""\n')))
    expect_error(
        "an expired override is rejected", gates_module.GateError,
        lambda: gates_module.load(write(root / "expired.toml", body + '\n[[override]]\n'
                                        'gate = "lint"\nreason = "r"\napproved_by = "a"\n'
                                        'change = "c"\nexpires = "2020-01-01"\n')))
    expect_error(
        "an override of a gate that does not exist is rejected", gates_module.GateError,
        lambda: gates_module.load(write(root / "unknown.toml", body + '\n[[override]]\n'
                                        'gate = "vibes"\nreason = "r"\napproved_by = "a"\n'
                                        'change = "c"\nexpires = "2099-01-01"\n')))


def _area(root: Path, name: str) -> Path:
    """A scratch directory per group of tests, so a failure names which one wrote what."""
    area = root / name
    area.mkdir(parents=True, exist_ok=True)
    return area


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="cy-roadmap-selftest-") as directory:
        root = Path(directory)
        test_drift(_area(root, "drift"))
        test_record_rules(_area(root, "record"))
        test_criteria(_area(root, "criteria"))
        test_exit_tiers(_area(root, "tiers"))
        test_milestone_ladder(_area(root, "ladder"))
        test_requirements(_area(root, "requirements"))
        test_gates(_area(root, "gates"))
    passed = len(_cases) - len(_failures)
    print(f"\nselftest: {passed}/{len(_cases)} passed")
    return 1 if _failures else 0


if __name__ == "__main__":
    sys.exit(main())
