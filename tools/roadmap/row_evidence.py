#!/usr/bin/env python3
"""Evaluate the four capability rows whose Working tier no criterion has ever checked.

WHY THIS FILE EXISTS, and it is not "another linter".

`m9:record-matches-plan-history` names four cells over four closed milestones — M3
`testing-and-quality`, M4 `build-system-and-platforms`, M6 `developer-workflow-and-just`, M8.b
`thirdparty-dependencies`. Each column claims **Working** and `docs/roadmap/status.yaml` holds
**Seed** from M0. M10 task 6.5 closed the gap by RECORDING all four at the tier their column
claimed, and M10's closing gate put the record back, for a reason that is the whole point of this
module: **no criterion anywhere evaluates any of the four rows.** Over every ledger the only
`expect_tiers` entry naming one of them expected `seed`, and `criteria._check_tiers` treats an exit
tier as a FLOOR — so a `seed` expectation can never contradict a Working claim, and nothing in the
repository re-checks one. A tier recorded because a document argued well for it, with nothing that
re-checks it, is the same defect as a column that outlives its evidence, one level down.

So `implement-m11a-foundations` tasks 5.1 and 5.2: write criteria that EVALUATE the rows, checking
**the evidence `docs/roadmap/capability-matrix.md` already writes out per row** rather than
re-deriving a new argument. That table is quoted at the head of each check below, because a check
whose subject drifts away from the claim it was written for is the defect one more level down.

    python3 tools/roadmap/row_evidence.py <row> [--list]

One row per invocation, one criterion per row in `tools/roadmap/milestones/m11a.toml`. Every check
prints what it looked at and what it found, and the exit status is the verdict: 0 when every leg of
that row's evidence holds, 1 naming the legs that do not.

WHAT THIS IS NOT. It is not a tier recording and it does not touch the record: `delivery-roadmap`
makes recording a tier a closing gate's act, and M11.a's design §7 says so in as many words. It
answers one question per row — "is the evidence the matrix argues from still true today" — and a
`no` here is a finding, not a failure of this tool.

Standard library only, like the rest of `tools/roadmap/`: these run on every pull request on three
platforms, and a gate may not depend on a package that happens to be installed.

Governed by: delivery-roadmap (A recorded tier is evaluated by a criterion), testing-and-quality,
build-system-and-platforms, developer-workflow-and-just, thirdparty-dependencies.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

#: The probe projects beside this file. Each one is a real CMake project that includes the file whose
#: behaviour a leg is about and reports what CMake DID — see the header comment in each.
PROBES = REPO_ROOT / "tools" / "roadmap" / "probes"


class Report:
    """What one row's evidence came to. Every leg prints; the shortfalls decide the exit status."""

    def __init__(self, row: str, argued_at: str) -> None:
        self.row = row
        self.argued_at = argued_at
        self.short: list[str] = []

    def leg(self, holds: bool, claim: str, detail: str = "") -> bool:
        """One leg of the matrix's argument, and whether the tree still supports it."""
        print(f"  {'ok  ' if holds else 'SHORT'}  {claim}")
        if detail:
            for line in detail.splitlines():
                print(f"          {line}")
        if not holds:
            self.short.append(claim)
        return holds

    def finish(self) -> int:
        print()
        if self.short:
            print(f"{self.row}: {len(self.short)} leg(s) of the evidence argued at {self.argued_at} "
                  f"DO NOT HOLD on this tree:")
            for claim in self.short:
                print(f"    {claim}")
            return 1
        print(f"{self.row}: every leg of the evidence argued at {self.argued_at} holds.")
        return 0


def read(path: str) -> str:
    return (REPO_ROOT / path).read_text(encoding="utf-8")


def cmake_files() -> list[Path]:
    """Every CMake file THIS REPOSITORY COMMITS — asked of git rather than of the filesystem.

    Walking the tree was the first implementation and it was wrong in a way worth recording: it
    reached `bindings/swift/.build/checkouts/swift-syntax/`, twenty-three CMakeLists.txt files
    SwiftPM had fetched. A check whose file set depends on which dependencies happen to be populated
    on this machine is not reproducible, and the pin check below would have reported a third party's
    commit as this engine's defect. `git ls-files` answers with what is committed and nothing else.
    """
    listed = subprocess.run(["git", "ls-files", "-z", "--", "*.cmake", "CMakeLists.txt",
                             "*/CMakeLists.txt"], cwd=REPO_ROOT, capture_output=True, text=True,
                            check=False)
    if listed.returncode == 0:
        return sorted(REPO_ROOT / name for name in listed.stdout.split("\0") if name)
    return _walked_cmake_files()


#: Directories a POPULATED tree grows and a tracked tree never has. `build` is deliberately NOT here:
#: `tools/build/CMakeLists.txt` is committed, and pruning by that name would drop it.
_NEVER_TRACKED = frozenset({".git", ".build", "node_modules", "target", "_deps", "checkouts"})


def _walked_cmake_files() -> list[Path]:
    """The same set, for a tree that is not a git repository — which is where a PROOF runs.

    `tools/roadmap/falsify.py` judges a criterion against a sandbox: a tar of `git ls-files`
    unpacked outside any repository. git answers "not a repository" there, so the call above used to
    raise and the three criteria over this file crashed rather than ran — reported as
    `not provable here` about checks that are perfectly able to fail. The sandbox holds the tracked
    files and nothing else, so walking it gives exactly the answer git would have given.

    The reproducibility the comment above is about is kept in the direction that matters. A
    configured build tree is pruned by its `CMakeCache.txt` rather than by its name — `build` is a
    directory this repository COMMITS (`tools/build/CMakeLists.txt`) — and the fetched-dependency
    directories by name. What a leftover file that slipped past both would do is ADD a file to the
    pin check, which can only produce another finding: this path cannot manufacture a green, and a
    walk cannot miss a file git would have listed.
    """
    found: list[Path] = []
    for directory, subdirectories, names in os.walk(REPO_ROOT):
        here = Path(directory)
        subdirectories[:] = [name for name in subdirectories if name not in _NEVER_TRACKED
                             and not (here / name / "CMakeCache.txt").is_file()]
        found.extend(here / name for name in names
                     if name == "CMakeLists.txt" or name.endswith(".cmake"))
    return sorted(found)


