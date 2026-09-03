"""The committed identity manifest: assignment, tombstones, and the gate. Section 1.2.

An identifier is an **opaque number assigned on first sight and recorded here** (design.md §1). It
is not a hash of the name, not a hash of the type, not an index, and not a content digest — there is
no function in this file from a name to a number, and that absence is the design. Anything derived
from a name changes when the name changes, which is precisely the event this file exists to survive;
a hash also collides silently, and a collision in this number produces data that loads successfully
and is wrong.

The generator only ever **appends**. Removing a declaration writes a **tombstone**, and a tombstoned
number is never issued again. Matching an existing entry considers live entries only, so a type
removed and later re-added with the same name receives a fresh identifier — `core-type-system` is
explicit that a recycled identifier is the failure with no diagnostic.

The gate is the point, not the manifest. Because identity is not derived from a name, the generator
cannot tell a rename from a removal, and it must not guess: a live entry that has left the tree
fails the build naming the declaration, its identifier and the two fixes. One of them keeps the
identifier and records the new name; the other tombstones it. Both are a reviewed diff of this file.
"""

from __future__ import annotations

import tomllib
from dataclasses import dataclass, field
from pathlib import Path

MANIFEST_VERSION = 1

LIVE = "live"
REMOVED = "removed"


class IdentityError(Exception):
    """A failure of the identity rules. The message is the whole diagnostic, already formatted."""


@dataclass
class FieldEntry:
    id: int
    name: str
    status: str = LIVE
    removed_in: str = ""


@dataclass
class TypeEntry:
    id: int
    name: str
    module: str
    header: str
    status: str = LIVE
    next_field_id: int = 1
    removed_in: str = ""
    fields: list[FieldEntry] = field(default_factory=list)

    def live_field(self, name: str) -> FieldEntry | None:
        return next((f for f in self.fields if f.status == LIVE and f.name == name), None)

    def field_by_id(self, identifier: int) -> FieldEntry | None:
        return next((f for f in self.fields if f.id == identifier), None)


@dataclass
class Manifest:
    version: int = MANIFEST_VERSION
    next_type_id: int = 1
    types: list[TypeEntry] = field(default_factory=list)

    def live_type(self, name: str) -> TypeEntry | None:
        return next((t for t in self.types if t.status == LIVE and t.name == name), None)

    def type_by_id(self, identifier: int) -> TypeEntry | None:
        return next((t for t in self.types if t.id == identifier), None)

    def type_named(self, name: str) -> TypeEntry | None:
        """Any entry with this name, live or tombstoned. For diagnostics only."""
        return next((t for t in self.types if t.name == name), None)


# --- Reading ---------------------------------------------------------------------------------------


def load(path: Path) -> Manifest:
    if not path.exists():
        return Manifest()
    document = tomllib.loads(path.read_text(encoding="utf-8"))
    manifest = Manifest(
        version=int(document.get("version", MANIFEST_VERSION)),
        next_type_id=int(document.get("next_type_id", 1)),
    )
    if manifest.version != MANIFEST_VERSION:
        raise IdentityError(
            f"{path}: manifest version {manifest.version}, but this generator writes version "
            f"{MANIFEST_VERSION}. A version change is a migration, not a rewrite."
        )
    for entry in document.get("type", []):
        manifest.types.append(_type_from(entry, path))
    _verify_internal_consistency(manifest, path)
    return manifest


def loads(text: str) -> Manifest:
    """Parse a manifest from text. Used by the gate to read the committed version from git."""
    document = tomllib.loads(text)
    manifest = Manifest(
        version=int(document.get("version", MANIFEST_VERSION)),
        next_type_id=int(document.get("next_type_id", 1)),
    )
    for entry in document.get("type", []):
        manifest.types.append(_type_from(entry, Path("<git>")))
    return manifest


def _type_from(entry: dict, where: Path) -> TypeEntry:
    for key in ("id", "name"):
        if key not in entry:
            raise IdentityError(f"{where}: a [[type]] table has no '{key}'.")
    parsed = TypeEntry(
        id=int(entry["id"]),
        name=str(entry["name"]),
        module=str(entry.get("module", "")),
        header=str(entry.get("header", "")),
        status=str(entry.get("status", LIVE)),
        next_field_id=int(entry.get("next_field_id", 1)),
        removed_in=str(entry.get("removed_in", "")),
    )
    for sub in entry.get("field", []):
        parsed.fields.append(
            FieldEntry(
                id=int(sub["id"]),
                name=str(sub["name"]),
                status=str(sub.get("status", LIVE)),
                removed_in=str(sub.get("removed_in", "")),
            )
        )
    return parsed


