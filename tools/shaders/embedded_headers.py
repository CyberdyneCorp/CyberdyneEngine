#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Regenerate, or check the currency of, every committed embedded-shader header.

The engine checks compiled shaders in as C++ headers (`*_spirv.h`, `*_msl.h`, `*_dxil.h`) because
`CY_SHADER_SLANG` is off in Profile and Shipping and a pass must exist in a build with no shader
compiler. Each header is produced by a script beside its sources; what none of those scripts can do
is notice that a Slang file changed and the header did not. This tool is that notice.

`tools/shaders/embedded_headers.toml` declares, for every committed header, the slangc invocations
and the embed command that reproduce it. `check` copies the working tree into a scratch directory,
runs every group there, formats the outputs with clang-format as `just quality-format` would, and
compares them byte for byte with the committed headers. It also fails when a header matching one
of the embedded-shader name patterns is in the tree but in no group and not excluded, so a new
header cannot escape the check by never being listed.

    python3 tools/shaders/embedded_headers.py check --slangc <build>/Development/bin/slangc
    python3 tools/shaders/embedded_headers.py regenerate --slangc <slangc> [--group <name> ...]

`just quality-shader-headers` runs `check` against the dev build's slangc.
"""

from __future__ import annotations

import argparse
import dataclasses
import pathlib
import shutil
import subprocess
import sys
import tempfile
import tomllib

ROOT = pathlib.Path(__file__).resolve().parents[2]
MANIFEST = pathlib.Path("tools/shaders/embedded_headers.toml")
TARGET_ARGS = {
    "spirv": ["-target", "spirv", "-profile", "spirv_1_5"],
    "metal": ["-target", "metal"],
}


@dataclasses.dataclass(frozen=True)
class Tools:
    slangc: str
    clang_format: str
    python: str


@dataclasses.dataclass(frozen=True)
class Group:
    name: str
    headers: list[str]
    cwd: str
    source: str
    include: list[str]
    spirv_args: list[str]
    metal_args: list[str]
    compile: list[list[str]]
    run: list[list[str]]


@dataclasses.dataclass(frozen=True)
class Manifest:
    groups: list[Group]
    patterns: list[str]
    excluded: dict[str, str]


def load_manifest(root: pathlib.Path) -> Manifest:
    data = tomllib.loads((root / MANIFEST).read_text(encoding="utf-8"))
    groups = [
        Group(name=entry["name"], headers=entry["headers"], cwd=entry.get("cwd", "."),
              source=entry.get("source", ""), include=entry.get("include", []),
              spirv_args=entry.get("spirv_args", []),
              metal_args=entry.get("metal_args", []), compile=entry.get("compile", []),
              run=entry.get("run", []))
        for entry in data.get("group", [])
    ]
    excluded = {entry["header"]: entry["reason"] for entry in data.get("excluded", [])}
    return Manifest(groups=groups, patterns=data["patterns"], excluded=excluded)


def expand(argument: str, tools: Tools, scratch: pathlib.Path) -> str:
    return argument.format(slangc=tools.slangc, clang_format=tools.clang_format,
                           python=tools.python, scratch=scratch)


def compile_command(group: Group, row: list[str], tools: Tools,
                    scratch: pathlib.Path) -> list[str]:
    """One `compile` row: [target, stage, entry, output name] and optionally the source file."""
    target, stage, entry, out = row[:4]
    source = row[4] if len(row) > 4 else group.source
    command = [tools.slangc, source]
    for include in group.include:
        command += ["-I", include]
    command += ["-entry", entry, "-stage", stage, *TARGET_ARGS[target]]
    command += group.metal_args if target == "metal" else group.spirv_args
    return command + ["-o", str(scratch / out)]


def run_group(group: Group, root: pathlib.Path, tools: Tools) -> None:
    """Run one group's compiles and commands in `root`, then format its headers in place."""
    with tempfile.TemporaryDirectory(prefix=f"cy-{group.name}-") as directory:
        scratch = pathlib.Path(directory)
        for row in group.compile:
            subprocess.run(compile_command(group, row, tools, scratch), cwd=root / group.cwd,
                           check=True)
        for command in group.run:
            subprocess.run([expand(argument, tools, scratch) for argument in command], cwd=root,
                           check=True, stdout=subprocess.DEVNULL)
    subprocess.run([tools.clang_format, "-i", *group.headers], cwd=root, check=True)