# --- Asking the build system, rather than reading it ----------------------------------------------
#
# THE EIGHTH INSTANCE OF THIS PROJECT'S ONE DEFECT WAS FOUND HERE, and it is why these two probes
# exist. The legs below used to be regular expressions over `tests/CMakeLists.txt` and
# `cmake/profiles.cmake` — `fragment in taxonomy`, `re.search(r"set\(CY_PROFILES ...")`. M11's second
# repair round broke both files' BEHAVIOUR while leaving their WORDS in place:
#
#   * `cy_add_test()` with its `PRIVATE_DEFINITIONS CY_TEST_BUDGET_NS=...` line and its whole
#     `set_tests_properties(... LABELS ... TIMEOUT ...)` call commented out — so no suite would carry
#     a budget, a label or a timeout — and `testing-and-quality` stayed GREEN on all three legs,
#     because the commented-out lines still contained the strings the legs looked for.
#   * every `set(CY_PROFILE...)` line in `cmake/profiles.cmake` commented out — so the build has no
#     profile table at all — and `build-system-and-platforms` stayed GREEN on every profile leg.
#
# A check a comment satisfies measures spelling. So the subject is handed to CMake instead: a probe
# project includes the file, calls the function, and reports what CMake produced. The mutation above
# turns both criteria red now, for the reason the mutation is about, and so does any other way of
# breaking those functions — including ways nobody thought to write a regular expression for.


class Probe:
    """One probe project, configured. `records` is what CMake wrote; `failure` is why it could not.

    A probe that will not configure is a FINDING, not an error: the subject of every leg that reads
    it is the file the probe includes, so `cmake/profiles.cmake` raising FATAL_ERROR because it no
    longer defines a profile is exactly the answer the leg wanted.
    """

    def __init__(self, records: list[list[str]], failure: str) -> None:
        self.records = records
        self.failure = failure

    def of(self, kind: str) -> list[list[str]]:
        """Every record of one kind, with the kind itself dropped."""
        return [record[1:] for record in self.records if record and record[0] == kind]

    def first(self, kind: str) -> list[str]:
        found = self.of(kind)
        return found[0] if found else []


def _configure(source: Path, build: Path, defines: dict[str, str]) -> str:
    """Configure a probe project. "" when CMake succeeded, or what it said when it refused."""
    command = ["cmake", "-S", str(source), "-B", str(build), f"-DCY_REPO={REPO_ROOT}"]
    command += [f"-D{name}={value}" for name, value in defines.items()]
    try:
        done = subprocess.run(command, cwd=REPO_ROOT, capture_output=True, text=True, check=False)
    except OSError as error:  # noqa: BLE001 — no cmake on PATH is a finding like any other
        return f"cmake could not be run: {error}"
    return "" if done.returncode == 0 else (done.stdout + done.stderr).strip()[-900:]


def _records(report: Path) -> list[list[str]]:
    text = report.read_text(encoding="utf-8") if report.is_file() else ""
    return [line.split("|") for line in text.splitlines() if line.strip()]


#: CMake 3.x writes the test name as `[=[<test>]=]`; CMake 4.4 writes it as `"<test>"`.
#: Both are generator output in CTestTestfile.cmake, not syntax this repository controls.
_CTEST_PROPERTIES_RE = re.compile(
    r'^set_tests_properties\((?:\[=\[([^\]]+)\]=\]|"([^"]+)") PROPERTIES\s+(.*)$',
    re.MULTILINE,
)


def ctest_properties(build: Path) -> dict[str, dict[str, str]]:
    """Every test the generated CTestTestfile.cmake registers, and the properties it gives it."""
    testfile = build / "CTestTestfile.cmake"
    registered: dict[str, dict[str, str]] = {}
    if not testfile.is_file():
        return registered
    for bracket_name, quoted_name, body in _CTEST_PROPERTIES_RE.findall(
        testfile.read_text(encoding="utf-8")
    ):
        name = bracket_name or quoted_name
        given = dict(re.findall(r'(\w+) "([^"]*)"', body))
        registered[name] = {key: given[key] for key in ("LABELS", "TIMEOUT") if key in given}
    return registered


def taxonomy_probe() -> tuple[Probe, dict[str, dict[str, str]]]:
    """Configure tools/roadmap/probes/taxonomy: what cy_add_test() does, asked of CMake.

    Two answers come back, from two places neither of which is the text of tests/CMakeLists.txt: the
    arguments cy_add_test() passed to cy_add_module(), recorded by the probe's stub, and the CTest
    properties CMake's own generator wrote for the suites it declared.
    """
    with tempfile.TemporaryDirectory(prefix="cy-taxonomy-probe-") as work:
        workspace, build = Path(work), Path(work) / "build"
        report, source = workspace / "report.txt", workspace / "probe.cpp"
        # Written here rather than committed: a .cpp under tools/ is a file the formatting gate, the
        # layer checker and the lint gate would each then scan, over a fixture that exists to be
        # named on a cy_add_test() call and never compiled.
        source.write_text("#include <cy/test/test.h>\n", encoding="utf-8")
        failure = _configure(PROBES / "taxonomy", build,
                             {"CY_PROBE_REPORT": str(report), "CY_PROBE_SOURCE": str(source)})
        return Probe(_records(report), failure), ctest_properties(build)


