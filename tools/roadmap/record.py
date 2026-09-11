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
# The ladder, in order. It is written out rather than generated because it is no longer a range:
# `implement-m5b-operable` INSERTS M5.5 between M5 and M6 rather than renumbering M6 through M11, so
# that every existing reference to a later milestone stays valid. Its identifier is `m5b` and not
# `m5.5` because a milestone id is also a file name under `tools/roadmap/milestones/` and a record
# value, and a dot in either is a needless special case.
#
# The ORDER is the whole point of the tuple. `criteria.rung` reads its index to decide which closed
# milestones a ledger inherits, so `m5b` sitting between `m5` and `m6` is what makes M6's ledger
# inherit M5.5's criteria rather than rank below them. `m5b.toml`'s header asked for exactly this
# edit and said why it could not make it itself.
#
# A capability row in docs/roadmap/status.yaml that M5.5 advanced therefore names `M5B`.
#
# M8 IS SPLIT THE SAME WAY, and for a reason worth stating rather than repeating by habit.
# `split-m8-authorable-and-systems` divides it into `m8a` — a scene a person builds by hand:
# primitives, import from inside the editor, a body on an object, play mode — and `m8b`, everything
# that lowers through one graph IR. The rule it added to `delivery-roadmap` is the general form: a
# milestone whose artefact cannot be reached without its own risk spike succeeding, when some other
# coherent artefact could be reached without it, contains two. M8's settled half was blocked by an IR
# that seven consumers have to agree on, and nothing in the settled half is architecturally open.
MILESTONES = (
    "m0",
    "m1",
    "m2",
    "m3",
    "m4",
    "m5",
    "m5b",
    "m6",
    "m7",
    "m8a",
    "m8b",
    "m8c",
    "m9",
    "m10",
    "m11",
)
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


# --- The three lists, generated rather than hand-maintained ----------------------------------------
#
# M9 TASK 7.6. `docs/roadmap/capability-matrix.md` carried three lists — Complete, Working, Seed —
# written out by hand at each gate. They were last refreshed at M7's, and by M9 they were three
# milestones stale: M8.b moved eleven capabilities and M8.c three, and none of it was in them.
# M8.c's closing gate found that by running `just roadmap-status` and reading, recorded it as a
# finding, and could not fix it inside its own scope; the fix it named is this one.
#
# A list a person retypes is a list that goes stale silently. A list a recipe renders is either
# current or a failing gate, and `just roadmap-status` is the recipe that already reads this record.
LISTS_BEGIN = "<!-- BEGIN generated by `just roadmap-status` — do not edit between these markers -->"
LISTS_END = "<!-- END generated -->"
LISTS_WIDTH = 100


def newest_rung(entries: tuple[Entry, ...]) -> str:
    """The furthest milestone any entry names. Derived, so the heading cannot be forgotten."""
    named = [entry.milestone.lower() for entry in entries if entry.milestone]
    known = [name for name in named if name in MILESTONES]
    return max(known, key=MILESTONES.index).upper() if known else "M0"


def render_lists(entries: tuple[Entry, ...]) -> str:
    """The Complete/Working/Seed lists, exactly as the matrix must carry them."""
    import textwrap

    rung = newest_rung(entries)
    started = [entry for entry in entries if entry.started]
    complete = [entry for entry in entries if entry.tier == "complete"]
    lines = [
        LISTS_BEGIN,
        "",
        f"As of {rung} {len(started)} capabilities have left `—`, and {len(complete)} have "
        "reached Complete.",
        "",
    ]
    for tier in ("complete", "working", "seed"):
        named = sorted(entry.capability for entry in entries if entry.tier == tier)
        body = ", ".join(f"`{name}`" for name in named) + "."
        head = f"- **{TIER_LABEL[tier].capitalize()} ({len(named)})**: "
        lines.extend(textwrap.wrap(head + body, width=LISTS_WIDTH, subsequent_indent="  ",
                                   break_long_words=False, break_on_hyphens=False))
    lines.extend(["", LISTS_END])
    return "\n".join(lines)


def lists_drift(document: Path, entries: tuple[Entry, ...]) -> str | None:
    """None when the document's generated block is current; otherwise what it should say."""
    if not document.is_file():
        raise RecordError(f"{document}: the document carrying the generated lists does not exist")
    text = document.read_text(encoding="utf-8")
    if LISTS_BEGIN not in text or LISTS_END not in text:
        raise RecordError(
            f"{display(document)}: the generated-list markers are gone. The block between "
            f"{LISTS_BEGIN!r} and {LISTS_END!r} is rendered from the record and may not be removed."
        )
    start = text.index(LISTS_BEGIN)
    end = text.index(LISTS_END) + len(LISTS_END)
    expected = render_lists(entries)
    return None if text[start:end] == expected else expected


def write_lists(document: Path, entries: tuple[Entry, ...]) -> bool:
    """Replace the generated block in place. Returns True when the document changed."""
    expected = lists_drift(document, entries)
    if expected is None:
        return False
    text = document.read_text(encoding="utf-8")
    start = text.index(LISTS_BEGIN)
    end = text.index(LISTS_END) + len(LISTS_END)
    document.write_text(text[:start] + expected + text[end:], encoding="utf-8")
    return True
