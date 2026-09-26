#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""`unit.build_symbols` — every refusal `symbols.py` makes, each on a binary built to deserve it.

M11.d task 7.5. A check that has only ever been seen green is a claim, so each case below builds the
exact input the check exists to refuse and requires the refusal — naming the property, not merely a
non-zero exit. The probes are compiled here with the host's C compiler, because the property under
test is about what a LINKER produces and a committed binary would be a photograph of one linker.

    python3 tools/build/test_symbols.py            run every case
    python3 tools/build/test_symbols.py --flags "<compile flags>" --compiler-id <id>
                                                   also check a configuration keeps debug info
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import symbols  # noqa: E402  — the path above is what makes it importable

PROBE = "int helper(int x) { return x * 3; }\nint main(int c, char** v) { (void)v; return helper(c); }\n"
OTHER = "int main(void) { return 7; }\n"


def compile_probe(work: Path, name: str, source: str, *flags: str) -> Path:
    compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if compiler is None:
        raise SystemExit("unit.build_symbols: no C compiler on PATH; the probes cannot be built")
    source_path = work / f"{name}.c"
    source_path.write_text(source)
    out = work / name
    subprocess.run([compiler, *flags, str(source_path), "-o", str(out)], check=True)
    return out


def refused(what: str, needle: str, action) -> None:
    try:
        action()
    except symbols.SymbolError as refusal:
        if needle not in str(refusal):
            raise AssertionError(f"{what}: refused for the wrong reason: {refusal}") from None
        print(f"  ok   {what}: refused — {refusal}")
        return
    raise AssertionError(f"{what}: was ACCEPTED")


def cases(work: Path) -> None:
    store = work / "store"
    probe = compile_probe(work, "probe", PROBE, "-g", "-O1", "-Wl,--build-id")

    done = symbols.split(probe, work / "ship" / "probe", store, "package-0001")
    verified = symbols.verify(done.shipped, store, "probe.c")
    print(f"  ok   a split binary verifies against its archive: main -> {verified.location}")
    located = symbols.locate(store, "package-0001")
    assert located == [("probe", done.symbols)], located
    assert symbols.locate(store, done.build_id)[0][1] == done.symbols
    print("  ok   the package build identity and the GNU build-id both locate the symbols")

    refused("an unstripped binary", "is not stripped",
            lambda: symbols.verify(probe, store, "probe.c"))

    nodebug = compile_probe(work, "nodebug", PROBE, "-O1", "-Wl,--build-id")
    refused("a binary compiled without -g", "no debug information to archive",
            lambda: symbols.split(nodebug, work / "ship" / "nodebug", store))

    anonymous = compile_probe(work, "anonymous", PROBE, "-g", "-Wl,--build-id=none")
    refused("a binary with no build-id", "no GNU build-id",
            lambda: symbols.split(anonymous, work / "ship" / "anonymous", store))

    # ANOTHER BUILD'S SYMBOLS, filed under this build's id: what a careless archive, or a rebuilt
    # binary copied over the archived one, would produce. It symbolicates without complaint.
    other = compile_probe(work, "other", OTHER, "-g", "-Wl,--build-id")
    other_split = symbols.split(other, work / "ship" / "other", work / "other-store")
    shutil.copyfile(other_split.symbols, done.symbols)
    refused("another build's symbols under this build-id", "are for build-id",
            lambda: symbols.verify(done.shipped, store, "probe.c"))

    # THE SAME BUILD-ID, DIFFERENT BYTES: the archive edited after the split. Only the debuglink's
    # CRC can see this, which is why it is checked rather than trusted.
    symbols.split(probe, work / "ship" / "probe", store)
    archived = bytearray(done.symbols.read_bytes())
    archived[-1] ^= 0xFF
    done.symbols.write_bytes(bytes(archived))
    refused("symbols changed after the split", "CRC-32",
            lambda: symbols.verify(done.shipped, store, "probe.c"))

    symbols.split(probe, work / "ship" / "probe", store)
    refused("symbols that resolve main to the wrong file", "resolved to",
            lambda: symbols.verify(done.shipped, store, "main.cpp"))

    text = work / "script.sh"
    text.write_text("#!/bin/sh\n")
    refused("a file that is not ELF", "not ELF", lambda: symbols.read_elf(text))


def check_configuration(flags: str, compiler_id: str) -> None:
    """A configuration that ships must compile WITH debug information, or `split` has nothing to
    archive. This is the regression case for cmake/profiles.cmake's Shipping row, which said symbols
    were split at packaging time while compiling with no `-g` at all."""
    words = flags.split()
    wanted = ("/Zi", "/Z7") if compiler_id == "MSVC" else ("-g", "-g1", "-g2", "-g3", "-ggdb")
    if not any(word in wanted for word in words):
        raise AssertionError(f"the shipping configuration compiles with {flags!r}: no debug "
                             "information, so there are no symbols to archive and a crash in a "
                             "shipped build cannot be symbolicated")
    print(f"  ok   the shipping configuration keeps debug information: {flags!r}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--flags", help="a configuration's compile flags to check")
    parser.add_argument("--compiler-id", default="GNU")
    arguments = parser.parse_args()
    try:
        if arguments.flags is not None:
            check_configuration(arguments.flags, arguments.compiler_id)
        if sys.platform.startswith("linux"):
            with tempfile.TemporaryDirectory() as scratch:
                cases(Path(scratch))
        else:
            print("  NOT EVALUATED  the ELF cases: this host does not produce ELF")
    except AssertionError as failure:
        print(f"unit.build_symbols: FAILED: {failure}", file=sys.stderr)
        return 1
    print("unit.build_symbols: every case passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
