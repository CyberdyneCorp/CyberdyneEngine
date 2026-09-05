#!/usr/bin/env python3
"""Produce the machine-readable description of the C ABI. Task 2.8, `native-abi`.

`native-abi`: "The build SHALL generate a machine-readable description of the ABI (function names,
signatures, struct layouts, enum values) and CI SHALL diff it against the committed baseline."

This is the generator half. `abi_gate.py` is the diff, and design.md §2 names a third consumer: the
Swift overlay and the Rust SDK are generated from THIS description, so that neither can drift from
the ABI or from each other.

--- WHY THE LAYOUT IS COMPUTED HERE RATHER THAN MEASURED FROM A COMPILED PROBE -------------------

The obvious way to learn a struct's layout is to compile a program that prints `offsetof`. It was
rejected for two reasons.

  * A description produced by compiling is a description of ONE toolchain on ONE machine. The
    baseline is committed and diffed across every platform in the matrix; if it were a measurement,
    a Windows runner and a Linux runner would legitimately produce different files and the gate
    would have nothing stable to compare against.
  * The whole point of the C ABI is that its layout is FIXED BY THE DECLARATION rather than chosen
    by the compiler. Every member is a fixed-width integer, a float, a pointer, or an array of
    those; there are no bitfields, no `long`, no `int`, and no implementation-defined enum width in
    a struct. So the layout is derivable, and deriving it states the rule instead of sampling it.

That is a claim, so it is checked: src/abi/tests/test_layout.cpp asserts the compiler's `sizeof` and
`offsetof` against exactly the numbers this file computes. If the model is ever wrong on a platform,
that test fails there rather than the description quietly describing a struct that does not exist.

--- THE PARSER IS SMALL, AND DELIBERATELY UNFORGIVING -------------------------------------------

It reads the subset of C that cy/abi/cy_abi.h is written in and refuses anything else, naming the
line. A parser that skipped what it did not understand would silently drop an ABI entry from the
description, and the gate would then approve removing it. So an unknown declaration is an error.

Usage:
    python3 tools/abi/abi_describe.py [--header PATH] [--output PATH]
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys

REPOSITORY = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_HEADER = REPOSITORY / "src" / "abi" / "include" / "cy" / "abi" / "cy_abi.h"

# The layout model. Every one of these is fixed-width by the C standard or by the platform ABI this
# engine targets; a type not in this table and not declared in the header is an error, because
# guessing its size is exactly how a description stops describing anything.
PRIMITIVES: dict[str, tuple[int, int]] = {
    "bool": (1, 1),
    "char": (1, 1),
    "int8_t": (1, 1),
    "uint8_t": (1, 1),
    "int16_t": (2, 2),
    "uint16_t": (2, 2),
    "int32_t": (4, 4),
    "uint32_t": (4, 4),
    "int64_t": (8, 8),
    "uint64_t": (8, 8),
    "float": (4, 4),
    "double": (8, 8),
    # LP64 and LLP64 agree on both of these. A 32-bit target would not, and the engine does not
    # target one; if it ever does, this table is where that decision is written down.
    "size_t": (8, 8),
    "void*": (8, 8),
}

POINTER = (8, 8)


class ParseError(Exception):
    """A declaration this parser does not understand. Always names the text it choked on."""


def strip_comments(text: str) -> str:
    """Remove /* */ and // comments without disturbing line structure."""
    out: list[str] = []
    index = 0
    length = len(text)
    while index < length:
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = length if end < 0 else end + 2
            # Keep the newlines so that error messages can still name a line.
            out.append("\n" * text.count("\n", index, end))
            index = end
        elif text.startswith("//", index):
            end = text.find("\n", index)
            index = length if end < 0 else end
        else:
            out.append(text[index])
            index += 1
    return "".join(out)


def split_declarations(text: str) -> list[str]:
    """Every top-level declaration, split on the semicolons that are not inside braces."""
    declarations: list[str] = []
    depth = 0
    current: list[str] = []
    for character in text:
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
        if character == ";" and depth == 0:
            declarations.append("".join(current).strip())
            current = []
            continue
        current.append(character)
    tail = "".join(current).strip()
    if tail:
        declarations.append(tail)
    return declarations


def normalise(text: str) -> str:
    """One space between tokens, and none around `*`, so two spellings compare equal."""
    text = re.sub(r"\s+", " ", text).strip()
    text = re.sub(r"\s*\*\s*", "* ", text)
    return text.strip()


