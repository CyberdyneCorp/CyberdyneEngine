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

SECTIONS = ("[fault]", "[breadcrumbs]", "[backtrace]", "[end]")
FRAME = re.compile(r"^(?P<module>[^(]+)\((?P<symbol>[^)]*)\)\[(?P<address>0x[0-9a-f]+)\]")


def read_fields(report: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in report.splitlines():
        if line.startswith("[") or ": " not in line or line.startswith("  "):
            continue
        key, _, value = line.partition(": ")
        fields.setdefault(key.strip(), value.strip())
    return fields


def frames_of(report: str) -> list[tuple[str, str]]:
    section = report.split("[backtrace]", 1)[-1].split("[end]", 1)[0]
    frames = []
    for line in section.splitlines():
        match = FRAME.match(line.strip())
        if match:
            frames.append((match.group("module"), match.group("symbol") or match.group("address")))
    return frames


def print_report(path: Path, report: str) -> int:
    fields = read_fields(report)
    print(f"{path}")
    for key in ("engine_version", "build_configuration", "build_identity", "classifications",
                "process", "description", "signal", "code", "address", "last_frame", "detail"):
        if key in fields:
            print(f"  {key:<20} {fields[key]}")

    crumbs = report.split("[breadcrumbs]", 1)[-1].split("[backtrace]", 1)[0].strip().splitlines()
    if len(crumbs) > 1:
        print(f"  breadcrumbs          {crumbs[0].strip()}")
        for line in crumbs[1:]:
            print(f"    {line.strip()}")

    frames = frames_of(report)
    print(f"  backtrace            {len(frames)} frames")
    for module, symbol in frames[:16]:
        print(f"    {Path(module).name} {symbol}")

    missing = [section for section in SECTIONS if section not in report]
    if missing:
        print(f"  INCOMPLETE           missing {' '.join(missing)}", file=sys.stderr)
        return 1
    return 0


def symbolicate(report: str) -> None:
    """Offsets into source lines, if the binary this report names is present with its symbols."""
    frames = frames_of(report)
    tool = shutil.which("addr2line")
    if not tool:
        print("\naddr2line is not installed; the offsets above symbolicate on a machine that has "
              "it, against the symbols the build archived.")
        return
    by_module: dict[str, list[str]] = {}
    for module, symbol in frames:
        if symbol.startswith("+0x"):
            by_module.setdefault(module, []).append(symbol[1:])
    print("\nsymbolication")
    for module, offsets in by_module.items():
        if not Path(module).exists():
            print(f"  {module}: not on this machine — run addr2line where it is")
            continue
        completed = subprocess.run([tool, "-e", module, "-f", "-C", "-p", *offsets],
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
        symbolicate(report)
    return status


if __name__ == "__main__":
    sys.exit(main())