def profiles_probe(profile: str) -> Probe:
    """Configure tools/roadmap/probes/profiles for one profile: what the build derives from it."""
    with tempfile.TemporaryDirectory(prefix="cy-profiles-probe-") as work:
        report = Path(work) / "report.txt"
        failure = _configure(PROBES / "profiles", Path(work) / "build",
                             {"CY_PROFILE": profile, "CY_PROBE_REPORT": str(report)})
        return Probe(_records(report), failure)


def without_cmake_comments(text: str) -> str:
    """The same file with every whole-line comment removed.

    The scans below look for a DECLARATION. A commented-out declaration is not one, and leaving them
    in is how a leg comes to report on a file that declares nothing — which is the defect this whole
    section exists to end, in the one place a probe cannot reach.
    """
    return "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))


def recipe_surface() -> tuple[list[str], str]:
    """Every public recipe `just` will run, and why it could not be asked when it could not."""
    summary = subprocess.run(["just", "--summary"], cwd=REPO_ROOT, capture_output=True, text=True,
                             check=False)
    if summary.returncode != 0:
        return [], summary.stderr.strip()[:400] or "just --summary failed"
    return summary.stdout.split(), ""


#: `run: just <recipe> ...`, with just's own flags skipped so that `just --yes maintenance-clean`
#: names the recipe rather than the flag.
_JUST_CALL_RE = re.compile(r"\bjust\s+(?:--[\w-]+(?:[ =][^\s]+)?\s+)*([a-z][a-z0-9-]*)")


def ci_jobs() -> dict[str, str]:
    """Each job under `jobs:` in ci.yml, and the text of its block.

    Split on the two-space job header, starting AFTER `jobs:` so that `on:`'s own `push:` and
    `schedule:` keys are not mistaken for jobs.
    """
    body = read(".github/workflows/ci.yml").split("\njobs:\n", 1)[-1]
    blocks: dict[str, list[str]] = {}
    name = None
    for line in body.splitlines():
        header = re.match(r"^  ([a-z0-9-]+):\s*$", line)
        if header:
            name = header.group(1)
            blocks[name] = []
        elif name is not None:
            blocks[name].append(line)
    return {job: "\n".join(lines) for job, lines in blocks.items()}


def recipes_a_job_runs(block: str) -> list[str]:
    """The `just` recipes a job's steps invoke. Comment lines are dropped before looking.

    ci.yml's jobs carry long explanatory comments that name recipes — the `sanitize` job's own notes
    name four — so a scan that counted those would find a recipe invocation in a job with no steps.
    """
    commands = "\n".join(line for line in block.splitlines() if not line.lstrip().startswith("#"))
    return sorted({match.group(1) for match in _JUST_CALL_RE.finditer(commands)})


# --- testing-and-quality — argued Working at M3 ---------------------------------------------------
#
# capability-matrix.md: "At M3's closing commit: `cy_add_test` carrying the taxonomy's per-case
# budgets and CTest labels, the unit, integration, smoke and render suites, golden images,
# `benchmarks/` with a committed baseline and per-benchmark tolerances, and `sanitize`,
# `sanitize-nightly`, `quality`, `specs`, `generated`, `identity` and `profiles` jobs in CI."

#: The continuous-integration jobs the row's evidence names by name.
TESTING_CI_JOBS = ("sanitize", "sanitize-nightly", "quality", "specs", "generated", "identity",
                   "profiles")


def _kinds_detail(kinds: dict[str, tuple[str, str]], incomplete: list[str]) -> str:
    """What CMake says the taxonomy is, for the reader of a run."""
    if not kinds:
        return "the probe found no CY_TEST_BUDGET_<kind> at all"
    stated = "  ".join(f"{kind}={budget}ns/{timeout}s"
                       for kind, (budget, timeout) in sorted(kinds.items()))
    return stated + (f"\nwithout both: {', '.join(incomplete)}" if incomplete else "")


def _budget_leg(report: Report, probe: Probe, kinds: dict[str, tuple[str, str]]) -> None:
    """`PRIVATE_DEFINITIONS` as cy_add_test() actually passed them, captured by the probe's stub.

    A budget that is spelled in tests/CMakeLists.txt and not passed on does not appear here.
    """
    handed = dict(probe.of("module"))
    wrong = []
    for kind, (budget, _timeout) in sorted(kinds.items()):
        given = handed.get(f"cy_test_{kind}_probe_{kind}", "")
        if f"CY_TEST_BUDGET_NS={budget}ULL" not in given:
            wrong.append(f"{kind}: cy_add_module got {given or 'no definitions'!r}, "
                         f"not CY_TEST_BUDGET_NS={budget}ULL")
    report.leg(bool(kinds) and not wrong,
               "cy_add_test compiles the kind's per-case budget into the binary it declares",
               "\n".join(wrong) or "\n".join(f"{name}: {definitions}"
                                             for name, definitions in sorted(handed.items())))