class Description:
    """The ABI as data: versions, types in declaration order, functions, and the table."""

    def __init__(self) -> None:
        self.version: dict[str, int] = {}
        self.types: list[dict] = []
        self.functions: list[dict] = []
        self.by_name: dict[str, dict] = {}

    # --- the layout model ------------------------------------------------------------------------

    def size_align(self, spelling: str) -> tuple[int, int]:
        spelling = normalise(spelling)
        if spelling.endswith("*"):
            return POINTER
        spelling = spelling.replace("const ", "").replace("struct ", "").strip()
        if spelling.endswith("*"):
            return POINTER
        if spelling in PRIMITIVES:
            return PRIMITIVES[spelling]
        record = self.by_name.get(spelling)
        if record is None:
            raise ParseError(f"unknown type '{spelling}' — add it to PRIMITIVES or declare it")
        if record["kind"] in ("struct", "union"):
            return record["size"], record["alignment"]
        if record["kind"] == "enum":
            # A C enum whose enumerators fit in an int is an int. The header keeps every enum's
            # values inside that range on purpose, and none of them is a struct member — the
            # structs carry uint32_t where a tag is meant, precisely so that the enum's width is
            # never part of a layout.
            return 4, 4
        if record["kind"] in ("handle", "function_pointer"):
            return POINTER
        if record["kind"] == "alias":
            return self.size_align(record["underlying"])
        raise ParseError(f"cannot size '{spelling}'")

    def add(self, record: dict) -> None:
        self.types.append(record)
        self.by_name[record["name"]] = record

    # --- members ---------------------------------------------------------------------------------

    def parse_member(self, text: str) -> dict:
        """One struct or union member: a plain field, an array, or a function pointer."""
        text = normalise(text)

        pointer_match = re.fullmatch(r"(.+?)\(\* ?(\w+)\)\((.*)\)", text)
        if pointer_match:
            returns = normalise(pointer_match.group(1))
            # Parameter NAMES are dropped here for the same reason they are dropped from a plain
            # function's signature: renaming one is a documentation change, and a gate that failed
            # on it would be switched off within a month.
            params = parameter_types(pointer_match.group(3))
            return {
                "name": pointer_match.group(2),
                "type": f"{returns}(*)({', '.join(params)})",
                "size": POINTER[0],
                "alignment": POINTER[1],
            }

        array_match = re.fullmatch(r"(.+?) (\w+)\[(\d+)\]", text)
        if array_match:
            element = normalise(array_match.group(1))
            count = int(array_match.group(3))
            size, alignment = self.size_align(element)
            return {
                "name": array_match.group(2),
                "type": f"{element}[{count}]",
                "size": size * count,
                "alignment": alignment,
            }

        plain_match = re.fullmatch(r"(.+?)\s*\b(\w+)", text)
        if not plain_match:
            raise ParseError(f"cannot read the member '{text}'")
        spelling = normalise(plain_match.group(1))
        size, alignment = self.size_align(spelling)
        return {
            "name": plain_match.group(2),
            "type": spelling,
            "size": size,
            "alignment": alignment,
        }

    def lay_out(self, kind: str, members: list[dict]) -> tuple[int, int]:
        """Offsets, size and alignment, by the rules a C compiler applies to this subset."""
        alignment = 1
        offset = 0
        for member in members:
            alignment = max(alignment, member["alignment"])
            if kind == "union":
                member["offset"] = 0
                offset = max(offset, member["size"])
                continue
            padding = (-offset) % member["alignment"]
            offset += padding
            member["offset"] = offset
            offset += member["size"]
        size = offset + ((-offset) % alignment)
        return size, alignment


def split_parameters(text: str) -> list[str]:
    """Split a parameter list on the commas that are not inside parentheses."""
    parts: list[str] = []
    depth = 0
    current: list[str] = []
    for character in text:
        if character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
        if character == "," and depth == 0:
            parts.append("".join(current))
            current = []
            continue
        current.append(character)
    if "".join(current).strip():
        parts.append("".join(current))
    return [part for part in parts if part.strip()]


def parameter_types(text: str) -> list[str]:
    """The TYPES of a parameter list, with the names dropped.

    A parameter's name is documentation and renaming one is not an ABI change, so it must not be one
    in the description either — otherwise the gate would fail on a comment improvement and be
    switched off within a month.
    """
    types: list[str] = []
    for part in split_parameters(text):
        part = normalise(part)
        if part in ("void", ""):
            continue
        # A function-pointer parameter keeps its whole spelling; anything else loses a trailing
        # identifier if there is one.
        if "(*" in part:
            types.append(re.sub(r"\(\* ?\w+\)", "(*)", part))
            continue
        match = re.fullmatch(r"(.+?)\s*\b(\w+)", part)
        if match and match.group(1).strip() and match.group(2) not in PRIMITIVES:
            candidate = normalise(match.group(1))
            if candidate not in ("const", ""):
                types.append(candidate)
                continue
        types.append(part)
    return types


