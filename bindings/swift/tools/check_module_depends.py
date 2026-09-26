#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""A SwiftPM-built game module's build edge names every file SwiftPM compiles. M11.d task 9.8.

    check_module_depends.py --ninja <ninja> --build-dir <dir> --output <module> --package <dir>

The custom commands that run `cy_swift_module.py` (bindings/swift/CMakeLists.txt,
samples/04-character/CMakeLists.txt) are the only thing that tells Ninja when to run SwiftPM again,
and Ninja reruns an edge only when one of the inputs it DECLARES changes. Their DEPENDS once globbed
`Sources/*.swift`, while SwiftPM also compiles the CyberdyneABI C target — `shim.c`, its module map
and the `cy_abi.h` copy — so `#error` appended to `shim.c` rebuilt nothing, the tests loaded last
week's module, and only a fresh build failed. The incremental ledger inherited the same blind spot,
because it reads the same graph.

This reads the graph the build really has (`ninja -t inputs <module>`) and fails naming every file
under the package's `Sources/` that is not among the module's inputs. It fails, too, when the
listing names nothing under `Sources/` at all: a check over an empty set proves nothing.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys


def declared_inputs(ninja: str, build_dir: pathlib.Path, output: pathlib.Path) -> set[pathlib.Path]:
    listing = subprocess.run([ninja, "-C", str(build_dir), "-t", "inputs", str(output)],
                             capture_output=True, text=True, check=True).stdout
    return {pathlib.Path(os.path.normpath(build_dir / line.strip()))
            for line in listing.splitlines() if line.strip()}


def compiled_sources(package: pathlib.Path) -> list[pathlib.Path]:
    return sorted(path for path in (package / "Sources").rglob("*") if path.is_file())


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--ninja", required=True)
    parser.add_argument("--build-dir", required=True, type=pathlib.Path)
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--package", required=True, type=pathlib.Path)
    arguments = parser.parse_args(argv)

    inputs = declared_inputs(arguments.ninja, arguments.build_dir.resolve(), arguments.output)
    sources = [path.resolve() for path in compiled_sources(arguments.package)]
    missing = [path for path in sources if path not in inputs]
    print(f"{arguments.output.name}: {len(sources) - len(missing)} of {len(sources)} package "
          "source(s) are inputs of its build edge")
    if not sources:
        print(f"FAIL: {arguments.package}/Sources holds no file, so this checked nothing")
        return 1
    for path in missing:
        relative = path.relative_to(arguments.package.resolve())
        print(f"FAIL: SwiftPM compiles {relative}, and a change to it does not rebuild the module "
              "— add it to the custom command's DEPENDS")
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