def _ctest_legs(report: Report, kinds: dict[str, tuple[str, str]],
                properties: dict[str, dict[str, str]]) -> None:
    """What CTest will see, read from the file CMake's own generator wrote."""
    unlabelled, mistimed = [], []
    for kind, (_budget, timeout) in sorted(kinds.items()):
        given = properties.get(f"{kind}.probe_{kind}")
        if given is None:
            unlabelled.append(f"{kind}: cy_add_test registered no test for it at all")
            mistimed.append(f"{kind}: no test registered")
        else:
            if given.get("LABELS") != kind:
                unlabelled.append(f"{kind}: CTest sees LABELS {given.get('LABELS', 'none')!r}")
            if given.get("TIMEOUT") != timeout:
                mistimed.append(f"{kind}: CTest sees TIMEOUT {given.get('TIMEOUT', 'none')!r}, "
                                f"the taxonomy says {timeout}")
    registered = "\n".join(f"{name}: LABELS {given.get('LABELS', 'none')!r} "
                           f"TIMEOUT {given.get('TIMEOUT', 'none')!r}"
                           for name, given in sorted(properties.items()))
    report.leg(bool(properties) and not unlabelled,
               "cy_add_test gives every suite its kind as a CTest label",
               "\n".join(unlabelled) or registered)
    report.leg(bool(properties) and not mistimed,
               "cy_add_test gives every suite the taxonomy's CTest timeout",
               "\n".join(mistimed) or "every registered suite carries the taxonomy's timeout")


def _taxonomy_legs(report: Report, probe: Probe,
                   properties: dict[str, dict[str, str]]) -> dict[str, tuple[str, str]]:
    """The four legs about what cy_add_test() PRODUCES, read out of CMake's own answers."""
    kinds = {kind: (budget, timeout) for kind, budget, timeout in probe.of("taxonomy")}
    incomplete = sorted(kind for kind, (budget, timeout) in kinds.items()
                        if not budget or not timeout)
    report.leg(bool(kinds) and not incomplete,
               "the taxonomy gives every kind both a per-case budget and a CTest timeout",
               _kinds_detail(kinds, incomplete))
    _budget_leg(report, probe, kinds)
    _ctest_legs(report, kinds, properties)
    return kinds


#: Both registration macros of benchmarks/harness: `CY_BENCHMARK` and `CY_BENCHMARK_STARTING_AT`,
#: the form a millisecond-scale body uses. Matching only the first read benchmarks/save/'s two
#: bodies as absent and their baseline entries as thresholds nobody runs.
_BENCHMARK_REGISTRATION = re.compile(r'\bCY_BENCHMARK(?:_STARTING_AT)?\(\s*"([^"]+)"')


def registered_benchmarks(source: str) -> set[str]:
    """The benchmark names a C++ source registers, through either registration macro."""
    return set(_BENCHMARK_REGISTRATION.findall(source))


def _benchmark_leg(report: Report) -> None:
    """"a committed baseline and PER-BENCHMARK tolerances" — over what the tree registers.

    Not that the file exists but that it covers what the tree actually registers, in both
    directions. A benchmark with no threshold cannot regress and a threshold with no benchmark is a
    threshold nobody runs.
    """
    registered = set()
    for path in (REPO_ROOT / "benchmarks").rglob("*.cpp"):
        registered.update(registered_benchmarks(path.read_text(encoding="utf-8")))
    baseline = json.loads(read("benchmarks/baseline.json"))["benchmarks"]
    missing = sorted(registered - set(baseline))
    orphans = sorted(set(baseline) - registered)
    untoleranced = sorted(name for name, entry in baseline.items()
                          if "tolerance" not in entry or "ratio" not in entry)
    report.leg(bool(registered) and not missing and not orphans and not untoleranced,
               "every registered benchmark has a committed ratio and tolerance, and no threshold "
               "names a benchmark that does not exist",
               f"{len(registered)} registered, {len(baseline)} in benchmarks/baseline.json\n"
               f"no threshold for: {', '.join(missing) or 'none'}\n"
               f"threshold with no benchmark: {', '.join(orphans) or 'none'}\n"
               f"threshold missing ratio or tolerance: {', '.join(untoleranced) or 'none'}")


def _ci_jobs_leg(report: Report) -> None:
    """The seven quality jobs, and WHAT THEY RUN.

    THIS LEG WAS A WORD-GREP AND THE M11 GATE SAID SO IN THOSE WORDS: `^  <job>:$` in ci.yml is
    "a word-grep a dummy job satisfies", demonstrated in the repair round by replacing all seven
    jobs with `run: echo dummy` and watching the criterion stay green. The row's evidence is that
    these jobs RUN the quality work, so that is what is asked: every one of them invokes at least
    one
    `just` recipe, and every recipe any of them invokes is one the recipe surface actually carries —
    a step calling a recipe that no longer exists is a job that fails on the runner and a gate that
    gates nothing until someone reads the log.
    """
    jobs = ci_jobs()
    recipes, unavailable = recipe_surface()
    absent = [job for job in TESTING_CI_JOBS if job not in jobs]
    inert = [job for job in TESTING_CI_JOBS if job in jobs and not recipes_a_job_runs(jobs[job])]
    unknown = sorted({f"{job} -> just {recipe}" for job in TESTING_CI_JOBS if job in jobs
                      for recipe in recipes_a_job_runs(jobs[job]) if recipe not in recipes})
    detail = "\n".join(f"{job:<18} "
                       f"{', '.join(recipes_a_job_runs(jobs.get(job, ''))) or 'runs no recipe'}"
                       for job in TESTING_CI_JOBS)
    report.leg(not absent and not inert and not unknown and not unavailable,
               "the quality jobs the row is argued on are declared in ci.yml AND each runs "
               "a recipe the workflow carries",
               detail
               + (f"\nnot declared: {', '.join(absent)}" if absent else "")
               + (f"\ndeclared but running no recipe: {', '.join(inert)}" if inert else "")
               + (f"\nrecipe not in the workflow: {', '.join(unknown)}" if unknown else "")
               + (f"\nthe recipe surface could not be read: {unavailable}" if unavailable else ""))