def parse(header_text: str) -> Description:
    description = Description()

    for key in ("MAJOR", "MINOR", "PATCH"):
        match = re.search(rf"^#define CY_ABI_{key}\s+(\d+)u?\s*$", header_text, re.MULTILINE)
        if not match:
            raise ParseError(f"CY_ABI_{key} is not declared")
        description.version[key.lower()] = int(match.group(1))

    body = strip_comments(header_text)
    # Preprocessor lines, the C++ linkage braces and the assertion calls are not declarations. They
    # are removed by name rather than skipped silently, so that a declaration this parser has never
    # seen still reaches the error at the bottom of the loop.
    body = re.sub(r"^\s*#.*$", "", body, flags=re.MULTILINE)
    body = body.replace('extern "C" {', "").replace("}\n#endif", "")

    for declaration in split_declarations(body):
        declaration = declaration.strip()
        if not declaration or declaration == "}":
            continue
        if declaration.startswith("CY_ABI_STATIC_ASSERT"):
            continue
        parse_declaration(description, declaration)

    return description


def parse_declaration(description: Description, declaration: str) -> None:
    flat = normalise(declaration)

    enum_match = re.fullmatch(r"typedef enum (\w+) \{(.*)\} (\w+)", flat)
    if enum_match:
        if enum_match.group(1) != enum_match.group(3):
            raise ParseError(f"enum tag and typedef name differ: {flat[:60]}")
        description.add(parse_enum(enum_match.group(1), enum_match.group(2)))
        return

    aggregate_match = re.fullmatch(r"typedef (struct|union) (\w+) \{(.*)\} (\w+)", flat)
    if aggregate_match:
        kind, tag, body, name = aggregate_match.groups()
        if tag != name:
            raise ParseError(f"{kind} tag and typedef name differ: {flat[:60]}")
        members = [
            description.parse_member(member)
            for member in split_declarations(body)
            if member.strip()
        ]
        size, alignment = description.lay_out(kind, members)
        description.add(
            {"kind": kind, "name": name, "size": size, "alignment": alignment, "members": members}
        )
        return

    handle_match = re.fullmatch(r"typedef struct (\w+)\* (\w+)", flat)
    if handle_match:
        description.add(
            {"kind": "handle", "name": handle_match.group(2), "tag": handle_match.group(1)}
        )
        return

    fnptr_match = re.fullmatch(r"typedef (.+?)\(\* ?(\w+)\)\((.*)\)", flat)
    if fnptr_match:
        description.add(
            {
                "kind": "function_pointer",
                "name": fnptr_match.group(2),
                "returns": normalise(fnptr_match.group(1)),
                "parameters": parameter_types(fnptr_match.group(3)),
            }
        )
        return

    alias_match = re.fullmatch(r"typedef (.+?) (\w+)", flat)
    if alias_match and "(" not in flat:
        description.add(
            {
                "kind": "alias",
                "name": alias_match.group(2),
                "underlying": normalise(alias_match.group(1)),
            }
        )
        return

    function_match = re.fullmatch(r"(.+?)\b(cy_\w+)\((.*)\)", flat)
    if function_match:
        description.functions.append(
            {
                "name": function_match.group(2),
                "returns": normalise(function_match.group(1)),
                "parameters": parameter_types(function_match.group(3)),
            }
        )
        return

    raise ParseError(f"unrecognised declaration: {flat[:120]}")


def parse_enum(name: str, body: str) -> dict:
    values: list[dict] = []
    next_value = 0
    for entry in body.split(","):
        entry = entry.strip()
        if not entry:
            continue
        match = re.fullmatch(r"(\w+)(?:\s*=\s*(-?\w+))?", entry)
        if not match:
            raise ParseError(f"cannot read the enumerator '{entry}' of {name}")
        if match.group(2) is not None:
            next_value = int(match.group(2), 0)
        values.append({"name": match.group(1), "value": next_value})
        next_value += 1
    return {"kind": "enum", "name": name, "values": values}


def describe(header: pathlib.Path) -> dict:
    """The whole description, as the JSON document the gate diffs and the overlays are built from."""
    description = parse(header.read_text(encoding="utf-8"))

    table = description.by_name.get("CyInterface")
    if table is None or table["kind"] != "struct":
        raise ParseError("the header declares no CyInterface table")

    entries = []
    for index, member in enumerate(table["members"]):
        entries.append({"index": index, "name": member["name"], "type": member["type"]})

    return {
        "abi": {
            "major": description.version["major"],
            "minor": description.version["minor"],
            "patch": description.version["patch"],
        },
        "types": description.types,
        "functions": description.functions,
        "table": {"name": "CyInterface", "size": table["size"], "entries": entries},
    }


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Describe the C ABI as JSON.")
    parser.add_argument("--header", type=pathlib.Path, default=DEFAULT_HEADER)
    parser.add_argument("--output", type=pathlib.Path, default=None)
    arguments = parser.parse_args(argv)

    try:
        document = describe(arguments.header)
    except ParseError as error:
        print(f"abi_describe: {arguments.header}: {error}", file=sys.stderr)
        return 1

    # Sorted keys and a trailing newline: the file is committed and diffed, so its formatting has to
    # be a property of this program rather than of the Python version that ran it.
    text = json.dumps(document, indent=2, sort_keys=True) + "\n"
    if arguments.output is None:
        sys.stdout.write(text)
    else:
        arguments.output.write_text(text, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
