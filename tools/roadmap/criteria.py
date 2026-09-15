"""Milestone exit criteria: the data under tools/roadmap/milestones/, and how one is evaluated.

Task 4.3.3 and 4.3.4. `delivery-roadmap` requires a milestone's exit criteria to be executable
checks rather than judgements, and requires every milestone to be closable by one recipe that runs
its full criteria set. M1 through M11 each add their own criteria, so the criteria are data — one
TOML file per milestone — and this module is the only thing that knows how to run them.

Three kinds of check, which is all the specification's definition allows: a recipe that exits zero,
a command that exits zero, and a committed artefact that is compared — here, the status record's
tiers, since a milestone that does not update the record has not closed.

**Nothing is silently skipped.** Every criterion names the continuous-integration job that runs it,
whether or not this machine can evaluate it. A criterion that cannot run here — another operating
system, no display, no GPU — says so, names that job, and is counted separately in the summary.

**A ledger is flat, and every distinct criterion in it runs once.** `build_plan` below is that rule:
it merges the permanent set — the criteria of every milestone whose gate is already green — with the
milestone's own, collapses the declarations that do the same work, and hands back one entry per
distinct check. Chaining was the previous implementation: each ledger's first criterion ran the
previous milestone's recipe, so closing M4 ran M3's ledger, which ran M2's, which ran M1's, which
ran M0's. That is measured in the change that removed it — one run of M4's ledger was 118 criterion
evaluations over 91 distinct checks, 27 of them re-running something another ledger had already run,
with `four-profiles`, a full four-configuration build and test, executed four times because four
ledgers declared it. The same run is now 87 evaluations, one per distinct check. Re-running one
criterion n times also multiplies its failure probability by n, and a marginal test in this
repository duly became a flake that failed four ledgers at once: deduplication is a correctness
property rather than an optimisation.

Governed by: delivery-roadmap (Milestone exit criteria are executable, Milestone gates do not
regress, Forbidden roadmap patterns).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import time
import tomllib
from dataclasses import dataclass, field, replace
from pathlib import Path

from record import MILESTONES, REPO_ROOT, TIERS, Entry

MILESTONES_DIR = Path(__file__).resolve().parent / "milestones"
SCHEMA = 1
KINDS = ("recipe", "command", "path", "tiers")
#: The mutations `falsify.py` knows how to APPLY. A criterion declares one of these when the tooling
#: cannot derive a mutation from the criterion's own text; what it declares is the verb and the
#: target, never a sentence, because a sentence is what seven unfalsifiable criteria were written in.
MUTATIONS = ("delete-path", "delete-lines", "rename-token", "truncate", "lower-tiers")
FALSIFIES_KEYS = frozenset({"mutate", "target", "token", "note"})
#: What a `where = "ci"` criterion declares so that the prover can judge it HERE: the command that
#: builds the environment continuous integration hands it, and the mutation of that environment which
#: must turn the criterion red. See `_check_ci_proof`.
CI_PROOF_KEYS = frozenset({"provide", "mutate", "target", "token", "note"})
WHERE = ("local", "ci")
REQUIREMENTS = ("display", "gpu")
DEFAULT_TIMEOUT_S = 1800

CRITERION_KEYS = frozenset(
    {"id", "describe", "source", "kind", "run", "path", "expect_tiers", "where", "ci_job",
     "requires", "reason", "timeout_s", "known_gap", "known_gap_closes", "falsifies", "evaluates",
     "ci_proof"}
)
MILESTONE_KEYS = frozenset({"schema", "id", "name", "artefact", "notes", "criterion"})

OK, FAILED, NOT_EVALUATED = "ok", "failed", "not evaluated"


class CriteriaError(Exception):
    """Criteria that cannot be run as written."""


@dataclass(frozen=True)
class Criterion:
    id: str
    describe: str
    source: str
    kind: str
    ci_job: str
    run: str = ""
    path: str = ""
    expect_tiers: dict = field(default_factory=dict)
    where: str = "local"
    requires: str = ""
    reason: str = ""
    timeout_s: int = DEFAULT_TIMEOUT_S
    #: A gap this milestone shipped KNOWINGLY, in the criterion's own words. The criterion still
    #: runs on every ledger evaluation and its failure is still printed; what it does not do is make
    #: the milestone's permanent gate red forever. See `is_declared_gap` and `_summarise`.
    known_gap: str = ""
    #: The milestone that must close it. Required with `known_gap`, must be a rung ABOVE the one
    #: declaring it, and it is what makes a gap a deadline rather than a shrug.
    known_gap_closes: str = ""
    #: The mutation that must turn this criterion RED, as data `falsify.py` applies itself. Declared
    #: only where the tooling cannot derive one from the criterion's own text; `falsify.py` derives
    #: the mutation for a `path` criterion, a `tiers` criterion and a text search without being told.
    falsifies: dict = field(default_factory=dict)
    #: The capability ROWS this criterion evaluates — a machine-readable declaration, not prose.
    #:
    #: WHY IT IS NOT `source`. M11.a's guard over the four unevaluated rows asked whether the row's
    #: name appeared in some criterion's `source`, and M11's gate refuted it in one line: it "cannot
    #: detect the deletion of two of the four evaluators". `source` is a citation — eight criteria
    #: name `testing-and-quality` because that specification governs them, and seven name
    #: `developer-workflow-and-just` — so deleting the criterion that actually EVALUATES the row left
    #: the guard green with seven bystanders standing in for it. A substring of a citation is not a
    #: declaration; this field is, it is declared by exactly the criterion that does the evaluating,
    #: and `evaluators` below is the only way to ask who they are.
    evaluates: list = field(default_factory=list)
    #: HOW A `where = "ci"` CRITERION IS JUDGED ON A MACHINE THAT CANNOT EVALUATE IT.
    #:
    #: `falsify.prove` refuses to judge a criterion this host cannot evaluate, and it is right to:
    #: `just test-determinism --compare-legs` fails on a machine with one architecture, and reading
    #: that as "watched going red" would be a verdict about the laptop rather than about the
    #: repository. What it left behind was a criterion NOBODY had shown could fail — two of M11.a's
    #: seventy, and M11's repair gate called that out as dispositive.
    #:
    #: The missing piece was never the criterion; it was the ENVIRONMENT. What CI supplies to
    #: `m11a:lockstep-agrees-across-architectures` is a directory of digests published by several
    #: legs, and that directory is something this repository can construct: `tools/ci/cross_leg_audit`
    #: already writes well-formed legs from the publisher's own field list. So the criterion declares
    #: the command that CONSTRUCTS that environment and the mutation of it that must turn the
    #: criterion red, and the prover runs the criterion's own body, verbatim, three times against it.
    #:
    #: THIS IS NOT A LICENCE TO PASS. `provide` is executed, not read; the criterion has to go GREEN
    #: against what it produced, RED under the mutation, and GREEN again once restored. A `provide`
    #: that supplied nothing would leave the criterion red at its positive control and the verdict is
    #: `not provable here`, exactly as before. And it is refused on `requires` (a GPU, a display): no
    #: command constructs a graphics device, and pretending otherwise is the defect upside down.
    ci_proof: dict = field(default_factory=dict)

    @property
    def is_declared_gap(self) -> bool:
        return bool(self.known_gap)

    @property
    def command(self) -> str:
        """What a person would type to check this criterion, or a description when it is not one."""
        if self.kind in ("recipe", "command"):
            return self.run
        if self.kind == "path":
            return f"a file matching {self.path}"
        return "the status record is at this milestone's exit tiers"


@dataclass(frozen=True)
class Milestone:
    id: str
    name: str
    artefact: str
    notes: tuple[str, ...]
    criteria: tuple[Criterion, ...]


@dataclass(frozen=True)
class Result:
    criterion: Criterion
    status: str
    detail: str
    seconds: float = 0.0
    output: str = ""


def available() -> tuple[str, ...]:
    return tuple(sorted(path.stem for path in MILESTONES_DIR.glob("*.toml")))


def load(milestone_id: str, directory: Path = MILESTONES_DIR) -> Milestone:
    """Read one milestone's criteria, validating everything a run would otherwise discover late."""
    identifier = milestone_id.strip().lower()
    source = directory / f"{identifier}.toml"
    if not source.is_file():
        known = ", ".join(sorted(path.stem for path in directory.glob("*.toml"))) or "none"
        raise CriteriaError(f"no criteria for milestone '{milestone_id}'. Known milestones: {known}")

    with source.open("rb") as handle:
        document = tomllib.load(handle)
    _reject_unknown(document.keys(), MILESTONE_KEYS, f"{source.name}: milestone")
    if document.get("schema") != SCHEMA:
        raise CriteriaError(f"{source.name}: schema is {document.get('schema')!r}, expected {SCHEMA}")
    if document.get("id") != identifier:
        raise CriteriaError(f"{source.name}: declares id {document.get('id')!r}, not '{identifier}'")

    criteria = tuple(_criterion(table, source.name) for table in document.get("criterion", ()))
    if not criteria:
        raise CriteriaError(f"{source.name}: a milestone with no criteria cannot be closed")
    _reject_duplicates(criteria, source.name)
    return Milestone(
        id=identifier,
        name=str(document.get("name", identifier.upper())),
        artefact=str(document.get("artefact", "")),
        notes=tuple(document.get("notes", ())),
        criteria=criteria,
    )