def _verify_internal_consistency(manifest: Manifest, where: Path) -> None:
    """Checks a hand edit could break. Every one of them is a way to recycle a number."""
    seen: dict[int, str] = {}
    for entry in manifest.types:
        if entry.id <= 0:
            raise IdentityError(f"{where}: type '{entry.name}' has identifier {entry.id}; zero is "
                                f"the null identifier and is never assigned.")
        if entry.id in seen:
            raise IdentityError(
                f"{where}: types '{seen[entry.id]}' and '{entry.name}' both claim TypeId "
                f"{entry.id}. An identifier is never reused."
            )
        seen[entry.id] = entry.name
        if entry.id >= manifest.next_type_id:
            raise IdentityError(
                f"{where}: type '{entry.name}' has TypeId {entry.id}, but next_type_id is "
                f"{manifest.next_type_id}. The counter must be above every identifier ever issued, "
                f"or the next assignment recycles one."
            )
        _verify_fields(entry, where)


def _verify_fields(entry: TypeEntry, where: Path) -> None:
    seen: dict[int, str] = {}
    for sub in entry.fields:
        if sub.id <= 0:
            raise IdentityError(f"{where}: field '{entry.name}::{sub.name}' has identifier "
                                f"{sub.id}; zero is the null identifier.")
        if sub.id in seen:
            raise IdentityError(
                f"{where}: fields '{entry.name}::{seen[sub.id]}' and '{entry.name}::{sub.name}' "
                f"both claim FieldId {sub.id}. An identifier is never reused."
            )
        seen[sub.id] = sub.name
        if sub.id >= entry.next_field_id:
            raise IdentityError(
                f"{where}: field '{entry.name}::{sub.name}' has FieldId {sub.id}, but "
                f"'{entry.name}' has next_field_id {entry.next_field_id}."
            )


# --- Writing ---------------------------------------------------------------------------------------

HEADER = """\
# The identity manifest — the source of truth for type and field identifiers.
#
# Generated and appended to by tools/gen/reflect_gen.py; committed, reviewed, and never rewritten by
# hand except to record a rename or a removal. `just quality-identity` is the gate.
#
# An identifier is an OPAQUE NUMBER assigned on first sight. It is not a hash of the name, not a
# hash of the type, not an index, and not a content digest — see design.md section 1. Renaming a
# type or a field, moving it between namespaces, or relocating it between modules changes its `name`
# here and nothing else, so every scene, prefab override, save, animation binding and replication
# schema written against it stays valid.
#
# `status = "removed"` is a TOMBSTONE. The number stays here forever and is never issued again: a
# recycled identifier produces data that loads successfully and is wrong, which is the one failure
# mode with no diagnostic.
#
# `next_type_id` and each type's `next_field_id` are above every number ever issued, tombstones
# included. Lowering one recycles an identifier and is rejected.
"""


def dumps(manifest: Manifest) -> str:
    """The manifest as text. Deterministic: entries in identifier order, one field per line."""
    lines = [HEADER, "", f"version = {manifest.version}",
             f"next_type_id = {manifest.next_type_id}", ""]
    for entry in sorted(manifest.types, key=lambda t: t.id):
        lines.append("[[type]]")
        lines.append(f"id = {entry.id}")
        lines.append(f'name = "{_escape(entry.name)}"')
        lines.append(f'module = "{_escape(entry.module)}"')
        lines.append(f'header = "{_escape(entry.header)}"')
        lines.append(f'status = "{entry.status}"')
        if entry.status == REMOVED:
            lines.append(f'removed_in = "{_escape(entry.removed_in)}"')
        lines.append(f"next_field_id = {entry.next_field_id}")
        lines.append("")
        for sub in sorted(entry.fields, key=lambda f: f.id):
            lines.append("[[type.field]]")
            lines.append(f"id = {sub.id}")
            lines.append(f'name = "{_escape(sub.name)}"')
            lines.append(f'status = "{sub.status}"')
            if sub.status == REMOVED:
                lines.append(f'removed_in = "{_escape(sub.removed_in)}"')
            lines.append("")
    return "\n".join(lines).rstrip("\n") + "\n"


def _escape(text: str) -> str:
    return text.replace("\\", "\\\\").replace('"', '\\"')


# --- Reconciliation --------------------------------------------------------------------------------


@dataclass
class Identity:
    """What the emitter needs: the numbers, keyed by the names the parse found."""

    types: dict[str, int] = field(default_factory=dict)
    fields: dict[str, dict[str, int]] = field(default_factory=dict)


@dataclass
class Drift:
    """A live entry that is no longer declared anywhere in the tree."""

    kind: str  # "type" or "field"
    selector: str
    identifier: int
    header: str


