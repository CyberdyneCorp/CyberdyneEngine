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

`test_flat_ledger` covers the property that replaced ledger chaining, and it is two halves that are
each silent when lost: every distinct criterion runs ONCE however many milestones declared it, and
an earlier milestone's criteria are still IN the newest ledger so a regression against M0 still
fails it. Chaining gave the second for free and paid for it four times over in the first.

`test_ladder_rungs` covers what that inheritance is computed FROM: the rung a milestone occupies.
M5.5 was inserted between M5 and M6 rather than appended, and while its identifier was missing from
`record.MILESTONES` it ranked at the end of the ladder — which would have made M6's ledger drop
every criterion M5.5 closed with, quietly, the moment M6 existed.

Run directly, or through `just roadmap-test`.
"""

from __future__ import annotations

import contextlib
import io
import itertools
import os
import re
import subprocess
import sys
import tempfile
import time
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import criteria as criteria_module  # noqa: E402
import debts as debts_module  # noqa: E402
import falsify as falsify_module  # noqa: E402
import gates as gates_module  # noqa: E402
import plan as plan_module  # noqa: E402
import record as record_module  # noqa: E402
import requirements as requirements_module  # noqa: E402
import roadmap as roadmap_module  # noqa: E402
import schedule as schedule_module  # noqa: E402

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

# The fewest criteria each milestone's ledger may carry, from its row in docs/ROADMAP.md and its
# section 6. A floor rather than an equality: a ledger that grows a criterion is a ledger that got
# better, and one that loses several has quietly stopped covering its milestone. `test_criteria`
# requires every ledger under milestones/ to appear here, so this table cannot fall behind them.
# M10's floor was 2 while the spike owned the file — the two questions this host cannot ask — and is
# raised by task 8.1 to the eleven rows the milestone moves, the substrate every one of them writes
# into, the artefact, the record and the next rung. It carries 70; the floor is what may not be lost.
# M11.d's floor stayed at 25 when its spike moved Metal, D3D12 and the three-backend image
# comparison out to M11.d.5: it declared 30 and declares 27, which is still above the floor, and a
# floor is what may not be LOST rather than a count of what is there. M11.d.5's own floor of 16 is
# the deliberate answer to "how many exit conditions does this rung have" that adding a ledger is
# supposed to force — nine static gates it shares with the ladder, the three moved claims, the three
# a Linux host can still judge, the artefact, the tier and the handover.
# M12 AND M13 ARE DECLARED RUNGS WHOSE CRITERIA ARE NOT YET AUTHORED, and their floors say 1 because
# that is honestly what may not be lost today — not because 1 is a sensible number of exit conditions
# for a rung. M13 exists so that three deferrals have a rung to name instead of a date, and its one
# criterion checks precisely that: that m13 is present in the ladder, the gates, the ledgers and the
# changes, all four. M12 exists to build the consumer that proves the engine, and its one criterion
# is the defect register. THE FLOOR RISES WHEN EITHER RUNG IS WORKED — a rung reaching its gate with
# one criterion would be the token gesture this table exists to refuse, and whoever opens M12 or M13
# raises the number here in the same change that writes the criteria.
MINIMUM_CRITERIA = {"m0": 10, "m1": 15, "m2": 20, "m3": 20, "m4": 20, "m5": 20, "m5b": 20,
                    "m6": 26, "m7": 32, "m8a": 26, "m8b": 40, "m8c": 40, "m9": 44, "m10": 62,
                    "m11a": 27, "m11b": 28, "m11c": 25, "m11d": 25, "m11d5": 16, "m11e": 25,
                    "m12": 1, "m13": 1}


def milestone_file(root: Path, name: str, body: str) -> Path:
    directory = root / name
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "m0.toml").write_text(body, encoding="utf-8")
    return directory


def test_criteria(root: Path) -> None:
    milestone = criteria_module.load("m0")
    check("M0's criteria load, and every one names a source and a CI job",
          all(criterion.source and criterion.ci_job for criterion in milestone.criteria))
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


def test_evaluates(root: Path) -> None:
    """`evaluates` is a declaration, and `evaluators` is the only way to ask who declared it.

    THE DEFECT IT REPLACES, in one line from M11's gate: the guard over the four rows whose Working
    tier nothing evaluated "cannot detect the deletion of two of the four evaluators". It asked
    whether the row's name appeared in a criterion's `source`, and `source` is a CITATION — eight
    criteria in M11.a's plan cite `testing-and-quality` because that specification governs them — so
    deleting the criterion that did the evaluating left seven bystanders answering for it.
    """
    head = 'schema = 1\nid = "m0"\nname = "Ground"\n'
    body = ('[[criterion]]\nid = "x"\ndescribe = "d"\nsource = "s"\nkind = "recipe"\n'
            'run = "just quality-layers"\nci_job = "layering"\n')
    loaded = criteria_module.load("m0", milestone_file(
        root, "evaluates", head + body + 'evaluates = ["testing-and-quality"]\n'))
    check("a criterion declares the capability rows it evaluates",
          loaded.criteria[0].evaluates == ["testing-and-quality"])
    for name, value in (("not-a-list", '"testing-and-quality"'), ("empty", "[]"),
                        ("not-a-string", "[3]"), ("twice", '["a", "a"]')):
        expect_error(
            f"'evaluates' as {name} is rejected", criteria_module.CriteriaError,
            lambda value=value, name=name: criteria_module.load("m0", milestone_file(
                root, f"evaluates-{name}", head + body + f"evaluates = {value}\n")))

    declares = criteria_module.PlanEntry(loaded.criteria[0], ("m0",), False)
    cites = criteria_module.PlanEntry(
        criteria_module.Criterion(id="y", describe="d", source="testing-and-quality", kind="command",
                                  ci_job="layering", run="true"), ("m0",), False)
    entries = (declares, cites)
    check("`evaluators` answers with the criterion that DECLARED the row",
          [entry.criterion.id for entry in criteria_module.evaluators(entries, "testing-and-quality")]
          == ["x"])
    check("AND NOT with the one that merely cites the specification, which is the refuted check",
          not criteria_module.evaluators((cites,), "testing-and-quality"))


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

    # A LEDGER IS NOT A TOKEN GESTURE, and the floor is per milestone because the number of exit
    # conditions is: it is the count of the milestone's row in docs/ROADMAP.md plus its section 6.
    # The floors used to be written out one `check` at a time, which made this itself something the
    # next author had to remember to extend — M4's ledger landed and was covered by nothing. So the
    # numbers are data and the LAST check below is the one that matters: a ledger under milestones/
    # with no floor recorded here FAILS, rather than being quietly unchecked. Adding a milestone
    # therefore forces a deliberate answer to "how many exit conditions does it have", which is the
    # question the floor is asking.
    for identifier, floor in sorted(MINIMUM_CRITERIA.items()):
        ledger = criteria_module.load(identifier)
        check(f"{identifier.upper()} has a criterion for each of the milestone's exit conditions",
              len(ledger.criteria) >= floor, f"{len(ledger.criteria)} criteria, floor {floor}")
    check("every ledger under milestones/ has a floor recorded, so a new one is not unchecked",
          set(criteria_module.available()) == set(MINIMUM_CRITERIA),
          f"no floor for: {sorted(set(criteria_module.available()) - set(MINIMUM_CRITERIA))}")


# --- The flat ledger ------------------------------------------------------------------------------


def test_flat_ledger(root: Path) -> None:
    """A ledger evaluates the permanent set once, plus its own criteria, and invokes no other one.

    REGRESSION, and the reason this group exists. Each ledger used to open with a criterion running
    the previous milestone's recipe, so closing M4 ran M3's ledger, which ran M2's, which ran M1's,
    which ran M0's. One run of M4's ledger was 118 criterion evaluations over 91 distinct checks —
    27 of them redundant — `four-profiles`, a full four-configuration build and test, ran four times
    because four ledgers declared it, and every criterion's failure probability was multiplied by
    the number of ledgers naming it. A unit case sitting on its time budget duly failed four
    ledgers at once. The same run is now 87 evaluations, one per distinct check.

    Two properties have to hold together, and losing either is silent. Deduplication: each distinct
    check appears once however many milestones declared it. The ladder: an earlier milestone's
    criteria are still IN the newest ledger, so a regression against M0 still fails it. Chaining
    provided the second by construction; nothing but these checks provides it now.
    """
    del root
    milestones = criteria_module.ladder_order(criteria_module.available())
    newest = milestones[-1]
    permanent = gates_module.permanent_milestones(gates_module.load())

    for identifier in milestones:
        ledger = criteria_module.load(identifier)
        chained = [criterion.id for criterion in ledger.criteria
                   if criterion.run.startswith("just roadmap-milestone")]
        check(f"{identifier.upper()}'s ledger invokes no other milestone's ledger",
              not chained, f"{identifier}.toml chains through: {', '.join(chained)}")

    # A gate flipped green for a milestone with no ledger would shrink the permanent set silently:
    # `build_plan` would fail to load it, and the newest ledger would stop evaluating that rung.
    orphans = [identifier for identifier in permanent if identifier not in milestones]
    check("every milestone whose gate is green has a ledger for the permanent set to inherit",
          not orphans, f"gates.toml is green for {', '.join(orphans)}, with no milestones/*.toml")

    plan = criteria_module.build_plan(newest, permanent)
    prints = [criteria_module.fingerprint(entry.criterion) for entry in plan.entries]
    check(f"{newest.upper()}'s ledger evaluates each distinct criterion exactly once",
          len(prints) == len(set(prints)),
          f"{len(prints) - len(set(prints))} check(s) appear more than once")
    check(f"{newest.upper()}'s ledger deduplicates the declarations it merges",
          plan.deduplicated == plan.declarations - len(plan.entries) and plan.deduplicated > 0,
          f"{plan.declarations} declarations, {len(plan.entries)} entries")

    # THE LADDER, which is what the chaining was for. Every criterion of every milestone whose gate
    # is green is in the newest ledger, so breaking one of M0's still fails the newest recipe.
    merged = set(prints)
    for identifier in permanent:
        if criteria_module.rung(identifier) >= criteria_module.rung(newest):
            continue
        missing = [criterion.id for criterion in criteria_module.load(identifier).criteria
                   if criteria_module.fingerprint(criterion) not in merged]
        check(f"every criterion {identifier.upper()} closed with is in {newest.upper()}'s ledger",
              not missing, f"{identifier}.toml: {', '.join(missing)} would not be evaluated")

    # A closed milestone's ledger keeps meaning ITS OWN criteria. `milestone-m0` is a permanent
    # merge gate; widening it with everything a later milestone added would turn it red for work M0
    # never claimed, with no correct fix — the same shape as the exit-tier equality bug above.
    oldest = criteria_module.build_plan(milestones[0], permanent)
    check(f"{milestones[0].upper()}'s ledger is still only {milestones[0].upper()}'s criteria",
          len(oldest.entries) == len(criteria_module.load(milestones[0]).criteria)
          and not oldest.inherited,
          f"{len(oldest.entries)} entries, {len(oldest.inherited)} inherited")

    _check_four_profiles(plan)
    _check_failure_evidence_names_the_failure()
    _check_an_insertion_takes_a_rung()
    _check_every_reader_admits_an_insertion()
    _check_collapse_rules()


def _check_every_reader_admits_an_insertion() -> None:
    """A milestone heading with an insertion suffix parses in all THREE places that read one.

    REGRESSION, and it cost three separate silent failures in one change. `M5.5`, `M8.a` and `M8.b`
    are read by a matrix column header, a ROADMAP section heading and a load-table row label — each
    with its own regular expression, each admitting only `.5`. Splitting M8 therefore:

      * dropped both new columns from the matrix, taking every M8 cell with them, so nine
        capabilities' Complete column pointed at a milestone the matrix no longer contained;
      * read `## M8.b — Systems` as a continuation of M7, so thirty-six of M8.b's tier claims
        briefly became M7's;
      * and found no load row for either half.

    None of it was visible in the documents. All of it was visible to `check_documents_agree`, which
    is the argument for that check existing — but only after the readers could see the headings at
    all, which is what this asserts.

    AND IT CAME BACK AT M11's SPLIT, one letter further along. The three patterns had been widened to
    admit `.a`, `.b` and `.c` — the suffixes M8 needed — so `M11.d` and `M11.e` parsed as nothing,
    fourteen capabilities' Complete column pointed at columns the matrix did not contain, and the
    load table had no row for either. The lesson is the same one and the fix is not a wider pattern
    but this list: every suffix on the ladder is named here, so the next insertion fails loudly in
    one place instead of quietly in three.

    AND THE NEXT INSERTION CAME: `M11.d.5`, an insertion INTO a split, when M11.d's spike found that
    neither Metal nor D3D12 can be compiled on the Linux host this project works on. It is the first
    heading on the ladder carrying both a letter rung and a `.5`, so all three patterns had to admit
    a `.5` AFTER a letter rather than only instead of one — and this list failing loudly is exactly
    how that was discovered in one place rather than in three.
    """
    columns = ("M5.5", "M8.a", "M8.b", "M8.c", "M11.a", "M11.b", "M11.c", "M11.d", "M11.d.5",
               "M11.e")
    rungs = ("m5b", "m8a", "m8b", "m8c", "m11a", "m11b", "m11c", "m11d", "m11d5", "m11e")
    for column in columns:
        check(f"the matrix header admits {column}",
              plan_module.milestone_id(column) in record_module.MILESTONES,
              f"{column} -> {plan_module.milestone_id(column)!r}")

    matrix = plan_module.read_matrix()
    for rung in rungs:
        check(f"{rung} is a column the matrix reader returns", rung in matrix.milestones,
              f"columns are {matrix.milestones}")

    sections = plan_module.read_sections()
    for rung in rungs:
        check(f"{rung} is a section the roadmap reader returns", rung in sections,
              f"sections are {sorted(sections)}")

    summary = plan_module.read_load_summary()
    for rung in rungs:
        check(f"{rung} is a row the load table reader returns", rung in summary,
              f"rows are {sorted(summary)}")

    # THE FOURTH READER, added when M11.d.5 caught it rendering `M11D5`. `debts.milestone_label` is
    # the one that turns an identifier back into the label a person reads, and it was written the
    # same way as the three above — which is exactly the pattern that let M8's split break three
    # readers at once. It is checked here rather than in its own group so the next insertion fails in
    # the SAME place as the other three.
    for column, rung in zip(columns, rungs):
        check(f"the debts reader renders {rung} as {column}",
              debts_module.milestone_label(rung) == column,
              f"{rung} -> {debts_module.milestone_label(rung)!r}, expected {column!r}")


def _check_an_insertion_takes_a_rung() -> None:
    """An inserted milestone sorts BETWEEN its neighbours, not at one end of the ladder.

    REGRESSION, and the bug is real rather than hypothetical: `m5b` was inserted without being added
    to `record.MILESTONES`, so `criteria.rung` answered with the length of the tuple and M5.5's
    ledger sorted to the END of the ladder. That was harmless only because nothing sat above it —
    the next milestone to close would have inherited the wrong set in both directions. M6's gate had
    to repair it before M6's ledger was written, and `m8a`/`m8b` are the same insertion again.
    """
    order = record_module.MILESTONES
    for inserted, below, above in (("m5b", "m5", "m6"), ("m8a", "m7", "m8b"),
                                   ("m8b", "m8a", "m8c"), ("m8c", "m8b", "m9"),
                                   ("m11a", "m10", "m11b"), ("m11b", "m11a", "m11c"),
                                   ("m11c", "m11b", "m11d"), ("m11d", "m11c", "m11d5"),
                                   ("m11d5", "m11d", "m11e")):
        check(f"{inserted} sits between {below} and {above} on the ladder",
              inserted in order and below in order and above in order
              and order.index(below) < order.index(inserted) < order.index(above),
              f"order is {order}")

    # The property the rung exists for: a ledger inherits what is BELOW it and nothing above.
    check("a rung past the end of the ladder is not silently assigned a position",
          criteria_module.rung("m8a") < criteria_module.rung("m8b")
          < criteria_module.rung("m8c") < criteria_module.rung("m9"),
          f"m8a={criteria_module.rung('m8a')} m8b={criteria_module.rung('m8b')} "
          f"m8c={criteria_module.rung('m8c')} m9={criteria_module.rung('m9')}")

    # M11's six, the same property one split and one insertion later. `m11e` is the last rung on the
    # ladder, so `rung()` returning a position past the end for any of them would be invisible in the
    # same way `m5b`'s was — harmless until something sits above it, and then wrong in both
    # directions. `m11d5` is the one that would have been: it was inserted BELOW the last rung, so a
    # wrong position there costs M11.e the criteria it is supposed to inherit.
    order_of_six = [criteria_module.rung(name)
                    for name in ("m10", "m11a", "m11b", "m11c", "m11d", "m11d5", "m11e")]
    check("M11's six rungs sit in order between M10 and the end of the ladder",
          order_of_six == sorted(order_of_six) and len(set(order_of_six)) == 7,
          f"positions are {order_of_six}")
    check("no identifier `m11` survives the split, so a gap or a column naming it is caught",
          "m11" not in record_module.MILESTONES,
          f"record.MILESTONES: {', '.join(record_module.MILESTONES)}")


def _check_failure_evidence_names_the_failure() -> None:
    """REGRESSION, M5.5's gate: a failure whose name is not in the last lines was printed nameless.

    `just test-all` runs every suite after a failing one and prints its verdict last, so the tail of
    the output is passing summary. The gate re-ran a suite 433 times without reproducing a failure it
    had never been told the name of.
    """
    output = "\n".join(
        ["the test that failed: FAILED integration.physics_jolt"]
        + [f"filler line {index}" for index in range(200)]
    )
    lines = [line for line in output.splitlines() if line.strip()]
    named = [index for index, line in enumerate(lines) if roadmap_module._names_a_failure(line)]
    check("a line naming the failure is kept however far from the end it is",
          named == [0],
          f"kept {named}")
    check("an ordinary line is not mistaken for one that names a failure",
          not roadmap_module._names_a_failure("filler line 3"))

    # M7's gate: THE FIXTURE ABOVE IS NOT WHAT CTEST PRINTS, and that is why this check was green
    # while the thing it guards was broken. CTest writes the header and the names on separate lines:
    #
    #     The following tests FAILED:
    #             178 - smoke.fidelity (Failed)
    #
    # The header carries the marker; the NAME carries "(Failed)", mixed case, which no marker
    # matched. `m0:test` in M7's ledger run therefore printed "The following tests FAILED:" followed
    # by "... 752 line(s)" and the suite had to be read out of Testing/Temporary/LastTestsFailed.log.
    # A fixture written to match the marker list instead of the tool's output is a fixture that
    # cannot fail.
    ctest = "\n".join(
        ["The following tests FAILED:", "\t178 - smoke.fidelity (Failed)",
         "\t 12 - unit.slow (Timeout)", "\t  3 - unit.crashed (Subprocess aborted)",
         "\t  9 - smoke.absent (Not Run)"]
        + [f"filler line {index}" for index in range(200)]
    )
    lines = [line for line in ctest.splitlines() if line.strip()]
    named = [index for index, line in enumerate(lines) if roadmap_module._names_a_failure(line)]
    check("CTest's own two-line shape keeps the NAME, not only the header",
          named == [0, 1, 2, 3, 4],
          f"kept {named} of the first five lines")


def _check_four_profiles(plan: criteria_module.Plan) -> None:
    """The measured case: four ledgers declare `four-profiles`, and one run must execute it once."""
    declared = sum(
        1
        for identifier in criteria_module.available()
        for criterion in criteria_module.load(identifier).criteria
        if criterion.id == "four-profiles"
    )
    entries = [entry for entry in plan.entries if entry.criterion.id == "four-profiles"]
    check("`four-profiles` is declared by several ledgers and evaluated once",
          declared > 1 and len(entries) == 1,
          f"declared {declared} time(s), planned {len(entries)} time(s)")
    if len(entries) == 1:
        check("`four-profiles` keeps the most generous of the budgets its declarers gave it",
              entries[0].criterion.timeout_s == max(
                  criterion.timeout_s
                  for identifier in criteria_module.available()
                  for criterion in criteria_module.load(identifier).criteria
                  if criterion.id == "four-profiles"),
              f"{entries[0].criterion.timeout_s} s")


def _check_collapse_rules() -> None:
    """Two declarations collapse when they do the same work, and only then."""
    def criterion(identifier: str, run: str, timeout: int = 60) -> criteria_module.Criterion:
        return criteria_module.Criterion(
            id=identifier, describe="d", source="s", kind="recipe", ci_job="lint", run=run,
            timeout_s=timeout)

    same = criteria_module._collapse(
        [("m0", criterion("specs", "just quality-specs", 60)),
         ("m3", criterion("specs", "just quality-specs", 900))], "m3")
    check("two declarations of the same command collapse into one permanent entry",
          same.declared_by == ("m0", "m3") and same.permanent and same.criterion.timeout_s == 900,
          f"{same.declared_by}, permanent={same.permanent}, {same.criterion.timeout_s} s")
    check("a collapsed entry is labelled by the milestone that declared it first",
          same.label == "m0:specs", same.label)

    # `sample-recipe` names a different sample in M2, M3 and M4. Collapsing by id would drop two
    # milestones' closing artefacts, which is why the fingerprint is the work rather than the name.
    prints = {
        criteria_module.fingerprint(criterion("sample-recipe", "just run-sample headless-sim")),
        criteria_module.fingerprint(criterion("sample-recipe", "just run-sample first-light")),
    }
    check("two criteria sharing an id but not a command stay two checks", len(prints) == 2)


# --- The rung an inserted milestone takes ---------------------------------------------------------

# One ledger, one criterion, nothing a fixture does not need. `run` differs per milestone because
# `criteria.fingerprint` collapses declarations that do the same work, and a fixture whose three
# ledgers all ran `true` would collapse into one entry and prove nothing about inheritance.
FIXTURE_LEDGER = """\
schema = 1
id = "{identifier}"
name = "{identifier}"