def _reject_unknown(keys, allowed: frozenset[str], where: str) -> None:
    unknown = sorted(set(keys) - allowed)
    if unknown:
        raise CriteriaError(f"{where}: unknown key(s) {', '.join(unknown)}")


def _reject_duplicates(criteria: tuple[Criterion, ...], source: str) -> None:
    seen: set[str] = set()
    for criterion in criteria:
        if criterion.id in seen:
            raise CriteriaError(f"{source}: criterion '{criterion.id}' is declared twice")
        seen.add(criterion.id)


def _criterion(table: dict, source: str) -> Criterion:
    where = f"{source}: criterion '{table.get('id', '<unnamed>')}'"
    _reject_unknown(table.keys(), CRITERION_KEYS, where)
    for key in ("id", "describe", "source", "kind", "ci_job"):
        if not str(table.get(key, "")).strip():
            raise CriteriaError(f"{where}: '{key}' is required — every criterion is checked in CI")
    _check_kind(table, where)
    _check_scope(table, where)
    _check_falsifies(table, where)
    _check_ci_proof(table, where)
    _check_evaluates(table, where)
    return Criterion(**{key: value for key, value in table.items()})


def _check_kind(table: dict, where: str) -> None:
    """A criterion carries what its kind needs to run, and nothing runs a kind this tool invented."""
    kind = table["kind"]
    if kind not in KINDS:
        raise CriteriaError(f"{where}: kind {kind!r} is not one of {', '.join(KINDS)}")
    if kind in ("recipe", "command") and not table.get("run"):
        raise CriteriaError(f"{where}: kind {kind!r} needs 'run'")
    if kind == "recipe" and not str(table.get("run", "")).startswith("just "):
        raise CriteriaError(f"{where}: kind 'recipe' runs a just recipe; use kind 'command' instead")
    if kind == "path" and not table.get("path"):
        raise CriteriaError(f"{where}: kind 'path' needs 'path'")
    if kind == "tiers" and not table.get("expect_tiers"):
        raise CriteriaError(f"{where}: kind 'tiers' needs an [criterion.expect_tiers] table")