def check_testing_and_quality() -> int:
    report = Report("testing-and-quality", "M3")

    probe, properties = taxonomy_probe()
    report.leg(not probe.failure,
               "the taxonomy configures, so cy_add_test can be asked what it does rather than read",
               probe.failure or f"tools/roadmap/probes/taxonomy configured; "
                                f"{len(probe.of('taxonomy'))} kind(s), "
                                f"{len(properties)} suite(s) registered with CTest")
    kinds = _taxonomy_legs(report, probe, properties)

    # A kind in the table with no suite behind it is a budget nobody is under. Matched over every
    # committed CMakeLists.txt because suites are declared beside the module they test, and with
    # comment lines removed, because a commented-out declaration declares nothing.
    declared: dict[str, int] = {kind: 0 for kind in sorted(kinds)}
    for path in cmake_files():
        text = without_cmake_comments(path.read_text(encoding="utf-8"))
        for kind in re.findall(r"cy_add_test\([^)]*?KIND\s+(\w+)", text, re.DOTALL):
            declared[kind] = declared.get(kind, 0) + 1
    empty = [kind for kind in sorted(kinds) if not declared.get(kind)]
    report.leg(bool(kinds) and not empty,
               "every kind in the taxonomy has at least one suite declared",
               "  ".join(f"{kind}={declared.get(kind, 0)}" for kind in sorted(kinds))
               + (f"\nno suite for: {', '.join(empty)}" if empty else ""))

    references = sorted((REPO_ROOT / "tests/render/references").glob("*.png"))
    report.leg(bool(references), "golden images are committed, not described",
               f"{len(references)} reference image(s) in tests/render/references/")

    _benchmark_leg(report)
    _ci_jobs_leg(report)
    return report.finish()


# --- build-system-and-platforms — argued Working at M4 --------------------------------------------
#
# capability-matrix.md: "Four profiles that mean the same thing in every toolchain, feature options,
# dependency management driven from the manifest, code generation, the compiler matrix — and at M4
# the Swift toolchain integration the requirement names, which is the rung at which the build system
# had to serve a second toolchain."

#: The four profiles, in the order `developer-workflow-and-just`'s own table gives them.
PROFILES = ("debug", "dev", "profile", "release")


#: The three columns the profile table carries beside the profile's own name, as
#: (the key in the probe's `selected|` record, the justfile column, a label). The probe reports each
#: one as CMake COMPUTED it: the configuration comes back from `cy_declare_build_configurations()`,
#: which is the macro the top-level CMakeLists.txt calls, and the other two from the variables that
#: macro's file defines.
PROFILE_COLUMNS = ((1, 1, "CMake configuration"),
                   (2, 4, "Cargo profile"),
                   (3, 5, "slangc flags"))


def justfile_profile_table() -> dict[str, list[str]]:
    """`profile_table` in the justfile, one row per profile, fields left to right."""
    table = re.search(r"profile_table := '(.*?)'", read("justfile"), re.DOTALL)
    rows: dict[str, list[str]] = {}
    for line in (table.group(1).splitlines() if table else []):
        fields = [field.strip() for field in line.split("|")]
        if len(fields) >= 5:
            rows[fields[0]] = fields
    return rows


def profile_columns_disagreeing(rows: dict[str, list[str]],
                                derived: dict[str, list[str]]) -> list[str]:
    """Where the justfile's table and what CMake DERIVES say different things, or say nothing.

    This is the comparison nothing in the tree makes end to end. `cmake/profiles.cmake` cross-checks
    itself against the justfile at CONFIGURE time and only when a caller supplies both, and neither
    file has ever been compared with the Cargo profile the editor is actually built with.

    `derived` is the probe's answer per profile, not a regular expression over the file: the leg
    this replaced matched commented-out `set()` lines and stayed green over a build system with no
    profile table at all.
    """
    found = []
    for profile in PROFILES:
        fields, stated = rows.get(profile, []), derived.get(profile, [])
        for index, column, label in PROFILE_COLUMNS:
            answered = stated[index] if len(stated) > index else None
            expected = fields[column] if len(fields) > column else None
            if not answered or expected is None:
                found.append(f"{profile}/{label}: not stated in both files")
            elif " ".join(answered.split()) != " ".join(expected.split()):
                found.append(f"{profile}/{label}: CMake derives {answered!r} "
                             f"against the justfile's {expected!r}")
    return found


def pins_outside_the_manifest() -> list[str]:
    """Every commit or git URL written into a CMake file, which `deps/manifest.toml` forbids.

    Its header states the rule: "a version that appears in a CMake file rather than in this table is
    a defect". A pin there is invisible to the audit, to THIRD_PARTY.md and to the feature gating.
    """
    found = []
    for path in cmake_files():
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
            if re.search(r"\b[0-9a-f]{40}\b", line) or re.search(r"https://\S+\.git\b", line):
                found.append(f"{path.relative_to(REPO_ROOT)}:{number}: {line.strip()[:90]}")
    return found


def cargo_profiles(rows: dict[str, list[str]], cargo: str) -> tuple[list[str], list[str]]:
    """What `editor/Cargo.toml` declares, and which of the table's selections it does not.

    The leg nothing checked: the Cargo column has to name a profile Cargo actually has, or
    `just build-editor --profile release` selects one that does not exist.
    """
    declared = sorted(set(re.findall(r"^\[profile\.(\w+)\]$", cargo, re.MULTILINE)))
    selected = [rows[profile][4] for profile in PROFILES if len(rows.get(profile, [])) > 4]
    return declared, [name for name in selected if name not in declared]


