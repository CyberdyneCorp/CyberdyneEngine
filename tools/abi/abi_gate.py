#!/usr/bin/env python3
"""The ABI compatibility gate. Tasks 2.3 and 2.8, `native-abi`, design.md §1.

`native-abi`: "CI SHALL diff it against the committed baseline. Any change other than an append
SHALL fail CI unless accompanied by an explicit, reviewed approval entry recording the rationale and
the version bump."

design.md §1: "The gate lands with the FIRST EXPORTED SYMBOL, not with the first consumer. The
obligation starts the moment anything links against the table. An ABI that has been reordered once
is an ABI nobody can trust, and by M5 the editor is compiled against it — so the window in which a
reordering is cheap is this milestone and no other."

--- WHAT IT REFUSES, AND WHY EACH ONE IS A BREAK -------------------------------------------------

  reordered entry     A module holds an offset into the table. Swapping two entries does not fail to
                      compile anywhere: it calls the wrong function with the right-looking
                      arguments, in a shipped module nobody rebuilt.
  removed entry       An older module calls it. There is no version of "gone" that an already
                      compiled call can survive.
  changed signature   The caller pushes what it was compiled to push. A parameter widened from 32 to
                      64 bits is a silently corrupt argument, not a link error.
  inserted member     Same as a reorder, one level down: every member after it moves.
  changed enum value  A module compiled against the old value passes the old number.
  lowered version     A minor that goes backwards makes "is this at least 1.3?" answer wrongly.

--- WHAT IT ACCEPTS ------------------------------------------------------------------------------

  an appended table entry, with `CY_ABI_MINOR` incremented
  an appended enumerator with a value nothing else has used
  an appended member of a struct that carries `struct_size` (or, for the table, `table_size`) —
      because that prefix is what lets both sides read only the part they agree on
  a new top-level function
  a renamed PARAMETER, or any comment change: neither is in the description at all

--- THE ESCAPE HATCH IS A FILE, NOT A FLAG -------------------------------------------------------

A genuinely necessary break is recorded in src/abi/abi_approvals.toml with its rationale and the
version bump it belongs to, and the gate names the exact key to add when it fails. A `--force` flag
would be the same mechanism with none of the record.

Usage:
    python3 tools/abi/abi_gate.py            # check; exits 1 on an incompatible change
    python3 tools/abi/abi_gate.py --update   # rewrite the baseline after an accepted change
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import tomllib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from abi_describe import ParseError, describe  # noqa: E402  (after the path insert, deliberately)

REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
HEADER = REPOSITORY / "src" / "abi" / "include" / "cy" / "abi" / "cy_abi.h"
BASELINE = REPOSITORY / "src" / "abi" / "abi_baseline.json"
APPROVALS = REPOSITORY / "src" / "abi" / "abi_approvals.toml"


class Finding:
    """One incompatible change: the key an approval would name, and what it means."""

    def __init__(self, key: str, message: str) -> None:
        self.key = key
        self.message = message

    def render(self) -> str:
        return f"  {self.key}\n      {self.message}"


def index_by_name(records: list[dict]) -> dict[str, dict]:
    return {record["name"]: record for record in records}


def has_size_prefix(record: dict) -> bool:
    """Whether a struct may legitimately grow: its first member is its own size in bytes.

    This is the whole append-only mechanism at the struct level. `struct_size` (and `table_size`,
    inside `CyInterfaceHeader`) is what lets the reader copy only `min(theirs, mine)` bytes, so
    appending to such a struct is invisible to a module that predates the new member. Appending to a
    struct without one changes `sizeof` for everybody and is a break.
    """
    members = record.get("members", [])
    if not members:
        return False
    if members[0]["name"] == "struct_size":
        return True
    # The interface table's size lives in its header, which is its first member.
    return members[0]["name"] == "header" and members[0]["type"] == "CyInterfaceHeader"


def compare_enum(findings: list[Finding], name: str, old: dict, new: dict) -> None:
    new_by_name = {value["name"]: value["value"] for value in new["values"]}
    old_values = {value["value"] for value in old["values"]}
    for entry in old["values"]:
        if entry["name"] not in new_by_name:
            findings.append(
                Finding(
                    f"enum.{name}.{entry['name']}.removed",
                    f"the enumerator {entry['name']} = {entry['value']} was removed; a module "
                    f"compiled against it still passes that number",
                )
            )
        elif new_by_name[entry["name"]] != entry["value"]:
            findings.append(
                Finding(
                    f"enum.{name}.{entry['name']}.value",
                    f"{entry['name']} changed from {entry['value']} to "
                    f"{new_by_name[entry['name']]}; already-compiled callers keep passing "
                    f"{entry['value']}",
                )
            )
    for entry in new["values"]:
        old_named = {value["name"] for value in old["values"]}
        if entry["name"] not in old_named and entry["value"] in old_values:
            findings.append(
                Finding(
                    f"enum.{name}.{entry['name']}.recycled",
                    f"the new enumerator {entry['name']} reuses the value {entry['value']}, which "
                    f"another enumerator already had",
                )
            )


def compare_aggregate(findings: list[Finding], name: str, old: dict, new: dict) -> None:
    if old["kind"] != new["kind"]:
        findings.append(
            Finding(f"type.{name}.kind", f"{name} changed from a {old['kind']} to a {new['kind']}")
        )
        return

    old_members = old["members"]
    new_members = new["members"]
    for index, member in enumerate(old_members):
        if index >= len(new_members):
            findings.append(
                Finding(
                    f"struct.{name}.{member['name']}.removed",
                    f"the member {member['name']} at offset {member['offset']} is gone; every "
                    f"member after it in an already-compiled module has moved",
                )
            )
            continue
        current = new_members[index]
        if current["name"] != member["name"]:
            findings.append(
                Finding(
                    f"struct.{name}.{member['name']}.reordered",
                    f"member {index} was {member['name']} and is now {current['name']}; a module "
                    f"reads the field at the offset it was compiled with",
                )
            )
        elif current["type"] != member["type"]:
            findings.append(
                Finding(
                    f"struct.{name}.{member['name']}.type",
                    f"{member['name']} changed from '{member['type']}' to '{current['type']}'",
                )
            )
        elif current["offset"] != member["offset"]:
            findings.append(
                Finding(
                    f"struct.{name}.{member['name']}.offset",
                    f"{member['name']} moved from offset {member['offset']} to "
                    f"{current['offset']}",
                )
            )

    if len(new_members) > len(old_members) and not has_size_prefix(new):
        added = ", ".join(member["name"] for member in new_members[len(old_members) :])
        findings.append(
            Finding(
                f"struct.{name}.grown",
                f"{name} gained {added} but carries no `struct_size` first member, so its sizeof "
                f"changed for every module that already knows it",
            )
        )


def compare_signature(findings: list[Finding], key: str, label: str, old: dict, new: dict) -> None:
    if old["returns"] != new["returns"]:
        findings.append(
            Finding(
                f"{key}.returns",
                f"{label} returns '{new['returns']}' where it returned '{old['returns']}'",
            )
        )
    if old["parameters"] != new["parameters"]:
        findings.append(
            Finding(
                f"{key}.parameters",
                f"{label} takes ({', '.join(new['parameters'])}) where it took "
                f"({', '.join(old['parameters'])})",
            )
        )


def compare_table(findings: list[Finding], old: dict, new: dict) -> int:
    """Compare the interface table entry by entry. Returns how many entries were appended."""
    old_entries = old["entries"]
    new_entries = new["entries"]
    for index, entry in enumerate(old_entries):
        if index >= len(new_entries):
            findings.append(
                Finding(
                    f"table.{entry['name']}.removed",
                    f"entry {index} ({entry['name']}) was removed; an older module still calls "
                    f"through that slot",
                )
            )
            continue
        current = new_entries[index]
        if current["name"] != entry["name"]:
            findings.append(
                Finding(
                    f"table.{entry['name']}.reordered",
                    f"slot {index} held {entry['name']} and now holds {current['name']}; a module "
                    f"calls the slot, not the name",
                )
            )
        elif current["type"] != entry["type"]:
            findings.append(
                Finding(
                    f"table.{entry['name']}.signature",
                    f"{entry['name']} is now '{current['type']}' where it was '{entry['type']}'",
                )
            )
    return max(0, len(new_entries) - len(old_entries))


def compare(old: dict, new: dict) -> tuple[list[Finding], list[str]]:
    """Everything the gate has to say: the breaks, and the appends worth reporting."""
    findings: list[Finding] = []
    notes: list[str] = []

    if new["abi"]["major"] != old["abi"]["major"]:
        findings.append(
            Finding(
                "abi.major",
                f"the major version went from {old['abi']['major']} to {new['abi']['major']}. A "
                f"major bump is a new ABI and needs an approval entry saying what happens to the "
                f"old one",
            )
        )
    if new["abi"]["minor"] < old["abi"]["minor"]:
        findings.append(
            Finding(
                "abi.minor",
                f"the minor version went backwards, {old['abi']['minor']} to "
                f"{new['abi']['minor']}",
            )
        )

    old_types = index_by_name(old["types"])
    new_types = index_by_name(new["types"])
    for name, record in old_types.items():
        current = new_types.get(name)
        if current is None:
            findings.append(
                Finding(f"type.{name}.removed", f"the type {name} is gone from the header")
            )
            continue
        if record["kind"] == "enum" and current["kind"] == "enum":
            compare_enum(findings, name, record, current)
        elif record["kind"] in ("struct", "union"):
            compare_aggregate(findings, name, record, current)
        elif record["kind"] == "function_pointer" and current["kind"] == "function_pointer":
            compare_signature(findings, f"typedef.{name}", name, record, current)
        elif record["kind"] == "alias" and current["kind"] == "alias":
            if record["underlying"] != current["underlying"]:
                findings.append(
                    Finding(
                        f"type.{name}.underlying",
                        f"{name} is now {current['underlying']} where it was "
                        f"{record['underlying']}",
                    )
                )
    for name in new_types:
        if name not in old_types:
            notes.append(f"new type {name}")

    old_functions = index_by_name(old["functions"])
    new_functions = index_by_name(new["functions"])
    for name, record in old_functions.items():
        current = new_functions.get(name)
        if current is None:
            findings.append(
                Finding(f"function.{name}.removed", f"the exported function {name} is gone")
            )
            continue
        compare_signature(findings, f"function.{name}", name, record, current)
    for name in new_functions:
        if name not in old_functions:
            notes.append(f"new exported function {name}")

    appended = compare_table(findings, old["table"], new["table"])
    if appended:
        names = ", ".join(entry["name"] for entry in new["table"]["entries"][-appended:])
        notes.append(f"{appended} appended table entr{'y' if appended == 1 else 'ies'}: {names}")
        # AN APPEND IS ADDITIVE, AND ADDITIVE CHANGES BUMP THE MINOR. Without this the "Newer
        # engine, older module" scenario has no way to be true: a module asking for 1.0 and a module
        # asking for 1.1 would be handed the same table and one of them would be wrong about what it
        # contains.
        if new["abi"]["minor"] <= old["abi"]["minor"]:
            findings.append(
                Finding(
                    "abi.minor.not-bumped",
                    f"the table grew by {appended} entr{'y' if appended == 1 else 'ies'} but "
                    f"CY_ABI_MINOR is still {new['abi']['minor']}. An additive change increments "
                    f"it — `native-abi`'s \"Additive change passes\"",
                )
            )

    return findings, notes


def load_approvals() -> dict[str, dict]:
    if not APPROVALS.exists():
        return {}
    document = tomllib.loads(APPROVALS.read_text(encoding="utf-8"))
    approvals: dict[str, dict] = {}
    for entry in document.get("approval", []):
        if "change" not in entry or "rationale" not in entry or "version" not in entry:
            raise ValueError(
                "every [[approval]] needs `change`, `rationale` and `version`; an approval without "
                "a reason recorded is a flag with extra steps"
            )
        approvals[entry["change"]] = entry
    return approvals


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Diff the C ABI against its committed baseline.")
    parser.add_argument("--header", type=pathlib.Path, default=HEADER)
    parser.add_argument("--baseline", type=pathlib.Path, default=BASELINE)
    parser.add_argument(
        "--update",
        action="store_true",
        help="rewrite the baseline from the header, after the change has been accepted",
    )
    arguments = parser.parse_args(argv)

    try:
        current = describe(arguments.header)
    except ParseError as error:
        print(f"abi-gate: {arguments.header}: {error}", file=sys.stderr)
        return 1

    text = json.dumps(current, indent=2, sort_keys=True) + "\n"

    if not arguments.baseline.exists():
        if not arguments.update:
            print(
                f"abi-gate: no baseline at {arguments.baseline.relative_to(REPOSITORY)}.\n"
                f"  The baseline is the thing an ABI is compared against, so the first one is\n"
                f"  written deliberately:\n"
                f"      python3 tools/abi/abi_gate.py --update",
                file=sys.stderr,
            )
            return 1
        arguments.baseline.write_text(text, encoding="utf-8")
        print(f"abi-gate: wrote the first baseline, {len(current['table']['entries'])} entries")
        return 0

    baseline = json.loads(arguments.baseline.read_text(encoding="utf-8"))
    findings, notes = compare(baseline, current)

    try:
        approvals = load_approvals()
    except ValueError as error:
        print(f"abi-gate: {APPROVALS.relative_to(REPOSITORY)}: {error}", file=sys.stderr)
        return 1

    approved = [finding for finding in findings if finding.key in approvals]
    refused = [finding for finding in findings if finding.key not in approvals]

    for finding in approved:
        entry = approvals[finding.key]
        print(
            f"abi-gate: approved break {finding.key} (version {entry['version']}): "
            f"{entry['rationale']}"
        )

    if refused:
        print(
            f"abi-gate: {len(refused)} incompatible change(s) against "
            f"{arguments.baseline.relative_to(REPOSITORY)}:\n",
            file=sys.stderr,
        )
        for finding in refused:
            print(finding.render(), file=sys.stderr)
        print(
            "\n  The interface table is append-only within a major version: entries are added at "
            "the\n"
            "  end and CY_ABI_MINOR is incremented. Existing entries are never reordered, removed "
            "or\n"
            "  given a different signature — see cy/abi/cy_abi.h and openspec/specs/native-abi.\n"
            "\n"
            "  If a break is genuinely unavoidable, record it rather than force it. Add to "
            f"{APPROVALS.relative_to(REPOSITORY)}:\n",
            file=sys.stderr,
        )
        for finding in refused:
            print(
                f'      [[approval]]\n'
                f'      change = "{finding.key}"\n'
                f'      rationale = "why this could not be an append"\n'
                f'      version = "the version this lands in"\n',
                file=sys.stderr,
            )
        print(
            "  Then rewrite the baseline: python3 tools/abi/abi_gate.py --update", file=sys.stderr
        )
        return 1

    if arguments.update:
        arguments.baseline.write_text(text, encoding="utf-8")
        print(f"abi-gate: baseline updated, {len(current['table']['entries'])} entries")
        return 0

    if text != arguments.baseline.read_text(encoding="utf-8"):
        # Compatible, but the committed description no longer matches the header. The description is
        # what the Swift overlay and the Rust SDK are generated from (design.md §2), so a stale one
        # is a generated overlay that is missing the entry somebody just added.
        for note in notes:
            print(f"abi-gate: {note}")
        print(
            "abi-gate: the change is compatible, but the committed description is stale.\n"
            "  The overlays are generated from it, so refresh it in the same commit:\n"
            "      python3 tools/abi/abi_gate.py --update",
            file=sys.stderr,
        )
        return 1

    print(
        f"abi-gate: {len(current['table']['entries'])} table entries, "
        f"{len(current['functions'])} exported functions, ABI "
        f"{current['abi']['major']}.{current['abi']['minor']}.{current['abi']['patch']} — "
        f"matches the baseline"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