def _check_scope(table: dict, where: str) -> None:
    """Where the criterion can be evaluated, and why it cannot be evaluated everywhere."""
    if table.get("where", "local") not in WHERE:
        raise CriteriaError(f"{where}: 'where' is one of {', '.join(WHERE)}")
    if table.get("requires", "") and table["requires"] not in REQUIREMENTS:
        raise CriteriaError(f"{where}: 'requires' is one of {', '.join(REQUIREMENTS)}")
    # A criterion this machine cannot evaluate has to say why, or the report reads as a silent cap.
    if (table.get("where") == "ci" or table.get("requires")) and not table.get("reason"):
        raise CriteriaError(f"{where}: a criterion not evaluated everywhere needs a 'reason'")
    _check_known_gap(table, where)


def _check_known_gap(table: dict, where: str) -> None:
    """A gap a milestone ships knowingly is declared, dated, and still run.

    THE PROBLEM THIS SOLVES, AND IT WAS FOUND BY M8.c's CLOSING GATE RATHER THAN PREDICTED.
    `m8c:steam-audio-configures` is a criterion M8.c declared **expecting it to fail**, deliberately,
    so that the gap would keep saying so instead of quietly disappearing from the plan — which is
    the right instinct and is what `delivery-roadmap` asks for. But `_summarise` returns a non-zero
    exit for any failure, `milestone-m8c` is a permanent merge gate the moment it goes green, and
    ci.yml runs `just roadmap-milestone m8c`. Flipping that gate green would therefore have shipped
    a continuous-integration job that can never pass, which is the sibling of the forbidden pattern
    "a milestone gate disabled rather than fixed or explicitly superseded".

    The answer is not to delete the criterion and not to override the whole gate — an override is
    per-gate and would hide two hundred and fifty green checks to excuse one. It is to let a
    criterion say, in data, "this milestone shipped without me, here is why, and here is the rung
    that must close it". The criterion still RUNS every time and its failure is still PRINTED; what
    changes is only whether one declared gap makes a gate red forever.

    AND THE MECHANISM CANNOT ROT, because the interesting direction is the other one: a declared gap
    that PASSES is a ledger failure, reported as "the gap is closed; delete the declaration". A
    marker that outlived its gap would otherwise be the next thing nobody notices.
    """
    gap = str(table.get("known_gap", "")).strip()
    closes = str(table.get("known_gap_closes", "")).strip().lower()
    if not gap and not closes:
        return
    if not gap:
        raise CriteriaError(f"{where}: 'known_gap_closes' without 'known_gap' says a deadline with "
                            "nothing behind it")
    if not closes:
        raise CriteriaError(f"{where}: 'known_gap' needs 'known_gap_closes' — a gap with no rung "
                            "that must close it is a shrug, not a plan")
    if closes not in MILESTONES:
        raise CriteriaError(f"{where}: 'known_gap_closes' is {closes!r}, which is not a milestone; "
                            f"they are {', '.join(MILESTONES)}")


