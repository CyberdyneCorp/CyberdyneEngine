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

# The fewest criteria each milestone's ledger may carry, from its row in docs/ROADMAP.md and its
# section 6. A floor rather than an equality: a ledger that grows a criterion is a ledger that got
# better, and one that loses several has quietly stopped covering its milestone. `test_criteria`
# requires every ledger under milestones/ to appear here, so this table cannot fall behind them.
MINIMUM_CRITERIA = {"m0": 10, "m1": 15, "m2": 20, "m3": 20, "m4": 20, "m5": 20, "m5b": 20}


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
    _check_collapse_rules()


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
        test_flat_ledger(_area(root, "flat"))
        test_requirements(_area(root, "requirements"))
        test_gates(_area(root, "gates"))
    passed = len(_cases) - len(_failures)
    print(f"\nselftest: {passed}/{len(_cases)} passed")
    return 1 if _failures else 0


if __name__ == "__main__":
    sys.exit(main())