def profiles_as_cmake_derives_them() -> tuple[dict[str, Probe], Probe, list[str]]:
    """Configure the profiles probe once per profile. What comes back is CMake's own answer.

    One configure per profile rather than one for all four, because the question is what
    `cy_declare_build_configurations()` DOES with `CY_PROFILE=<p>` — which is how `just` invokes the
    real build — and that macro answers for the profile it was given. A profile the table no longer
    carries is a FATAL_ERROR raised by the shipped macro, and arrives here as a refusal naming it.
    """
    probed: dict[str, Probe] = {}
    refused: list[str] = []
    last = Probe([], "no profile was probed")
    for profile in PROFILES:
        last = profiles_probe(profile)
        if last.failure:
            refused.append(f"{profile}: {_first_line(last.failure)}")
            continue
        probed[profile] = last
    return probed, last, refused


def _first_line(text: str) -> str:
    """The most useful line of a CMake refusal: what it complained about, not that it stopped.

    CMake's last word is always "Configuring incomplete, errors occurred!", which tells a reader
    nothing. The line after `CMake Error` is the message the shipped code raised, and that is the
    finding.
    """
    lines = [line.strip() for line in text.splitlines() if line.strip()]
    for index, line in enumerate(lines):
        if line.startswith("CMake Error"):
            return " ".join(lines[index:index + 2])[:240]
    return lines[0][:200] if lines else "no output"


def _profile_legs(report: Report, rows: dict[str, list[str]]) -> Probe:
    """The four legs about the profile table, answered by configuring rather than by reading."""
    probed, probe, refused = profiles_as_cmake_derives_them()
    derived = {profile: answer.first("selected") for profile, answer in probed.items()}
    report.leg(not refused,
               "every profile configures: cmake/profiles.cmake derives a configuration for each",
               "\n".join(refused) or "\n".join(f"CY_PROFILE={profile} -> {' | '.join(stated[1:])}"
                                               for profile, stated in sorted(derived.items())))

    listed = probe.first("profiles")
    report.leg(bool(listed) and tuple(listed[0].split()) == PROFILES,
               "CY_PROFILES, as CMake computes it, is the same four in the same order",
               f"CY_PROFILES = {listed[0] if listed else 'the probe reported none'}")

    disagreeing = profile_columns_disagreeing(rows, derived)
    report.leg(not disagreeing and not refused,
               "every profile means the same thing in CMake, Cargo and the shader toolchain",
               "\n".join(disagreeing)
               or "the CMake, Cargo and Slang columns agree between the justfile and what "
                  "cmake/profiles.cmake derives")

    # The map back. `cy_profile_for_configuration()` is the function anything holding only a
    # configuration uses, and a table half of whose rows were lost round-trips for the other half.
    broken = [f"{profile}: {' -> '.join(mapped) or 'the probe reported no round trip'}"
              for profile, answer in sorted(probed.items())
              for mapped in [answer.first("roundtrip")]
              if len(mapped) < 2 or mapped[1] != profile]
    report.leg(not broken and not refused,
               "and the configuration maps back to the profile it came from",
               "\n".join(broken) or "cy_profile_for_configuration answers with the profile for "
                                    "each of the four configurations")
    return probe


def check_build_system_and_platforms() -> int:
    report = Report("build-system-and-platforms", "M4")
    rows = justfile_profile_table()

    report.leg(tuple(sorted(rows)) == tuple(sorted(PROFILES)),
               "the justfile's profile table names exactly the four profiles",
               f"named: {', '.join(sorted(rows)) or 'none'}")

    probe = _profile_legs(report, rows)

    declared, missing = cargo_profiles(rows, read("editor/Cargo.toml"))
    report.leg(not missing, "editor/Cargo.toml declares every profile the table selects",
               f"declared: {', '.join(declared) or 'none'}\n"
               f"selected but not declared: {', '.join(missing) or 'none'}")

    answered = subprocess.run(["just", "_profiles"], cwd=REPO_ROOT, capture_output=True,
                              text=True, check=False)
    report.leg(answered.returncode == 0 and tuple(answered.stdout.split()) == PROFILES,
               "the workflow itself answers with the four profiles when asked",
               f"just _profiles -> {' '.join(answered.stdout.split()) or answered.stderr.strip()}")

    # THE FEATURE OPTIONS ARE DECLARED RATHER THAN MATCHED, for the same reason as the profiles:
    # `cy_declare_features()` runs every row through `_cy_resolve_default()` and calls `option()`, so
    # a row with a default the resolver does not accept is a configure failure — reported by the leg
    # above — and a row that exists only in a comment declares no option and is absent here.
    options = {name: value for name, value in probe.of("feature")}
    report.leg(bool(options), "the feature options are declared by cmake/features.cmake's own table "
                              "through its own resolver",
               f"{len(options)} option(s) declared: "
               f"{', '.join(f'{name}={value}' for name, value in sorted(options.items()))}"
               if options else "cy_declare_features() declared nothing")

    pinned = pins_outside_the_manifest()
    report.leg(not pinned, "no CMake file carries a pin the dependency manifest should own",
               "\n".join(pinned)
               or "every fetch reads its repository and commit from deps/manifest.toml")

    # And every gated dependency names a feature that exists, or `-D CY_X=OFF` excludes nothing
    # because there is no CY_X. `options` is what CMake DECLARED, so a feature the manifest gates on
    # that features.cmake only mentions in a comment is a finding rather than a match.
    unknown = [f"{entry.get('name')} -> {entry.get('feature', '')!r}" for entry in manifest_entries()
               if entry.get("optional") == "true" and entry.get("feature", "") not in options]
    report.leg(bool(options) and not unknown,
               "every optional dependency is gated by a feature option that exists",
               "\n".join(unknown) or "every `feature` in deps/manifest.toml is an option "
                                     "cy_declare_features() declared")

    # "THE SWIFT TOOLCHAIN INTEGRATION THE REQUIREMENT NAMES, which is the rung at which the build
    # system had to serve a second toolchain" — and the overlay is generated from the ABI rather than
    # written, which is the part that makes it a build-system claim rather than a binding one.
    overlay = subprocess.run(["python3", "tools/gen/swift/overlay_gen.py", "--check"],
                             cwd=REPO_ROOT, capture_output=True, text=True, check=False)
    report.leg(overlay.returncode == 0,
               "the committed Swift overlay still regenerates from the C ABI description",
               (overlay.stdout + overlay.stderr).strip()[:400])
    return report.finish()