def _check_falsifies(table: dict, where: str) -> None:
    """A declared mutation is a verb and a target the tooling can apply, or it is not declared.

    THE POINT IS THAT PROSE IS NOT ACCEPTED. `falsify.py` runs this mutation against a sandbox copy
    of the tree and requires the criterion to go red; a field it cannot execute would be a sentence
    about falsifiability rather than a demonstration of it, which is the defect being fixed.
    """
    declared = table.get("falsifies")
    if declared is None:
        return
    if not isinstance(declared, dict):
        raise CriteriaError(f"{where}: 'falsifies' is a table, not {type(declared).__name__}")
    _reject_unknown(declared.keys(), FALSIFIES_KEYS, f"{where}: falsifies")
    verb = str(declared.get("mutate", "")).strip()
    if verb not in MUTATIONS:
        raise CriteriaError(f"{where}: falsifies.mutate is {verb!r}, not one of "
                            f"{', '.join(MUTATIONS)} — a mutation nothing can apply proves nothing")
    if verb != "lower-tiers" and not str(declared.get("target", "")).strip():
        raise CriteriaError(f"{where}: falsifies.mutate = {verb!r} needs a 'target' path or glob")
    if verb in ("delete-lines", "rename-token") and not str(declared.get("token", "")).strip():
        raise CriteriaError(f"{where}: falsifies.mutate = {verb!r} needs the 'token' it removes")


