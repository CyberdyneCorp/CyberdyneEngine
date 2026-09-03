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
system, no display — says so, names that job, and is counted separately in the summary.

Governed by: delivery-roadmap (Milestone exit criteria are executable, Forbidden roadmap patterns).
"""

from __future__ import annotations

import os
import shutil
import subprocess
import time
import tomllib
from dataclasses import dataclass, field
from pathlib import Path

from record import REPO_ROOT, Entry

MILESTONES_DIR = Path(__file__).resolve().parent / "milestones"
SCHEMA = 1
KINDS = ("recipe", "command", "path", "tiers")
WHERE = ("local", "ci")
REQUIREMENTS = ("display",)
DEFAULT_TIMEOUT_S = 1800

CRITERION_KEYS = frozenset(
    {"id", "describe", "source", "kind", "run", "path", "expect_tiers", "where", "ci_job",
     "requires", "reason", "timeout_s"}
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


# --- Evaluation -----------------------------------------------------------------------------------


def unmet_requirement(criterion: Criterion, force_ci: bool = False) -> str:
    """Why this machine cannot evaluate the criterion, or an empty string when it can."""
    if criterion.where == "ci" and not force_ci:
        return criterion.reason
    if criterion.requires == "display" and not (
        os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")
    ):
        return criterion.reason
    return ""


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
    recorded = {entry.capability: entry for entry in entries}
    wrong = []
    for capability, expected in sorted(criterion.expect_tiers.items()):
        entry = recorded.get(capability)
        if entry is None:
            wrong.append(f"{capability}: not in the record")
        elif entry.tier != expected:
            wrong.append(f"{capability}: recorded '{entry.tier}', this milestone exits at '{expected}'")
    if wrong:
        return FAILED, f"{len(wrong)} capability tier(s) not at this milestone's exit", "\n".join(wrong)
    return OK, "", ""