# --- developer-workflow-and-just — argued Working at M6 -------------------------------------------
#
# capability-matrix.md: "Every category of the required recipe surface does something from M6:
# Environment, Build, Run, Test, Quality, Generate, Content, Diagnose, Roadmap, Maintenance,
# Release. Before M6 the Content category refused."

#: The eleven categories of `developer-workflow-and-just`'s own "Recipe surface" table, and the
#: recipe-name prefix each is reached by — the justfile's rule is `<category>-<verb>`, flat rather
#: than `just` modules so that bare `just` lists every recipe with its description.
CATEGORIES = (
    ("Environment", "env"),
    ("Build", "build"),
    ("Run", "run"),
    ("Test", "test"),
    ("Quality", "quality"),
    ("Generate", "generate"),
    ("Content", "content"),
    ("Diagnose", "diagnose"),
    ("Roadmap", "roadmap"),
    ("Maintenance", "maintenance"),
    ("Release", "release"),
)

#: The two recipes `developer-workflow-and-just` names for the Roadmap category explicitly: "at
#: least a status recipe that reports every capability's tier … and a recipe that runs a named
#: milestone's full exit criteria and exits non-zero if any fail".
ROADMAP_RECIPES = ("roadmap-status", "roadmap-milestone")


def refusing_recipes() -> dict[str, bool]:
    """Every public recipe, and whether its body is the `_not-implemented` refusal.

    Read from the imported `just/*.just` files rather than executed: a recipe that refuses does so
    at run time, and running the Release category to find out would publish something. The helper's
    own definition is checked by the caller, so renaming it away cannot make every recipe look
    implemented.
    """
    bodies: dict[str, bool] = {}
    for path in sorted((REPO_ROOT / "just").glob("*.just")) + [REPO_ROOT / "justfile"]:
        name = None
        for line in path.read_text(encoding="utf-8").splitlines():
            header = re.match(r"^([a-z][a-z0-9-]*)(\s+[^:]*)?:", line)
            if header:
                name = header.group(1)
                bodies.setdefault(name, False)
            elif name and line.startswith((" ", "\t")) and "_not-implemented" in line:
                bodies[name] = True
            elif line.strip() and not line.startswith((" ", "\t")):
                name = None
    return bodies


def category_tally(recipes: list[str], refusing: dict[str, bool]):
    """Per category: how many recipes it has, how many do something, and which categories fail.

    A category is reached by the `<category>-<verb>` prefix, which is the justfile's own naming rule
    — flat rather than `just` modules, so that bare `just` lists every recipe with its description.
    """
    lines, empty, all_refusing = [], [], []
    for category, prefix in CATEGORIES:
        members = [name for name in recipes if name == prefix or name.startswith(f"{prefix}-")]
        working = [name for name in members if not refusing.get(name, False)]
        lines.append(f"{category:<12} {len(members):>2} recipe(s), {len(working):>2} do something"
                     + ("" if working else "  <- the whole category refuses"))
        if not members:
            empty.append(category)
        elif not working:
            all_refusing.append(category)
    return lines, empty, all_refusing


def check_developer_workflow_and_just() -> int:
    report = Report("developer-workflow-and-just", "M6")

    recipes, unavailable = recipe_surface()
    report.leg(bool(recipes) and not unavailable,
               "the recipe surface loads: every imported category file parses",
               f"{len(recipes)} public recipe(s)" if recipes else unavailable)

    # THE HELPER IS RUN, NOT SPELLED. It is how a refusal is recognised below, so if it were gone
    # every recipe would read as implemented and this check would pass over a workflow that refuses
    # everywhere — and the leg that guarded against that used to be `"_not-implemented recipe task:"
    # in read("justfile")`, which a comment satisfies. `just` is asked to run it instead: a refusal
    # is an exit status and a sentence naming the recipe and its task, and a helper that has been
    # renamed away answers `Justfile does not contain recipe`, which carries neither.
    refusal = subprocess.run(["just", "_not-implemented", "probe-recipe", "probe-task"],
                             cwd=REPO_ROOT, capture_output=True, text=True, check=False)
    said = (refusal.stdout + refusal.stderr).strip()
    report.leg(refusal.returncode != 0
               and "just probe-recipe: not implemented (task probe-task)" in said,
               "the one shape an unimplemented recipe takes still refuses when it is run",
               f"just _not-implemented probe-recipe probe-task -> exit {refusal.returncode}: "
               f"{said.splitlines()[0][:200] if said else 'it said nothing'}")

    detail, empty, all_refusing = category_tally(recipes, refusing_recipes())
    report.leg(not empty and not all_refusing,
               "every category of the required recipe surface does something rather than refusing",
               "\n".join(detail)
               + (f"\ncategory with no recipe at all: {', '.join(empty)}" if empty else "")
               + (f"\ncategory in which every recipe refuses: {', '.join(all_refusing)}"
                  if all_refusing else ""))

    missing = [name for name in ROADMAP_RECIPES if name not in recipes]
    report.leg(not missing, "the Roadmap category provides the two recipes the requirement names",
               f"expected: {', '.join(ROADMAP_RECIPES)}\n"
               f"absent: {', '.join(missing) or 'none'}")
    return report.finish()