def _check_ci_proof(table: dict, where: str) -> None:
    """The environment CI supplies, as a command the tooling runs — and the mutation of it.

    Same rule as `_check_falsifies` and for the same reason: what is declared is executed, so it is a
    command and a verb rather than a sentence. The two extra rules here are about SCOPE.

    A `ci_proof` belongs only to a criterion whose `where` is `ci`. On a `requires = "gpu"` or
    `requires = "display"` criterion it would be a claim that a shell command can conjure a graphics
    device, which is the defect this whole mechanism exists to refuse, wearing the mechanism's own
    clothes. And on an ordinary criterion it is dead weight: the sandbox already runs those.
    """
    declared = table.get("ci_proof")
    if declared is None:
        return
    if not isinstance(declared, dict):
        raise CriteriaError(f"{where}: 'ci_proof' is a table, not {type(declared).__name__}")
    _reject_unknown(declared.keys(), CI_PROOF_KEYS, f"{where}: ci_proof")
    if table.get("where") != "ci":
        raise CriteriaError(f"{where}: 'ci_proof' declares the environment CI supplies, so it "
                            "belongs only to a criterion with where = \"ci\"")
    if table.get("requires"):
        raise CriteriaError(f"{where}: 'ci_proof' may not stand in for requires = "
                            f"{table['requires']!r} — no command constructs a device or a display")
    if not str(declared.get("provide", "")).strip():
        raise CriteriaError(f"{where}: ci_proof needs 'provide', the command that builds the "
                            "environment continuous integration hands this criterion")
    verb = str(declared.get("mutate", "")).strip()
    if verb not in MUTATIONS:
        raise CriteriaError(f"{where}: ci_proof.mutate is {verb!r}, not one of "
                            f"{', '.join(MUTATIONS)} — a mutation nothing can apply proves nothing")
    if verb != "lower-tiers" and not str(declared.get("target", "")).strip():
        raise CriteriaError(f"{where}: ci_proof.mutate = {verb!r} needs a 'target' path or glob")
    if verb in ("delete-lines", "rename-token") and not str(declared.get("token", "")).strip():
        raise CriteriaError(f"{where}: ci_proof.mutate = {verb!r} needs the 'token' it removes")


def _check_evaluates(table: dict, where: str) -> None:
    """`evaluates` is a list of capability rows, and a row is named once.

    The shape is all that is checked here: whether the rows are real capabilities is the RECORD's
    question, and `criteria.load` does not read the record. A row misspelt here does not quietly
    pass — it leaves the real row with no evaluator, which is exactly what the guard over the four
    rows fails on.
    """
    declared = table.get("evaluates")
    if declared is None:
        return
    if not isinstance(declared, list) or not declared:
        raise CriteriaError(f"{where}: 'evaluates' is a non-empty list of capability rows")
    seen: set[str] = set()
    for row in declared:
        if not isinstance(row, str) or not row.strip():
            raise CriteriaError(f"{where}: 'evaluates' holds capability names, not {row!r}")
        if row in seen:
            raise CriteriaError(f"{where}: 'evaluates' names {row!r} twice")
        seen.add(row)


def evaluators(entries, row: str) -> list:
    """The plan entries whose criterion DECLARES that it evaluates this row.

    One function so that no caller re-invents the question as a substring search over prose, which
    is the defect this field exists to end.
    """
    return [entry for entry in entries if row in getattr(entry.criterion, "evaluates", ())]


# --- The flat, deduplicated plan ------------------------------------------------------------------


@dataclass(frozen=True)
class PlanEntry:
    """One distinct check in a ledger, and every milestone that declared it."""

    criterion: Criterion
    declared_by: tuple[str, ...]
    permanent: bool

    @property
    def label(self) -> str:
        """`m1:four-profiles`. The id alone is ambiguous once several ledgers are merged."""
        return f"{self.declared_by[0]}:{self.criterion.id}"


@dataclass(frozen=True)
class Plan:
    """What one run of a milestone's ledger evaluates: each distinct criterion, exactly once."""

    milestone: Milestone
    ledgers: tuple[str, ...]
    entries: tuple[PlanEntry, ...]
    declarations: int

    @property
    def deduplicated(self) -> int:
        """Declarations that a chaining ledger would have executed a second time."""
        return self.declarations - len(self.entries)

    @property
    def inherited(self) -> tuple[PlanEntry, ...]:
        """The permanent set: criteria an already-closed milestone declared first."""
        return tuple(entry for entry in self.entries if entry.permanent)

    @property
    def own(self) -> tuple[PlanEntry, ...]:
        """The criteria this milestone is the first to declare."""
        return tuple(entry for entry in self.entries if not entry.permanent)


