#!/usr/bin/env python3
"""Run `just env-doctor` against deliberately broken environments — task 2.2.4.

`developer-workflow-and-just` requires that a failure caused by a missing or wrong-version tool is
diagnosed by `doctor` rather than surfacing as a build error from a nested tool, and that the report
states, for each item, whether it is present, its version, whether that version is supported, and
**how to install or correct it**. A doctor is only worth having if it is right when the environment
is wrong, and the only way to know that is to break one.

Each case builds a sandbox directory of symlinks to the real tools, removes or replaces exactly one
of them, and runs the recipe with PATH pointing at the sandbox. Asserted: the exit status, the
status word and version in the report line, and the presence of a correction for this host.

Run through `just ci-check`. Exits 0 when every case passes, 1 naming the ones that did not.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import platform
import re
import shutil
import subprocess
import sys
import tempfile

# Everything `just env-doctor` may look for, plus what the recipe itself runs. `node` is here
# because the OpenSpec CLI is a Node program: without it, `openspec --version` fails and the case
# stops testing what it meant to.
SANDBOX_TOOLS = (
    "bash",
    "sed",
    "awk",
    "uname",
    "just",
    "cmake",
    "ninja",
    "git",
    "python3",
    "openspec",
    "node",
    "c++",
    "g++",
    "clang-format",
    "clang-tidy",
)

# The install hint each host is expected to give for a missing ninja. The recipe's hint table is
# indexed by host; this is the other side of that table, and a host whose column was left empty
# would fail here rather than silently report an absence with no correction.
NINJA_HINT = {
    "Linux": "apt install ninja-build",
    "Darwin": "brew install ninja",
    "Windows": "winget install",
}


class Case:
    """One broken environment, and what the report must say about it."""

    def __init__(self, name: str, *, remove=(), fake=None, status: int, expected=()) -> None:
        self.name = name
        self.remove = remove
        self.fake = fake or {}
        self.status = status
        self.expected = expected


def sandbox(directory: pathlib.Path, case: Case) -> pathlib.Path:
    """A PATH directory holding every tool except the ones this case breaks."""
    binaries = directory / "bin"
    binaries.mkdir(parents=True)
    for tool in SANDBOX_TOOLS:
        if tool in case.remove or tool in case.fake:
            continue
        real = shutil.which(tool)
        if real:
            (binaries / tool).symlink_to(real)
    for tool, output in case.fake.items():
        script = binaries / tool
        script.write_text(f'#!/usr/bin/env bash\necho "{output}"\n', encoding="utf-8")
        script.chmod(0o755)
    return binaries


def run_doctor(root: pathlib.Path, binaries: pathlib.Path) -> subprocess.CompletedProcess:
    environment = dict(os.environ)
    environment["PATH"] = str(binaries)
    # CXX would name a compiler outside the sandbox, and the no-compiler case would then find one.
    for name in ("CXX", "CC", "CY_PROFILE", "CY_BUILD_DIR"):
        environment.pop(name, None)
    return subprocess.run(
        ["just", "env-doctor"],
        cwd=root,
        env=environment,
        capture_output=True,
        text=True,
    )


def corrections_follow_failures(report: str) -> str | None:
    """Every missing or too-old line is followed by an install command — requirement 2.2.2."""
    lines = report.splitlines()
    for index, line in enumerate(lines):
        if not re.match(r"^\s{2}(missing|old)\s", line):
            continue
        window = " ".join(lines[index + 1 : index + 3])
        if "install it with:" not in window:
            return f"no correction after: {line.strip()!r}"
    return None


def check(case: Case, root: pathlib.Path) -> list[str]:
    with tempfile.TemporaryDirectory() as directory:
        binaries = sandbox(pathlib.Path(directory), case)
        result = run_doctor(root, binaries)

    report = result.stdout + result.stderr
    failures = []
    if result.returncode != case.status:
        failures.append(f"exit status {result.returncode}, expected {case.status}")
    for pattern in case.expected:
        if not re.search(pattern, report, re.MULTILINE):
            failures.append(f"report does not match {pattern!r}")
    missing_correction = corrections_follow_failures(report)
    if missing_correction:
        failures.append(missing_correction)
    if failures:
        failures.append("--- report ---\n" + report.rstrip())
    return failures


def cases() -> list[Case]:
    hint = NINJA_HINT.get(platform.system(), "install")
    return [
        Case(
            "a healthy environment passes",
            status=0,
            expected=(r"^\s+ok\s+cmake\s", r"env-doctor: the environment satisfies M0"),
        ),
        Case(
            "a missing tool is named, with the correction for this host",
            remove=("ninja",),
            status=1,
            expected=(
                r"^\s+missing\s+ninja\s+-\s+not on PATH",
                re.escape(hint),
                r"env-doctor: 1 problem\(s\) — ninja",
            ),
        ),
        Case(
            "a too-old tool reports what was found and what is required",
            fake={"cmake": "cmake version 3.20.1"},
            status=1,
            expected=(
                r"^\s+old\s+cmake\s+3\.20\.1\s",
                r"needs 3\.28 or later",
                r"env-doctor: 1 problem\(s\) — cmake",
            ),
        ),
        Case(
            "no C++ compiler is a diagnosis, not a configure error five minutes later",
            remove=("c++", "g++"),
            status=1,
            expected=(r"^\s+missing\s+compiler\s", r"problem\(s\).*compiler"),
        ),
        Case(
            "several problems are all reported, not just the first",
            remove=("ninja", "openspec"),
            status=1,
            expected=(r"^\s+missing\s+ninja\s", r"^\s+missing\s+openspec\s", r"2 problem\(s\)"),
        ),
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[2],
        help="repository root (default: this file's repository)",
    )
    arguments = parser.parse_args()
    root = arguments.root.resolve()

    failed = 0
    for case in cases():
        failures = check(case, root)
        if failures:
            failed += 1
            print(f"fail {case.name}", file=sys.stderr)
            for failure in failures:
                print(f"       {failure}", file=sys.stderr)
        else:
            print(f"ok   {case.name}")

    total = len(cases())
    if failed:
        print(f"env-doctor selftest: {failed} of {total} cases failed", file=sys.stderr)
        return 1
    print(f"env-doctor selftest: {total}/{total} passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