def selected(manifest: Manifest, names: list[str]) -> list[Group]:
    if not names:
        return manifest.groups
    known = {group.name: group for group in manifest.groups}
    unknown = sorted(set(names) - known.keys())
    if unknown:
        raise SystemExit(f"unknown group(s): {', '.join(unknown)}")
    return [known[name] for name in names]


def unlisted_headers(manifest: Manifest, root: pathlib.Path) -> list[str]:
    listed = {header for group in manifest.groups for header in group.headers}
    listed |= manifest.excluded.keys()
    found = set()
    for pattern in manifest.patterns:
        found |= {path.relative_to(root).as_posix() for path in root.glob(pattern)}
    return sorted(found - listed)


def missing_headers(manifest: Manifest, root: pathlib.Path) -> list[str]:
    declared = [header for group in manifest.groups for header in group.headers]
    declared += list(manifest.excluded)
    return sorted(header for header in declared if not (root / header).is_file())


def tracked_files(root: pathlib.Path) -> list[str]:
    listing = subprocess.run(["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"],
                             cwd=root, check=True, capture_output=True).stdout
    return [name for name in listing.decode("utf-8").split("\0") if name]


def copy_tree(root: pathlib.Path, destination: pathlib.Path) -> None:
    for name in tracked_files(root):
        source = root / name
        if not source.is_file():
            continue
        target = destination / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)


def stale_headers(groups: list[Group], root: pathlib.Path,
                  copy: pathlib.Path) -> list[tuple[str, str]]:
    return [(header, group.name) for group in groups for header in group.headers
            if (root / header).read_bytes() != (copy / header).read_bytes()]


def keep_regenerated(stale: list[str], copy: pathlib.Path, artifacts: pathlib.Path) -> None:
    """Save what the check produced, so a machine without this slangc can take it as is."""
    for header in stale:
        target = artifacts / header
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(copy / header, target)


def check(manifest: Manifest, groups: list[Group], root: pathlib.Path, tools: Tools,
          artifacts: pathlib.Path | None) -> int:
    problems = [f"not in {MANIFEST}: {header}" for header in unlisted_headers(manifest, root)]
    problems += [f"declared but absent: {header}" for header in missing_headers(manifest, root)]
    with tempfile.TemporaryDirectory(prefix="cy-shader-headers-") as directory:
        copy = pathlib.Path(directory)
        copy_tree(root, copy)
        ran = []
        for group in groups:
            print(f"==> {group.name}", flush=True)
            try:
                run_group(group, copy, tools)
                ran.append(group)
            except subprocess.CalledProcessError as error:
                problems.append(f"group {group.name} did not regenerate: {error}")
        stale = stale_headers(ran, root, copy)
        problems += [f"stale: {header} (group {group})" for header, group in stale]
        if artifacts is not None and stale:
            keep_regenerated([header for header, _ in stale], copy, artifacts)
    if problems:
        for problem in problems:
            print(f"embedded shader headers: {problem}", file=sys.stderr)
        print("  Regenerate with: python3 tools/shaders/embedded_headers.py regenerate "
              "--slangc <slangc> [--group <name>]", file=sys.stderr)
        return 1
    print(f"embedded shader headers: {len(groups)} group(s) reproduce the committed headers")
    return 0


def regenerate(groups: list[Group], root: pathlib.Path, tools: Tools) -> int:
    for group in groups:
        print(f"==> {group.name}", flush=True)
        run_group(group, root, tools)
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("mode", choices=("check", "regenerate"))
    parser.add_argument("--slangc", required=True)
    parser.add_argument("--clang-format", default="clang-format")
    parser.add_argument("--group", action="append", default=[], help="limit to this group")
    parser.add_argument("--artifacts", type=pathlib.Path,
                        help="check: copy every stale header's regenerated text under this directory")
    parser.add_argument("--root", type=pathlib.Path, default=ROOT, help=argparse.SUPPRESS)
    args = parser.parse_args(argv)

    root = args.root.resolve()
    slangc = shutil.which(args.slangc) or args.slangc
    tools = Tools(slangc=str(pathlib.Path(slangc).resolve()), clang_format=args.clang_format,
                  python=sys.executable)
    manifest = load_manifest(root)
    groups = selected(manifest, args.group)
    if args.mode == "check":
        return check(manifest, groups, root, tools, args.artifacts)
    return regenerate(groups, root, tools)


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