def fingerprint(criterion: Criterion) -> tuple:
    """What makes two declarations the same check: the work, and the conditions it runs under.

    The id is deliberately not part of it. `sample-recipe` names a different sample in M2, M3 and
    M4, so collapsing the three by id would drop two milestones' closing artefacts; and `specs`
    running `just quality-specs` is the same check whichever ledger declared it. `timeout_s` is not
    part of it either — a budget is not a check — and `_collapse` keeps the most generous one.
    """
    if criterion.kind == "tiers":
        work: object = tuple(sorted(criterion.expect_tiers.items()))
    elif criterion.kind == "path":
        work = criterion.path
    else:
        work = criterion.run
    return (criterion.kind, work, criterion.where, criterion.requires)


def rung(identifier: str) -> int:
    """Where a milestone sits on the ladder. Text order is wrong: 'm10' sorts before 'm2'."""
    return MILESTONES.index(identifier) if identifier in MILESTONES else len(MILESTONES)


def ladder_order(identifiers) -> tuple[str, ...]:
    return tuple(sorted(set(identifiers), key=lambda identifier: (rung(identifier), identifier)))


def build_plan(milestone_id: str, permanent=(), directory: Path = MILESTONES_DIR) -> Plan:
    """The flat evaluation plan for one milestone: the permanent set once, plus its own criteria.

    `permanent` names the milestones whose criteria have already joined the permanent set — in
    `gates.toml`, a `class = "milestone"` gate at `state = "green"`. Only the ones BELOW this
    milestone on the ladder are inherited: a closed milestone's ledger has to keep meaning its own
    criteria, or `milestone-m0` — a permanent merge gate — would go red for something M4 did, with
    no correct fix. Running this milestone's ledger therefore evaluates the ladder up to and
    including it, once each, and invokes no other ledger.
    """
    target = load(milestone_id, directory)
    earlier = tuple(name for name in ladder_order(permanent) if rung(name) < rung(target.id))
    declared: dict[tuple, list[tuple[str, Criterion]]] = {}
    declarations = 0
    for source_id in (*earlier, target.id):
        ledger = target if source_id == target.id else load(source_id, directory)
        for criterion in ledger.criteria:
            declarations += 1
            declared.setdefault(fingerprint(criterion), []).append((source_id, criterion))
    entries = tuple(_collapse(group, target.id) for group in declared.values())
    return Plan(milestone=target, ledgers=(*earlier, target.id), entries=entries,
                declarations=declarations)


def _collapse(group: list[tuple[str, Criterion]], target_id: str) -> PlanEntry:
    """One check from its declarations: the earliest declarer reports it, the largest budget
    wins."""
    milestones = tuple(source_id for source_id, _ in group)
    criterion = group[0][1]
    budget = max(candidate.timeout_s for _, candidate in group)
    if budget != criterion.timeout_s:
        criterion = replace(criterion, timeout_s=budget)
    return PlanEntry(criterion=criterion, declared_by=milestones,
                     permanent=milestones[0] != target_id)


# --- Evaluation -----------------------------------------------------------------------------------


def unmet_requirement(criterion: Criterion, force_ci: bool = False) -> str:
    """Why this machine cannot evaluate the criterion, or an empty string when it can."""
    if criterion.where == "ci" and not force_ci:
        return criterion.reason
    if criterion.requires == "display" and not (
        os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")
    ):
        return criterion.reason
    if criterion.requires == "gpu" and not _has_gpu():
        return criterion.reason
    return ""


