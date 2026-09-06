"""What the description cannot say about a table entry, said once and checked. Task 2.3.

`tools/abi/abi_describe.py` records each entry's name and its signature with **parameter names
stripped** — deliberately, because a parameter name is not part of a C ABI and a baseline that
diffed on one would fail a review that renamed `entity` to `target`. A generated Rust API whose
parameters read `a0`, `a1`, `a2` is one nobody will call, and one whose doc comment cannot name what
it takes is one no agent can call either.

So the names live here, hand-written, beside the one other thing a C declaration does not carry:
whether an entry returning `CyResult` is reporting a **failure** (the wrapper yields
`Result<_, Status>`) or answering a **question** (`get_last_error_code` returns a status and never
fails, so a mechanical "returns CyResult therefore fallible" rule would make asking what went wrong
itself a fallible call).

THIS TABLE CANNOT DRIFT SILENTLY, WHICH IS THE ONLY REASON IT IS ACCEPTABLE. `validate()` refuses a
description containing an entry with no record here, refuses a record naming an entry that is not in
the description, and refuses a record whose parameter-name count differs from the signature's arity.
Appending an entry to `CyInterface` therefore stops generation until somebody names its parameters —
the same shape as the ABI gate itself: the change is legal, and it is not silent.

WHY THIS IS A SECOND COPY OF `tools/gen/swift/overlay/entries.py` RATHER THAN A SHARED ONE. The two
tables answer the same question for different languages and they are validated against the same
description, so neither can drift away from the ABI without failing generation. Sharing them would
couple two generators through a module that belongs to neither, and would put Swift's argument
labels — which are part of Swift's API and not of Rust's — into the Rust generator's vocabulary.
The duplication is checked; the coupling would not be.
"""

from __future__ import annotations

from dataclasses import dataclass


class EntryError(Exception):
    """The entry table and the ABI description disagree. Names every disagreement."""


@dataclass(frozen=True)
class Entry:
    """One table entry's Rust face.

    `result` is what the generated wrapper does with the C return value:
      `value`    — hand it back unchanged, in its FFI spelling. The safe layer decides what a null
                   pointer means, because C cannot say and a generator that trusted the header's
                   prose would be wrong on the day the prose was.
      `fallible` — it is a `CyResult`; the wrapper yields `Result<(), Status>`, `Ok` for
                   `CY_RESULT_OK` and the status otherwise.
    """

    parameters: tuple[str, ...]
    result: str = "value"
    #: One line for the generated doc comment. Written for a reader who has the SDK and not the C
    #: header — the same audience `editor-agent-interface` writes command descriptions for.
    doc: str = ""


