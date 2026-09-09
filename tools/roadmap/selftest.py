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

import os
import re
import subprocess
import sys
import tempfile
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import criteria as criteria_module  # noqa: E402
import gates as gates_module  # noqa: E402
import plan as plan_module  # noqa: E402
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

# The fewest criteria each milestone's ledger may carry, from its row in docs/ROADMAP.md and its
# section 6. A floor rather than an equality: a ledger that grows a criterion is a ledger that got
# better, and one that loses several has quietly stopped covering its milestone. `test_criteria`
# requires every ledger under milestones/ to appear here, so this table cannot fall behind them.
MINIMUM_CRITERIA = {"m0": 10, "m1": 15, "m2": 20, "m3": 20, "m4": 20, "m5": 20, "m5b": 20,
                    "m6": 26, "m7": 32, "m8a": 26}


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
    """
    for column in ("M5.5", "M8.a", "M8.b"):
        check(f"the matrix header admits {column}",
              plan_module.milestone_id(column) in record_module.MILESTONES,
              f"{column} -> {plan_module.milestone_id(column)!r}")

    matrix = plan_module.read_matrix()
    for rung in ("m5b", "m8a", "m8b"):
        check(f"{rung} is a column the matrix reader returns", rung in matrix.milestones,
              f"columns are {matrix.milestones}")

    sections = plan_module.read_sections()
    for rung in ("m5b", "m8a", "m8b"):
        check(f"{rung} is a section the roadmap reader returns", rung in sections,
              f"sections are {sorted(sections)}")

    summary = plan_module.read_load_summary()
    for rung in ("m5b", "m8a", "m8b"):
        check(f"{rung} is a row the load table reader returns", rung in summary,
              f"rows are {sorted(summary)}")


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
                                   ("m8b", "m8a", "m9")):
        check(f"{inserted} sits between {below} and {above} on the ladder",
              inserted in order and below in order and above in order
              and order.index(below) < order.index(inserted) < order.index(above),
              f"order is {order}")

    # The property the rung exists for: a ledger inherits what is BELOW it and nothing above.
    check("a rung past the end of the ladder is not silently assigned a position",
          criteria_module.rung("m8a") < criteria_module.rung("m8b")
          < criteria_module.rung("m9"),
          f"m8a={criteria_module.rung('m8a')} m8b={criteria_module.rung('m8b')} "
          f"m9={criteria_module.rung('m9')}")


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


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="cy-roadmap-selftest-") as directory:
        root = Path(directory)
        test_drift(_area(root, "drift"))
        test_record_rules(_area(root, "record"))
        test_criteria(_area(root, "criteria"))
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
    passed = len(_cases) - len(_failures)
    print(f"\nselftest: {passed}/{len(_cases)} passed")
    return 1 if _failures else 0


if __name__ == "__main__":
    sys.exit(main())
