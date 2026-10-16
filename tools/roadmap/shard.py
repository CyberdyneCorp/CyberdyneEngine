# SPDX-License-Identifier: MIT
"""One ledger run split across several hosted jobs, and their records merged into one verdict.

THE NIGHTLY LEDGER HAS NEVER FINISHED ON A HOSTED RUNNER. `milestone — the closed milestones' exit
criteria` ran `just roadmap-milestone m11d5 --ci --reclaim-builds` on one ubuntu-24.04 runner, and
GitHub cancels a hosted job at six hours: run 38687291984 reached 134 of 484 criteria in that time.
`m1:four-profiles` alone took 7680 s there. No amount of reclaiming makes 484 criteria fit in one
job, so the run is split into N shards, each its own job with its own disk and its own six hours,
and a final job merges what they recorded.

WHAT A SHARD IS. `--shard i/n` narrows the plan to the criteria `assign` gives shard i. The plan is
the same flat plan an unsharded run evaluates — the same criteria, the same `where = "ci"` handling,
the same `known_gap` rules — and nothing about a criterion changes with the shard it lands in.

HOW THE CRITERIA ARE DIVIDED, from data the ledger already has:

  * criteria that share a build tree other than the default one stay together. `reclaim.trees_of`
    says which trees a criterion builds or reads; `m1:four-profiles` builds `dev-default` and
    `debug-default` and `m9:multiplayer-profiles-agree` reads them 250 criteria later, so splitting
    the two would build both rows twice. A group whose estimate exceeds `GROUP_CEILING_S` is split
    back into its members instead, because a shard that cannot finish costs more than a rebuild
    (every criterion builds what it reads, so a split costs time and never a verdict).
  * each group's cost is the seconds MEASURED for its criteria on the hosted runner
    (`shard-costs.toml`, written from the shards' own records by `roadmap.py merge --write-costs`),
    and for a criterion not measured yet an estimate from what it holds: a tree of its own, Cargo or
    the whole host costs `HEAVY_S`, a check that reads a file costs nothing, anything else `LIGHT_S`.
  * groups are placed largest first, each into the least-loaded shard (longest-processing-time
    first). Ties go to the lower index, so the assignment is a pure function of the tree.

HOW THE VERDICT IS ONE VERDICT. Each shard writes a record (`--record`): its index and count, the
commit, the labels it evaluated and each one's result. `merge` recomputes the assignment from its own
checkout and accepts the records only when every shard 1..n is present exactly once, every record is
of this commit, this milestone and this count, and every record evaluated exactly the criteria the
assignment gives it. Then it summarises the whole plan exactly as an unsharded run would.

A MISSING SHARD IS A FAILURE, NEVER A SMALLER GREEN. A shard cancelled at the job limit, a runner
that died, a record not uploaded — each leaves a record missing, and `merge` names the shard and
exits non-zero. A ledger that reported 400 of 484 criteria passing would be the green tick over
nothing this repository has shipped before.

Governed by: delivery-roadmap (Milestone exit criteria are executable), developer-workflow-and-just.
"""

from __future__ import annotations

import json
import re
import tomllib
from dataclasses import dataclass
from pathlib import Path

import criteria as criteria_module
import matrix as matrix_module
import reclaim as reclaim_module
import schedule as schedule_module

#: Measured seconds per criterion label on the hosted runner. Rewritten by `merge --write-costs`.
COSTS_FILE = Path(__file__).resolve().parent / "shard-costs.toml"

#: The estimate for a criterion nobody has measured yet that builds a tree of its own: a sanitizer
#: build, a matrix row, an option-off configuration. Deliberately pessimistic: an overestimate wastes
#: a little balance, an underestimate can put two such criteria in one shard and past the job limit.
HEAVY_S = 1800.0
#: An unmeasured criterion that drives Cargo or holds the whole host without a tree of its own: an
#: editor test, or a body the scheduler could not read and so runs alone.
HOST_S = 300.0
#: The estimate for an unmeasured criterion that only builds and tests the default tree, which is
#: mostly an up-to-date check and one test binary: 10-15 s each on the hosted runner.
LIGHT_S = 60.0
#: A shard of tree-sharing criteria larger than this is split back into its members. Four hours of
#: the six GitHub allows a hosted job, leaving the set-up, the unpack and a margin.
GROUP_CEILING_S = 4 * 3600.0

