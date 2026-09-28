#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""shaders/embed_msl.py, exercised through the compiler that builds the engine.

The forward fragment grew past 65,536 bytes and Apple clang refused frame_msl.h: a string literal,
concatenation included, is capped (-Woverlength-strings under -Werror). The embedder now joins a long
module from chunks at compile time. What has to hold is observable only by compiling the header, so
each case writes one, compiles it with the engine's warnings, runs it, and compares the bytes.

Run:  ctest -R integration.render_embed_msl   (or)  python3 test_embed_msl.py --cxx c++
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
EMBED = HERE.parent / "shaders" / "embed_msl.py"
REPO = HERE.parents[3]

# The engine's warning set, the part that decides whether a long literal is accepted.
FLAGS = ["-std=c++20", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]

PROGRAM = """\
#include "frame_msl.h"

#include <cstdio>

int main() {
    const char* text = cy::rendering::pipeline::kModuleMsl;
    const unsigned long size = sizeof(cy::rendering::pipeline::kModuleMsl) - 1;
    return std::fwrite(text, 1, size, stdout) == size ? 0 : 1;
}
"""


class Failure(Exception):
    pass


def module(size: int) -> str:
    """MSL-shaped text of roughly `size` bytes, with a UTF-8 line so chunks split on bytes."""
    lines = ["#include <metal_stdlib>\n", "// été — not ASCII\n"]
    index = 0
    while sum(len(line.encode("utf-8")) for line in lines) < size:
        lines.append(f"float value_{index}(float x) {{ return x * {index}.0; }}\n")
        index += 1
    return "".join(lines)


def embed_and_run(cxx: list[str], source: str, workspace: Path) -> tuple[str, bytes]:
    metal = workspace / "Module.metal"
    metal.write_text(source, encoding="utf-8")
    header = workspace / "frame_msl.h"
    subprocess.run([sys.executable, str(EMBED), str(header), f"kModuleMsl={metal}"], check=True)
    (workspace / "main.cpp").write_text(PROGRAM, encoding="utf-8")
    binary = workspace / "main"
    compiled = subprocess.run(
        [*cxx, *FLAGS, "-I", str(workspace), "-I", str(REPO / "src/core/base/include"),
         str(workspace / "main.cpp"), "-o", str(binary)],
        capture_output=True, text=True, check=False)
    if compiled.returncode != 0:
        raise Failure(f"the generated header does not compile:\n{compiled.stderr}")
    return header.read_text(encoding="utf-8"), subprocess.run([str(binary)], capture_output=True,
                                                              check=True).stdout


def test_a_module_longer_than_a_literal_compiles_and_round_trips(cxx: list[str], workspace: Path) -> None:
    source = module(70_000)
    header, emitted = embed_and_run(cxx, source, workspace)
    if "detail::join_msl" not in header:
        raise Failure("a 70,000-byte module was not chunked")
    if emitted != source.encode("utf-8"):
        raise Failure("the joined module differs from the source it was embedded from")


def test_a_short_module_stays_one_literal(cxx: list[str], workspace: Path) -> None:
    source = module(2_000)
    header, emitted = embed_and_run(cxx, source, workspace)
    if "join_msl" in header or 'inline constexpr char kModuleMsl[] = R"cy_msl(' not in header:
        raise Failure("a short module should be emitted exactly as before, as one literal")
    if emitted != source.encode("utf-8"):
        raise Failure("the short module differs from its source")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", required=True, help="the C++ compiler the engine is built with")
    parser.add_argument("--cxx-flag", action="append", default=[],
                        help="a flag the build passes the compiler, e.g. -isysroot=<sdk> on macOS")
    arguments = parser.parse_args(argv)
    tests = [value for name, value in sorted(globals().items()) if name.startswith("test_")]
    failures = 0
    for test in tests:
        with tempfile.TemporaryDirectory(prefix="cy-embed-msl-") as workspace:
            try:
                test([arguments.cxx, *arguments.cxx_flag], Path(workspace))
            except (Failure, subprocess.CalledProcessError) as failure:
                failures += 1
                print(f"FAIL  {test.__name__}\n      {failure}")
            else:
                print(f"ok    {test.__name__}")
    print(f"\n{len(tests) - failures}/{len(tests)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