# In the table's order, which is the order `CyInterface` declares and the order the gate protects.
# The list is not itself the contract — the description is — but keeping it in order makes a
# reviewer's diff of an append read as an append.
ENTRIES: dict[str, Entry] = {
    # diagnostics
    "log": Entry(("engine", "severity", "message"), doc="Emit a message through the engine's log."),
    "get_last_error": Entry((), doc="The last error message on this thread, or null."),
    "get_last_error_code": Entry((), doc="The last error code on this thread. Never fails."),
    "set_last_error": Entry(("result", "message"), doc="Set this thread's last error."),
    # values
    "var_make_string": Entry(("engine", "utf8", "length"), doc="A heap-backed string `CyVar`."),
    "var_make_bytes": Entry(("engine", "data", "size"), doc="A heap-backed byte-buffer `CyVar`."),
    "var_clone": Entry(("value",), doc="A copy that owns its own heap allocation, if any."),
    "var_release": Entry(("value",), doc="Release a `CyVar`'s heap allocation, if any."),
    "var_live_count": Entry(("engine",), doc="Live heap-backed `CyVar` count, for leak detection."),
    # the world
    "engine_world": Entry(("engine",), doc="The world bound to this engine, or null."),
    "world_create_entity": Entry(("world",), doc="Create an entity and return its identifier."),
    "world_destroy_entity": Entry(("world", "entity"), result="fallible",
                                  doc="Destroy an entity."),
    "world_entity_alive": Entry(("world", "entity"), doc="Whether an entity identifier is live."),
    "world_epoch": Entry(("world",), doc="The world's structural-change counter."),
    # components
    "world_register_component": Entry(("world", "descriptor"),
                                      doc="Register a component type; 0 on failure."),
    "world_find_component": Entry(("world", "name"),
                                  doc="Look a component type up by name; 0 when absent."),
    "world_add_component": Entry(("world", "entity", "component", "initial"), result="fallible",
                                 doc="Add a component, optionally copying an initial value."),
    "world_remove_component": Entry(("world", "entity", "component"), result="fallible",
                                    doc="Remove a component from an entity."),
    "world_has_component": Entry(("world", "entity", "component"),
                                 doc="Whether an entity has a component."),
    "world_borrow_component": Entry(("world", "entity", "component"),
                                    doc="Borrow a component's storage with the world's epoch."),
    "borrow_valid": Entry(("world", "borrow"),
                          doc="Whether a borrow predates the world's current epoch."),
    "component_get_var": Entry(("world", "entity", "component", "field", "into"),
                               result="fallible", doc="Read one field as a `CyVar`."),
    "component_set_var": Entry(("world", "entity", "component", "field", "value"),
                               result="fallible", doc="Write one field from a `CyVar`."),
    "component_get_f32": Entry(("world", "entity", "component", "field", "into"),
                               result="fallible", doc="Read one `f32` field."),
    "component_set_f32": Entry(("world", "entity", "component", "field", "value"),
                               result="fallible", doc="Write one `f32` field."),
    "component_get_vec3": Entry(("world", "entity", "component", "field", "into"),
                                result="fallible", doc="Read one three-float field."),
    "component_set_vec3": Entry(("world", "entity", "component", "field", "xyz"),
                                result="fallible", doc="Write one three-float field."),
    # behaviours
    "register_behaviour": Entry(("engine", "name", "vtable"),
                                doc="Register a behaviour type; null on failure."),
    "find_behaviour": Entry(("engine", "name"), doc="Look a behaviour type up by name."),
    "behaviour_generation": Entry(("behaviour",), doc="The reload generation a behaviour is from."),
    # 1.1: describing a world the caller did not build
    "world_component_count": Entry(("world",), doc="How many component types the world knows."),
    "world_component_info": Entry(("world", "component", "into"), result="fallible",
                                  doc="Describe a component type: size, alignment, field count."),
    "world_component_field": Entry(("world", "component", "field", "into"), result="fallible",
                                   doc="Describe one field of a component type."),
    # 1.1: the hierarchy
    "world_parent": Entry(("world", "entity"), doc="An entity's parent, or the null entity."),
    "world_set_parent": Entry(("world", "child", "parent"), result="fallible",
                              doc="Reparent an entity."),
    "world_child_count": Entry(("world", "entity"), doc="How many children an entity has."),
    "world_child": Entry(("world", "entity", "index"),
                         doc="One child, in the ECS's order rather than the authored order."),
    # 1.1: chunks
    "world_chunks": Entry(("world", "component", "into", "capacity", "count"), result="fallible",
                          doc="Enumerate a component's chunks; a null buffer asks for the count."),
}

RESULT_KINDS = frozenset({"value", "fallible"})


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
    name, fixing it, and being told about the next is three round trips for one edit.
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
        if len(record.parameters) != len(parameters):
            problems.append(
                f"  {name}: {len(record.parameters)} name(s) for {len(parameters)} parameter(s) "
                f"({entry['type']})")
        if record.result not in RESULT_KINDS:
            problems.append(f"  {name}: result kind {record.result!r} is not one of "
                            f"{sorted(RESULT_KINDS)}")
        if record.result == "fallible" and signature(entry)[0] != "CyResult":
            problems.append(f"  {name}: marked fallible but returns {signature(entry)[0]}")
        if not record.doc:
            problems.append(f"  {name}: no doc line. The SDK is read by callers who do not have "
                            f"cy_abi.h in front of them")

    for name in ENTRIES:
        if name not in described:
            problems.append(f"  {name}: named here, absent from CyInterface. Remove the Entry — the "
                            f"ABI is append-only, so this means it was never added or the parser "
                            f"stopped recognising it")

    if problems:
        raise EntryError(
            "tools/gen/rust/sdk/entries.py does not cover the ABI description:\n"
            + "\n".join(problems))