# --- thirdparty-dependencies — argued Working at M8.b ---------------------------------------------
#
# capability-matrix.md: "The manifest carries every field the requirement lists, for fifteen
# dependencies at M8.b and seventeen today, with `deps/host-tools.toml` for build-time tooling that
# is never linked; `THIRD_PARTY.md` is generated from both; optional dependencies are feature-gated
# with a test that proves a disabled feature fetches, builds and links nothing; and from M6 the
# editor's Rust crates are under the same governance."


def manifest_entries() -> list[dict]:
    """`deps/manifest.toml`'s `[[dependency]]` tables, parsed the way the build parses them.

    cmake/dependencies.cmake reads this file WITHOUT a TOML library, which is why the format is a
    deliberately small subset — one `key = value` per line, whole-line comments only. Reading it the
    same way here means this check sees what the build sees.
    """
    entries: list[dict] = []
    current: dict | None = None
    for line in read("deps/manifest.toml").splitlines():
        stripped = line.strip()
        if stripped == "[[dependency]]":
            current = {}
            entries.append(current)
        elif current is not None and "=" in stripped and not stripped.startswith("#"):
            key, value = stripped.split("=", 1)
            current[key.strip()] = value.strip().strip('"')
    return entries


def check_thirdparty_dependencies() -> int:
    report = Report("thirdparty-dependencies", "M8.b")
    sys.path.insert(0, str(REPO_ROOT / "tools" / "deps"))
    import manifest as manifest_module  # noqa: PLC0415 — the tool is the check's subject

    # "THE MANIFEST CARRIES EVERY FIELD THE REQUIREMENT LISTS." `tools/deps/manifest.py` is the
    # loader that enforces it, so the check is that loading succeeds over the committed records
    # rather than a second field list that could drift away from the first.
    try:
        dependencies = manifest_module.load()
        tools = manifest_module.load_host_tools()
        failure = ""
    except Exception as error:  # noqa: BLE001 — any refusal of the record is the finding
        dependencies, tools, failure = (), (), f"{type(error).__name__}: {error}"
    report.leg(bool(dependencies) and bool(tools) and not failure,
               "every dependency and host tool carries the full field set the requirement lists",
               failure or f"{len(dependencies)} fetched dependency(ies), "
                          f"{len(tools)} host tool(s), all fields present")

    # "THIRD_PARTY.md IS GENERATED FROM BOTH" — and from the third record since M6. Currency is
    # `just maintenance-deps-check`'s job and `m11a:generated-code` runs it; what is checked here is
    # COVERAGE, which currency alone does not give: a document regenerated from a record that lost an
    # entry is current and wrong.
    # A host tool is credited under the name the machine installs it by — `package`, which is what
    # `just env-doctor` prints and what a reader would search a licence page for — so that is the
    # string looked for. `clang-python` is installed as `clang`, `libclang` as `libclang1-18`.
    attribution = read("THIRD_PARTY.md")
    uncredited = [entry.name for entry in dependencies if entry.name not in attribution]
    uncredited += [f"{tool.name} (as {tool.package})" for tool in tools
                   if tool.package not in attribution and tool.name not in attribution]
    report.leg(not uncredited, "THIRD_PARTY.md names every record in the dependency manifests",
               f"uncredited: {', '.join(uncredited) or 'none'}")

    crates = read("deps/rust-crates.toml")
    report.leg(bool(re.search(r"^\[\[crate\]\]", crates, re.MULTILINE)),
               "the editor's Rust crates are under the same governance, in a record of their own",
               f"{len(re.findall(r'^\[\[crate\]\]', crates, re.MULTILINE))} crate(s) declared in "
               "deps/rust-crates.toml")

    # "OPTIONAL DEPENDENCIES ARE FEATURE-GATED WITH A TEST THAT PROVES A DISABLED FEATURE FETCHES,
    # BUILDS AND LINKS NOTHING." That test is tools/deps/test_gating.py and until M11.a NO CRITERION
    # AND NO CI JOB RAN IT — the row's evidence named a test nobody was running, which is the same
    # defect as a tier nobody re-checks. It configures twice and looks at what the build system
    # actually did, because a stubbed dependency is still downloaded and still compiled and the
    # difference is only observable at configure time.
    gating = subprocess.run(["python3", "tools/deps/test_gating.py"], cwd=REPO_ROOT,
                            capture_output=True, text=True, check=False)
    report.leg(gating.returncode == 0,
               "a disabled feature fetches, builds and links nothing, proved rather than asserted",
               (gating.stdout + gating.stderr).strip()[-600:])
    return report.finish()


ROWS = {
    "testing-and-quality": check_testing_and_quality,
    "build-system-and-platforms": check_build_system_and_platforms,
    "developer-workflow-and-just": check_developer_workflow_and_just,
    "thirdparty-dependencies": check_thirdparty_dependencies,
}


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("row", nargs="?", choices=sorted(ROWS),
                        help="the capability row whose Working evidence is to be evaluated")
    parser.add_argument("--list", action="store_true", help="print the rows this tool evaluates")
    arguments = parser.parse_args(argv)
    if arguments.list:
        for row in sorted(ROWS):
            print(row)
        return 0
    if not arguments.row:
        parser.error("name a row, or pass --list")
    print(f"{arguments.row}: the evidence docs/roadmap/capability-matrix.md argues its Working tier "
          f"from, re-checked against this tree")
    return ROWS[arguments.row]()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
