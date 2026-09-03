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
cannot be closed, an override with a missing field or a past expiry.

Run directly, or through `just roadmap-test`.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import criteria as criteria_module  # noqa: E402
import gates as gates_module  # noqa: E402
import record as record_module  # noqa: E402

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
        test_gates(_area(root, "gates"))
    passed = len(_cases) - len(_failures)
    print(f"\nselftest: {passed}/{len(_cases)} passed")
    return 1 if _failures else 0


if __name__ == "__main__":
    sys.exit(main())