#: Which hosted runner answers a `where = "ci"` criterion only one other operating system can.
PLATFORMS_FILE = Path(__file__).resolve().parent / "shard-platforms.toml"
PLATFORMS = ("macos", "windows")

RECORD_SCHEMA = 2
_SHARD = re.compile(r"^\s*(\d+|[a-z]+)\s*/\s*(\d+)\s*$")


class ShardError(Exception):
    """A shard request or a set of shard records that cannot be honoured."""


@dataclass(frozen=True)
class Shard:
    """Linux shard `index` of `count`, or — with `platform` set — the shard of the criteria pinned to
    that operating system in `shard-platforms.toml`, in a run of `count` Linux shards."""

    index: int
    count: int
    platform: str = ""

    @property
    def key(self) -> str:
        return self.platform or str(self.index)

    def __str__(self) -> str:
        return f"{self.key}/{self.count}"


def parse(text: str) -> Shard:
    """`i/n`, 1-based: `--shard 3/12` is the third of twelve. `macos/12` and `windows/12` are the
    shards of the criteria pinned to those systems, in the same run."""
    match = _SHARD.match(text or "")
    if not match:
        raise ShardError(f"--shard takes i/n, such as 3/12, or <platform>/n, not {text!r}")
    which, count = match.group(1), int(match.group(2))
    if count < 1:
        raise ShardError(f"--shard {text}: the count must be at least 1")
    if not which.isdigit():
        if which not in PLATFORMS:
            raise ShardError(f"--shard {text}: {which!r} is none of {', '.join(PLATFORMS)}")
        return Shard(0, count, which)
    index = int(which)
    if not 1 <= index <= count:
        raise ShardError(f"--shard {text}: the index must be between 1 and the count")
    return Shard(index, count)


def load_platforms(path: Path = PLATFORMS_FILE) -> dict[str, str]:
    """label -> platform, for the criteria a Linux shard must leave to another system's runner."""
    if not path.exists():
        return {}
    try:
        table = tomllib.loads(path.read_text(encoding="utf-8")).get("runner", {})
    except tomllib.TOMLDecodeError as error:
        raise ShardError(f"{path.name}: {error}") from error
    for label, platform in table.items():
        if platform not in PLATFORMS:
            raise ShardError(f"{path.name}: {label} = {platform!r} is none of "
                             f"{', '.join(PLATFORMS)}")
    return dict(table)


def check_platforms(plan: criteria_module.Plan, platforms: dict[str, str]) -> None:
    """A pin may only move a `where = "ci"` criterion: one that runs everywhere and was pinned would
    simply stop running on Linux, which is a criterion removed rather than relocated."""
    for entry in plan.entries:
        if entry.label in platforms and entry.criterion.where != "ci":
            raise ShardError(f"{PLATFORMS_FILE.name}: {entry.label} is pinned to "
                             f"{platforms[entry.label]} and is not a `where = \"ci\"` criterion")


# --- Costs ----------------------------------------------------------------------------------------


