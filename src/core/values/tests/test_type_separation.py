#!/usr/bin/env python3
"""The compile-time half of "asset ids are distinct from handles".

`core-type-system` states it plainly: an `AssetId` is a 128-bit stable identifier for content, a
`Handle` is a runtime slot reference, and the two are never interchangeable. A runtime test can
only show that the values differ. This one shows the stronger property the specification actually
asks for — that the confusion does not compile — and it covers the tag separation between two
handle kinds, which has the same shape and the same consequence.

Six compilations. One must succeed and five must fail, each for a different confusion. Task 1.3.2.
"""

from __future__ import annotations

import argparse
import pathlib
import subprocess
import sys


CASES = (
    ("ok.cpp", True, "an asset id and a handle used as themselves compile"),
    ("handle_to_asset.cpp", False, "a handle does not convert to an asset id"),
    ("asset_to_handle.cpp", False, "an asset id does not convert to a handle"),
    ("compare_asset_and_handle.cpp", False, "an asset id and a handle do not compare"),
    ("mixed_handle_tags.cpp", False, "a handle of one tag is not a handle of another"),
    ("entity_from_handle.cpp", False, "a handle does not convert to an entity id"),
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

    print("type-separation: PASS" if failures == 0 else f"type-separation: {failures} failures")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
