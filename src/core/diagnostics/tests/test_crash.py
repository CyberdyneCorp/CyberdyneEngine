#!/usr/bin/env python3
"""Run a program that crashes, then read the artefact it left.

`diagnostics-profiling-and-crash` — "Crash artefacts": the scenarios are that a shipping build's
crash is diagnosable from the artefact alone, and that no editor, debugger or network is required to
write it. This test is the second scenario performed: a bare process, a real signal, and a file.

Task 3.5.8. Exit status is 0 when the artefact is complete.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile

# What "contains no absolute path from the build machine" looks like as a pattern. The same three
# shapes the `m9:crash-artefact-paths` ledger criterion greps for, held here as well so the property
# is defended by `ctest -R diag_crash` and not only by a milestone recipe nobody runs on a branch.
ABSOLUTE_PATH = re.compile(r"(/home/|/Users/|[A-Za-z]:\\)")


# Printed by the probe when the mode it was asked for cannot run in this build. Two reasons, and
# both are the build being honest rather than the handler being broken: Profile and Shipping compile
# CY_ASSERT out, so the `assert` mode has nothing to fire; UndefinedBehaviorSanitizer diagnoses the
# `segv` mode's null store before the hardware faults, so no signal reaches the handler.
SKIPPED = ("assertions-compiled-out", "mode-not-available")

SKIP_REASON = {
    "assertions-compiled-out": "assertions are compiled out in this configuration",
    "mode-not-available": "UndefinedBehaviorSanitizer diagnoses the fault before the signal",
}

REQUIRED = (
    ("cyberdyne-crash-report", "the artefact identifies itself"),
    ("engine_version: 0.0.0-probe", "the build that produced it"),
    ("classifications:", "what the artefact may contain, declared"),
    ("[fault]", "the fault section"),
    ("last_frame: 4242", "the last frame the process reached"),
    ("[breadcrumbs]", "the breadcrumb ring"),
    ("level.transition", "a breadcrumb that survived the crash"),
    ("[modules]", "the module table captured when the handler installed"),
    ("[backtrace]", "the backtrace section"),
    ("[end]", "the report is complete, not truncated"),
)


def run(probe: str, directory: str, mode: str) -> tuple[int, str]:
    completed = subprocess.run([probe, directory, mode], capture_output=True, text=True,
                               timeout=60, check=False)
    lines = completed.stdout.strip().splitlines()
    if not lines:
        print(f"the probe printed no report path; stderr: {completed.stderr}", file=sys.stderr)
        return 1, ""
    # The probe prints the report path at installation, before it knows which mode it will take, so
    # a mode that cannot run says so on a later line rather than by withholding the path.
    for sentinel in SKIPPED:
        if sentinel in lines:
            return completed.returncode, sentinel
    return completed.returncode, lines[0]


def check(report: str, mode: str) -> int:
    failures = 0
    for needle, why in REQUIRED:
        if needle not in report:
            print(f"missing {needle!r}: {why}", file=sys.stderr)
            failures += 1
    if mode == "segv" and "SIGSEGV" not in report:
        print("the signal is not named in the report", file=sys.stderr)
        failures += 1
    if mode == "assert":
        # A failed CY_ASSERT is a fault the engine detected itself, and it produces the same
        # artefact a signal does, naming the expression that was false.
        for needle in ("fatal assertion", "resident < 0"):
            if needle not in report:
                print(f"the assertion report is missing {needle!r}", file=sys.stderr)
                failures += 1
    # One frame per line, each carrying an address; a report with none is valid only on a platform
    # that provides no backtrace, and says so.
    section = report.split("[backtrace]", 1)[-1]
    frames = [line for line in section.splitlines() if "0x" in line]
    if len(frames) < 3 and "<no backtrace available" not in section:
        print(f"the backtrace has {len(frames)} frames", file=sys.stderr)
        failures += 1
    failures += check_no_paths(report, section)
    return failures


def check_no_paths(report: str, backtrace: str) -> int:
    """`diagnostics-profiling-and-crash`: a produced artefact carries no absolute build-machine path.

    THIS IS THE REGRESSION TEST FOR `m9:crash-artefact-paths`, and it is written against the failure
    that actually happened rather than against the requirement's wording alone. The frames used to
    be written by `backtrace_symbols_fd()`, which emits each module exactly as the loader resolved
    it — an absolute path under the account's home for any binary that was not invoked by a relative
    one. So two checks, and the second is the one with teeth: the artefact as a whole carries no
    `/home/…`, `/Users/…` or `C:\…`, AND the backtrace section carries no directory separator at
    all, which fails the moment a frame goes back to being written as a path regardless of where
    this machine happens to keep its home directory.
    """
    failures = 0
    leaked = [line for line in report.splitlines() if ABSOLUTE_PATH.search(line)]
    if leaked:
        print(f"the artefact carries {len(leaked)} absolute build-machine path(s); the first is "
              f"{leaked[0].strip()!r}", file=sys.stderr)
        failures += 1
    pathy = [line for line in backtrace.splitlines() if "/" in line or "\\" in line]
    if pathy:
        print(f"a backtrace frame carries a path separator: {pathy[0].strip()!r}. Frames name a "
              f"module by BASENAME and an offset — see crash_handler_posix.cpp.", file=sys.stderr)
        failures += 1
    return failures


def check_reader(inspect: str, report: str) -> int:
    """The artefact is read back by the tool that ships to read it.

    `tools/trace/crash_inspect.py` is the whole of `just diagnose-crash`, and until M10 changed the
    frame format to close `m9:crash-artefact-paths` it was reachable from no test at all — so a
    reader that stopped parsing the artefact would have been found by a person with a crash on their
    hands. It is run here over the report this test just produced, and its own completeness check
    (every named section present, parsed) is what decides.
    """
    completed = subprocess.run([sys.executable, inspect, report], capture_output=True, text=True,
                               timeout=60, check=False)
    failures = 0
    if completed.returncode != 0:
        print(f"crash_inspect.py rejected the artefact it is meant to read: "
              f"{completed.stderr.strip()}", file=sys.stderr)
        failures += 1
    # It must have PARSED the frames, not merely not crashed: "0 frames" is what a reader that no
    # longer understands the format prints, and it prints it with status 0.
    for needle in ("backtrace  ", "modules  "):
        if needle not in completed.stdout:
            print(f"crash_inspect.py printed no {needle.strip()} line", file=sys.stderr)
            failures += 1
    if "backtrace            0 frames" in completed.stdout:
        print("crash_inspect.py parsed no frames out of a report that has them; the reader and the "
              "writer have gone out of step", file=sys.stderr)
        failures += 1
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True, help="the crash probe executable")
    parser.add_argument("--inspect", help="tools/trace/crash_inspect.py, run over what the probe "
                                          "wrote so the reader and the writer stay in step")
    args = parser.parse_args()

    failures = 0
    with tempfile.TemporaryDirectory() as directory:
        for mode in ("segv", "abort", "report", "assert"):
            status, path = run(args.probe, directory, mode)
            if not path:
                failures += 1
                continue
            if path in SKIPPED:
                # Skipping is the honest result; every other mode still runs here, and an
                # ordinary development build runs all four.
                print(f"{mode}: skipped — {SKIP_REASON[path]}")
                continue
            if mode != "report" and status >= 0:
                # A process killed by a signal reports a negative status through subprocess.
                pass
            if not os.path.exists(path):
                print(f"{mode}: no report at {path}", file=sys.stderr)
                failures += 1
                continue
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                report = handle.read()
            problems = check(report, mode)
            if args.inspect:
                problems += check_reader(args.inspect, path)
            if problems:
                print(f"--- {mode} report ---\n{report}", file=sys.stderr)
            failures += problems
            print(f"{mode}: {os.path.basename(path)}, {len(report)} bytes, "
                  f"{'complete' if problems == 0 else 'INCOMPLETE'}")

    print("crash: PASS" if failures == 0 else f"crash: {failures} failures")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