def load_costs(path: Path = COSTS_FILE) -> dict[str, float]:
    """The measured seconds, by label. An absent file is an empty table: every cost is estimated."""
    if not path.exists():
        return {}
    try:
        document = tomllib.loads(path.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as error:
        raise ShardError(f"{path.name}: {error}") from error
    table = document.get("seconds", {})
    if not isinstance(table, dict):
        raise ShardError(f"{path.name}: [seconds] must be a table of label = seconds")
    costs: dict[str, float] = {}
    for label, seconds in table.items():
        if not isinstance(seconds, (int, float)) or seconds < 0:
            raise ShardError(f"{path.name}: {label} = {seconds!r} is not a number of seconds")
        costs[label] = float(seconds)
    return costs


def estimate(entry, costs: dict[str, float]) -> float:
    """Seconds this criterion is expected to take on the hosted runner."""
    if entry.label in costs:
        return costs[entry.label]
    criterion = entry.criterion
    if criterion.kind in ("path", "tiers"):
        return 0.0
    if reclaim_module.trees_of(entry, reclaim_module.build_root(), matrix_module.root()):
        return HEAVY_S
    if set(schedule_module.needs(criterion)) & {schedule_module.EXCLUSIVE, "cargo"}:
        return HOST_S
    return LIGHT_S


# --- The assignment -------------------------------------------------------------------------------


def groups(entries, costs: dict[str, float]) -> list[list[int]]:
    """Indices of the criteria that share a non-default tree, each group in ledger order."""
    root, matrix_root = reclaim_module.build_root(), matrix_module.root()
    parent = list(range(len(entries)))

    def find(index: int) -> int:
        while parent[index] != index:
            parent[index] = parent[parent[index]]
            index = parent[index]
        return index

    first_user: dict[Path, int] = {}
    for index, entry in enumerate(entries):
        for tree in reclaim_module.trees_of(entry, root, matrix_root):
            if tree in first_user:
                parent[find(index)] = find(first_user[tree])
            else:
                first_user[tree] = index

    collected: dict[int, list[int]] = {}
    for index in range(len(entries)):
        collected.setdefault(find(index), []).append(index)
    result: list[list[int]] = []
    for members in collected.values():
        if len(members) > 1 and sum(estimate(entries[i], costs) for i in members) > GROUP_CEILING_S:
            result.extend([member] for member in members)
        else:
            result.append(members)
    return sorted(result, key=lambda members: members[0])


def assign(entries, count: int, costs: dict[str, float] | None = None,
           platforms: dict[str, str] | None = None) -> list[list[int]]:
    """Each Linux shard's criteria, as indices into `entries` in ledger order. Shard i is element
    i-1. A criterion pinned to another platform is in none of them."""
    if count < 1:
        raise ShardError("a ledger is split into at least one shard")
    costs = load_costs() if costs is None else costs
    platforms = load_platforms() if platforms is None else platforms
    weighed = [(sum(estimate(entries[i], costs) for i in members), members)
               for members in groups(entries, costs)]
    weighed.sort(key=lambda pair: (-pair[0], pair[1][0]))
    loads = [0.0] * count
    shards: list[list[int]] = [[] for _ in range(count)]
    for cost, members in weighed:
        members = [i for i in members if entries[i].label not in platforms]
        if not members:
            continue
        target = min(range(count), key=lambda shard: (loads[shard], shard))
        loads[target] += cost
        shards[target].extend(members)
    return [sorted(members) for members in shards]


def expected(entries, count: int, costs: dict[str, float] | None = None,
             platforms: dict[str, str] | None = None) -> dict[str, list[int]]:
    """Every shard a run of `count` Linux shards has, by key, and its criteria: `1`..`n`, and each
    platform at least one of these criteria is pinned to."""
    platforms = load_platforms() if platforms is None else platforms
    shards = {str(index): members
              for index, members in enumerate(assign(entries, count, costs, platforms), start=1)}
    for platform in PLATFORMS:
        pinned = [i for i, entry in enumerate(entries) if platforms.get(entry.label) == platform]
        if pinned:
            shards[platform] = pinned
    return shards


def narrow(plan: criteria_module.Plan, shard: Shard, costs: dict[str, float] | None = None,
           platforms: dict[str, str] | None = None) -> criteria_module.Plan:
    """The plan with only this shard's criteria in it, in ledger order."""
    platforms = load_platforms() if platforms is None else platforms
    check_platforms(plan, platforms)
    chosen = expected(plan.entries, shard.count, costs, platforms).get(shard.key, [])
    return criteria_module.Plan(milestone=plan.milestone, ledgers=plan.ledgers,
                                entries=tuple(plan.entries[i] for i in chosen),
                                declarations=plan.declarations)


def estimated_seconds(plan: criteria_module.Plan, count: int,
                      costs: dict[str, float] | None = None) -> list[tuple[str, int, float]]:
    """(shard, criteria, estimated seconds) per shard, for choosing a count."""
    costs = load_costs() if costs is None else costs
    return [(key, len(members), sum(estimate(plan.entries[i], costs) for i in members))
            for key, members in expected(plan.entries, count, costs).items()]


# --- Records --------------------------------------------------------------------------------------


def record_document(plan: criteria_module.Plan, shard: Shard, commit: str, force_ci: bool,
                    results) -> dict:
    """What one shard hands to `merge`: who it was, what it evaluated, and what each one said."""
    return {
        "schema": RECORD_SCHEMA,
        "milestone": plan.milestone.id,
        "shard": shard.key,
        "count": shard.count,
        "commit": commit,
        "ci": force_ci,
        "seconds": round(sum(result.seconds for result in results), 1),
        "criteria": [{"label": entry.label, "status": result.status, "detail": result.detail,
                      "seconds": round(result.seconds, 1)}
                     for entry, result in zip(plan.entries, results)],
    }


def write_record(path: Path, document: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")


_STATUSES = (criteria_module.OK, criteria_module.FAILED, criteria_module.NOT_EVALUATED)


def _read_record(path: Path) -> dict:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise ShardError(f"{path}: not a readable shard record ({error})") from error
    if not isinstance(document, dict) or document.get("schema") != RECORD_SCHEMA:
        raise ShardError(f"{path}: not a shard record of schema {RECORD_SCHEMA}")
    for key in ("milestone", "shard", "count", "commit", "ci", "criteria"):
        if key not in document:
            raise ShardError(f"{path}: the record has no `{key}`")
    for item in document["criteria"]:
        if item.get("status") not in _STATUSES:
            raise ShardError(f"{path}: {item.get('label')} has status {item.get('status')!r}, "
                             f"which is none of {', '.join(_STATUSES)}")
    return document


def record_paths(arguments) -> list[Path]:
    """The record files named, a directory standing for every `*.json` in it.

    A directory that does not exist stands for none: the download step of a run whose every shard
    died before uploading leaves nothing behind, and that must read as every shard MISSING — which
    `merge` reports by name — rather than as an argument error that says nothing about the shards.
    """
    found: list[Path] = []
    for argument in arguments:
        path = Path(argument)
        if path.is_dir():
            found.extend(sorted(path.rglob("*.json")))
        elif path.exists():
            found.append(path)
        elif path.suffix != ".json":
            continue
        else:
            raise ShardError(f"{path}: no such shard record")
    return found


@dataclass(frozen=True)
class Merged:
    results: tuple
    #: (shard, criteria, seconds) per shard, in shard order.
    durations: tuple[tuple[str, int, float], ...]


def merge(plan: criteria_module.Plan, count: int, paths, commit: str, force_ci: bool,
          costs: dict[str, float] | None = None,
          platforms: dict[str, str] | None = None) -> Merged:
    """Every shard's record checked against the assignment, and the results in ledger order."""
    platforms = load_platforms() if platforms is None else platforms
    check_platforms(plan, platforms)
    shards = expected(plan.entries, count, costs, platforms)
    records = _records_by_shard(paths, plan.milestone.id, count, commit, force_ci)

    missing = [key for key in shards if key not in records]
    if missing:
        names = ", ".join(f"{key}/{count}" for key in missing)
        raise ShardError(f"{len(missing)} of {len(shards)} shard record(s) missing — {names}. A "
                         "shard that never reported is not a shard that passed: its criteria were "
                         "not evaluated, so the ledger has no verdict")
    unknown = sorted(key for key in records if key not in shards)
    if unknown:
        raise ShardError(f"record(s) of shard(s) this run does not have: {', '.join(unknown)}")

    by_label: dict[str, dict] = {}
    durations = []
    for key, members in shards.items():
        path, document = records[key]
        _check_assignment(path, f"{key}/{count}", [plan.entries[i].label for i in members],
                          [item["label"] for item in document["criteria"]])
        for item in document["criteria"]:
            by_label[item["label"]] = item
        durations.append((key, len(members), float(document.get("seconds", 0.0))))

    results = tuple(
        criteria_module.Result(entry.criterion, by_label[entry.label]["status"],
                               by_label[entry.label].get("detail", ""),
                               float(by_label[entry.label].get("seconds", 0.0)))
        for entry in plan.entries)
    return Merged(results=results, durations=tuple(durations))


def _records_by_shard(paths, milestone: str, count: int, commit: str,
                      force_ci: bool) -> dict[str, tuple[Path, dict]]:
    records: dict[str, tuple[Path, dict]] = {}
    for path in paths:
        document = _read_record(Path(path))
        _check_identity(Path(path), document, milestone, count, commit, force_ci)
        key = str(document["shard"])
        if key in records:
            raise ShardError(f"shard {key}/{count} has two records: {records[key][0]} and {path}")
        records[key] = (Path(path), document)
    return records


def _check_assignment(path: Path, shard: str, wanted: list[str], got: list[str]) -> None:
    if got == wanted:
        return
    absent = sorted(set(wanted) - set(got))
    extra = sorted(set(got) - set(wanted))
    raise ShardError(
        f"{path}: shard {shard} evaluated a different set from the one this checkout assigns it "
        f"— {len(absent)} assigned and absent ({', '.join(absent[:5]) or 'none'}), {len(extra)} "
        f"present and not assigned ({', '.join(extra[:5]) or 'none'}), or the same set in another "
        "order")


def _check_identity(path: Path, document: dict, milestone: str, count: int, commit: str,
                    force_ci: bool) -> None:
    if document["milestone"] != milestone:
        raise ShardError(f"{path}: a record of {document['milestone']}, and this merge is of "
                         f"{milestone}")
    if document["count"] != count:
        raise ShardError(f"{path}: shard {document['shard']} of {document['count']}, and this "
                         f"merge expects {count} shards — the workflow and the merge disagree "
                         "about the count")
    if document["commit"] != commit:
        raise ShardError(f"{path}: evaluated {document['commit'][:12]}, and this checkout is "
                         f"{commit[:12]}; records of two commits are not one ledger")
    if bool(document["ci"]) != force_ci:
        raise ShardError(f"{path}: evaluated with --ci {'on' if document['ci'] else 'off'}, and "
                         f"this merge has it {'on' if force_ci else 'off'}")


def costs_document(merged: Merged, plan: criteria_module.Plan, source: str,
                   previous: dict[str, float]) -> str:
    """`shard-costs.toml`, from what a merged run measured.

    ONLY A PASS IS A MEASUREMENT. A criterion that failed stopped wherever it failed — a missing tool
    fails in a second, a full disk an hour in — so its time says nothing about what a passing run
    costs; it keeps its previous measurement, or none, and is estimated. A criterion this checkout no
    longer has is dropped."""
    lines = [
        "# SPDX-License-Identifier: MIT",
        "# Seconds each criterion took on the hosted ubuntu-24.04 runner, read by tools/roadmap/",
        "# shard.py to balance the nightly ledger's shards. Written by",
        "#     python3 tools/roadmap/roadmap.py merge <id> --shards <n> --write-costs <records...>",
        f"# from {source}. Data, not a budget: no criterion is judged against these numbers.",
        "",
        "[seconds]",
    ]
    for entry, result in zip(plan.entries, merged.results):
        if result.status == criteria_module.OK:
            lines.append(f'"{entry.label}" = {round(result.seconds, 1)}')
        elif entry.label in previous:
            lines.append(f'"{entry.label}" = {previous[entry.label]}')
    return "\n".join(lines) + "\n"
