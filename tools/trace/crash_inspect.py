#!/usr/bin/env python3
"""Read a CyberdyneEngine crash report, and say how to symbolicate it.

`diagnostics-profiling-and-crash` — "Crash artefacts": the artefact is self-contained and
symbol-independent, usable on a machine with no symbols, and symbolication happens later against the
symbols the build archived. This reads the artefact, checks it is complete, and prints the
`addr2line` invocation that turns its offsets into source locations on a machine that has them.

Run by `just diagnose-crash`. Task 3.5.8.
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

SECTIONS = ("[fault]", "[breadcrumbs]", "[modules]", "[backtrace]", "[end]")

# `  #3 libcy.so+0x000000000001b2c0 pc=0x00007f3c9a41b2c0`, and on a platform with no module table
# the same line without the module half. The engine stopped writing frames with backtrace_symbols_fd
# at M10 because that call emits the loader's absolute module path — `m9:crash-artefact-paths`; the
# offset here is what addr2line wants and the basename is all that is left of the path.
FRAME = re.compile(
    r"^#(?P<frame>\d+)\s+(?:(?P<module>[^\s+]+)\+(?P<offset>0x[0-9a-f]+)\s+)?"
    r"pc=(?P<address>0x[0-9a-f]+)$")

# `  libcy.so base=0x00007f3c9a400000`, one per loaded object, captured when the handler installed.
MODULE = re.compile(r"^(?P<module>\S+)\s+base=(?P<base>0x[0-9a-f]+)$")


def section_of(report: str, heading: str) -> str:
    """The lines under `heading`, up to the next `[section]` line. The report gained sections between
    the breadcrumbs and the backtrace, so a reader that splits on a named later heading is a reader
    that breaks the next time one is added."""
    body = report.split(heading, 1)[-1]
    lines: list[str] = []
    for line in body.splitlines():
        if line.startswith("["):
            break
        lines.append(line)
    return "\n".join(lines).strip()


def read_fields(report: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in report.splitlines():
        if line.startswith("[") or ": " not in line or line.startswith("  "):
            continue
        key, _, value = line.partition(": ")
        fields.setdefault(key.strip(), value.strip())
    return fields


def frames_of(report: str) -> list[tuple[str, str]]:
    """(module basename, offset-or-address) per frame, in call order."""
    section = section_of(report, "[backtrace]")
    frames = []
    for line in section.splitlines():
        match = FRAME.match(line.strip())
        if match:
            frames.append((match.group("module") or "<unknown>",
                           match.group("offset") or match.group("address")))
    return frames


def modules_of(report: str) -> dict[str, str]:
    """Basename -> load base, from the table the handler captured at installation."""
    section = section_of(report, "[modules]")
    modules: dict[str, str] = {}
    for line in section.splitlines():
        match = MODULE.match(line.strip())
        if match:
            modules.setdefault(match.group("module"), match.group("base"))
    return modules


def print_report(path: Path, report: str) -> int:
    fields = read_fields(report)
    print(f"{path}")
    for key in ("engine_version", "build_configuration", "build_identity", "classifications",
                "process", "description", "signal", "code", "address", "last_frame", "detail"):
        if key in fields:
            print(f"  {key:<20} {fields[key]}")

    # Up to the NEXT section rather than up to [backtrace]: [reproduction] and [modules] sit between
    # them, and splitting on the far one printed both of those as breadcrumbs.
    crumbs = section_of(report, "[breadcrumbs]").splitlines()
    if len(crumbs) > 1:
        print(f"  breadcrumbs          {crumbs[0].strip()}")
        for line in crumbs[1:]:
            print(f"    {line.strip()}")

    modules = modules_of(report)
    if modules:
        print(f"  modules              {len(modules)} loaded")

    frames = frames_of(report)
    print(f"  backtrace            {len(frames)} frames")
    for module, offset in frames[:16]:
        print(f"    {module} {offset}")

    missing = [section for section in SECTIONS if section not in report]
    if missing:
        print(f"  INCOMPLETE           missing {' '.join(missing)}", file=sys.stderr)
        return 1
    return 0


def locate(module: str, search: list[Path]) -> Path | None:
    """Find the binary a frame names.

    THE REPORT NO LONGER CARRIES A PATH, DELIBERATELY — it carries a basename, because the artefact
    is prepared to leave the machine and `diagnostics-profiling-and-crash` forbids an absolute build
    path in it. So the binary is looked for where the reader says it is: `--binaries`, then the
    working directory. That is the same trade the specification describes for symbols, which are
    archived by the build rather than shipped inside the artefact.
    """
    for directory in search:
        candidate = directory / module
        if candidate.is_file():
            return candidate
    return None


def symbolicate(report: str, search: list[Path]) -> None:
    """Offsets into source lines, if the binary a frame names is present with its symbols."""
    frames = frames_of(report)
    tool = shutil.which("addr2line")
    if not tool:
        print("\naddr2line is not installed; the offsets above symbolicate on a machine that has "
              "it, against the symbols the build archived.")
        return
    by_module: dict[str, list[str]] = {}
    for module, offset in frames:
        if module != "<unknown>":
            by_module.setdefault(module, []).append(offset)
    print("\nsymbolication")
    if not by_module:
        print("  this report's frames carry no module, so there is nothing to resolve them "
              "against; see [modules].")
        return
    for module, offsets in by_module.items():
        binary = locate(module, search)
        if binary is None:
            searched = ", ".join(str(directory) for directory in search)
            print(f"  {module}: not found in {searched} — pass --binaries <dir> where the build "
                  f"that produced this report is")
            continue
        completed = subprocess.run([tool, "-e", str(binary), "-f", "-C", "-p", *offsets],
                                   capture_output=True, text=True, check=False)
        for line in completed.stdout.strip().splitlines():
            print(f"  {line}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("report", type=Path, nargs="?",
                        help="the crash report; the newest in the crash directory when omitted")
    parser.add_argument("--directory", type=Path, help="where reports are, for the default choice")
    parser.add_argument("--symbolicate", action="store_true",
                        help="resolve offsets with addr2line where the binary is present")
    parser.add_argument("--binaries", type=Path, action="append", default=[],
                        help="a directory holding the build's binaries, for --symbolicate; "
                             "repeatable. The report names each module by basename only, because "
                             "it may not carry an absolute path from the build machine")
    args = parser.parse_args()

    path = args.report
    if path is None:
        directory = args.directory or Path.home() / ".local/state/cyberdyne/crashes"
        reports = sorted(directory.glob("crash-*.txt"), key=lambda item: item.stat().st_mtime)
        if not reports:
            print(f"no crash reports in {directory}", file=sys.stderr)
            return 2
        path = reports[-1]
    if not path.exists():
        print(f"{path}: no such file", file=sys.stderr)
        return 2

    report = path.read_text(encoding="utf-8", errors="replace")
    if not report.startswith("cyberdyne-crash-report"):
        print(f"{path}: not a CyberdyneEngine crash report", file=sys.stderr)
        return 2
    status = print_report(path, report)
    if args.symbolicate:
        symbolicate(report, [*args.binaries, Path.cwd()])
    return status


if __name__ == "__main__":
    sys.exit(main())
