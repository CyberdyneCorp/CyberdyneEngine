"""What the description cannot say about a table entry, said once and checked. Task 3.1.

`tools/abi/abi_describe.py` records each entry's name and its signature with **parameter names
stripped** — deliberately, because a parameter name is not part of a C ABI and a baseline that
diffed on one would fail a review that renamed `entity` to `target`. Swift, though, has argument
labels, and a generated API whose labels read `_ a0:`, `_ a1:` is a generated API nobody will call.

So the labels live here, hand-written, beside two other things the C declaration genuinely does not
carry: whether an entry that returns `CyResult` is reporting a **failure** (the wrapper throws) or
answering a **question** (`get_last_error_code` returns a status and never fails), and whether a
returned pointer may be null.

THIS TABLE CANNOT DRIFT SILENTLY, WHICH IS THE ONLY REASON IT IS ACCEPTABLE. `validate()` refuses a
description containing an entry with no record here, refuses a record naming an entry that is not in
the description, and refuses a record whose label count differs from the signature's arity. So
appending an entry to `CyInterface` fails generation until somebody names its parameters — which is
the same shape as the ABI gate itself: the change is legal, and it is not silent.
"""

from __future__ import annotations

from dataclasses import dataclass


class EntryError(Exception):
    """The entry table and the ABI description disagree. Names every disagreement."""


@dataclass(frozen=True)
class Entry:
    """One table entry's Swift face.

    `result` is what the wrapper does with the C return value:
      `value`    — hand it back unchanged. A returned pointer becomes an Optional either way,
                   mechanically, because C cannot say that one is never null and a generator that
                   trusted the prose would trap on the day the prose was wrong.
      `throwing` — it is a `CyResult`; throw `CyberdyneError` on anything but `CY_RESULT_OK`.

    Only two kinds, and the distinction is real rather than cosmetic: `get_last_error_code` returns
    a `CyResult` and never fails — it *answers* with a status — so a mechanical "returns CyResult
    therefore throws" rule would make asking what went wrong itself throw.
    """

    labels: tuple[str, ...]
    result: str = "value"


# In the table's order, which is the order `CyInterface` declares and the order the gate protects.
# The list is not itself the contract — the description is — but keeping it in order makes a
# reviewer's diff of an append read as an append.
ENTRIES: dict[str, Entry] = {
    # diagnostics
    "log": Entry(("engine", "severity", "message")),
    "get_last_error": Entry(()),
    "get_last_error_code": Entry(()),
    "set_last_error": Entry(("result", "message")),
    # values
    "var_make_string": Entry(("engine", "utf8", "length")),
    "var_make_bytes": Entry(("engine", "data", "size")),
    "var_clone": Entry(("value",)),
    "var_release": Entry(("value",)),
    "var_live_count": Entry(("engine",)),
    # the world
    "engine_world": Entry(("engine",)),
    "world_create_entity": Entry(("world",)),
    "world_destroy_entity": Entry(("world", "entity"), result="throwing"),
    "world_entity_alive": Entry(("world", "entity")),
    "world_epoch": Entry(("world",)),
    # components
    "world_register_component": Entry(("world", "desc")),
    "world_find_component": Entry(("world", "name")),
    "world_add_component": Entry(("world", "entity", "component", "initial"), result="throwing"),
    "world_remove_component": Entry(("world", "entity", "component"), result="throwing"),
    "world_has_component": Entry(("world", "entity", "component")),
    "world_borrow_component": Entry(("world", "entity", "component")),
    "borrow_valid": Entry(("world", "borrow")),
    "component_get_var": Entry(("world", "entity", "component", "field", "into"),
                               result="throwing"),
    "component_set_var": Entry(("world", "entity", "component", "field", "value"),
                               result="throwing"),
    "component_get_f32": Entry(("world", "entity", "component", "field", "into"),
                               result="throwing"),
    "component_set_f32": Entry(("world", "entity", "component", "field", "value"),
                               result="throwing"),
    "component_get_vec3": Entry(("world", "entity", "component", "field", "into"),
                                result="throwing"),
    "component_set_vec3": Entry(("world", "entity", "component", "field", "xyz"),
                                result="throwing"),
    # behaviours
    "register_behaviour": Entry(("engine", "name", "vtable")),
    "find_behaviour": Entry(("engine", "name")),
    "behaviour_generation": Entry(("type",)),
}

RESULT_KINDS = frozenset({"value", "throwing"})


def signature(entry: dict) -> tuple[str, list[str]]:
    """Split `void(*)(CyEngine, uint32_t, const char*)` into its return type and its parameters."""
    text = entry["type"]
    marker = "(*)("
    index = text.find(marker)
    if index < 0 or not text.endswith(")"):
        raise EntryError(f"entry {entry['name']!r}: {text!r} is not a function-pointer signature")
    returns = text[:index].strip()
    inside = text[index + len(marker) : -1].strip()
    parameters = [item.strip() for item in inside.split(",")] if inside else []
    return returns, parameters


def function_entries(description: dict) -> list[dict]:
    """The table's function-pointer entries: everything but the `CyInterfaceHeader` at index 0."""
    return [entry for entry in description["table"]["entries"] if "(*)(" in entry["type"]]


def validate(description: dict) -> None:
    """Refuse a description this table does not exactly cover. Reports every problem, not the first.

    Reporting all of them matters when the ABI grows by several entries at once, which is what
    `native-abi`'s append-only rule makes the normal shape of a change: being told about one missing
    label, fixing it, and being told about the next is three round trips for one edit.
    """
    problems: list[str] = []
    described = {entry["name"]: entry for entry in function_entries(description)}

    for name, entry in described.items():
        record = ENTRIES.get(name)
        if record is None:
            problems.append(
                f"  {name}: in CyInterface, absent here. Add an Entry naming its parameters; the "
                f"signature is {entry['type']}")
            continue
        _, parameters = signature(entry)
        if len(record.labels) != len(parameters):
            problems.append(
                f"  {name}: {len(record.labels)} label(s) for {len(parameters)} parameter(s) "
                f"({entry['type']})")
        if record.result not in RESULT_KINDS:
            problems.append(f"  {name}: result kind {record.result!r} is not one of "
                            f"{sorted(RESULT_KINDS)}")

    for name in ENTRIES:
        if name not in described:
            problems.append(f"  {name}: named here, absent from CyInterface. Remove the Entry — the "
                            f"ABI is append-only, so this means it was never added or the parser "
                            f"stopped recognising it")

    if problems:
        raise EntryError(
            "tools/gen/swift/overlay/entries.py does not cover the ABI description:\n"
            + "\n".join(problems))