def _has_gpu() -> bool:
    """Whether this machine has a graphics device a rendering criterion could run against.

    M3 is the first milestone with criteria that need one — the conventions sampled back off a
    device, and the golden images. Those cannot be evaluated on a machine with no GPU and they must
    not be reported as passing there, which is what this requirement exists to prevent: a milestone
    recipe that quietly skipped its rendering criteria would report the milestone green on precisely
    the machines least able to judge it.

    The probe is the presence of a DRM render node, plus `CY_HAS_GPU` as an override for the cases a
    file cannot answer — a container that has a device but no node, or a developer who wants the
    criterion reported as unevaluated. It is deliberately not "run vulkaninfo": this module runs on
    every pull request on three platforms and a gate that shells out to a tool that may not be
    installed is a gate that fails for the wrong reason.
    """
    override = os.environ.get("CY_HAS_GPU")
    if override is not None:
        return override not in ("", "0", "false", "no")
    nodes = Path("/dev/dri")
    if nodes.is_dir():
        return any(entry.name.startswith("render") for entry in nodes.iterdir())
    # macOS and Windows always have one; there is no headless variant of either that this project
    # builds for, and `where = "ci"` is how a criterion says "another operating system" anyway.
    return os.name != "posix" or sys.platform == "darwin"


def evaluate(criterion: Criterion, entries: tuple[Entry, ...], force_ci: bool = False) -> Result:
    """Run one criterion. It passes, it fails, or this machine cannot run it — never anything else."""
    unmet = unmet_requirement(criterion, force_ci)
    if unmet:
        return Result(criterion, NOT_EVALUATED, f"{unmet} — CI job '{criterion.ci_job}'")

    started = time.monotonic()
    if criterion.kind == "tiers":
        status, detail, output = _check_tiers(criterion, entries)
    elif criterion.kind == "path":
        status, detail, output = _check_path(criterion)
    else:
        status, detail, output = _run(criterion)
    return Result(criterion, status, detail, time.monotonic() - started, output)


def _run(criterion: Criterion) -> tuple[str, str, str]:
    shell = shutil.which("bash") or "/bin/bash"
    try:
        completed = subprocess.run(  # noqa: S603 — the command is committed data, not input
            [shell, "-c", criterion.run],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            timeout=criterion.timeout_s,
            check=False,
        )
    except subprocess.TimeoutExpired:
        return FAILED, f"no result within {criterion.timeout_s} s", ""
    output = (completed.stdout or "") + (completed.stderr or "")
    if completed.returncode == 0:
        return OK, "", output
    return FAILED, f"exit {completed.returncode}", output


def _check_path(criterion: Criterion) -> tuple[str, str, str]:
    matches = sorted(str(path.relative_to(REPO_ROOT)) for path in REPO_ROOT.glob(criterion.path))
    if matches:
        return OK, "", "\n".join(matches)
    return FAILED, f"nothing matches {criterion.path}", ""


def _check_tiers(criterion: Criterion, entries: tuple[Entry, ...]) -> tuple[str, str, str]:
    """An exit tier is a FLOOR, not an equality.

    `delivery-roadmap`'s ladder is ordered and a capability only ever moves up it, so "M0 exits with
    `project-and-plugins` at Seed" is the claim that it had reached Seed by then — not that it must
    stay there forever. Comparing for equality made a closed milestone's recipe fail the moment a
    later one advanced anything it named: M1 raised `project-and-plugins` to Working and
    `just roadmap-milestone m0` went red on a criterion M0 had satisfied, with no way back short of
    editing M0's own criteria. A milestone's exit criteria must stay runnable after it closes,
    because `milestone-m0` is a permanent merge gate.
    """
    recorded = {entry.capability: entry for entry in entries}
    wrong = []
    for capability, expected in sorted(criterion.expect_tiers.items()):
        entry = recorded.get(capability)
        if entry is None:
            wrong.append(f"{capability}: not in the record")
            continue
        if expected not in TIERS:
            wrong.append(f"{capability}: expected tier {expected!r} is not one of {', '.join(TIERS)}")
        elif TIERS.index(entry.tier) < TIERS.index(expected):
            wrong.append(
                f"{capability}: recorded '{entry.tier}', below this milestone's exit of '{expected}'")
    if wrong:
        return FAILED, f"{len(wrong)} capability tier(s) below this milestone's exit", "\n".join(wrong)
    return OK, "", ""