@dataclass
class Reconciliation:
    identity: Identity
    drift: list[Drift]
    appended: list[str]  # selectors that were newly assigned, for the log
    changed: bool


def reconcile(manifest: Manifest, parsed_types: list, *, assign: bool) -> Reconciliation:
    """Match the tree against the manifest.

    `assign` is what separates generation from checking: with it, a newly seen declaration is
    appended and given the next number; without it, one is reported as a change the manifest has not
    recorded. Neither mode ever changes an identifier that already exists.
    """
    identity = Identity()
    appended: list[str] = []
    changed = False
    present_types: set[str] = set()

    for parsed in sorted(parsed_types, key=lambda t: t.name):
        present_types.add(parsed.name)
        entry = manifest.live_type(parsed.name)
        if entry is None:
            if not assign:
                appended.append(parsed.name)
                changed = True
                identity.types[parsed.name] = 0
                identity.fields[parsed.name] = {f.name: 0 for f in parsed.fields}
                continue
            entry = TypeEntry(id=manifest.next_type_id, name=parsed.name, module=parsed.module,
                              header=parsed.header)
            manifest.next_type_id += 1
            manifest.types.append(entry)
            appended.append(parsed.name)
            changed = True
        if entry.module != parsed.module or entry.header != parsed.header:
            # Metadata, not identity. Moving a type between modules is exactly what must not change
            # its number, so this is recorded and nothing else.
            entry.module, entry.header = parsed.module, parsed.header
            changed = True
        identity.types[parsed.name] = entry.id
        identity.fields[parsed.name] = _reconcile_fields(entry, parsed, appended, assign)
        changed = changed or bool(appended)

    drift = _drift(manifest, parsed_types, present_types)
    return Reconciliation(identity=identity, drift=drift, appended=appended, changed=changed)


def _reconcile_fields(entry: TypeEntry, parsed, appended: list[str], assign: bool) -> dict[str, int]:
    numbers: dict[str, int] = {}
    for parsed_field in parsed.fields:
        existing = entry.live_field(parsed_field.name)
        if existing is None:
            selector = f"{parsed.name}::{parsed_field.name}"
            appended.append(selector)
            if not assign:
                numbers[parsed_field.name] = 0
                continue
            existing = FieldEntry(id=entry.next_field_id, name=parsed_field.name)
            entry.next_field_id += 1
            entry.fields.append(existing)
        numbers[parsed_field.name] = existing.id
    return numbers


def _drift(manifest: Manifest, parsed_types: list, present_types: set[str]) -> list[Drift]:
    by_name = {parsed.name: parsed for parsed in parsed_types}
    headers = {parsed.header for parsed in parsed_types}
    found: list[Drift] = []
    for entry in manifest.types:
        if entry.status != LIVE:
            continue
        # Only a header that was actually parsed can testify about its own types. A manifest entry
        # whose header is not in this run's input set is not missing; it was not looked for.
        if entry.header not in headers:
            continue
        if entry.name not in present_types:
            found.append(Drift("type", entry.name, entry.id, entry.header))
            continue
        declared = {f.name for f in by_name[entry.name].fields}
        for sub in entry.fields:
            if sub.status == LIVE and sub.name not in declared:
                found.append(
                    Drift("field", f"{entry.name}::{sub.name}", sub.id, entry.header)
                )
    return sorted(found, key=lambda d: d.selector)


def drift_message(drift: list[Drift]) -> str:
    """The gate's diagnostic: what left the tree, its identifier, and the two fixes."""
    count = len(drift)
    subject = "declaration is" if count == 1 else "declarations are"
    lines = [
        f"identity: {count} {subject} recorded in the manifest but no longer declared in the tree.",
        "",
    ]
    for entry in drift:
        label = "TypeId " if entry.kind == "type" else "FieldId"
        lines.append(f"  {entry.selector}")
        lines.append(f"      {label} {entry.identifier}   declared in {entry.header}")
    lines += [
        "",
        "An identifier is assigned once and is never derived from a name, so the generator cannot",
        "tell a rename from a removal. Say which it was — both are a reviewed diff of the manifest:",
        "",
        "  It was RENAMED. The identifier does not change; the manifest records the new name:",
        f"      just generate-headers --rename '{drift[0].selector}=<the new name>'",
        "",
        "  It was REMOVED. The identifier is tombstoned and is never issued again:",
        f"      just generate-headers --tombstone '{drift[0].selector}'",
        "",
        "Nothing is written until one of those runs: a build that guessed would be a build that",
        "silently invalidated every scene, save and replication schema referring to it.",
    ]
    return "\n".join(lines)


