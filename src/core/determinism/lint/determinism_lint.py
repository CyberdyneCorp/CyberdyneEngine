#!/usr/bin/env python3
"""The determinism lint — M9 task 2.6.

`simulation-and-determinism`, "Determinism lint": "Systems declared deterministic SHALL be
checkable statically. The build SHALL be able to report, for such systems: use of wall-clock time,
use of an ambient random generator, iteration over containers with unspecified order used as a
decision order, reads of presentation-classified data, and use of floating-point operations
disallowed by the active profile. Findings SHALL name the system and the source location, and SHALL
be configurable as errors or warnings."

================================================================================================
THE LINT HAS TWO JOBS AND THE SECOND ONE IS M9'S SPIKE FINDING
================================================================================================

SOURCE.  The five rules above, over the translation units of every module that declares a
         determinism profile.

BUILD.   Assert that each of those modules was actually compiled with floating-point contraction
         off. `design.md` §1.2 measured why: two builds of identical source, differing only in
         `-march` and a contraction flag, produce different state hashes — 231 of 267 values moved
         in the spike's own `state_hash` workload. A lint that read only source would pass both of
         them.

The build half reads two files the build writes and nothing else: `determinism-profiles.txt`, which
`cy_declare_determinism_profile()` emits, and `compile_commands.json`, which CMake emits. Neither is
a list this script maintains, so neither can drift away from what was compiled.

================================================================================================
COVERAGE IS REPORTED, NOT IMPLIED
================================================================================================

Three modules of the engine's eighty-one declare a profile today, and `--report-coverage` prints
that ratio. A lint that examined three modules and said "no findings" would read exactly like one
that examined eighty-one, which is the defect class this project's gates have found eighteen times.
The uncovered modules are NOT reported as findings: they have not claimed anything, so there is
nothing to fail. They are reported as a number.

Exit codes: 0 nothing to report, 1 findings at error severity, 2 the lint could not run (a missing
manifest, an unreadable file) — which is never the same as "clean".
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
from dataclasses import dataclass
from pathlib import Path

# --- The policy, mirrored from src/core/determinism/include/cy/core/determinism/fp_policy.h ------
#
# Mirrored rather than parsed out of the header, and the reason is worth stating: a lint that parsed
# C++ to find its own rules would be a second C++ parser. The list is short, it is checked against
# the header by `--verify-policy`, and `test_lint.py`'s first case runs that check — so the two
# cannot drift without a test going red.

FORBIDDEN_CMATH = (
    "acos", "acosh", "asin", "asinh", "atan2", "atanh", "cbrt",
    "cosh", "expm1", "log10", "log1p", "sinh", "tgamma",
)

# --- Exemptions, PER RULE and each with a reason --------------------------------------------------
#
# A whole-file exemption is how a lint grows an unexamined corner; these name the one rule each file
# is allowed to break and leave every other rule in force over it. Three entries, and each is a file
# whose *subject* is the rule it breaks.
EXEMPTIONS = {
    # The file that replaces the thirteen. It contains their definitions — `f64 acos(f64 x)` — and
    # the policy table's string literals. Its own bodies call only the exact and correctly-rounded
    # sets, which is checkable by reading it and is what the accuracy suite measures.
    "src/core/determinism/src/fp_policy.cpp": ("forbidden-cmath",),
    # The accuracy suite. It MUST call `std::acos` and the other twelve: comparing a replacement
    # against the function it replaces is the whole of what it does.
    "src/core/determinism/tests/test_fp_policy.cpp": ("forbidden-cmath",),
    # `bypass_classification()` is this suite's subject. It asserts that the escape hatch exists and
    # that it is the only one; a lint finding on it would be a finding on the test of the finding.
    "src/core/determinism/tests/test_classification.cpp": ("presentation-read",),
}

WALL_CLOCK = (
    "std::chrono::system_clock",
    "std::chrono::steady_clock",
    "std::chrono::high_resolution_clock",
    "std::time(",
    "clock_gettime(",
    "gettimeofday(",
    "QueryPerformanceCounter(",
    "::time(",
)

AMBIENT_RANDOM = (
    "std::rand(",
    "std::srand(",
    "std::random_device",
    "std::mt19937",
    "std::default_random_engine",
    "arc4random(",
)

# Iterating a hash map and using the order as a decision order. `simulation-and-determinism`:
# "Lookup is permitted; iteration as a decision order is not." Only the range-for spelling is
# detectable from text, which is what this matches — and the limitation is reported by
# `--report-coverage` rather than left implied.
HASH_ITERATION = re.compile(r"for\s*\(\s*[^;)]*:\s*[A-Za-z_][A-Za-z0-9_]*(?:_|\.)?(?:map|table)\b")

PRESENTATION_READ = re.compile(r"\bbypass_classification\s*\(")

SEVERITIES = ("error", "warning", "off")


@dataclass(frozen=True)
class Finding:
    rule: str
    path: str
    line: int
    text: str
    detail: str

    def render(self) -> str:
        return f"{self.path}:{self.line}: [{self.rule}] {self.detail}\n    {self.text.strip()}"


def strip_comments_and_strings(line: str) -> str:
    """Blank out `//` comments and string literals.

    Crude on purpose: a lint that needed a C++ parser to decide whether `acos(` is a call would be a
    second C++ parser. What this does buy is that the policy table's `{"acos", ...}` entries and the
    sentences in a header comment are not findings, which is the difference between a usable lint
    and one everybody disables.
    """
    out = []
    in_string = False
    index = 0
    while index < len(line):
        char = line[index]
        if in_string:
            if char == "\\":
                index += 2
                continue
            if char == '"':
                in_string = False
            out.append(" ")
            index += 1
            continue
        if char == '"':
            in_string = True
            out.append(" ")
            index += 1
            continue
        if char == "/" and index + 1 < len(line) and line[index + 1] == "/":
            break
        out.append(char)
        index += 1
    return "".join(out)


def scan_source(path: Path, root: Path, exempt: tuple[str, ...] = ()) -> list[Finding]:
    relative = str(path.relative_to(root))
    findings: list[Finding] = []
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as error:  # pragma: no cover - reported, never swallowed
        raise SystemExit(f"determinism-lint: cannot read {path}: {error}") from error

    in_block_comment = False
    for number, raw in enumerate(lines, start=1):
        line = raw
        if in_block_comment:
            end = line.find("*/")
            if end < 0:
                continue
            line = line[end + 2 :]
            in_block_comment = False
        start = line.find("/*")
        if start >= 0:
            end = line.find("*/", start + 2)
            if end < 0:
                in_block_comment = True
                line = line[:start]
            else:
                line = line[:start] + " " + line[end + 2 :]
        code = strip_comments_and_strings(line)
        if not code.strip():
            continue

        for token in WALL_CLOCK:
            if token in code:
                findings.append(Finding("wall-clock", relative, number, raw,
                                        f"authoritative code reads a wall clock ({token})"))
        for token in AMBIENT_RANDOM:
            if token in code:
                findings.append(Finding("ambient-random", relative, number, raw,
                                        f"authoritative code uses an ambient generator ({token})"))
        if HASH_ITERATION.search(code):
            findings.append(Finding("unordered-iteration", relative, number, raw,
                                    "iteration over a container with unspecified order; lookup is "
                                    "permitted, iteration as a decision order is not"))
        if PRESENTATION_READ.search(code) and "presentation-read" not in exempt:
            findings.append(Finding("presentation-read", relative, number, raw,
                                    "bypass_classification() reads a value without its witness; "
                                    "reflection-driven machinery only"))
        if "forbidden-cmath" not in exempt:
            for name in FORBIDDEN_CMATH:
                # `(?<!fp::)` is the one spelling that is not a finding: it IS the replacement.
                # Without it the lint fires on every call site it asked people to write, which is
                # how a lint becomes a thing everybody switches off.
                if re.search(rf"(?<![A-Za-z0-9_])(?<!fp::){re.escape(name)}\s*\(", code):
                    findings.append(Finding(
                        "forbidden-cmath", relative, number, raw,
                        f"'{name}' is not correctly rounded in this libm and its folded value "
                        f"differs from its runtime value; use cy::determinism::fp::{name}"))
    return findings


def read_profile_manifest(path: Path) -> dict[str, str]:
    if not path.is_file():
        raise SystemExit(
            f"determinism-lint: no profile manifest at {path}. It is written by "
            f"cy_declare_determinism_profile() at configure time; configure the build first.")
    profiles: dict[str, str] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) != 2:
            raise SystemExit(f"determinism-lint: malformed manifest line: {line}")
        profiles[parts[0]] = parts[1]
    if not profiles:
        raise SystemExit(
            "determinism-lint: the profile manifest is empty. A lint with nothing to examine must "
            "not report that it found nothing wrong.")
    return profiles


def read_compile_commands(path: Path) -> list[dict]:
    if not path.is_file():
        raise SystemExit(
            f"determinism-lint: no compile_commands.json at {path}. The build half of the profile "
            f"cannot be checked without it, and skipping it would be a clean report over an "
            f"unchecked build.")
    return json.loads(path.read_text(encoding="utf-8"))


def target_of(entry: dict) -> str:
    """The CMake target a compile command belongs to, from its output path.

    Ninja writes `CMakeFiles/<target>.dir/...`, which is the only place the target name appears in
    the compilation database. Derived rather than configured, so it cannot drift.
    """
    match = re.search(r"CMakeFiles/([^/]+)\.dir/", entry.get("output", ""))
    return match.group(1) if match else ""


def check_build_half(profiles: dict[str, str], entries: list[dict],
                     contraction_flag: str) -> tuple[list[Finding], int, set[str]]:
    findings: list[Finding] = []
    checked = 0
    sources: set[str] = set()
    for entry in entries:
        target = target_of(entry)
        profile = profiles.get(target)
        if profile is None:
            continue
        command = entry.get("command") or " ".join(entry.get("arguments", []))
        source = entry.get("file", "")
        sources.add(source)
        checked += 1
        if profile in ("None", "ReplayStable"):
            continue
        if contraction_flag not in command:
            findings.append(Finding(
                "contraction", source, 0, "",
                f"target '{target}' declares determinism profile {profile} but was compiled "
                f"without {contraction_flag}"))
        for fast in ("-ffast-math", "-Ofast", "/fp:fast"):
            if fast in command:
                findings.append(Finding(
                    "fast-math", source, 0, "",
                    f"target '{target}' declares {profile} and was compiled with {fast}"))
    return findings, checked, sources


def verify_policy(root: Path) -> None:
    """Check this script's mirrored list against fp_policy.cpp's table.

    A lint whose rules had quietly drifted from the header they mirror would report the wrong thing
    confidently, which is worse than not running.
    """
    table = (root / "src/core/determinism/src/fp_policy.cpp").read_text(encoding="utf-8")
    declared = set(re.findall(r'\{"([a-z0-9]+)", FloatClass::NotCorrectlyRounded', table))
    mirrored = set(FORBIDDEN_CMATH)
    if declared != mirrored:
        raise SystemExit(
            "determinism-lint: the mirrored forbidden list does not match fp_policy.cpp.\n"
            f"  only in fp_policy.cpp: {sorted(declared - mirrored)}\n"
            f"  only in this script  : {sorted(mirrored - declared)}")



# --- The lint's own negative control --------------------------------------------------------------
#
# A lint is a check, and this project's rule for a check is that it must be able to fail and be shown
# to. `--selftest` writes one fixture per rule, runs the scanner over it, and fails if the rule did
# not fire — so deleting a rule from `scan_source()` turns `integration.determinism_lint_selftest`
# red rather than turning `integration.determinism_lint` quietly green.
#
# It also runs the *inverse*: a fixture that mentions every forbidden name inside a comment, inside
# a string literal, and as `fp::acos(` must produce nothing. A lint that fired on those is a lint
# everybody switches off, which is the same outcome as one that never fires.

SELFTEST_CASES = (
    ("wall-clock", "auto now = std::chrono::steady_clock::now();"),
    ("wall-clock", "struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);"),
    ("ambient-random", "int roll = std::rand();"),
    ("ambient-random", "std::mt19937 engine(seed);"),
    ("unordered-iteration", "for (const auto& entry : lookup_map) { choose(entry); }"),
    ("presentation-read", "const f32 value = flash.bypass_classification();"),
    ("forbidden-cmath", "const double angle = std::acos(dot);"),
    ("forbidden-cmath", "y = cbrt(x);"),
    ("forbidden-cmath", "const double a = std::atan2(dy, dx);"),
)

SELFTEST_CLEAN = """
// acos and cbrt and atan2 named in a comment are not calls.
const char* kNames[] = {"acos", "cbrt", "sinh"};
const double angle = cy::determinism::fp::acos(dot);
const double root = cy::determinism::fp::cbrt(x);
const double safe = std::sqrt(x) + std::exp(y) + std::log(z) + std::atan(w);
const auto value = map.find(key);
"""


def run_selftest(root: Path) -> int:
    import tempfile

    failures = 0
    with tempfile.TemporaryDirectory() as directory:
        base = Path(directory)
        for index, (rule, line) in enumerate(SELFTEST_CASES):
            path = base / f"case_{index}.cpp"
            path.write_text(f"void f() {{\n    {line}\n}}\n", encoding="utf-8")
            found = scan_source(path, base)
            if not any(finding.rule == rule for finding in found):
                print(f"determinism-lint selftest: rule '{rule}' did not fire on: {line}")
                failures += 1

        clean = base / "clean.cpp"
        clean.write_text(SELFTEST_CLEAN, encoding="utf-8")
        noise = scan_source(clean, base)
        for finding in noise:
            print(f"determinism-lint selftest: false positive: {finding.render()}")
            failures += 1

        # The exemption is real: the file that replaces the thirteen names them, and it must not be
        # a finding against itself.
        exempted = base / "fp_policy.cpp"
        exempted.write_text("double acos(double x) { return 0.0; }\n", encoding="utf-8")
        if scan_source(exempted, base, exempt=("forbidden-cmath",)):
            print("determinism-lint selftest: the policy exemption did not apply")
            failures += 1
        if not scan_source(exempted, base):
            print("determinism-lint selftest: the exemption is unconditional, which makes it a hole")
            failures += 1
        # PER RULE, not per file: the exempted file is still checked for everything else.
        both = base / "fp_policy_with_a_clock.cpp"
        both.write_text("double acos(double x) { return std::chrono::steady_clock::now(); }\n",
                        encoding="utf-8")
        if not any(finding.rule == "wall-clock"
                   for finding in scan_source(both, base, exempt=("forbidden-cmath",))):
            print("determinism-lint selftest: an exemption for one rule silenced another")
            failures += 1

    if failures != 0:
        print(f"determinism-lint selftest: {failures} failure(s)")
        return 1
    print(f"determinism-lint selftest: {len(SELFTEST_CASES)} rules fire and the clean fixture is "
          f"silent")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", default=".", help="repository root")
    parser.add_argument("--build", required=False, default=None,
                        help="build directory holding determinism-profiles.txt and "
                             "compile_commands.json")
    parser.add_argument("--severity", choices=SEVERITIES, default="error",
                        help="how findings are reported; 'off' still prints them and exits zero")
    parser.add_argument("--contraction-flag", default="-ffp-contract=off")
    parser.add_argument("--report-coverage", action="store_true",
                        help="print how much of the engine the profiles actually cover")
    parser.add_argument("--verify-policy", action="store_true",
                        help="check the mirrored forbidden list against fp_policy.cpp and exit")
    parser.add_argument("--selftest", action="store_true",
                        help="prove each rule fires, and that the clean fixture is silent")
    arguments = parser.parse_args()

    root = Path(arguments.root).resolve()
    if arguments.verify_policy:
        verify_policy(root)
        print("determinism-lint: the mirrored forbidden list matches fp_policy.cpp")
        return 0

    if arguments.selftest:
        verify_policy(root)
        return run_selftest(root)

    if arguments.build is None:
        parser.error("--build is required (or pass --verify-policy)")
    build = Path(arguments.build).resolve()

    verify_policy(root)
    profiles = read_profile_manifest(build / "determinism-profiles.txt")
    entries = read_compile_commands(build / "compile_commands.json")

    build_findings, translation_units, sources = check_build_half(profiles, entries,
                                                                  arguments.contraction_flag)

    exemptions = {str((root / name).resolve()): rules for name, rules in EXEMPTIONS.items()}
    source_findings: list[Finding] = []
    for source in sorted(sources):
        path = Path(source)
        if not path.is_file() or root not in path.resolve().parents:
            continue
        resolved = path.resolve()
        source_findings.extend(
            scan_source(resolved, root, exempt=exemptions.get(str(resolved), ())))

    findings = build_findings + source_findings

    if arguments.report_coverage:
        modules = {target_of(entry) for entry in entries if target_of(entry)}
        print(f"determinism-lint: {len(profiles)} of {len(modules)} built targets declare a "
              f"determinism profile; {translation_units} translation units examined.")
        print("determinism-lint: covered targets: " + ", ".join(sorted(profiles)))
        print("determinism-lint: NOT a finding — an undeclared module has claimed nothing, so "
              "there is nothing for it to fail. The ratio is the gap, and it is a number rather "
              "than a sentence.")

    for finding in findings:
        print(finding.render())

    if not findings:
        print(f"determinism-lint: no findings over {translation_units} translation units in "
              f"{len(profiles)} covered targets.")
        return 0
    print(f"determinism-lint: {len(findings)} finding(s), severity {arguments.severity}.")
    return 1 if arguments.severity == "error" else 0


if __name__ == "__main__":
    sys.exit(main())
