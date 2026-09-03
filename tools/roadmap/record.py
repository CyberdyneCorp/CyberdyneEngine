"""The status record: docs/roadmap/status.yaml, and its agreement with openspec/specs/.

Task 4.3.1 and 4.3.2. `delivery-roadmap` requires exactly one authoritative record of per-capability
implementation status, reported by a recipe rather than read by hand, and requires that recipe to
fail when the record and the specification set disagree. Drift is a build failure, not a note.

The record is read with a parser written here rather than with a YAML library, because this file is
a gate: it runs on every pull request, on three platforms, and it may not depend on a package that
happens to be installed. status.yaml is deliberately a restricted shape — `schema`, then
`capabilities`, then one two-space block per capability with three scalar keys — and anything
outside that shape is reported with its line number rather than quietly accepted.

Governed by: delivery-roadmap (Implementation status is recorded in one place).
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_RECORD = REPO_ROOT / "docs" / "roadmap" / "status.yaml"
DEFAULT_SPECS = REPO_ROOT / "openspec" / "specs"

SCHEMA = 1
TIERS = ("none", "seed", "working", "complete")
TIER_LABEL = {"none": "not started", "seed": "seed", "working": "working", "complete": "complete"}
MILESTONES = tuple(f"m{index}" for index in range(12))
ENTRY_KEYS = ("tier", "milestone", "change")


class RecordError(Exception):
    """A record that cannot be trusted to answer 'what is implemented today'."""


@dataclass(frozen=True)
class Entry:
    """One capability's line in the record."""

    capability: str
    tier: str
    milestone: str | None
    change: str | None

    @property
    def started(self) -> bool:
        return self.tier != "none"


@dataclass(frozen=True)
class Drift:
    """Where the record and the specification set disagree."""

    unrecorded: tuple[str, ...]  # a capability has a spec and no entry
    unspecified: tuple[str, ...]  # an entry names a capability with no spec

    def __bool__(self) -> bool:
        return bool(self.unrecorded or self.unspecified)


def _strip_comment(line: str) -> str:
    """Remove a trailing comment. A '#' only starts one at the beginning of a line or after space."""
    quote = ""
    for index, character in enumerate(line):
        if quote:
            quote = "" if character == quote else quote
        elif character in "\"'":
            quote = character
        elif character == "#" and (index == 0 or line[index - 1] in " \t"):
            return line[:index]
    return line


def _scalar(value: str) -> str | None:
    value = value.strip()
    if value in ("", "null", "~"):
        return None
    if len(value) >= 2 and value[0] == value[-1] and value[0] in "\"'":
        return value[1:-1]
    return value


class _Parser:
    """The line-by-line reader for the restricted shape status.yaml is written in.

    Three indents and nothing else: a top-level key, a capability at two spaces, one of its fields
    at four. Everything the shape does not allow is an error naming the line it was on.
    """

    def __init__(self) -> None:
        self.schema: int | None = None
        self.capabilities: dict[str, dict[str, str | None]] = {}
        self._current: str | None = None
        self._in_capabilities = False

    def feed(self, line: str, where: str) -> None:
        indent = len(line) - len(line.lstrip())
        key, separator, value = line.strip().partition(":")
        if not separator:
            raise RecordError(f"{where}: expected 'key: value', found {line.strip()!r}")
        if indent == 0:
            self._top_level(key, value, where)
        elif indent == 2 and self._in_capabilities:
            self._capability(key, where)
        elif indent == 4 and self._current is not None:
            self.capabilities[self._current][key] = _scalar(value)
        else:
            raise RecordError(f"{where}: unexpected indentation for {key!r}")

    def _top_level(self, key: str, value: str, where: str) -> None:
        self._in_capabilities = key == "capabilities"
        self._current = None
        if key == "schema":
            self.schema = int(_scalar(value) or 0)
        elif not self._in_capabilities:
            raise RecordError(f"{where}: unknown top-level key {key!r}")

    def _capability(self, key: str, where: str) -> None:
        if key in self.capabilities:
            raise RecordError(f"{where}: capability {key!r} is listed twice")
        self._current = key
        self.capabilities[key] = {}


def _parse(text: str, source: str) -> tuple[int | None, dict[str, dict[str, str | None]]]:
    parser = _Parser()
    for number, raw in enumerate(text.splitlines(), start=1):
        line = _strip_comment(raw).rstrip()
        if line.strip():
            parser.feed(line, f"{source}:{number}")
    return parser.schema, parser.capabilities


def _entry(capability: str, fields: dict[str, str | None], source: str) -> Entry:
    unknown = sorted(set(fields) - set(ENTRY_KEYS))
    if unknown:
        raise RecordError(f"{source}: {capability}: unknown key(s) {', '.join(unknown)}")
    missing = [key for key in ENTRY_KEYS if key not in fields]
    if missing:
        raise RecordError(f"{source}: {capability}: missing key(s) {', '.join(missing)}")

    tier = fields["tier"] or "none"
    if tier not in TIERS:
        raise RecordError(f"{source}: {capability}: tier {tier!r} is not one of {', '.join(TIERS)}")

    milestone, change = fields["milestone"], fields["change"]
    if milestone is not None and milestone.lower() not in MILESTONES:
        raise RecordError(f"{source}: {capability}: milestone {milestone!r} is not on the ladder")

    # A tier claim is traceable: delivery-roadmap requires the record to name the change that
    # advanced a capability, so an entry above 'none' that names neither is not a claim, it is a
    # rumour.
    if tier != "none" and (milestone is None or change is None):
        raise RecordError(
            f"{source}: {capability}: tier {tier!r} names no "
            f"{'milestone' if milestone is None else 'change'}"
        )
    if tier == "none" and (milestone is not None or change is not None):
        raise RecordError(f"{source}: {capability}: tier 'none' cannot name a milestone or a change")

    return Entry(capability=capability, tier=tier, milestone=milestone, change=change)


def load(record: Path = DEFAULT_RECORD) -> tuple[Entry, ...]:
    """Read and validate the record. Raises RecordError with a line reference on anything wrong."""
    if not record.is_file():
        raise RecordError(f"{record}: the status record does not exist")
    source = display(record)
    schema, capabilities = _parse(record.read_text(encoding="utf-8"), source)
    if schema != SCHEMA:
        raise RecordError(f"{source}: schema is {schema!r}, this tool reads schema {SCHEMA}")
    if not capabilities:
        raise RecordError(f"{source}: no capabilities are recorded")
    return tuple(_entry(name, fields, source) for name, fields in capabilities.items())


def specified(specs: Path = DEFAULT_SPECS) -> tuple[str, ...]:
    """Every capability with a specification: a directory under openspec/specs/ holding a spec.md."""
    if not specs.is_dir():
        raise RecordError(f"{specs}: the specification directory does not exist")
    return tuple(sorted(path.name for path in specs.iterdir() if (path / "spec.md").is_file()))


def drift(entries: tuple[Entry, ...], capabilities: tuple[str, ...]) -> Drift:
    """A capability added, renamed or removed without a record entry."""
    recorded = {entry.capability for entry in entries}
    return Drift(
        unrecorded=tuple(sorted(set(capabilities) - recorded)),
        unspecified=tuple(sorted(recorded - set(capabilities))),
    )


def display(path: Path) -> str:
    """A repository-relative path where possible: absolute paths in gate output are noise."""
    try:
        return str(path.resolve().relative_to(REPO_ROOT))
    except ValueError:
        return str(path)