[[criterion]]
id = "{identifier}-own"
describe = "the check only {identifier} declares"
source = "selftest"
kind = "command"
run = "true {identifier}"
ci_job = "lint"
"""


def _ladder_fixture(root: Path, identifiers: tuple[str, ...]) -> Path:
    """A milestones/ directory holding one single-criterion ledger per identifier."""
    directory = root / "rungs"
    directory.mkdir(parents=True, exist_ok=True)
    for identifier in identifiers:
        (directory / f"{identifier}.toml").write_text(
            FIXTURE_LEDGER.format(identifier=identifier), encoding="utf-8")
    return directory


def _declares(plan: criteria_module.Plan, identifier: str) -> bool:
    """Whether a plan evaluates the criterion that only `identifier` declares."""
    return any(entry.criterion.id == f"{identifier}-own" for entry in plan.entries)


def test_ladder_rungs(root: Path) -> None:
    """An INSERTED milestone takes the rung between its neighbours, and inheritance follows it.

    REGRESSION, and the reason it is a test rather than a comment beside `record.MILESTONES`.
    `implement-m5b-operable` inserted M5.5 between M5 and M6 rather than renumbering the ladder, and
    `criteria.rung` answers `len(MILESTONES)` for an identifier the tuple does not contain — so
    while `m5b` was missing from it, M5.5's ledger sorted to the END of the ladder, ABOVE M6.

    That was invisible while nothing sat above M5.5, and it stops being invisible the moment M6
    exists: `build_plan` inherits exactly the closed milestones whose rung is BELOW the target's, so
    a mis-ranked M5.5 would mean M6's ledger silently dropped every criterion M5.5 closed with, and
    M5.5's ledger would try to inherit M6's. Both failures are quiet — a smaller run that still says
    "all green" — which is precisely the kind the flattened evaluator cannot afford, because
    deduplication means nothing else re-runs an inherited check.

    The fixtures are three synthetic ledgers rather than the repository's own, so the property is
    asserted the day the ordering is written and not the day M6's ledger is; the assertions against
    the real ledgers below run as soon as both exist.
    """
    milestones = record_module.MILESTONES
    check("m5b is on the ladder rather than off the end of it", "m5b" in milestones,
          f"record.MILESTONES: {', '.join(milestones)}")
    check("an inserted milestone sorts between the two it was inserted between",
          criteria_module.rung("m5") < criteria_module.rung("m5b") < criteria_module.rung("m6"),
          f"m5={criteria_module.rung('m5')}, m5b={criteria_module.rung('m5b')}, "
          f"m6={criteria_module.rung('m6')}")
    check("a milestone absent from the ladder sorts off the end of it, which is what a missing "
          "rung produced", criteria_module.rung("m404") == len(milestones))

    # THE CHECK THAT WOULD HAVE CAUGHT IT THE DAY IT LANDED. `m5b.toml` and `milestone-m5b` both
    # existed while `m5b` was not in `record.MILESTONES`, and nothing anywhere said so: `rung`
    # answers `len(MILESTONES)` for an identifier it does not know, which is a position rather than
    # an error. That default is what the two checks below refuse to let a real ledger rely on, and
    # it is what `delivery-roadmap` means by "SHALL be a configuration error reported by
    # roadmap-test, never a value that silently sorts to one end".
    unranked = [identifier for identifier in criteria_module.available()
                if identifier not in milestones]
    check("every milestone with a ledger is on the ladder", not unranked,
          f"record.MILESTONES omits {', '.join(unranked)}, which has a ledger under milestones/. "
          f"Its criteria sort to the end of the ladder rather than to its rung.")
    gated = {gate.milestone for gate in gates_module.load().gates if gate.klass == "milestone"}
    unranked_gates = sorted(identifier for identifier in gated if identifier not in milestones)
    check("every milestone with a gate is on the ladder", not unranked_gates,
          f"gates.toml declares milestone-{', milestone-'.join(unranked_gates)} and "
          f"record.MILESTONES omits it")

    directory = _ladder_fixture(root, ("m5", "m5b", "m6"))
    # Every fixture milestone is offered as already-closed to BOTH plans, so the only thing deciding
    # what each inherits is the rung.
    permanent = ("m5", "m5b", "m6")
    above = criteria_module.build_plan("m6", permanent, directory)
    below = criteria_module.build_plan("m5b", permanent, directory)

    check("M6's ledger inherits M5.5's criteria",
          _declares(above, "m5b") and above.ledgers == ("m5", "m5b", "m6"),
          f"M6 evaluates the ledgers {', '.join(above.ledgers)}")
    check("M6 inherits M5.5's criteria rather than declaring them",
          any(entry.permanent and entry.criterion.id == "m5b-own" for entry in above.entries))
    check("M5.5's ledger does not inherit M6's criteria",
          not _declares(below, "m6") and below.ledgers == ("m5", "m5b"),
          f"M5.5 evaluates the ledgers {', '.join(below.ledgers)}")
    check("M5.5's ledger still evaluates its own criteria and M5's",
          _declares(below, "m5b") and _declares(below, "m5"))

    _check_real_ladder_inheritance()


def _check_real_ladder_inheritance() -> None:
    """The same property over the repository's own ledgers, once the one above M5.5 exists.

    Silent until then, because M6's ledger is a later task in the same change and a check that
    failed until it landed would be a check somebody turned off.
    """
    available = set(criteria_module.available())
    if not {"m5b", "m6"} <= available:
        return
    permanent = gates_module.permanent_milestones(gates_module.load())
    if "m5b" not in permanent:
        return
    plan = criteria_module.build_plan("m6", permanent)
    merged = {criteria_module.fingerprint(entry.criterion) for entry in plan.entries}
    missing = [criterion.id for criterion in criteria_module.load("m5b").criteria
               if criteria_module.fingerprint(criterion) not in merged]
    check("every criterion M5.5 closed with is in M6's own ledger", not missing,
          f"m5b.toml: {', '.join(missing)} would not be evaluated by M6")


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
    """The runner's own check, over just this milestone's criteria — an empty permanent set."""
    plan = criteria_module.build_plan(milestone.id)
    try:
        roadmap_module._check_criteria_are_gated(plan, gates_module.load())
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


# --- The plan's own two rules (tasks 12.6 and 12.7) ------------------------------------------------
#
# `delivery-roadmap` says every forbidden roadmap pattern "SHALL be checkable", and two of them were
# not. Both were found at M6's closing gate BY READING, which is not a mechanism:
#
#   * two rows reaching Complete before a prerequisite reached Working;
#   * three of the four plan documents disagreeing about M5's scope, and two about M6's.
#
# `plan.py` reads each fact out of the document that owns it. These cases run it against the
# repository — which is the check — and then against synthetic plans that are deliberately wrong,
# which is what stops the check from passing because it stopped looking. A parse that quietly
# returned nothing would make every finding list empty and every assertion below vacuous, so the
# sizes are asserted first.

#: What the documents hold today, as floors. A parser that silently stopped reading would fail here
#: rather than reporting a clean plan.
PLAN_FLOORS = {"capabilities": 70, "milestones": 13, "edges": 80, "sections": 13, "loads": 13}


def _synthetic_matrix(cells: dict) -> plan_module.Matrix:
    """A plan of two or three capabilities, with a `Complete` column that matches its own rows.

    The column is DERIVED rather than written, so a fixture built to exercise one rule cannot fail
    on another — the first draft of these cases hard-coded it and every fixture reported a Complete
    column finding it was not about.
    """
    milestones = ("m0", "m1", "m2")
    matrix = plan_module.Matrix(milestones, cells, {})
    completes = {name: (matrix.reaches(name, "complete") or "") for name in cells}
    return plan_module.Matrix(milestones, cells, completes)


def test_plan_documents(root: Path) -> None:
    del root
    matrix = plan_module.read_matrix()
    sections = plan_module.read_sections()
    claimed = plan_module.read_load_summary()
    expected = plan_module.read_expected_tiers()
    edges = plan_module.read_dependencies(set(matrix.cells))

    sizes = {
        "capabilities": len(matrix.cells),
        "milestones": len(matrix.milestones),
        "edges": len(edges),
        "sections": len(sections),
        "loads": len(claimed),
    }
    short = {key: value for key, value in sizes.items() if value < PLAN_FLOORS[key]}
    check("every plan document parsed into something worth checking", not short,
          f"below the floor: {short}; the rest is {sizes}")

    dependency_findings = plan_module.check_dependency_rules(matrix, edges)
    check("the tier plan obeys the dependency rules", not dependency_findings,
          "\n".join(dependency_findings))

    agreement_findings = plan_module.check_documents_agree(matrix, sections, claimed, expected)
    check("the four plan documents agree", not agreement_findings, "\n".join(agreement_findings))


def test_plan_checks_can_fail(root: Path) -> None:
    """The negative fixtures. A check that cannot fail is a check that has stopped working."""
    del root
    # A capability that reaches Working before its prerequisite is seeded, and one that reaches
    # Complete before its prerequisite is Working. Both are patterns `delivery-roadmap` forbids by
    # name and both were in M6's plan.
    early_working = _synthetic_matrix({"dependent": {"m0": "working"}, "required": {"m2": "seed"}})
    findings = plan_module.check_dependency_rules(early_working, {("required", "dependent")})
    check("a capability reaching Working before its prerequisite is seeded is caught",
          any("does not reach seed" in finding for finding in findings), str(findings))

    early_complete = _synthetic_matrix({"dependent": {"m0": "seed", "m1": "complete"},
                                        "required": {"m0": "seed", "m2": "working"}})
    findings = plan_module.check_dependency_rules(early_complete, {("required", "dependent")})
    check("a capability reaching Complete before its prerequisite is Working is caught",
          any("does not reach working" in finding for finding in findings), str(findings))

    together = _synthetic_matrix({"dependent": {"m1": "working"}, "required": {"m1": "seed"}})
    check("two capabilities landing at the same milestone are not a violation",
          not plan_module.check_dependency_rules(together, {("required", "dependent")}))

    # The four documents, one disagreement at a time.
    matrix = _synthetic_matrix({"alpha": {"m1": "working"}})
    load = plan_module.load_from_matrix(matrix)
    sections = {"m1": plan_module.Section({"alpha": "working"}, frozenset(), False)}
    check("a plan that agrees with itself reports nothing",
          not plan_module.check_documents_agree(matrix, sections, load, {}))

    stale = {"m1": plan_module.Section({"alpha": "complete"}, frozenset(), False)}
    findings = plan_module.check_documents_agree(matrix, stale, load, {})
    check("a work table whose tier the matrix does not carry is caught, unless the section says so",
          any("says nothing about the difference" in finding for finding in findings),
          str(findings))
    corrected = {"m1": plan_module.Section({"alpha": "complete"}, frozenset({"alpha"}), False)}
    check("the same disagreement passes once the milestone's section records it",
          not plan_module.check_documents_agree(matrix, corrected, load, {}))

    miscounted = dict(load)
    miscounted["m1"] = plan_module.Load(7, 0, ())
    findings = plan_module.check_documents_agree(matrix, sections, miscounted, {})
    check("a Milestone load row the matrix column does not support is caught",
          any("advanced" in finding for finding in findings), str(findings))

    findings = plan_module.check_documents_agree(matrix, sections, load,
                                                 {"m1": {"alpha": "complete"}})
    check("a ledger expecting a tier the plan does not schedule is caught",
          any("expects `alpha` at complete" in finding for finding in findings), str(findings))

    findings = plan_module.check_documents_agree(matrix, sections, load,
                                                 {"m1": {"beta": "seed"}})
    check("a ledger expecting a capability the matrix does not carry is caught",
          any("plans nothing for it" in finding for finding in findings), str(findings))

    wrong_column = plan_module.Matrix(("m0", "m1"), {"alpha": {"m1": "complete"}}, {"alpha": "m0"})
    findings = plan_module.check_documents_agree(
        wrong_column, {"m1": plan_module.Section({"alpha": "complete"}, frozenset(), False)},
        plan_module.load_from_matrix(wrong_column), {})
    check("a matrix whose Complete column disagrees with its own row is caught",
          any("Complete column" in finding for finding in findings), str(findings))


def _area(root: Path, name: str) -> Path:
    """A scratch directory per group of tests, so a failure names which one wrote what."""
    area = root / name
    area.mkdir(parents=True, exist_ok=True)
    return area


# --- The defect M6 shipped four of, made impossible ------------------------------------------------


#: The characters that break when they reach a `just` recipe's `*args`. NOT `$`: a `"$p"` in a
#: criterion is expanded by the shell that runs the criterion, so `just` never sees the dollar — five
#: ledgers pass a profile that way and they are correct. These are the ones that survive the
#: criterion's own shell as literal argv and are then re-spliced, unquoted, into another one.
RISKY_IN_JUST_ARGS = "()|;&<>*?[]`"

#: Tokens that end one command and begin the next, so that only what a `just` invocation actually
#: passes is examined. Deliberately crude: this is a lint over committed data, not a shell.
_ENDS_A_COMMAND = frozenset({"&&", "||", ";", "|", "do", "done", "then", "else", "fi", "(", ")"})


def just_arguments(run: str) -> list[tuple[str, str]]:
    """Every argument every `just` invocation in `run` passes, as (recipe, argument).

    `shlex.split` over the WHOLE string rather than over pieces of it, and that is the bug this
    function was written with and had to be corrected for: splitting on `|` first tore
    `-R "world_(activation|streaming)"` in half and the check then found nothing wrong with either
    piece — a negative fixture that failed to fail, which is the same class of defect as the one
    being checked for.
    """
    import re
    import shlex

    try:
        tokens = shlex.split(run)
    except ValueError:
        return []  # an unbalanced quote; the loader will have refused this criterion anyway
    redirection = re.compile(r"^\d*[<>]")
    found: list[tuple[str, str]] = []
    index = 0
    while index < len(tokens):
        if tokens[index] != "just" or index + 1 >= len(tokens):
            index += 1
            continue
        recipe = tokens[index + 1]
        index += 2
        while index < len(tokens):
            token = tokens[index]
            if (token in _ENDS_A_COMMAND or token.endswith(";")
                    or redirection.match(token) is not None):
                break
            found.append((recipe, token))
            index += 1
    return found


def test_just_arguments(root: Path) -> None:
    """No criterion passes a shell metacharacter through a `just` recipe's `*args`.

    THE DEFECT THIS MAKES IMPOSSIBLE IS THE ONE M6 SHIPPED FOUR OF, and it survived a stamp, a green
    tick, an exit code, two PERMANENT gates and a CI job. Four criteria in m6.toml were written

        just test-integration -R "world_(activation|streaming)"

    and `just`'s `*args` is a string interpolated TEXTUALLY into `bash -c`, so the parentheses
    reached bash unquoted. Every one of them reported `FAILED exit 2` in 0.0 s having run no test —
    and because three of them also sat in `.github/workflows/ci.yml`, three of that milestone's
    suites were covered by a command that had never executed anything.

    M7's ledger recorded the underlying defect in `just/test.just` as still live, and it still is:
    `test-unit`, `test-integration`, `test-smoke`, `test-render` and `_ctest` all pass `{{args}}`
    and `${rest}` through unquoted word splitting. Until that is fixed the discipline is "no
    metacharacter in an argument a recipe forwards", and a discipline nothing checks is a discipline
    that lasts one milestone. This is the check.

    It is deliberately a lint over the criterion's own text rather than a run of it: a criterion that
    executes nothing STILL FAILS, so a run cannot tell the two apart — that is precisely how M6's
    four hid. Only reading what was written can.
    """
    del root
    offenders = []
    for identifier in criteria_module.available():
        for criterion in criteria_module.load(identifier).criteria:
            for recipe, argument in just_arguments(criterion.run):
                bad = [letter for letter in RISKY_IN_JUST_ARGS if letter in argument]
                if bad:
                    offenders.append(
                        f"{identifier}:{criterion.id}: `just {recipe} … {argument}` carries "
                        f"{''.join(bad)}")
    check("no ledger passes a shell metacharacter through a just recipe's arguments",
          not offenders, "; ".join(offenders))

    # THE NEGATIVE FIXTURE, because a check that cannot fail is a check that has stopped working —
    # which is what M6's four criteria were. This is m6.toml's own dead criterion, restored.
    revived = 'just test-integration -R "world_(activation|streaming)"'
    check("and the check catches M6's own dead criterion, spelled exactly as it shipped",
          any(letter in argument
              for _, argument in just_arguments(revived)
              for letter in RISKY_IN_JUST_ARGS),
          f"{just_arguments(revived)} was not flagged")
    check("while the profile loop five ledgers use is not flagged, because the shell expands it",
          not [letter
               for _, argument in just_arguments(
                   'for p in debug dev; do just build-editor-check --profile "$p" || exit 1; done')
               for letter in RISKY_IN_JUST_ARGS if letter in argument],
          "a correct criterion was flagged")


# --- The Reqs column, which nothing checked until M8.a's gate found it stale ------------------------


#: Where the requirement count in the matrix's `Reqs` column can be read from.
REQUIREMENT_HEADING = "\n### Requirement:"

_MATRIX_ROW = re.compile(r"\|\s*\[`([a-z0-9-]+)`\]\([^)]*\)\s*\|\s*(\d+)\s*\|")


def matrix_requirement_counts(matrix: Path) -> list[tuple[str, int]]:
    """Every `| [`capability`](…) | N |` row of the capability matrix, as (capability, N)."""
    return [(row.group(1), int(row.group(2)))
            for row in _MATRIX_ROW.finditer(matrix.read_text(encoding="utf-8"))]


def test_matrix_requirement_counts(root: Path) -> None:
    """The matrix's `Reqs` column against the specification each row links to.

    FOUND STALE AT M8.a's CLOSING GATE, and found by counting rather than by reading:
    `delivery-roadmap`'s cell said 15 where its specification had 21. Six requirements had been added
    to it by `split-m8-authorable-and-systems` — the change that created M8.a — and the column that
    is supposed to say how large each capability is was never updated, because nothing read it.

    The column is not decoration. `capability-matrix.md` calls it "a rough indicator of size" and the
    Milestone load table above it is the argument for how a milestone's scope was decided; a row that
    understates its own size by a third is an argument made from a wrong number.

    THIS CHECK GOES RED WHEN A CHANGE THAT ADDS REQUIREMENTS IS ARCHIVED, which is the point: an
    archive syncs `openspec/changes/<id>/specs/**` into `openspec/specs/**` and the counts move. The
    failure names every row and both numbers, so the repair is arithmetic rather than an
    investigation.
    """
    del root
    stale = []
    for capability, claimed in matrix_requirement_counts(plan_module.MATRIX):
        spec = record_module.DEFAULT_SPECS / capability / "spec.md"
        if not spec.exists():
            stale.append(f"{capability}: the matrix links a specification that is not there")
            continue
        actual = spec.read_text(encoding="utf-8").count(REQUIREMENT_HEADING)
        if actual != claimed:
            stale.append(f"{capability}: the matrix says {claimed}, the specification has {actual}")
    check("the matrix's Reqs column matches the specification each row links to",
          not stale,
          "; ".join(stale) + "  — update docs/roadmap/capability-matrix.md's Reqs cell for each")

    # THE NEGATIVE FIXTURE. A count that is read out of the same file it is compared against would
    # pass over anything, which is the failure mode this whole file exists to make impossible.
    counted = dict(matrix_requirement_counts(plan_module.MATRIX))
    check("and the count really is read from the matrix rather than from the specification",
          counted.get("serialization-and-prefabs") == 23 and len(counted) > 60,
          f"parsed {len(counted)} row(s); serialization-and-prefabs = "
          f"{counted.get('serialization-and-prefabs')}")


def test_declared_gaps(root: Path) -> None:
    """A gap a milestone ships knowingly: declared, dated, still run — and it cannot rot.

    M8.c's closing gate built this, because `m8c:steam-audio-configures` was written to FAIL on
    purpose and `milestone-m8c` becomes a permanent merge gate the day it goes green. With only
    "pass" and "fail", flipping that gate would have shipped a continuous-integration job that can
    never pass — the sibling of the forbidden pattern "a milestone gate disabled rather than fixed
    or explicitly superseded". The cases below check BOTH directions, because the one that matters
    in a year is the second: a declared gap that starts passing must fail the ledger, or the marker
    outlives the gap and nobody notices.
    """
    head = 'schema = 1\nid = "m0"\nname = "Ground"\n'
    body = ('[[criterion]]\nid = "x"\ndescribe = "d"\nsource = "s"\nkind = "recipe"\n'
            'run = "just quality-layers"\nci_job = "layering"\n')

    expect_error(
        "a known_gap with no rung that must close it is rejected", criteria_module.CriteriaError,
        lambda: criteria_module.load("m0", milestone_file(
            root, "gap-no-rung", head + body + 'known_gap = "not built yet"\n')))
    expect_error(
        "a rung with no gap behind it is rejected", criteria_module.CriteriaError,
        lambda: criteria_module.load("m0", milestone_file(
            root, "gap-no-text", head + body + 'known_gap_closes = "m9"\n')))
    expect_error(
        "a known_gap_closes that is not a milestone is rejected, naming the ones that are",
        criteria_module.CriteriaError,
        lambda: criteria_module.load("m0", milestone_file(
            root, "gap-bad-rung",
            head + body + 'known_gap = "g"\nknown_gap_closes = "m99"\n')))

    declared = criteria_module.load("m0", milestone_file(
        root, "gap-ok", head + body + 'known_gap = "g"\nknown_gap_closes = "m9"\n'))
    check("a declared gap loads and knows which rung must close it",
          declared.criteria[0].is_declared_gap and declared.criteria[0].known_gap_closes == "m9")
    plain = criteria_module.load("m0", milestone_file(root, "gap-none", head + body))
    check("an ordinary criterion is not a declared gap", not plain.criteria[0].is_declared_gap)

    # --- A gap somebody adds LATER, over a gate that is already green, names its author.
    #
    # M11.c's gate-findings phase declared twenty of them across `m11a.toml` and `m11b.toml`, and
    # without this field `debts.never_seen_green` would have dropped every one of those criteria out
    # of "Red criteria under a green gate" — turning the discovery that two gates were flipped over
    # failing checks into a tidy row in the table of debts somebody planned. The field is what keeps
    # the two readings apart, so the rules around it are checked in both directions.
    expect_error(
        "an author with no declaration behind it is rejected", criteria_module.CriteriaError,
        lambda: criteria_module.load("m0", milestone_file(
            root, "gap-author-alone", head + body + 'known_gap_declared_by = "m11c"\n')))
    expect_error(
        "a known_gap_declared_by that is not a milestone is rejected",
        criteria_module.CriteriaError,
        lambda: criteria_module.load("m0", milestone_file(
            root, "gap-author-bad", head + body
            + 'known_gap = "g"\nknown_gap_closes = "m9"\nknown_gap_declared_by = "m99"\n')))
    expect_error(
        "a rung that declares a gap and also closes it is rejected as a declaration of nothing",
        criteria_module.CriteriaError,
        lambda: criteria_module.load("m0", milestone_file(
            root, "gap-author-is-closer", head + body
            + 'known_gap = "g"\nknown_gap_closes = "m9"\nknown_gap_declared_by = "m9"\n')))
    retro = criteria_module.load("m0", milestone_file(
        root, "gap-retro", head + body
        + 'known_gap = "g"\nknown_gap_closes = "m11e"\nknown_gap_declared_by = "m11c"\n'))
    check("a retroactive gap declaration carries the rung that wrote it",
          retro.criteria[0].is_declared_gap
          and retro.criteria[0].known_gap_declared_by == "m11c")
    check("a gap declared by its own rung leaves the author field empty",
          declared.criteria[0].known_gap_declared_by == "")

    # --- The verdict, which is where the mechanism earns its place.
    gap = criteria_module.Criterion(
        id="gap", describe="the gap", source="s", kind="recipe", run="just quality-layers",
        ci_job="milestone-m0", known_gap="not built yet", known_gap_closes="m9")
    ordinary = criteria_module.Criterion(
        id="plain", describe="an ordinary check", source="s", kind="recipe",
        run="just quality-specs", ci_job="specs")
    milestone = criteria_module.Milestone(id="m0", name="Ground", artefact="a", notes=(),
                                          criteria=(gap, ordinary))
    entries = (criteria_module.PlanEntry(criterion=gap, declared_by=("m0",), permanent=False),
               criteria_module.PlanEntry(criterion=ordinary, declared_by=("m0",), permanent=False))
    plan = criteria_module.Plan(milestone=milestone, entries=entries,
                                ledgers=("m0",), declarations=len(entries))

    def verdict(gap_status: str, other_status: str) -> tuple[int, str]:
        results = (criteria_module.Result(gap, gap_status, "", 0.0, ""),
                   criteria_module.Result(ordinary, other_status, "", 0.0, ""))
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            code = roadmap_module._summarise(plan, results, as_json=False)
        return code, buffer.getvalue()

    code, output = verdict(criteria_module.FAILED, criteria_module.OK)
    check("a declared gap that still fails does NOT make the milestone's gate red",
          code == 0, f"exit {code}")
    check("and it is printed anyway, with the rung that must close it",
          "declared gap, still open" in output and "M9" in output, output)

    code, output = verdict(criteria_module.OK, criteria_module.OK)
    check("A DECLARED GAP THAT PASSES FAILS THE LEDGER — the marker outlived the gap",
          code != 0, f"exit {code}")
    check("and the message says what to do about it",
          "DELETE THE DECLARATION" in output, output)

    code, _ = verdict(criteria_module.FAILED, criteria_module.FAILED)
    check("an ordinary failure beside a declared gap still fails the milestone", code != 0)


# --- Falsifiability: the defect this project has now shipped seven of ------------------------------
#
# A determinism test whose scene never contended; a sky test asserting against the producer's own
# statistics; a criterion running through a recipe that passes `--no-tests=ignore`; two criteria
# whose grep matched only the ledger file doing the grepping; a dependency check parsing backticks
# out of a table written in bold; a four-profiles criterion the runner had to help; and then five
# refuted claims at M11.a/M11.b's gate. Seven findings of one defect: A CRITERION NOBODY HAS SHOWN
# CAN FAIL IS NOT A CHECK, and every one of them was green until somebody read it.
#
# `falsify.py` is the mechanism and this is what makes it a gate rather than a tool nobody runs:
# `just roadmap-test` is the `plan-consistency` criterion of every ledger on the ladder, so a
# criterion that has not been shown able to go red turns every milestone gate red instead.
#
# The cases below come in pairs on purpose. Each rule is fired at the defect it was written for —
# spelled as the ledgers actually spell it — and then at the corrected version of the same check,
# because a rule that flags everything is as useless as one that flags nothing, and the first draft
# of three of these rules did exactly that.


def _criterion(identifier: str, run: str, **extra) -> criteria_module.Criterion:
    return criteria_module.Criterion(id=identifier, describe="a fixture", source="a fixture",
                                     kind=extra.pop("kind", "command"), ci_job="milestone-m0",
                                     run=run, **extra)


def _rules(run: str, **extra) -> set[str]:
    return {finding.rule for finding in falsify_module.inspect(_criterion("fixture", run, **extra))}


def test_falsifiability_rules(root: Path) -> None:
    """Each rule against the defect it was written for, and against the corrected check beside it."""
    del root

    # THE TWO FAILURE MODES THIS MECHANISM WAS ASKED TO MAKE IMPOSSIBLE.
    check("a grep over the whole tree is refused: it matches the ledger doing the grepping",
          "self-match" in _rules("grep -rq SeparateProcess ."))
    check("and it is refused a second time for matching a token ANYWHERE in the tree",
          "searches-the-repository-root" in _rules("grep -rq SeparateProcess ."))
    check("a grep of tools/ that does not filter out tools/roadmap/ is refused",
          "self-match" in _rules("grep -rniIl FieldImage src/ tools/"))
    check("the same grep is accepted once it filters its own ledger out — M11.b's spelling",
          not _rules('grep -rniIl SeparateProcess src/ editor/ tools/ | grep -v "^tools/roadmap/"'))
    check("and accepted when --include cannot open a .toml — M9's `replay-one-record` spelling",
          not _rules("grep -rn kRecordTypeId src/ tools/ --include='*.h' --include='*.cpp'"))
    check("a grep scoped to source directories is not accused of anything",
          not _rules("grep -rq CY_BREADCRUMB src/ --include='*.cpp'"))

    # THE DEFECT M8 SHIPPED: a recipe that reports a pass for having run nothing.
    check("`just test-render -R <suite>` is refused: --no-tests=ignore makes an empty selection pass",
          "vacuous-suite" in _rules("just test-render -R vfx"))
    check("the same suite is accepted once the criterion asserts the suite is registered — M10's",
          "vacuous-suite" not in _rules(
              'ctest --test-dir "$d" -N -R \'^render.vfx_gpu$\' | grep -q render.vfx_gpu\n'
              "just test-render -R '^render.vfx_gpu$'"))
    check("a ctest that selects and asserts nothing about the selection is refused",
          "vacuous-suite" in _rules('ctest --test-dir "$d" -R vfx'))

    # A BODY THAT CANNOT RETURN NON-ZERO, which is what "a word-grep a dummy job satisfies" becomes
    # once the grep is deleted.
    check("a criterion that only reports is refused",
          "no-assertion" in _rules('echo "the play modes exist"\nls src/'))
    check("a criterion that ends by discarding its own verdict is refused",
          "swallowed-verdict" in _rules("grep -q SeparateProcess src/editor/play.h || true"))
    check("an embedded Python program is NOT mistaken for a body with no assertion",
          "no-assertion" not in _rules("set -eo pipefail\npython3 - <<'CHECK'\nimport sys\n"
                                       "sys.exit(1)\nCHECK"))
    check("nor is `CY_BUILD_DIR=... cargo test`, whose command is cargo and not the assignment",
          "no-assertion" not in _rules('CARGO_TARGET_DIR="$(just _editor-target-dir)" cargo test'))

    # THE NINTH, AND `absent-recipe` ONE LEVEL DOWN. `m11a:world-budget-headless`,
    # `world-budget-on-a-device` and `world-streams` ran `just run-sample 10-world ...`; the recipe
    # exists, the sample is called `world` and `10-world` is the DIRECTORY, so run.just printed
    # `no sample '10-world'` and exited 2 having measured nothing — and all three were recorded by
    # the prover as `red against a built tree`, the verdict a criterion earns by being watched
    # failing for its subject's sake.
    check("a sample name `just run-sample` cannot resolve is refused: it exits 2 having run nothing",
          "absent-sample" in _rules(
              "just run-sample 10-world --headless --seed 1 --budget-ms 16.7"))
    check("and the same line is accepted once it names the binary samples/ actually declares",
          "absent-sample" not in _rules(
              "just run-sample world --headless --seed 1 --budget-ms 16.7"))
    check("`just run-sample --headless` with no name at all is not accused: run.just means `empty`",
          "absent-sample" not in _rules("just run-sample --headless"))
    check("a sample name assembled at run time is not guessed at, in either direction",
          "absent-sample" not in _rules('just run-sample "$name" --headless'))
    check("and a `just` recipe that is not run-sample is not read as one",
          "absent-sample" not in _rules("just build-engine --profile dev"))

    # AND WHAT THE RECORD SAYS ABOUT A RED CRITERION. The three above were filed with the detail
    # `==> configure   profile=dev ...` — the FIRST line every build-backed criterion prints — and
    # nothing of the failure, so a reader could not tell a criterion that ran and failed from one
    # that never resolved its own command.
    both_ends = falsify_module._why_it_is_red(
        "==> configure   profile=dev\nrun-sample: no sample '10-world'\n"
        "error: Recipe `run-sample` failed with exit code 2\n")
    check("a red criterion's recorded reason carries the LAST thing it said, not only the first",
          "no sample '10-world'" in both_ends, both_ends)
    check("and `just`'s own epilogue is not mistaken for the criterion's verdict",
          "failed with exit code" not in both_ends, both_ends)
    check("while a one-line failure is recorded once rather than twice",
          falsify_module._why_it_is_red("the only thing it said") == "the only thing it said")


def test_absent_recipe_rule(root: Path) -> None:
    """A criterion that names a `just` recipe the justfile does not define is REFUSED, not proven.

    THE EIGHTH DEFECT OF ITS KIND, AND THE FIRST ONE THE PROVER COMMITTED RATHER THAN CAUGHT.
    `m11a:network-at-complete-grade`, `m11b:gameplay-at-complete-grade` and
    `m11b:editor-at-complete-grade` each ran `just quality-requirements <rows...>`. There was no such
    recipe anywhere in `just/`, so `just` stopped at ARGUMENT PARSING with "Justfile does not contain
    recipes", ran nothing at all, and exited 1. A criterion that is red unmutated in the sandbox and
    in the repository is recorded by `falsify._red_in_the_tree` as `red in the tree`, which is inside
    `PROOF_VERDICTS` — so all three were counted as checks that had been watched going red, by the
    very mechanism built to stop exactly that.

    An exit code cannot tell a subject failing from a name failing to resolve, so the shape is
    refused before any run: `absent-recipe` is in `CANNOT_GO_RED`.
    """
    del root
    check("a criterion naming a recipe that does not exist is refused",
          "absent-recipe" in _rules("just quality-no-such-recipe networking-and-replication"))
    check("and `just quality-requirements` — the recipe all three named — now exists, so it passes",
          "absent-recipe" not in _rules("just quality-requirements networking-and-replication"),
          "just/quality.just must define quality-requirements, or the three criteria that run it "
          "are still names that do not resolve")
    check("a real recipe is not accused",
          "absent-recipe" not in _rules("just build-engine --profile dev"))
    check("a PRIVATE recipe is not accused: `just --summary` hides them, the JSON dump does not",
          "absent-recipe" not in _rules("just _ctest determinism -R '^determinism.cross_leg$'"))
    check("a recipe name assembled at run time is not read, rather than guessed at",
          "absent-recipe" not in _rules('just "$recipe" --profile dev'))
    check("a `just` inside a heredoc body is not read as a command of this criterion",
          "absent-recipe" not in _rules("python3 - <<'EOF'\nprint('just no-such-recipe-at-all')\nEOF"))
    check("the rule refuses the criterion rather than merely reporting it",
          "absent-recipe" in falsify_module.CANNOT_GO_RED)
    check("and it sees through a `just` reached after global options",
          "absent-recipe" in _rules("just --justfile Justfile no-such-recipe-at-all"))


def _a_suite_and_a_case_in_it() -> tuple[str, str]:
    """A real `<kind>.<name>` and a real test case declared beside it, read out of this tree.

    Derived rather than hard-coded, for the reason `tools/editor/selftest.py` gives about fixtures:
    a suite and a case named in a constant here go stale the day either is renamed, and a stale
    fixture agrees with a broken check.
    """
    for suite, directory in sorted(requirements_module.declared_tests().items()):
        for source in sorted(directory.glob("*.cpp")):
            found = re.search(r'TEST_CASE\("([^"]{12,})"', source.read_text(encoding="utf-8"))
            if found:
                return suite, found.group(1)
    raise AssertionError("no committed suite in this tree declares a TEST_CASE this reader can find")


def test_requirements_coverage(root: Path) -> None:
    """`just quality-requirements` can PASS and can FAIL, over fixtures that say which.

    A check that is red today says nothing about whether it can ever be green, and "it cannot pass"
    is the same defect as "it cannot fail" seen from the other side. So the both-directions proof is
    made here, against a specification tree and a map written for the occasion.

    AND THE FIVE LEGS THAT STOP AN ENTRY FROM BEING A SENTENCE. `requirements.py`'s first version
    asked only whether a suite of the named name was declared SOMEWHERE, which twenty-four
    requirements could have satisfied by naming one suite twenty-four times. Each of the rules that
    replaced that is proven able to fail below: a `test:` entry with no case, a case that belongs to
    another suite, a `criterion:` entry naming a criterion nobody has shown can fail, and a `rust:`
    entry naming a function that is not in the crate it claims.
    """
    specs = root / "openspec" / "specs" / "fixture-row"
    specs.mkdir(parents=True)
    (specs / "spec.md").write_text(
        "# fixture-row\n\n## Requirements\n\n"
        "### Requirement: The first thing\nIt SHALL happen.\n\n"
        "### Requirement: The second thing\nIt SHALL also happen.\n", encoding="utf-8")
    previous = requirements_module.SPECS
    requirements_module.SPECS = root / "openspec" / "specs"
    try:
        real_test, real_case = _a_suite_and_a_case_in_it()
        real_gate = sorted(requirements_module.declared_gates())[0]
        real_proven = sorted(requirements_module.proven_criteria())[0]
        unproven = sorted(requirements_module.declared_criteria()
                          - requirements_module.proven_criteria())[0]

        written = itertools.count()

        def audit(body: str) -> int:
            path = root / f"map-{next(written)}.toml"
            path.write_text("schema = 1\n" + body, encoding="utf-8")
            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                return requirements_module.main(["fixture-row", "--map", str(path)])

        answered = (f'[[coverage]]\nrow = "fixture-row"\nrequirement = "The first thing"\n'
                    f'evidence = "test:{real_test}"\ncase = "{real_case}"\n\n'
                    f'[[coverage]]\nrow = "fixture-row"\nrequirement = "The second thing"\n'
                    f'evidence = "gate:{real_gate}"\n')
        check("a row whose every requirement names a real suite or gate PASSES", audit(answered) == 0)

        half = answered.split("\n\n")[0] + "\n"
        check("and the same row goes RED when one requirement is answered by nothing",
              audit(half) == 1)

        broken = answered.replace(f"test:{real_test}", "test:unit.no_such_suite_exists")
        check("and RED when an answer names a suite no committed cy_add_test() declares",
              audit(broken) == 1)

        # --- THE CASE IS THE CLAIM, AND THESE TWO ARE WHY -------------------------------------------
        caseless = answered.replace(f'case = "{real_case}"\n', "")
        check("a `test:` entry with no case is REFUSED — a suite alone is satisfied by any suite",
              audit(caseless) == 1)

        borrowed = answered.replace(f'case = "{real_case}"',
                                    'case = "a case that belongs to some other suite entirely"')
        check("and RED when the case is not in the sources beside the cy_add_test that declares it",
              audit(borrowed) == 1)

        # --- A CRITERION ANSWERS ONLY IF IT HAS BEEN SHOWN ABLE TO FAIL -----------------------------
        proven = answered.replace(f'evidence = "gate:{real_gate}"',
                                  f'evidence = "criterion:{real_proven}"')
        check("a criterion that falsifiability.toml records a PROOF for is an answer",
              audit(proven) == 0)
        check("and one it does not is REFUSED, however green it is today",
              audit(answered.replace(f'evidence = "gate:{real_gate}"',
                                     f'evidence = "criterion:{unproven}"')) == 1)

        # --- THE EDITOR'S OWN SUITES ----------------------------------------------------------------
        rust = answered.replace(
            f'evidence = "gate:{real_gate}"',
            'evidence = "rust:cy-editor-interface::specialised::tests::'
            'every_graph_editor_opens_the_same_one_canvas"')
        check("a `rust:` entry naming a test function of an editor crate is an answer",
              audit(rust) == 0)
        check("and RED when that crate holds no such function",
              audit(rust.replace("every_graph_editor_opens_the_same_one_canvas",
                                 "every_graph_editor_opens_whatever_it_likes")) == 1)
        check("and RED when the crate itself is not there",
              audit(rust.replace("cy-editor-interface::", "cy-editor-imaginary::")) == 1)

        stale = answered + ('\n[[coverage]]\nrow = "fixture-row"\n'
                            'requirement = "A requirement nobody asks any more"\n'
                            f'evidence = "gate:{real_gate}"\n')
        check("and RED on a STALE entry: an answer to a question the specification stopped asking",
              audit(stale) == 1)

        exempt = (answered.split("\n\n")[0] + "\n\n"
                  '[[coverage]]\nrow = "fixture-row"\nrequirement = "The second thing"\n'
                  'evidence = "exempt:m11e"\nnote = "deferred because the subject does not exist '
                  'in this tree yet, and M11.e is the rung that builds it"\n')
        check("a recorded exemption naming a real milestone and a reason is an answer",
              audit(exempt) == 0)
        check("but not one parked at a rung that is not a milestone",
              audit(exempt.replace("exempt:m11e", "exempt:m99")) == 1)
        check("and not one whose reason is a shrug",
              audit(exempt.replace(exempt[exempt.index("note = "):], 'note = "too hard"\n')) == 1)

        check("a duplicate answer to one requirement is a malformed map, not a smaller one",
              audit(answered + answered.split("\n\n")[0] + "\n") == 2)
    finally:
        requirements_module.SPECS = previous


def test_a_wrapped_case_name_is_still_that_case(root: Path) -> None:
    """A test case whose name clang-format wrapped is found, and an absent one is still absent.

    THE REGRESSION THIS IS FOR. `_contains` searched the source bytes for the case name, and
    `CY_TEST_CASE` names longer than the line limit are wrapped into ADJACENT STRING LITERALS —
    `"a long name "` then `"continues"` on the next line — which the compiler concatenates and a
    substring search does not. Two entries of `vfx-system` were red for exactly that at M11.c's
    closing gate (`test_vfx_compiler.cpp:509`, `test_firewall.cpp:448`), both naming cases that were
    there under those names, and no run had ever reported it because no criterion runs
    `quality-requirements` over that row. The negative control is the point: the join is a
    quote-whitespace-quote boundary and nothing else, so it cannot fuse two unrelated strings in a
    list into a name nobody wrote, and a case that is genuinely not there is still not there.
    """
    wrapped = root / "wrapped.cpp"
    wrapped.write_text('CY_TEST_CASE(\n    "a project registers its own data interface and it "\n'
                       '    "reads") {\n}\n', encoding="utf-8")
    listed = root / "list.cpp"
    listed.write_text('const char* kNames[] = {"alpha", "beta"};\n', encoding="utf-8")
    check("a case name the source wrapped across adjacent literals is found",
          requirements_module._contains(
              [wrapped], "a project registers its own data interface and it reads"))
    check("and the name as written on one line is still found",
          requirements_module._contains([wrapped], "a project registers its own data interface"))
    check("a case that is not in the file is still not found",
          not requirements_module._contains([wrapped], "a case nobody ever wrote"))
    check("and two unrelated strings in a list are NOT fused into a name",
          not requirements_module._contains([listed], "alphabeta"),
          "only a quote-whitespace-quote boundary is a concatenation; a comma is not")


def test_falsifiability_reads_a_redirection(root: Path) -> None:
    """A file descriptor in front of a redirection is not a path the criterion searches.

    `grep -rl token src/ 2>/dev/null` — the reader over committed shell took the `2` as an operand,
    so the mutation derived from it named a file called "2" alongside the real one. It matched
    nothing, which is what made it worth fixing rather than urgent: a recorded proof that names a
    path which does not exist reads like a proof about that path. Both directions are checked,
    because a rule that ate a real operand would be the same defect the other way up.
    """
    del root
    searched = falsify_module.searches("grep -rl kToken src/ 2>/dev/null")
    check("a redirection's file descriptor is not one of the paths searched",
          searched and searched[0].paths == ("src/",), str(searched))
    kept = falsify_module.searches("grep -rl kToken src/ 2 3")
    check("while an operand that happens to be a digit is still an operand",
          kept and kept[0].paths == ("src/", "2", "3"), str(kept))
    derived = falsify_module.derive(_criterion(
        "fixture", 'count=$(grep -rl kTokenLiteral src/ 2>/dev/null | wc -l)\ntest "$count" -ge 1'))
    check("and the mutation derived from it names only the real path",
          derived is not None and derived.target == "src/",
          derived.target if derived else "nothing derived")


def test_falsifiability_digest(root: Path) -> None:
    """The digest is over what a criterion CHECKS, so prose is free and a changed check is not."""
    del root
    original = _criterion("x", "grep -q Token src/a.h")
    reworded = criteria_module.Criterion(**{**original.__dict__, "describe": "a much better wording",
                                           "source": "tasks 99.9"})
    changed = criteria_module.Criterion(**{**original.__dict__, "run": "grep -q Other src/a.h"})
    check("re-wording a criterion costs nothing",
          falsify_module.digest(original) == falsify_module.digest(reworded))
    check("changing what it runs costs a re-proof",
          falsify_module.digest(original) != falsify_module.digest(changed))
    declared = criteria_module.Criterion(
        **{**original.__dict__, "falsifies": {"mutate": "delete-path", "target": "src/a.h"}})
    check("and so does changing the mutation it declares",
          falsify_module.digest(original) != falsify_module.digest(declared))


def test_falsifiability_reconciliation(root: Path) -> None:
    """The four directions the inventory can disagree with the ladder, none of which may be silent."""
    del root
    proven = falsify_module.Proof("m0", "good", "aaaa", falsify_module.PROVEN, "delete-path", "red")
    unproven = falsify_module.Proof("m0", "debt", "bbbb", falsify_module.NO_MUTATION, "-", "")

    empty = falsify_module.Inventory()
    findings = falsify_module.reconcile([proven], empty)
    check("A CRITERION NOTHING HAS JUDGED IS REFUSED — this is what stops the eighth",
          any("nothing in falsifiability.toml has judged" in finding for finding in findings),
          str(findings))

    inventory = falsify_module.Inventory(proofs={("m0", "good"): proven},
                                         unproven={("m0", "debt"): unproven})
    check("a ladder that matches its inventory reports nothing",
          not falsify_module.reconcile([proven, unproven], inventory))

    edited = falsify_module.Proof("m0", "good", "cccc", falsify_module.PROVEN, "delete-path", "red")
    findings = falsify_module.reconcile([edited, unproven], inventory)
    check("a criterion edited since it was proven is refused until it is proven again",
          any("has changed since it was last judged" in finding for finding in findings),
          str(findings))

    paid = falsify_module.Proof("m0", "debt", "bbbb", falsify_module.PROVEN, "rename-token", "red")
    findings = falsify_module.reconcile([proven, paid], inventory)
    check("A DEBT THAT HAS BEEN PAID FAILS TOO: the entry outlived the gap",
          any("DELETE THE ENTRY" in finding for finding in findings), str(findings))

    lapsed = falsify_module.Proof("m0", "good", "aaaa", falsify_module.REFUTED, "-", "vacuous-suite")
    findings = falsify_module.reconcile([lapsed, unproven], inventory)
    check("a proof that has stopped proving fails",
          any("no longer proves" in finding for finding in findings), str(findings))

    findings = falsify_module.reconcile([proven], inventory)
    check("an entry for a criterion no ledger declares any more fails",
          any("in no ledger" in finding for finding in findings), str(findings))


def test_falsifiability_ledger_blind(root: Path) -> None:
    """The control that catches a grep matching its own ledger, run against one that does.

    THE REGEX IS NOT THE MECHANISM. `self-match` reads the criterion's text and is caught out by
    every spelling nobody anticipated — a `find | xargs grep`, a path assembled in a variable, a
    search of a directory that happens to contain the ledgers. Deleting tools/roadmap/milestones/ and
    re-running the criterion catches all of them, because a criterion whose verdict CHANGES when the
    roadmap is deleted was reading the roadmap. This is that control, fired at a criterion whose
    token exists nowhere else in the tree.
    """
    sandbox = falsify_module.Sandbox.materialise(root / "tree")
    planted = sandbox.root / "tools" / "roadmap" / "milestones" / "fixture.toml"
    planted.write_text("# kTokenThatExistsOnlyInALedger\n", encoding="utf-8")

    reads_its_own_ledger = _criterion(
        "blind", "grep -rq kTokenThatExistsOnlyInALedger tools/roadmap/milestones/")
    code, _output = sandbox.run(reads_its_own_ledger)
    check("the fixture criterion passes while its ledger is there — the positive control", code == 0)
    reason = falsify_module._ledger_blind(sandbox, reads_its_own_ledger)
    check("A CRITERION THAT READS ITS OWN LEDGER IS CAUGHT BY DELETING THE LEDGERS",
          "comes from its own ledger" in reason, reason or "the control said nothing")

    reads_the_repository = _criterion("real", "grep -rq CyberdyneEngine README.md")
    code, _output = sandbox.run(reads_the_repository)
    check("and a criterion that reads the repository passes the same control untouched",
          code == 0 and not falsify_module._ledger_blind(sandbox, reads_the_repository))


def test_falsifiability_declared_mutations(root: Path) -> None:
    """Every mutation verb, applied for real — including the ones no derivation produces.

    `delete-lines` and `truncate` exist for the criteria whose mutation cannot be derived, so nothing
    on the ladder exercises them yet. A verb nobody has run is the next thing to quietly stop
    working, which is the failure mode this whole file is about.
    """
    sandbox = falsify_module.Sandbox.materialise(root / "tree")
    target = sandbox.root / "docs" / "fixture.txt"
    target.parent.mkdir(parents=True, exist_ok=True)
    original = "keep this line\nLiveEditPolicy is named here\nkeep this one too\n"

    def apply(verb: str, **fields) -> str:
        target.write_text(original, encoding="utf-8")
        sandbox.forget()
        mutation = falsify_module.Mutation(verb=verb, target="docs/fixture.txt", derived=False,
                                           **fields)
        changed = sandbox.apply(mutation)
        after = target.read_text(encoding="utf-8") if target.is_file() else "<deleted>"
        check(f"the `{verb}` mutation changes the sandbox", changed == 1, f"{changed} file(s)")
        return after

    check("`delete-lines` removes the line carrying the token, and only that line",
          apply("delete-lines", token="LiveEditPolicy")
          == "keep this line\nkeep this one too\n")
    check("`truncate` empties the file", apply("truncate") == "")
    check("`delete-path` removes it", apply("delete-path") == "<deleted>")
    renamed = apply("rename-token", token="LiveEditPolicy")
    check("`rename-token` replaces the token and leaves everything else alone",
          "LiveEditPolicy" not in renamed and "keep this line" in renamed, renamed)

    target.write_text(original, encoding="utf-8")
    sandbox.forget()
    missing = falsify_module.Mutation(verb="delete-path", target="docs/not-here-at-all.txt",
                                      derived=False)
    check("A MUTATION THAT CHANGES NOTHING IS A FINDING, not a silent pass",
          sandbox.apply(missing) == 0)

    declared = criteria_module.Criterion(
        id="declared", describe="a fixture", source="a fixture", kind="command",
        ci_job="milestone-m0", run="grep -q LiveEditPolicy docs/fixture.txt",
        falsifies={"mutate": "delete-lines", "target": "docs/fixture.txt",
                   "token": "LiveEditPolicy"})
    derived = falsify_module.derive(declared)
    check("a declared mutation is used in place of the derived one",
          derived is not None and derived.verb == "delete-lines" and not derived.derived)
    proof = falsify_module.prove(sandbox, "m0", declared)
    check("and a criterion is proven end to end through the mutation it declared",
          proof.verdict == falsify_module.PROVEN, f"{proof.verdict}: {proof.detail}")


def test_falsifiability_in_the_ci_environment(root: Path) -> None:
    """A `where = "ci"` criterion judged against the environment it declares — and the four refusals.

    THE SHAPE THIS REPLACED. `falsify.prove` returned `not provable here` for every criterion this
    host cannot evaluate, correctly: `just test-determinism --compare-legs` fails on a machine with
    one architecture, and reading that as "watched going red" would be a verdict about the laptop.
    What it left was two of M11.a's seventy that NOBODY had shown could fail, which M11's repair gate
    called dispositive and was right to.

    So the criterion declares what CI hands it, as a command the prover EXECUTES. The four cases
    below are why `provide` cannot be filled in falsely: an environment that is not built, one that
    does not make the criterion pass, a mutation that misses, and a mutation the criterion survives
    are each refused, and only the three-run sequence earns the verdict.
    """
    sandbox = falsify_module.Sandbox.materialise(root / "tree")

    def criterion(provide: str, target: str, token: str) -> criteria_module.Criterion:
        return criteria_module.Criterion(
            id="needs-ci", describe="a fixture", source="a fixture", kind="command",
            ci_job="milestone-m0", where="ci", reason="this fixture host is not the CI matrix",
            run="grep -q agreed downloaded/leg.txt",
            ci_proof={"provide": provide, "mutate": "rename-token", "target": target,
                      "token": token})

    supplies = "mkdir -p downloaded && printf 'agreed\\n' > downloaded/leg.txt"
    good = criterion(supplies, "downloaded/leg.txt", "agreed")
    proof = falsify_module.prove(sandbox, "m0", good)
    check("a where=ci criterion is proven against the environment its ci_proof builds",
          proof.verdict == falsify_module.PROVEN_IN_THE_CI_ENVIRONMENT,
          f"{proof.verdict}: {proof.detail}")
    check("and the verdict counts as a proof",
          falsify_module.PROVEN_IN_THE_CI_ENVIRONMENT in falsify_module.PROOF_VERDICTS)
    check("and the environment is REMOVED afterwards, so the next criterion does not inherit it",
          not (sandbox.root / "downloaded").exists())

    beaten = falsify_module.prove(sandbox, "m0", criterion("true", "downloaded/leg.txt", "agreed"))
    check("a `provide` that supplies nothing leaves the criterion red at its positive control",
          beaten.verdict == falsify_module.UNPROVABLE, f"{beaten.verdict}: {beaten.detail}")
    check("and says so in those words rather than counting it",
          "positive control" in beaten.detail, beaten.detail)

    missed = falsify_module.prove(sandbox, "m0", criterion(supplies, "downloaded/leg.txt", "absent"))
    check("a mutation that names a token the environment does not carry is REFUTED",
          missed.verdict == falsify_module.REFUTED, f"{missed.verdict}: {missed.detail}")

    survived = falsify_module.prove(
        sandbox, "m0", criterion(supplies + " && printf 'spare\\n' > downloaded/spare.txt",
                                 "downloaded/spare.txt", "spare"))
    check("and a mutation the criterion SURVIVES is refuted rather than recorded",
          survived.verdict == falsify_module.REFUTED, f"{survived.verdict}: {survived.detail}")

    # THE SCOPE RULES, which are what stop this becoming a way to pass a criterion nothing can judge.
    expect_error(
        "a ci_proof on a criterion that is not where = \"ci\" is rejected",
        criteria_module.CriteriaError,
        lambda: criteria_module.load("m0", milestone_file(
            root, "ci-proof-local",
            'schema = 1\nid = "m0"\n[[criterion]]\nid = "c"\ndescribe = "d"\nsource = "s"\n'
            'kind = "command"\nrun = "true"\nci_job = "build-and-test"\n'
            '[criterion.ci_proof]\nprovide = "true"\nmutate = "truncate"\ntarget = "x"\n')))
    expect_error(
        "and one standing in for a device is rejected: no command conjures a GPU",
        criteria_module.CriteriaError,
        lambda: criteria_module.load("m0", milestone_file(
            root, "ci-proof-gpu",
            'schema = 1\nid = "m0"\n[[criterion]]\nid = "c"\ndescribe = "d"\nsource = "s"\n'
            'kind = "command"\nrun = "true"\nci_job = "build-and-test"\nwhere = "ci"\n'
            'requires = "gpu"\nreason = "r"\n'
            '[criterion.ci_proof]\nprovide = "true"\nmutate = "truncate"\ntarget = "x"\n')))
    expect_error(
        "and a ci_proof with no 'provide' is rejected: there would be no environment to judge",
        criteria_module.CriteriaError,
        lambda: criteria_module.load("m0", milestone_file(
            root, "ci-proof-empty",
            'schema = 1\nid = "m0"\n[[criterion]]\nid = "c"\ndescribe = "d"\nsource = "s"\n'
            'kind = "command"\nrun = "true"\nci_job = "build-and-test"\nwhere = "ci"\n'
            'reason = "r"\n[criterion.ci_proof]\nmutate = "truncate"\ntarget = "x"\n')))
    expect_error(
        "and a ci_proof whose mutation is a sentence is rejected, like every other mutation here",
        criteria_module.CriteriaError,
        lambda: criteria_module.load("m0", milestone_file(
            root, "ci-proof-prose",
            'schema = 1\nid = "m0"\n[[criterion]]\nid = "c"\ndescribe = "d"\nsource = "s"\n'
            'kind = "command"\nrun = "true"\nci_job = "build-and-test"\nwhere = "ci"\n'
            'reason = "r"\n[criterion.ci_proof]\nprovide = "true"\n'
            'mutate = "break the digest somehow"\ntarget = "x"\n')))


def test_falsifiability_of_a_declared_gap(root: Path) -> None:
    """A criterion its ledger declares as an expected failure is judged the other way round.

    It is RED on the unmutated tree — that is what a declared gap IS — so "show that it can go red"
    asks for what every run of that ledger already prints. What it has not shown is that it is not
    PERMANENTLY red, which is the difference between a deadline and a check that can never pass, so
    its mutation must take it GREEN. Both directions are exercised here, because the second one —
    a mutation that leaves it red proves nothing and must not count — is the one that would quietly
    turn this route into a way of recording anything at all as proven.
    """
    sandbox = falsify_module.Sandbox.materialise(root / "tree")
    target = sandbox.root / "docs" / "gap-fixture.txt"
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text("the release recipes still refuse\n", encoding="utf-8")
    sandbox.forget()

    def gap(identifier: str, run: str = "! grep -q refuse docs/gap-fixture.txt",
            **falsifies) -> criteria_module.Criterion:
        return criteria_module.Criterion(
            id=identifier, describe="a fixture", source="a fixture", kind="command",
            ci_job="milestone-m0", run=run,
            known_gap="the fixture refuses, and the rung that fixes it is named",
            known_gap_closes="m11d", falsifies=falsifies)

    red = falsify_module.prove(sandbox, "m0", gap(
        "closes", mutate="rename-token", target="docs/gap-fixture.txt", token="refuse"))
    check("a declared gap that is RED unmutated and GREEN under its mutation is proven",
          red.verdict == falsify_module.PROVEN, f"{red.verdict}: {red.detail}")
    check("and the proof says which way round it was judged",
          "RED unmutated" in red.detail and "GREEN under the mutation" in red.detail, red.detail)

    target.write_text("the release recipes still refuse\n", encoding="utf-8")
    sandbox.forget()
    stays = falsify_module.prove(sandbox, "m0", gap(
        "stays-red", mutate="rename-token", target="docs/gap-fixture.txt", token="recipes"))
    check("A MUTATION THAT LEAVES A DECLARED GAP RED PROVES NOTHING and does not count",
          stays.verdict == falsify_module.UNPROVABLE, f"{stays.verdict}: {stays.detail}")

    target.write_text("the release recipes still refuse\n", encoding="utf-8")
    sandbox.forget()
    # No search in the body, so nothing can be derived from it and nothing is declared either.
    none = falsify_module.prove(sandbox, "m0", gap("no-mutation", run="[ ! -s docs/gap-fixture.txt ]"))
    check("and a declared gap with no mutation at all is on the list, not in the proofs",
          none.verdict == falsify_module.NO_MUTATION, f"{none.verdict}: {none.detail}")

    # AND NEITHER OF THOSE TWO CONTRADICTS A STANDING RECORD, which is the third direction and the
    # one M11.c's ladder repair added. Eleven of this ladder's declared gaps are red because the
    # thing they name does not EXIST — a checker nobody wrote, a game nobody built, a screenshot
    # nobody captured — and every mutation verb this module has subtracts, so no mutation can make
    # one green and none can be written that would. Declaring a gap does not change what the
    # criterion checks (`digest` does not read `known_gap`), so a run that cannot ask "is this a
    # deadline?" has not contradicted the answer to "has it been watched going red?". Both verdicts
    # are therefore UNJUDGED and say why in the prover's own words; `reconcile` carries the standing
    # proof instead of reporting it as one that stopped proving.
    for label, observed in (("one whose mutation leaves it red", stays),
                            ("one with no mutation at all", none)):
        check(f"a declared gap that closes by an ADDITION is unjudged, not refuted — {label}",
              observed.unjudged and falsify_module.GAP_IS_ADDITIVE in observed.detail,
              f"unjudged={observed.unjudged}: {observed.detail}")

    standing = falsify_module.Proof("m0", "stays-red", stays.digest, falsify_module.RED_IN_THE_TREE,
                                    "-", "red unmutated, in the sandbox and in the repository")
    inventory = falsify_module.Inventory(proofs={("m0", "stays-red"): standing}, unproven={})
    check("and its recorded redness is CARRIED rather than reported as a proof that stopped proving",
          falsify_module.reconcile([stays], inventory) == [],
          falsify_module.reconcile([stays], inventory))

    # THE RATCHET IS NOT LOOSENED BY THAT. A gap with no standing record is still refused, because
    # `_record` carries an unjudged verdict only for a key the inventory already holds at the same
    # digest — which is what stops "declare it a gap" from being the way onto the ladder.
    refused = falsify_module._record([none], ("m0",))
    check("but a declared gap nothing has judged is still REFUSED a place on the ladder",
          len(refused) == 1 and "has not been shown able to fail" in refused[0], refused)
    target.unlink(missing_ok=True)


def test_falsifiability_red_in_the_tree(root: Path) -> None:
    """The second shape a proof comes in, and the control that keeps it from being a hole.

    A criterion that FAILS as written has been watched going red, which is the claim a mutation is
    there to demonstrate. What makes that a proof rather than an excuse is the TREE CONTROL: the
    sandbox is a copy of the TRACKED tree, so a criterion can be red in it for a reason that has
    nothing to do with its subject, and `test -d .git` is exactly such a criterion — true in the
    repository, false in every sandbox. Both directions are exercised, because the second one is
    what would otherwise let anything at all be recorded as proven by failing for the wrong reason.
    """
    sandbox = falsify_module.Sandbox.materialise(root / "tree")

    nowhere = _criterion("nowhere", "grep -q kTokenNoFileInThisRepositoryContains README.md")
    proof = falsify_module.prove(sandbox, "m0", nowhere)
    check("A CRITERION THAT IS RED IN THE SANDBOX AND RED IN THE REPOSITORY IS PROVEN BY THAT",
          proof.verdict == falsify_module.RED_IN_THE_TREE, f"{proof.verdict}: {proof.detail}")

    only_in_a_checkout = _criterion("checkout", "test -d .git")
    proof = falsify_module.prove(sandbox, "m0", only_in_a_checkout)
    check("and one that is red ONLY in the sandbox is refused, because the copy made it red",
          proof.verdict == falsify_module.UNPROVABLE
          and "the copy is what made it red" in proof.detail,
          f"{proof.verdict}: {proof.detail}")

    # THE REGRESSION THAT MADE THIS CONTROL LIE. `path` and `tiers` criteria carry no shell, and the
    # first draft of the tree control ran `bash -c criterion.run` for every kind — so an empty
    # command exited ZERO and every artefact and every tier claim was reported GREEN in the
    # repository, which is this module's own defect committed inside the control that exists to
    # catch it.
    missing = _criterion("missing", "", kind="path", path="docs/design/images/no-such-image*.png")
    code, _output = falsify_module.run_in_the_repository(missing, "", 30)
    check("the tree control EVALUATES a `path` criterion rather than running its empty shell",
          code != 0, f"exit {code} for an artefact that is not there")
    present = _criterion("present", "", kind="path", path="README.md")
    code, _output = falsify_module.run_in_the_repository(present, "", 30)
    check("and reports the artefact that is there as green", code == 0, f"exit {code}")

    # AND IT NEVER RUNS ITSELF. `plan-consistency` runs `just roadmap-test`, which runs this prover.
    runs_the_prover = _criterion("nested", "just roadmap-test")
    code, output = falsify_module.run_in_the_repository(runs_the_prover, "", 30)
    check("the tree control refuses a criterion that would start a second prover inside it",
          code < 0 and "cannot be controlled by running it again" in output, output)


def test_falsifiability_unjudged(root: Path) -> None:
    """A run that could not judge a criterion neither confirms a proof nor destroys one.

    The two cases are a build-backed proof re-run on a machine with no build, and a prover running
    inside another prover. Both are `not provable here`, and both must leave a standing proof
    exactly as it was — while a criterion with NO standing entry is still refused, because that is
    the direction that stops the eighth.
    """
    del root
    standing = falsify_module.Proof("m0", "built", "aaaa", falsify_module.RED_WITH_A_BUILD, "-",
                                    "red against build/dev")
    inventory = falsify_module.Inventory(proofs={("m0", "built"): standing})
    unjudged = falsify_module.Proof("m0", "built", "aaaa", falsify_module.UNPROVABLE, "-",
                                    "it needs a built tree", unjudged=True)
    check("a run with no build leaves a build-backed proof standing",
          not falsify_module.reconcile([unjudged], inventory),
          str(falsify_module.reconcile([unjudged], inventory)))
    judged = falsify_module.Proof("m0", "built", "aaaa", falsify_module.REFUTED, "-",
                                  "it still passes mutated")
    check("while a run that DID judge it and found nothing fails, as it always did",
          any("no longer proves" in finding
              for finding in falsify_module.reconcile([judged], inventory)))
    check("and an unjudged criterion nothing has ever judged is still refused",
          any("nothing in falsifiability.toml has judged" in finding
              for finding in falsify_module.reconcile([unjudged], falsify_module.Inventory())))

    # THE SHAPE OF A PROOF IS PART OF IT. A criterion recorded `red in the tree` that has gone GREEN
    # is a criterion whose evidence has lapsed: it owes an ordinary mutation proof now.
    was_red = falsify_module.Proof("m0", "gap", "bbbb", falsify_module.RED_IN_THE_TREE, "-", "red")
    inventory = falsify_module.Inventory(proofs={("m0", "gap"): was_red})
    now_proven = falsify_module.Proof("m0", "gap", "bbbb", falsify_module.PROVEN, "delete-path",
                                      "red under mutation")
    check("A CRITERION THAT WAS RED AND IS NOW GREEN HAS TO BE RE-JUDGED, not carried",
          any("was recorded as" in finding
              for finding in falsify_module.reconcile([now_proven], inventory)))


def test_falsifiability_by_mutating_the_tree(root: Path) -> None:
    """The fourth proof shape, and the guard that lets it touch the repository at all.

    WHAT IT IS FOR. A criterion whose subject is a COMPILED artefact cannot be judged in a source-only
    sandbox: with `--build-dir` it is run unmutated against a real tree, and when it PASSES the tool
    used to have nothing left to say. That is the shape of all seven unfalsifiable criteria this
    mechanism exists to end — green, with nothing able to turn it red — and sixteen of M11.a's and
    M11.b's criteria were sitting in it. `--mutate-the-tree` mutates the working tree instead, lets
    the criterion's own body rebuild over it, and requires RED and then GREEN again.

    WHAT IS CHECKED HERE IS THE DANGEROUS HALF: that the guard refuses a dirty tree, that it puts
    back what it changed, that `git status` agrees it did, that a criterion the mutation does not
    reach is REFUTED rather than proven, and that a tree whose restore was lost is recovered from git
    rather than left broken. Every one of those runs against a throwaway git repository of three
    files — this test never touches the repository it is running in.
    """
    tree_root = root / "checkout"
    tree_root.mkdir(parents=True, exist_ok=True)
    (tree_root / "subject.txt").write_text("the sampler is cySubjectToken here\n", encoding="utf-8")
    (tree_root / "bystander.txt").write_text("nothing to do with it\n", encoding="utf-8")
    for command in (["git", "init", "-q"], ["git", "add", "-A"],
                    ["git", "-c", "user.email=t@t", "-c", "user.name=t", "commit", "-qm", "fixture"]):
        subprocess.run(command, cwd=tree_root, check=True, capture_output=True)

    tree = falsify_module.WorkingTree(tree_root)
    check("the guard calls a freshly committed tree clean", not tree.dirty(), tree.dirty())

    (tree_root / "subject.txt").write_text("edited by somebody else\n", encoding="utf-8")
    check("AN EDIT BY SOMEBODY ELSE IS DRIFT FROM THE BASELINE, and the guard sees it",
          bool(tree.dirty()), "git called an edited tree unchanged")
    subprocess.run(["git", "checkout", "--", "."], cwd=tree_root, check=True, capture_output=True)

    before = (tree_root / "subject.txt").read_bytes()
    mutation = falsify_module.Mutation(verb="rename-token", target="subject.txt",
                                       token="cySubjectToken", derived=False)
    changed = tree.apply(mutation)
    check("the mutation reaches the working tree", changed == 1, f"{changed} file(s)")
    check("and while it is applied, git says so", bool(tree.dirty()))
    tree.restore()
    check("THE RESTORE PUTS THE BYTES BACK", (tree_root / "subject.txt").read_bytes() == before)
    check("and git — which was not involved in the bookkeeping — agrees the tree is clean",
          not tree.dirty(), tree.dirty())

    # THE PROOF ITSELF, over a criterion that reads the mutated file. `run_in_the_repository` takes
    # the root, so the three runs happen in the fixture checkout and not in this repository.
    finished = lambda verdict, mutated, detail, unjudged=False: falsify_module.Proof(  # noqa: E731
        "m0", "fixture", "aaaa", verdict, mutated, detail, 0.0, unjudged)
    reads_it = _criterion("reads-it", "grep -q cySubjectToken subject.txt")
    proof = falsify_module._prove_by_mutating_the_tree(reads_it, "build/none", mutation, tree,
                                                      finished)
    check("A CRITERION THAT GOES RED UNDER THE MUTATION AND GREEN AGAIN AFTER IT IS PROVEN",
          proof.verdict == falsify_module.PROVEN_BY_REBUILD, f"{proof.verdict}: {proof.detail}")
    check("and the tree is clean afterwards", not tree.dirty(), tree.dirty())

    ignores_it = _criterion("ignores-it", "grep -q nothing bystander.txt")
    proof = falsify_module._prove_by_mutating_the_tree(ignores_it, "build/none", mutation, tree,
                                                      finished)
    check("while one the mutation does not reach is REFUTED, not carried",
          proof.verdict == falsify_module.REFUTED, f"{proof.verdict}: {proof.detail}")
    check("and the tree is clean after that too", not tree.dirty(), tree.dirty())

    # A RESTORE THE TOOL LOST. `forget()` drops what it remembered, which is the worst case short of
    # the process dying: the recovery is git's, and it is checked rather than assumed.
    tree.apply(mutation)
    tree._files.forget()
    tree.restore()
    check("a tree whose remembered bytes were lost is recovered from git rather than left broken",
          not tree.dirty() and (tree_root / "subject.txt").read_bytes() == before, tree.dirty())

    # AND WORK THAT WAS IN FLIGHT BEFORE THE RUN IS NEVER DISCARDED TO ACHIEVE THAT. The recovery is
    # `git checkout`, which would happily throw an uncommitted edit away; it is offered only to paths
    # that were clean at the baseline, so a tree that starts modified — which is every tree a phase
    # editing this module works in — is restored to what it was, not to HEAD.
    (tree_root / "bystander.txt").write_text("somebody was in the middle of this\n", encoding="utf-8")
    in_flight = falsify_module.WorkingTree(tree_root)
    check("a file modified before the run is the baseline rather than drift",
          not in_flight.dirty() and len(in_flight.modified_at_the_baseline()) == 1,
          f"{in_flight.modified_at_the_baseline()}")
    in_flight.apply(mutation)
    in_flight._files.forget()
    in_flight.restore()
    check("and it survives even a restore that had to fall back on git",
          (tree_root / "bystander.txt").read_text(encoding="utf-8").startswith("somebody"),
          (tree_root / "bystander.txt").read_text(encoding="utf-8"))
    subprocess.run(["git", "checkout", "--", "."], cwd=tree_root, check=True, capture_output=True)

    # AND A FILE SOMEBODY ELSE CHANGED WHILE THE RUN WAS GOING IS LEFT ALONE. The recovery is
    # `git checkout`, which discards; a prover takes minutes and an editor does not stop for it, so
    # the recovery is aimed only at paths THIS mutation wrote to. Anything else is reported.
    elsewhere = falsify_module.WorkingTree(tree_root)
    elsewhere.apply(mutation)
    (tree_root / "bystander.txt").write_text("someone edited this mid-run\n", encoding="utf-8")
    try:
        elsewhere.restore()
        check("a file changed by somebody else mid-run is REPORTED, not discarded", False,
              "restore() returned quietly over a tree it had not put back")
    except falsify_module.TreeNotRestored as error:
        check("a file changed by somebody else mid-run is REPORTED, not discarded",
              "bystander.txt" in str(error), str(error))
    check("and it still has the bytes that other person wrote",
          (tree_root / "bystander.txt").read_text(encoding="utf-8").startswith("someone edited"),
          (tree_root / "bystander.txt").read_text(encoding="utf-8"))
    check("while the file the mutation DID write to was put back",
          (tree_root / "subject.txt").read_bytes() == before)
    subprocess.run(["git", "checkout", "--", "."], cwd=tree_root, check=True, capture_output=True)

    # A COMMIT TAKEN WHILE THE TREE WAS MUTATED IS THE ONE THING A RESTORE CANNOT UNDO, and this
    # repository met it on the very first run: an orchestrator snapshotting between phases caught a
    # renamed token in a test file and committed it. Afterwards the file on disk was right and HEAD
    # was wrong, which is the one direction `git status` calls clean — so HEAD is read before and
    # after, and a HEAD that moved is raised rather than reported.
    moved = falsify_module.WorkingTree(tree_root)
    moved.apply(mutation)
    subprocess.run(["git", "add", "-A"], cwd=tree_root, check=True, capture_output=True)
    subprocess.run(["git", "-c", "user.email=t@t", "-c", "user.name=t", "commit", "-qm", "snapshot"],
                   cwd=tree_root, check=True, capture_output=True)
    try:
        moved.restore()
        check("A COMMIT TAKEN DURING THE MUTATION WINDOW IS RAISED, not restored away", False,
              "restore() returned quietly with the mutation in the history")
    except falsify_module.TreeNotRestored as error:
        check("A COMMIT TAKEN DURING THE MUTATION WINDOW IS RAISED, not restored away",
              "HEAD MOVED" in str(error), str(error))
    check("and the bytes on disk are put back even so",
          (tree_root / "subject.txt").read_bytes() == before)

    # AND IT REFUSES TO RUN INSIDE ANOTHER PROVER, where two runs would mutate one tree.
    os.environ[NESTED_IN_THE_PROVER] = "1"
    try:
        check("--mutate-the-tree is refused inside another prover",
              "a prover is already running" in tree.unavailable(), tree.unavailable())
    finally:
        del os.environ[NESTED_IN_THE_PROVER]


def test_falsifiability_of_the_ladder(root: Path) -> None:
    """The real thing: every criterion on the ladder, proven or accounted for.

    This runs the prover — a sandboxed copy of the tracked tree, one mutation per criterion — rather
    than trusting the file, because a recorded verdict nothing re-earns is the shape of decay this
    whole mechanism exists to refuse. It costs about two seconds for six hundred criteria.
    """
    del root
    inventory = falsify_module.read_inventory()
    check("falsifiability.toml exists and has been written",
          bool(inventory.proofs or inventory.unproven),
          "no inventory: run `just roadmap-falsify --record --baseline`")
    observed = falsify_module.prove_the_ladder()
    check("the prover ran over the whole ladder", len(observed) > 500, f"{len(observed)} judged")
    proven = [proof for proof in observed if proof.verdict == falsify_module.PROVEN]
    check(f"{len(proven)} of {len(observed)} criteria have been shown to go red under a mutation "
          "the tooling applied itself", bool(proven))
    findings = falsify_module.reconcile(observed, inventory)
    check("every criterion on the ladder is either proven or on the list that only shrinks",
          not findings, "\n".join(findings[:20]))


#: `falsify.prove` sets this while it is running a criterion, and `plan-consistency` — a criterion of
#: every ledger on the ladder — runs THIS file. So this file runs inside the prover, inside a sandbox
#: that is a copy of the tracked tree and is not a git repository.
#:
#: WHAT THAT BREAKS, AND WHY THE ANSWER IS A SKIP RATHER THAN A FIX. The four tests below materialise
#: a sandbox of their own, which is `git ls-files` over a directory that has no `.git` — so they fail
#: for a reason that has nothing to do with what they check, and `plan-consistency` was RED in every
#: sandbox for that reason alone. Making the sandbox git-independent would only move the wall: the
#: ladder proof would then prove the ladder, reach `plan-consistency`, run this file again, and
#: descend without a bottom. A prover cannot prove the criterion that runs the prover, and the honest
#: shape of that is to say so out loud, once, at the one place it happens.
#:
#: THIS IS NOT A WAY TO SKIP THE LADDER. It fires only when a prover has set the variable, which no
#: pull request and no developer's `just roadmap-test` does; the line is printed rather than silent;
#: and what the criterion then proves is that `just roadmap-test` goes red when a ledger is broken,
#: which is the claim it makes.
NESTED_IN_THE_PROVER = "CY_FALSIFY"




# --- The scheduler: which criteria may run at the same time ----------------------------------------
#
# THE LEDGER RAN ON ONE CORE OF TWENTY-FOUR, and making it run on more of them is the only change in
# this tooling whose failure mode is a FLAKE rather than a wrong answer. A flake in a milestone
# ledger is worse than a slow ledger: a verdict nobody can reproduce is a verdict nobody can act on,
# and this project has spent whole phases chasing one. So the rules below are all of one shape —
# they are about what the scheduler REFUSES to do, not about how fast it is.


def _fake(needs=(), *, kind="command", run="", requires="") -> criteria_module.Criterion:
    return criteria_module.Criterion(
        id="x", describe="d", source="s", kind=kind, ci_job="j", run=run,
        requires=requires, reason="r" if requires else "", needs=list(needs))


class _Entry:
    """A stand-in for a PlanEntry: the scheduler reads `criterion` and `label` and nothing else."""

    def __init__(self, label: str, criterion: criteria_module.Criterion) -> None:
        self.label, self.criterion = label, criterion


def _observe(entries, jobs: int):
    """Run the entries under the scheduler and record what ever overlapped what.

    Returns (results, overlaps, peak): `overlaps` holds every pair that was running at the same time
    and should not have been, which is the property the whole module exists for.
    """
    import random
    import threading

    lock = threading.Lock()
    live: list[_Entry] = []
    overlaps: list[tuple[str, str]] = []
    peak = 0

    def evaluate(entry):
        nonlocal peak
        with lock:
            for other in live:
                if not schedule_module.independent(entry.criterion, other.criterion):
                    overlaps.append((entry.label, other.label))
            live.append(entry)
            peak = max(peak, len(live))
        time.sleep(random.uniform(0.002, 0.02))
        with lock:
            live.remove(entry)
        return entry.label

    results = schedule_module.run(entries, evaluate, jobs)
    return results, overlaps, peak


def test_scheduler_concurrency_is_opt_in(root: Path) -> None:
    """The ledger runs one criterion at a time unless somebody asks for more.

    THIS IS A REGRESSION TEST FOR A VERDICT, NOT FOR A SPEED. When `default_jobs()` returned
    `min(8, cores)`, M11.c's verification run reported five failures the sequential run before it
    did not have — `m2:nodes`, `m7:arbiter`, `m7:sky`, `m8b:animation`, `m8b:navigation` — and all
    five exit 0 when re-run alone. The cause is `tests/harness/src/budget.cpp`'s `stalled:` check,
    which compares wall clock against a CPU-derived ceiling and subtracts time spent waiting for a
    core but NOT time spent waiting for I/O or a page fault: one case spent 0.702 ms of CPU and
    0.000 ms on the runqueue and was failed for holding 2053.091 ms of wall clock against a
    227.106 ms ceiling, because the ledger's own scheduler was compiling beside it.

    A ledger that reports a failure the tree does not have is worse than a slow one, so the default
    returned to 1. Restoring a concurrent default without first teaching the harness about I/O
    waiting reintroduces exactly that, and this case is what refuses it.
    """
    previous = os.environ.pop("CY_LEDGER_JOBS", None)
    try:
        check("the ledger runs one criterion at a time unless asked for more",
              schedule_module.default_jobs() == 1,
              f"default_jobs() is {schedule_module.default_jobs()}, not 1 — concurrency is opt-in "
              f"until budget.cpp subtracts I/O waiting as well as runqueue contention")
        os.environ["CY_LEDGER_JOBS"] = "4"
        check("CY_LEDGER_JOBS still opts in to the pool",
              schedule_module.default_jobs() == 4,
              f"CY_LEDGER_JOBS=4 gave {schedule_module.default_jobs()}")
    finally:
        os.environ.pop("CY_LEDGER_JOBS", None)
        if previous is not None:
            os.environ["CY_LEDGER_JOBS"] = previous


def test_scheduler_rules(root: Path) -> None:
    """What the scheduler may and may not do with a set of criteria."""
    entries = [
        _Entry("a", _fake(["build:@"])), _Entry("b", _fake([])),
        _Entry("c", _fake(["build:@"])), _Entry("d", _fake([schedule_module.EXCLUSIVE])),
        _Entry("e", _fake([])), _Entry("f", _fake(["cargo"])),
        _Entry("g", _fake(["cargo"])), _Entry("h", _fake([])),
        _Entry("i", _fake(["build:@", "cargo"])), _Entry("j", _fake(["gpu"])),
        _Entry("k", _fake(["gpu"])), _Entry("l", _fake([])),
    ]
    order = [entry.label for entry in entries]

    for jobs in (2, 4, 8, 16):
        results, overlaps, peak = _observe(entries, jobs)
        check(f"results come back in ledger order with {jobs} at a time", results == order,
              f"got {results}")
        check(f"nothing runs beside something it shares a resource with, {jobs} at a time",
              not overlaps, f"overlapped: {overlaps}")
        check(f"the concurrency cap of {jobs} is respected", peak <= jobs, f"peak was {peak}")

    results, overlaps, peak = _observe(entries, 1)
    check("--jobs 1 is the sequential ledger: one at a time, in order",
          results == order and peak == 1 and not overlaps, f"peak {peak}, results {results}")

    # A criterion that runs alone is a BARRIER. Without that it would starve: the pool is always
    # busy, so a criterion needing the pool empty would never reach the front of it.
    alone = [_Entry(str(index), _fake([])) for index in range(6)]
    alone.insert(3, _Entry("alone", _fake([schedule_module.EXCLUSIVE])))
    _, overlaps, _ = _observe(alone, 8)
    check("a criterion that runs alone runs with nothing beside it",
          not overlaps, f"overlapped: {overlaps}")

    every_one_alone = [_Entry(str(index), _fake([schedule_module.EXCLUSIVE])) for index in range(5)]
    results, overlaps, peak = _observe(every_one_alone, 8)
    check("a ledger of nothing but exclusive criteria finishes, one at a time",
          results == [str(index) for index in range(5)] and peak == 1 and not overlaps)

    check("two criteria naming different build trees are independent",
          schedule_module.independent(_fake(["build:one"]), _fake(["build:two"])))
    check("two criteria naming the same build tree are not",
          not schedule_module.independent(_fake(["build:one"]), _fake(["build:one"])))
    check("an exclusive criterion is independent of nothing, not even an empty one",
          not schedule_module.independent(_fake([schedule_module.EXCLUSIVE]), _fake([])))


def test_scheduler_derivation(root: Path) -> None:
    """What a criterion's body shows it needs — and, far more important, what it does NOT show.

    Every case here is a body shape that appears in the real ledgers. The negative half is the half
    that matters: a body the tables cannot read has to come back UNKNOWN so that it runs alone, and
    a derivation that guessed instead would be indistinguishable from a correct one until the day it
    produced a flake.
    """
    derive = schedule_module.derive

    check("a recipe that only reads the tree holds nothing",
          derive(_fake(run="just quality-layers")) == ())
    check("a recipe that builds holds the ledger's build tree",
          derive(_fake(run="just build-engine")) == (schedule_module.DEFAULT_BUILD,))
    check("a test recipe holds the build tree too, because every test recipe builds first",
          derive(_fake(run="just test-unit -R abi")) == (schedule_module.DEFAULT_BUILD,))
    check("a body redirecting CY_BUILD_DIR to a named subdirectory holds THAT tree",
          derive(_fake(run='set -e; d="${CY_BUILD_DIR:+${CY_BUILD_DIR}/off-ml}"; '
                           'CY_BUILD_DIR="$d" just build-engine --profile dev -D CY_ML=OFF'))
          == ("build:off-ml",),
          str(derive(_fake(run='d="${CY_BUILD_DIR:+${CY_BUILD_DIR}/off-ml}"; '
                               'CY_BUILD_DIR="$d" just build-engine'))))
    check("${CY_BUILD_DIR:-build/dev} is the ledger's own tree, not a second one",
          derive(_fake(run='d="${CY_BUILD_DIR:-build/dev}"; just build-engine --profile dev'))
          == (schedule_module.DEFAULT_BUILD,))
    check("a sanitized build is a tree of its own",
          derive(_fake(run="just test-sanitize --sanitizer address --tests ecs"))
          == (schedule_module.SANITIZE_BUILD,))
    check("a Cargo criterion holds the Cargo target directory",
          "cargo" in (derive(_fake(run="just build-editor --profile dev")) or ()))
    check("a criterion requiring a device holds the device as well as its tree",
          derive(_fake(run="just build-engine", requires="gpu"))
          == (schedule_module.DEFAULT_BUILD, "gpu"))
    check("a path criterion runs a subprocess for nobody and holds nothing",
          derive(_fake(kind="path")) == ())
    check("a tiers criterion compares a record already in memory and holds nothing",
          derive(_fake(kind="tiers")) == ())

    for name, body in (
        ("a recipe the table does not know", "just some-recipe-nobody-declared"),
        ("a program the table does not know", "curl https://example.invalid"),
        ("a heredoc fed to an interpreter", "python3 - <<'X'\nimport os\nX\n"),
        ("a shell function of its own", "check() { grep -q x y; }; check"),
        ("a build directory built out of a loop variable",
         'base="${CY_BUILD_DIR:-build}"; for p in debug dev; do '
         'CY_BUILD_DIR="$base/agree-$p" just build-engine --profile "$p"; done'),
        ("a redirection into the working tree", "just build-engine > docs/out.txt"),
        ("a command that writes where its argument says", 'mkdir -p docs/design/images'),
        ("a recipe run through a program that runs its argument", "xargs just build-engine"),
    ):
        check(f"{name} is UNKNOWN, so the criterion runs alone", derive(_fake(run=body)) is None,
              f"derived {derive(_fake(run=body))!r} instead")

    # AN ARGUMENT CAN TURN A READER INTO A WRITER, and this is the case that says so.
    check("`just roadmap-status` reads the record and holds nothing",
          derive(_fake(run="just roadmap-status")) == ())
    check("but `--write-lists` rewrites the capability matrix, so that invocation runs alone",
          derive(_fake(run="just roadmap-status --write-lists")) is None)
    check("`just quality-abi --update` replaces the committed baseline, so it runs alone",
          derive(_fake(run="just quality-abi --update")) is None)
    check("and `just roadmap-debts` rewrites open-debts.md however it is called",
          derive(_fake(run="just roadmap-debts --check")) == (schedule_module.EXCLUSIVE,))

    check("an unknown body with no declaration runs alone",
          schedule_module.needs(_fake(run="curl https://example.invalid"))
          == (schedule_module.EXCLUSIVE,))
    check("a declaration wins over the derivation",
          schedule_module.needs(_fake(["build:mine"], run="curl https://example.invalid"))
          == ("build:mine",))


def test_scheduler_declarations(root: Path) -> None:
    """`needs` is a token from a closed vocabulary, checked where every other declaration is."""
    head = 'schema = 1\nid = "m0"\nname = "Ground"\n'
    body = ('[[criterion]]\nid = "x"\ndescribe = "d"\nsource = "s"\nkind = "recipe"\n'
            'run = "just quality-layers"\nci_job = "layering"\n')

    def load(name: str, needs: str):
        return criteria_module.load("m0", milestone_file(root, name, head + body + needs))

    check("a criterion may declare what it holds",
          load("good", 'needs = ["build:m1-bench", "net"]').criteria[0].needs
          == ["build:m1-bench", "net"])
    expect_error("a resource class nobody implements is rejected", criteria_module.CriteriaError,
                 lambda: load("bad-class", 'needs = ["quantum-computer"]'))
    expect_error("a resource named twice is rejected", criteria_module.CriteriaError,
                 lambda: load("twice", 'needs = ["cargo", "cargo"]'))
    expect_error("'needs' is a list, not a sentence", criteria_module.CriteriaError,
                 lambda: load("prose", 'needs = "it uses its own build directory"'))

    # THE COLLAPSE TAKES THE UNION. Two ledgers declaring the same check may know different things
    # about it; keeping only the first declarer's would silently drop the other's knowledge that the
    # check also binds a port, and the check would then run beside something that binds the same one.
    one = criteria_module.Criterion(id="a", describe="d", source="s", kind="recipe",
                                    ci_job="j", run="just test-unit", needs=["build:@"])
    other = criteria_module.Criterion(id="b", describe="d", source="s", kind="recipe",
                                      ci_job="j", run="just test-unit", needs=["net"])
    collapsed = criteria_module._collapse([("m0", one), ("m1", other)], "m1")
    check("collapsing two declarations of one check keeps BOTH resources",
          collapsed.criterion.needs == ["build:@", "net"], str(collapsed.criterion.needs))


def test_scheduler_tables_match_the_justfile(root: Path) -> None:
    """Every recipe RECIPE_NEEDS classifies still exists, and every recipe a ledger runs is in it.

    A stale row is the quiet failure here: rename a recipe and its row stops matching, so every
    criterion invoking it becomes underivable and runs alone. That is SAFE — which is exactly why
    nobody would notice, and why it is checked rather than trusted.
    """
    repository = HERE.parent.parent
    declared: set[str] = set()
    for source in [repository / "justfile", *sorted((repository / "just").glob("*.just"))]:
        for line in source.read_text(encoding="utf-8").splitlines():
            match = re.match(r"^([a-z_][a-z0-9_-]*)\s+[*a-z]|^([a-z_][a-z0-9_-]*):", line)
            if match:
                declared.add(match.group(1) or match.group(2))
    missing = sorted(name for name in schedule_module.RECIPE_NEEDS if name not in declared)
    check("every recipe the scheduler classifies is a recipe the justfile declares",
          not missing, f"no longer declared: {', '.join(missing)}")

    invoked: set[str] = set()
    stragglers: list[str] = []
    for entry in _every_criterion():
        recipes = {command[1] for command in schedule_module.commands(entry.criterion.run or "")
                   if command[0] == "just" and len(command) > 1}
        invoked |= recipes
        unknown = recipes - set(schedule_module.RECIPE_NEEDS)
        if unknown and not entry.criterion.needs and (
                schedule_module.needs(entry.criterion) != (schedule_module.EXCLUSIVE,)):
            stragglers.append(f"{entry.label} runs {', '.join(sorted(unknown))}")
    unclassified = sorted(name for name in invoked
                          if name not in schedule_module.RECIPE_NEEDS and not name.startswith("$"))
    check("a recipe a ledger runs but the scheduler cannot classify only costs concurrency",
          not stragglers,
          f"unclassified recipes: {', '.join(unclassified) or 'none'}\n"
          + "\n".join(stragglers))


_PLANS: dict[str, criteria_module.Plan] = {}


def _plan(identifier: str) -> criteria_module.Plan:
    """Every ledger's plan, built once. Building all twenty-two takes seconds, and three of the
    cases below want all of them."""
    if not _PLANS:
        permanent = gates_module.permanent_milestones(gates_module.load())
        for name in criteria_module.available():
            _PLANS[name] = criteria_module.build_plan(name, permanent)
    return _PLANS[identifier]


def _every_criterion():
    for identifier in criteria_module.available():
        yield from _plan(identifier).entries


def test_scheduler_over_the_real_ledgers(root: Path) -> None:
    """Every criterion in every ledger gets an answer, and the answers are not all 'run alone'.

    The second half is the one that would rot. `needs` is allowed to come back `exclusive` for any
    criterion at all, so a derivation that quietly stopped working — a regex that no longer matches,
    a table that fell out of step — would still be CORRECT and would simply make the ledger
    sequential again, which is the defect this whole change removes. So the floor is checked.
    """
    plan = _plan("m11c")
    resolved = {entry.label: schedule_module.needs(entry.criterion) for entry in plan.entries}

    check("every criterion in M11.c's ledger is given a resource set",
          all(isinstance(tokens, tuple) for tokens in resolved.values()))
    alone = [label for label, tokens in resolved.items()
             if tokens == (schedule_module.EXCLUSIVE,)]
    free = [label for label, tokens in resolved.items() if tokens == ()]
    check(f"the derivation still reads most of the corpus ({len(alone)} of {len(resolved)} run "
          f"alone)", len(alone) < len(resolved) // 2,
          f"{len(alone)} of {len(resolved)} criteria run alone; the derivation has stopped reading "
          "the ledger and the run is sequential again in all but name")
    check(f"the criteria that hold nothing can all run at once ({len(free)} of them)",
          len(free) >= 40, f"only {len(free)} criteria were found to hold nothing")

    # THE DIRECTION THAT MATTERS. The criteria that build and test share one tree, and if the
    # derivation ever stopped seeing that, they would run on top of each other in it — which is a
    # corrupted configure and a ctest reading another run's log, not a wrong answer that anybody
    # could trace back to here.
    by_tree = [entry for entry in plan.entries
               if schedule_module.DEFAULT_BUILD in resolved[entry.label]]
    check("the criteria that share the ledger's build tree take turns in it",
          len(by_tree) > 100 and not any(
              schedule_module.independent(one.criterion, other.criterion)
              for one, other in zip(by_tree, by_tree[1:])),
          f"{len(by_tree)} criteria named the ledger's build tree")

    broken = [entry.label for entry in _every_criterion()
              if not isinstance(schedule_module.needs(entry.criterion), tuple)]
    check(f"every ledger under milestones/ resolves every criterion's needs "
          f"({len(criteria_module.available())} ledgers)", not broken,
          f"unresolved: {', '.join(broken)}")

    # AND THE REAL PLAN IS RUN THROUGH THE REAL SCHEDULER. The cases above check the rules against
    # fixtures and the derivation against the corpus; this one puts the corpus's OWN resource graph —
    # 440 criteria, their build trees, their devices, their barriers — through `schedule.run` and
    # asserts that nothing ever ran beside something it shares a resource with. It evaluates nothing,
    # so it costs a second; `ledger_equivalence.py` is what runs the criteria themselves.
    results, overlaps, peak = _observe(
        [_Entry(entry.label, entry.criterion) for entry in plan.entries], 8)
    check("M11.c's whole plan schedules without one criterion running beside something it shares "
          "a resource with", not overlaps, f"overlapped: {overlaps[:5]}")
    check("and it comes back in ledger order",
          results == [entry.label for entry in plan.entries])
    check("and it did use the machine: more than one criterion ran at once",
          peak > 1, f"peak concurrency was {peak}")


def test_ledger_report_order_is_the_ledger_order(root: Path) -> None:
    """The report is part of the contract: same order, same text, whatever order results arrive in.

    A reader compares one ledger run against the previous one BY EYE, line by line. A parallel run
    that reported the same verdicts in completion order would be a regression even though every
    verdict was right, so the reporting path holds a finished result back until every earlier one
    has been printed — and `ledger_equivalence.py` compares two real runs on exactly this basis.
    """
    import random

    entries = [_Entry(f"m0:c{index}", _fake([] if index % 3 else ["build:@"]))
               for index in range(24)]

    printed: list[int] = []

    def evaluate(entry):
        time.sleep(random.uniform(0.001, 0.02))
        return entry.label

    schedule_module.run(entries, evaluate, 8,
                        finished=lambda index, entry, result: printed.append(index))
    check("results are handed to the reporter as they finish, not in order",
          printed != sorted(printed) or len(entries) < 4,
          "every result arrived in order, so this case proves nothing about reordering")

    # And the reporting rule on top of that: roadmap._evaluate_plan buffers until the prefix is
    # complete. The rule is re-implemented here in three lines because that is the whole of it.
    blocks: dict[int, str] = {}
    emitted: list[str] = []
    following = 0
    for index in printed:
        blocks[index] = f"==> {entries[index].label}"
        while following in blocks:
            emitted.append(blocks.pop(following))
            following += 1
    check("the report comes out in ledger order regardless",
          emitted == [f"==> {entry.label}" for entry in entries], str(emitted[:5]))



def _nested() -> bool:
    return bool(os.environ.get(NESTED_IN_THE_PROVER))


def main() -> int:
    if _nested():
        print(f"note: {NESTED_IN_THE_PROVER} is set — this self-test is running INSIDE the prover, "
              "so the four cases that materialise a sandbox of their own are not run here. They run "
              "on every ordinary `just roadmap-test`.")
    with tempfile.TemporaryDirectory(prefix="cy-roadmap-selftest-") as directory:
        root = Path(directory)
        test_drift(_area(root, "drift"))
        test_record_rules(_area(root, "record"))
        test_criteria(_area(root, "criteria"))
        test_evaluates(_area(root, "evaluates"))
        test_declared_gaps(_area(root, "declared-gaps"))
        test_exit_tiers(_area(root, "tiers"))
        test_milestone_ladder(_area(root, "ladder"))
        test_flat_ledger(_area(root, "flat"))
        test_ladder_rungs(_area(root, "rungs"))
        test_requirements(_area(root, "requirements"))
        test_gates(_area(root, "gates"))
        test_plan_documents(_area(root, "plan"))
        test_plan_checks_can_fail(_area(root, "plan-negative"))
        test_just_arguments(_area(root, "just-arguments"))
        test_matrix_requirement_counts(_area(root, "reqs-column"))
        test_falsifiability_rules(_area(root, "falsify-rules"))
        test_absent_recipe_rule(_area(root, "absent-recipe"))
        test_requirements_coverage(_area(root, "requirements-coverage"))
        test_a_wrapped_case_name_is_still_that_case(_area(root, "wrapped-case"))
        test_falsifiability_reads_a_redirection(_area(root, "falsify-redirect"))
        test_falsifiability_digest(_area(root, "falsify-digest"))
        test_falsifiability_reconciliation(_area(root, "falsify-reconcile"))
        test_scheduler_concurrency_is_opt_in(_area(root, "scheduler-concurrency"))
        test_scheduler_rules(_area(root, "scheduler"))
        test_scheduler_derivation(_area(root, "scheduler-derivation"))
        test_scheduler_declarations(_area(root, "scheduler-declarations"))
        test_scheduler_tables_match_the_justfile(_area(root, "scheduler-tables"))
        test_scheduler_over_the_real_ledgers(_area(root, "scheduler-corpus"))
        test_ledger_report_order_is_the_ledger_order(_area(root, "scheduler-report"))
        if not _nested():
            test_falsifiability_ledger_blind(_area(root, "falsify-blind"))
            test_falsifiability_declared_mutations(_area(root, "falsify-verbs"))
            test_falsifiability_in_the_ci_environment(_area(root, "falsify-ci-env"))
            test_falsifiability_of_a_declared_gap(_area(root, "falsify-gap"))
            test_falsifiability_red_in_the_tree(_area(root, "falsify-red"))
            test_falsifiability_by_mutating_the_tree(_area(root, "falsify-tree"))
            test_falsifiability_unjudged(_area(root, "falsify-unjudged"))
            test_falsifiability_of_the_ladder(_area(root, "falsify-ladder"))
    passed = len(_cases) - len(_failures)
    print(f"\nselftest: {passed}/{len(_cases)} passed")
    return 1 if _failures else 0


if __name__ == "__main__":
    sys.exit(main())
