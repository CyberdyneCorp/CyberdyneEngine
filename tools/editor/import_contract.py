#!/usr/bin/env python3
"""Compare the import catalogue exposed by cy_import_cli and CyberEditor."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
from typing import NamedTuple


class Importer(NamedTuple):
    name: str
    extensions: tuple[str, ...]
    settings: tuple[tuple[str, str], ...]


def parse_tool(text: str) -> tuple[Importer, ...]:
    rows: list[Importer] = []
    name: str | None = None
    extensions: tuple[str, ...] = ()
    settings: list[tuple[str, str]] = []

    def finish() -> None:
        nonlocal name, extensions, settings
        if name is not None:
            rows.append(Importer(name, extensions, tuple(settings)))
        name = None
        extensions = ()
        settings = []

    for line in text.splitlines():
        stripped = line.strip()
        if line and not line[0].isspace() and " (version " in stripped:
            finish()
            name = stripped.split(" (version ", 1)[0]
        elif stripped.startswith("extensions:"):
            extensions = tuple(value.lower() for value in stripped.split()[1:])
        elif stripped.startswith("--set ") and "=<" in stripped:
            declaration = stripped.removeprefix("--set ")
            option, kind = declaration.split("=<", 1)
            settings.append((option, kind.rstrip(">")))
    finish()
    return tuple(rows)


def parse_editor(text: str) -> tuple[Importer, ...]:
    rows: list[Importer] = []
    for number, line in enumerate(text.splitlines(), 1):
        fields = line.split("\t")
        if len(fields) != 3:
            raise ValueError(f"editor row {number} has {len(fields)} fields, expected 3: {line!r}")
        name, raw_extensions, raw_settings = fields
        extensions = tuple(filter(None, raw_extensions.split(",")))
        settings: list[tuple[str, str]] = []
        for declaration in filter(None, raw_settings.split(",")):
            if ":" not in declaration:
                raise ValueError(f"editor setting has no type: {declaration!r}")
            settings.append(tuple(declaration.split(":", 1)))
        rows.append(Importer(name, extensions, tuple(settings)))
    return tuple(rows)


def run(executable: Path, *arguments: str, environment: dict[str, str] | None = None) -> str:
    completed = subprocess.run(
        [str(executable), *arguments],
        check=False,
        capture_output=True,
        text=True,
        env=environment,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"{' '.join([str(executable), *arguments])} exited {completed.returncode}:\n"
            f"{completed.stderr.strip()}"
        )
    return completed.stdout


def compare(tool: Path, editor: Path) -> None:
    tool_rows = parse_tool(run(tool, "--list-importers"))
    environment = os.environ.copy()
    environment["CY_IMPORT_CLI"] = str(tool.resolve())
    editor_rows = parse_editor(run(editor, "--list-importers", environment=environment))
    if not tool_rows:
        raise AssertionError("cy_import_cli reported no importers")
    if editor_rows != tool_rows:
        print("tools/import reports:", file=sys.stderr)
        for row in tool_rows:
            print(f"  {row}", file=sys.stderr)
        print("CyberEditor reaches:", file=sys.stderr)
        for row in editor_rows:
            print(f"  {row}", file=sys.stderr)
        raise AssertionError("the editor importer catalogue differs from tools/import")
    extensions = sum(len(row.extensions) for row in tool_rows)
    settings = sum(len(row.settings) for row in tool_rows)
    print(
        f"editor import parity: {len(tool_rows)} importer(s), "
        f"{extensions} extension(s), {settings} setting(s)"
    )


def selftest() -> None:
    tool = """gltf (version 2)\n  glTF\n  extensions: .gltf .GLB\n  --set scale=<float>\n      scale\n\nobj (version 1)\n  OBJ\n  extensions: .obj\n"""
    expected = (
        Importer("gltf", (".gltf", ".glb"), (("scale", "float"),)),
        Importer("obj", (".obj",), ()),
    )
    assert parse_tool(tool) == expected
    assert parse_editor("gltf\t.gltf,.glb\tscale:float\nobj\t.obj\t\n") == expected
    assert parse_editor("gltf\t.gltf\tscale:float\n") != expected
    print("import contract selftest: pass")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("tool", nargs="?", type=Path)
    parser.add_argument("editor", nargs="?", type=Path)
    parser.add_argument("--selftest", action="store_true")
    arguments = parser.parse_args()
    if arguments.selftest:
        selftest()
        return 0
    if arguments.tool is None or arguments.editor is None:
        parser.error("TOOL and EDITOR are required unless --selftest is used")
    compare(arguments.tool, arguments.editor)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
