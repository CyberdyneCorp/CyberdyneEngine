#!/usr/bin/env python3
"""The engine core links no Swift runtime. Task 3.9 — proven, not asserted.

    check_no_swift_runtime.py --build-dir <dir> [--allow <glob>]...

`swift-scripting`, "Swift is a consumer of the ABI, not a dependency of the core": the engine core
SHALL NOT link against, require, or embed the Swift runtime, and "no Swift runtime SHALL be linked
and the binary SHALL contain no Swift symbols".

--- WHY THIS IS A PROGRAM AND NOT A SENTENCE IN A DOCUMENT ----------------------------------------

Because it is the kind of claim that stops being true by accident. Nothing in CMake would refuse a
`target_link_libraries(cy_runtime swiftCore)`; the layer checker would not see it, because
`libswiftCore.so` is not a target in this build. The only thing that can catch it is a look at what
was actually linked, and the only honest place to look is the built artefacts.

--- TWO CHECKS, BECAUSE ONE OF THEM CAN PASS WHILE THE CLAIM IS FALSE ------------------------------

  * DT_NEEDED — what the binary asks the dynamic loader for. Catches a link against the runtime.
  * Swift mangled symbols (`$s...`) in the dynamic symbol table. Catches Swift code STATICALLY
    linked in, which has no DT_NEEDED entry at all and which the first check would pass over.

--- AND ONE RULE THAT MAKES A PASS MEAN SOMETHING --------------------------------------------------

It fails when it inspected nothing. A checker that walks an empty directory and reports success is
the failure this milestone has already seen twice: M3's render gate passed in 0.03 s over two
device-free suites. So the number of inspected binaries is printed, and zero is an error.

WHAT IS DELIBERATELY EXCLUDED: the Swift game modules themselves. `libCyGame_g*.so` is where the
Swift runtime is SUPPOSED to be — `swift-scripting`'s "the Swift runtime libraries SHALL be bundled
with the game, not with the engine core" — so finding it there is the requirement being met, not
broken. The exclusion is by name and is printed, so it cannot quietly grow to cover a real binary.
"""

from __future__ import annotations

import argparse
import fnmatch
import pathlib
import subprocess
import sys

ELF_MAGIC = b"\x7fELF"

# The runtime libraries a Swift binary asks for. Matched as a prefix on the SONAME.
SWIFT_LIBRARIES = ("libswift", "libFoundation", "lib_Concurrency", "lib_StringProcessing")

# What a Swift symbol looks like once mangled. `$s` is Swift 5's stable prefix; `$S` and `_T0` are
# the two older ones, checked as well so that a binary built by an older toolchain is not missed.
SWIFT_SYMBOL_PREFIXES = ("$s", "$S", "_T0")

# Binaries that are ALLOWED to carry Swift, because carrying it is what they are for.
DEFAULT_ALLOWED = ("libCyGame_g*.so", "*.swift-module.so")


def is_elf(path: pathlib.Path) -> bool:
    try:
        with path.open("rb") as handle:
            return handle.read(4) == ELF_MAGIC
    except OSError:
        return False


def readelf(path: pathlib.Path, *flags: str) -> str:
    result = subprocess.run(["readelf", *flags, str(path)], check=False, text=True,
                            capture_output=True)
    return result.stdout


def needed_libraries(path: pathlib.Path) -> list[str]:
    names = []
    for line in readelf(path, "-d").splitlines():
        if "(NEEDED)" not in line:
            continue
        start = line.find("[")
        end = line.find("]", start)
        if start >= 0 and end > start:
            names.append(line[start + 1 : end])
    return names


def swift_symbols(path: pathlib.Path) -> list[str]:
    found = []
    for line in readelf(path, "--dyn-syms", "--wide").splitlines():
        fields = line.split()
        if len(fields) < 8:
            continue
        name = fields[7]
        if name.startswith(SWIFT_SYMBOL_PREFIXES):
            found.append(name)
    return found


def candidates(build_dir: pathlib.Path, allowed: list[str]) -> tuple[list[pathlib.Path], int]:
    """Every ELF the engine's build produced, minus the ones allowed to carry Swift.

    `_deps/` is skipped: those are third-party sources the build fetches, and what THEY link is not
    a statement about the engine core. `.build/` is skipped for the same reason and one more — it is
    the Swift package manager's scratch, so everything in it is Swift BY CONSTRUCTION: the macro
    plugin the compiler launches, swift-syntax's own libraries, the intermediate objects of the game
    modules. None of it is an engine binary, and none of it ships.
    """
    inspected: list[pathlib.Path] = []
    skipped = 0
    for path in sorted(build_dir.rglob("*")):
        if not path.is_file() or path.is_symlink():
            continue
        if "_deps" in path.parts or ".build" in path.parts:
            continue
        if path.suffix in (".o", ".a", ".json", ".txt", ".cmake", ".ninja"):
            continue
        if not is_elf(path):
            continue
        if any(fnmatch.fnmatch(path.name, pattern) for pattern in allowed):
            skipped += 1
            continue
        inspected.append(path)
    return inspected, skipped


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description="Assert the engine links no Swift runtime.")
    parser.add_argument("--build-dir", type=pathlib.Path, required=True)
    parser.add_argument("--allow", action="append", default=[],
                        help="a filename glob that MAY carry Swift (repeatable)")
    arguments = parser.parse_args(argv)

    if not arguments.build_dir.is_dir():
        print(f"no such build directory: {arguments.build_dir}", file=sys.stderr)
        return 2

    allowed = list(DEFAULT_ALLOWED) + arguments.allow
    inspected, skipped = candidates(arguments.build_dir, allowed)

    failures: list[str] = []
    for path in inspected:
        linked = [name for name in needed_libraries(path)
                  if name.startswith(SWIFT_LIBRARIES)]
        if linked:
            failures.append(f"  {path}: links {', '.join(linked)}")
        symbols = swift_symbols(path)
        if symbols:
            failures.append(f"  {path}: {len(symbols)} Swift symbol(s), first {symbols[0]}")

    print(f"==> swift runtime  {len(inspected)} engine binaries inspected, "
          f"{skipped} game module(s) exempt ({', '.join(allowed)})")
    if not inspected:
        # A vacuous pass is the failure mode this check exists downstream of. M3's render gate
        # passed in 0.03 s over two device-free suites; a checker that inspects nothing reports the
        # same green as one that inspects everything.
        print("nothing was inspected: the build directory holds no engine binaries. This is a "
              "vacuous pass, not a pass.", file=sys.stderr)
        return 1
    if failures:
        print("the engine core links or embeds Swift:", file=sys.stderr)
        print("\n".join(failures), file=sys.stderr)
        print("\n  swift-scripting: 'The engine core SHALL NOT link against, require, or embed the "
              "Swift runtime.' Swift reaches the engine through cy/abi/cy_abi.h and nothing else.",
              file=sys.stderr)
        return 1
    print("the engine core links no Swift runtime and contains no Swift symbols")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
