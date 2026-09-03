#!/usr/bin/env python3
"""The compile-time half of the privacy invariant.

design.md section 2 and `diagnostics-profiling-and-crash` — "Privacy classification": every field
carries a classification, and an unclassified field is reported. The other tests prove that an
unclassified *value* never reaches an artefact. This one proves the stronger property: an
unclassified field cannot be declared at all, because the declaration does not compile.

Four compilations. One must succeed and three must fail, each for a different reason: the
classification omitted, a classification that is not a compile-time constant, and a field type that
does not exist. Task 3.5.2.
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys


CASES = (
    ("classified.cpp", True, "a field with all three arguments compiles"),
    ("unclassified.cpp", False, "a field declared without a classification does not compile"),
    ("runtime_classification.cpp", False, "a classification decided at run time does not compile"),
    ("unregistered_type.cpp", False, "a field type that does not exist does not compile"),
)


def compile_one(compiler: str, includes: list[str], source: pathlib.Path) -> tuple[bool, str]:
    command = [compiler, "-std=c++20", "-fno-exceptions", "-fno-rtti", "-fsyntax-only"]
    for include in includes:
        command += ["-I", include]
    command.append(str(source))
    completed = subprocess.run(command, capture_output=True, text=True, check=False)
    return completed.returncode == 0, (completed.stderr or completed.stdout).strip()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--include", action="append", default=[])
    parser.add_argument("--sources", required=True, help="the compile_fail/ directory")
    args = parser.parse_args()

    directory = pathlib.Path(args.sources)
    failures = 0
    for name, must_compile, description in CASES:
        succeeded, output = compile_one(args.compiler, args.include, directory / name)
        if succeeded == must_compile:
            first = output.splitlines()[0] if output and not succeeded else ""
            print(f"ok   {name}: {description}" + (f"\n     {first}" if first else ""))
            continue
        failures += 1
        print(f"FAIL {name}: {description}", file=sys.stderr)
        print(output, file=sys.stderr)

    print("field-macro: PASS" if failures == 0 else f"field-macro: {failures} failures")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
