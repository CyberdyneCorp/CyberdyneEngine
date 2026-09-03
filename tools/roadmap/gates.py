"""The permanent merge-gate set: tools/roadmap/gates.toml, and the rules an override obeys.

Task 4.4.1, 4.4.2 and 4.4.3. `testing-and-quality` requires that a change does not merge unless the
platform builds, the tests, the static analysis, the generated code and the specification checks
pass, and that a failing gate is overridden explicitly and on the record rather than quietly
excepted. `delivery-roadmap` adds that a closed milestone's criteria join that set and stay green.

The set is data for the same reason the criteria are: continuous integration, the contributor
documentation and this tool must name the same gates, and three copies of a list diverge. An
override is data too — with a reason, an approver, the change that records it, and an expiry — so
that "we merged past the lint gate" is a line in the repository and not a memory. An override
missing a field, or past its expiry, fails this check: an override nobody can trace is the quiet
exception the requirement forbids.

Governed by: testing-and-quality (Quality gates for merge), delivery-roadmap (Milestone gates do
not regress).
"""

from __future__ import annotations

import datetime as _datetime
import tomllib
from dataclasses import dataclass
from pathlib import Path

GATES = Path(__file__).resolve().parent / "gates.toml"
SCHEMA = 1
CLASSES = ("permanent", "milestone")
STATES = ("green", "joins-on-close")

GATE_KEYS = frozenset({"id", "describe", "runs", "platforms", "since", "class", "milestone", "state"})
OVERRIDE_KEYS = frozenset({"gate", "reason", "approved_by", "change", "expires"})
DOCUMENT_KEYS = frozenset({"schema", "gate", "override"})


class GateError(Exception):
    """A gate set that cannot be enforced as written."""


@dataclass(frozen=True)
class Gate:
    id: str
    describe: str
    runs: tuple[str, ...]
    platforms: tuple[str, ...]
    since: str
    klass: str
    milestone: str = ""
    state: str = "green"


@dataclass(frozen=True)
class Override:
    gate: str
    reason: str
    approved_by: str
    change: str
    expires: _datetime.date


@dataclass(frozen=True)
class GateSet:
    gates: tuple[Gate, ...]
    overrides: tuple[Override, ...]


def load(source: Path = GATES) -> GateSet:
    """Read and validate the gate set. Raises GateError on anything that could not be enforced."""
    if not source.is_file():
        raise GateError(f"{source}: the gate set does not exist")
    with source.open("rb") as handle:
        document = tomllib.load(handle)
    _reject_unknown(document.keys(), DOCUMENT_KEYS, source.name)
    if document.get("schema") != SCHEMA:
        raise GateError(f"{source.name}: schema is {document.get('schema')!r}, expected {SCHEMA}")

    gates = tuple(_gate(table, source.name) for table in document.get("gate", ()))
    if not gates:
        raise GateError(f"{source.name}: no gates are declared")
    _reject_duplicates(gates, source.name)
    overrides = tuple(_override(table, source.name, gates) for table in document.get("override", ()))
    return GateSet(gates=gates, overrides=overrides)


def _reject_unknown(keys, allowed: frozenset[str], where: str) -> None:
    unknown = sorted(set(keys) - allowed)
    if unknown:
        raise GateError(f"{where}: unknown key(s) {', '.join(unknown)}")


def _reject_duplicates(gates: tuple[Gate, ...], source: str) -> None:
    seen: set[str] = set()
    for gate in gates:
        if gate.id in seen:
            raise GateError(f"{source}: gate '{gate.id}' is declared twice")
        seen.add(gate.id)


def _gate(table: dict, source: str) -> Gate:
    where = f"{source}: gate '{table.get('id', '<unnamed>')}'"
    _reject_unknown(table.keys(), GATE_KEYS, where)
    for key in ("id", "describe", "since", "class"):
        if not str(table.get(key, "")).strip():
            raise GateError(f"{where}: '{key}' is required")
    if table["class"] not in CLASSES:
        raise GateError(f"{where}: class is one of {', '.join(CLASSES)}")
    if not table.get("runs"):
        raise GateError(f"{where}: a gate that runs nothing gates nothing")
    if table.get("state", "green") not in STATES:
        raise GateError(f"{where}: state is one of {', '.join(STATES)}")
    if table["class"] == "milestone" and not table.get("milestone"):
        raise GateError(f"{where}: a milestone gate names its milestone")
    return Gate(
        id=table["id"],
        describe=table["describe"],
        runs=tuple(table["runs"]),
        platforms=tuple(table.get("platforms", ("linux", "windows", "macos"))),
        since=table["since"],
        klass=table["class"],
        milestone=table.get("milestone", ""),
        state=table.get("state", "green"),
    )


def _override(table: dict, source: str, gates: tuple[Gate, ...]) -> Override:
    where = f"{source}: override of '{table.get('gate', '<unnamed>')}'"
    _reject_unknown(table.keys(), OVERRIDE_KEYS, where)
    for key in sorted(OVERRIDE_KEYS):
        if not str(table.get(key, "")).strip():
            raise GateError(f"{where}: '{key}' is required — an untraceable override is not one")
    if table["gate"] not in {gate.id for gate in gates}:
        raise GateError(f"{where}: no such gate")
    expires = _date(table["expires"], where)
    if expires < _datetime.date.today():
        raise GateError(f"{where}: expired on {expires}. Fix the gate or record a new override.")
    return Override(
        gate=table["gate"],
        reason=table["reason"],
        approved_by=table["approved_by"],
        change=table["change"],
        expires=expires,
    )


def _date(value, where: str) -> _datetime.date:
    if isinstance(value, _datetime.date):
        return value
    try:
        return _datetime.date.fromisoformat(str(value))
    except ValueError as error:
        raise GateError(f"{where}: 'expires' is a date, YYYY-MM-DD ({error})") from error


def commands(gate_set: GateSet) -> tuple[str, ...]:
    """Every command the gate set requires, in order, deduplicated. What CI runs, and nothing else."""
    ordered: list[str] = []
    for gate in gate_set.gates:
        for command in gate.runs:
            if command not in ordered:
                ordered.append(command)
    return tuple(ordered)
