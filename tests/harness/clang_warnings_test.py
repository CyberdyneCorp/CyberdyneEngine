#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile the harness probe with every clang on this host, under the project's warning set.

The harness macros expand into every test in the tree, so a warning they raise under one compiler
fails every test binary under it. Clang 18 reported -Wdouble-promotion inside CY_CHECK_NEAR and
clang 22 reported `__COUNTER__` as a C2y extension inside every test declaration, while the GCC
builds that CI runs saw neither. This test is where a clang on the host sees them first.

    clang_warnings_test.py --probe <file.cpp> --include <dir>... --define <NAME>... -- <flags>...

Exit 3 (CTest's skip) when the host has no clang: nothing was evaluated, and that is not a pass.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

CANDIDATES = ["clang++"] + [f"clang++-{major}" for major in range(17, 31)]


def compilers() -> list[str]:
    """Every distinct clang++ on PATH, by resolved binary."""
    found: dict[str, str] = {}
    for name in CANDIDATES:
        path = shutil.which(name)
        if path:
            found.setdefault(os.path.realpath(path), path)
    return list(found.values())


def compile_probe(compiler: str, arguments: argparse.Namespace) -> subprocess.CompletedProcess:
    command = [compiler, "-std=c++20", "-fsyntax-only", "-Werror", *arguments.flags]
    command += [f"-D{name}" for name in arguments.define]
    command += [f"-isystem{path}" for path in arguments.system_include]
    command += [f"-I{path}" for path in arguments.include]
    return subprocess.run(command + [arguments.probe], capture_output=True, text=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--probe", required=True)
    parser.add_argument("--include", action="append", default=[])
    parser.add_argument("--system-include", action="append", default=[])
    parser.add_argument("--define", action="append", default=[])
    parser.add_argument("flags", nargs="*")
    arguments = parser.parse_args()

    found = compilers()
    if not found:
        print("NOT EVALUATED: no clang++ on PATH")
        return 3
    failed = 0
    for compiler in found:
        result = compile_probe(compiler, arguments)
        verdict = "ok  " if result.returncode == 0 else "FAIL"
        print(f"{verdict} {compiler}")
        if result.returncode != 0:
            failed += 1
            sys.stdout.write(result.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