# --- The explicit edits ------------------------------------------------------------------------------


def rename(manifest: Manifest, selector: str, new_name: str) -> str:
    """Record a rename. The identifier is untouched — that is the whole point of it."""
    if "::" in selector and _split_field(manifest, selector) is not None:
        entry, sub = _split_field(manifest, selector)
        if entry.live_field(new_name) is not None:
            raise IdentityError(
                f"'{entry.name}' already has a live field named '{new_name}'."
            )
        old, sub.name = sub.name, new_name
        return f"field {entry.name}::{old} -> {new_name} (FieldId {sub.id} unchanged)"

    entry = manifest.live_type(selector)
    if entry is None:
        raise IdentityError(_no_such_selector(selector))
    if manifest.live_type(new_name) is not None:
        raise IdentityError(f"a live type named '{new_name}' is already in the manifest.")
    old, entry.name = entry.name, new_name
    return f"type {old} -> {new_name} (TypeId {entry.id} unchanged)"


def tombstone(manifest: Manifest, selector: str, version: str) -> str:
    """Record a removal. The number stays in the manifest and is never issued again."""
    pair = _split_field(manifest, selector) if "::" in selector else None
    if pair is not None:
        entry, sub = pair
        sub.status, sub.removed_in = REMOVED, version
        return f"field {entry.name}::{sub.name} tombstoned (FieldId {sub.id} retired)"

    entry = manifest.live_type(selector)
    if entry is None:
        raise IdentityError(_no_such_selector(selector))
    entry.status, entry.removed_in = REMOVED, version
    for sub in entry.fields:
        if sub.status == LIVE:
            sub.status, sub.removed_in = REMOVED, version
    return f"type {entry.name} tombstoned (TypeId {entry.id} retired, with all of its fields)"


def _split_field(manifest: Manifest, selector: str):
    type_name, _, field_name = selector.rpartition("::")
    entry = manifest.live_type(type_name)
    if entry is None:
        return None
    sub = entry.live_field(field_name)
    return (entry, sub) if sub is not None else None


def _no_such_selector(selector: str) -> str:
    return (
        f"'{selector}' is not a live entry in the manifest. A selector is a fully qualified type "
        f"name, or a type name and a field name joined by '::' — for example "
        f"'cy::demo::Health' or 'cy::demo::Health::maximum'."
    )


# --- The gate ------------------------------------------------------------------------------------------


def compare_with_committed(current: Manifest, committed: Manifest) -> list[str]:
    """Every identifier that changed between two versions of the manifest.

    `core-type-system` requires a gate that "diffs the manifest and fails when an existing entry's
    identifier changes, so an accidental identity change is a red build rather than a corrupted save
    months later". Comparison is by identifier, not by name: a name is allowed to change, and a
    number is not.
    """
    problems: list[str] = []
    for old in committed.types:
        new = current.type_by_id(old.id)
        if new is None:
            problems.append(
                f"TypeId {old.id} ('{old.name}') is in the committed manifest and gone from this "
                f"one. An identifier is retired with a tombstone, never deleted."
            )
            continue
        problems.extend(_compare_fields(old, new))

    by_name = {entry.name: entry for entry in committed.types if entry.status == LIVE}
    for entry in current.types:
        previous = by_name.get(entry.name)
        if previous is not None and previous.id != entry.id:
            problems.append(
                f"type '{entry.name}' now has TypeId {entry.id}; the committed manifest gives it "
                f"TypeId {previous.id}. Every artefact written so far refers to {previous.id}."
            )
    return problems


def _compare_fields(old: TypeEntry, new: TypeEntry) -> list[str]:
    problems: list[str] = []
    for sub in old.fields:
        current = new.field_by_id(sub.id)
        if current is None:
            problems.append(
                f"FieldId {sub.id} of TypeId {old.id} ('{old.name}::{sub.name}') is in the "
                f"committed manifest and gone from this one. Tombstone it; never delete it."
            )
        elif sub.status == REMOVED and current.status == LIVE:
            problems.append(
                f"FieldId {sub.id} of '{old.name}' was tombstoned as '{sub.name}' and is live "
                f"again as '{current.name}'. A retired identifier is never issued again."
            )
    live_by_name = {sub.name: sub for sub in old.fields if sub.status == LIVE}
    for sub in new.fields:
        previous = live_by_name.get(sub.name)
        if previous is not None and previous.id != sub.id:
            problems.append(
                f"field '{new.name}::{sub.name}' now has FieldId {sub.id}; the committed manifest "
                f"gives it FieldId {previous.id}."
            )
    return problems
