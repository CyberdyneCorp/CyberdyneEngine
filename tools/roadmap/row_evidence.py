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
import re
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]


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
                            check=True)
    return sorted(REPO_ROOT / name for name in listed.stdout.split("\0") if name)


# --- testing-and-quality — argued Working at M3 ---------------------------------------------------
#
# capability-matrix.md: "At M3's closing commit: `cy_add_test` carrying the taxonomy's per-case
# budgets and CTest labels, the unit, integration, smoke and render suites, golden images,
# `benchmarks/` with a committed baseline and per-benchmark tolerances, and `sanitize`,
# `sanitize-nightly`, `quality`, `specs`, `generated`, `identity` and `profiles` jobs in CI."

#: The continuous-integration jobs the row's evidence names by name.
TESTING_CI_JOBS = ("sanitize", "sanitize-nightly", "quality", "specs", "generated", "identity",
                   "profiles")


def check_testing_and_quality() -> int:
    report = Report("testing-and-quality", "M3")
    taxonomy = read("tests/CMakeLists.txt")

    # The taxonomy is DATA in tests/CMakeLists.txt rather than prose, which is what makes "a suite
    # cannot be declared without a budget" true rather than a convention. Both halves — the per-case
    # budget compiled into the binary and the per-suite CTest timeout — or the claim is half made.
    budgets = set(re.findall(r"set\(CY_TEST_BUDGET_(\w+)\s", taxonomy))
    timeouts = set(re.findall(r"set\(CY_TEST_TIMEOUT_(\w+)\s", taxonomy))
    report.leg(bool(budgets) and budgets == timeouts,
               "the taxonomy gives every kind both a per-case budget and a CTest timeout",
               f"budgets: {', '.join(sorted(budgets)) or 'none'}\n"
               f"timeouts: {', '.join(sorted(timeouts)) or 'none'}")

    # `cy_add_test` is the one way a suite is declared, and the three things it must carry are the
    # three the row is argued on: the budget compiled in, the kind as a CTest label, the timeout.
    for fragment, claim in (
        ("CY_TEST_BUDGET_NS=${CY_TEST_BUDGET_${arg_KIND}}",
         "cy_add_test compiles the kind's per-case budget into the binary"),
        ('LABELS "${arg_KIND}"', "cy_add_test gives every suite its kind as a CTest label"),
        ("TIMEOUT ${CY_TEST_TIMEOUT_${arg_KIND}}",
         "cy_add_test gives every suite the taxonomy's CTest timeout"),
    ):
        report.leg(fragment in taxonomy, claim, f"looked for: {fragment}")

    # A kind in the table with no suite behind it is a budget nobody is under. `KIND` is matched over
    # every CMakeLists.txt because suites are declared beside the module they test, not centrally.
    declared: dict[str, int] = {kind: 0 for kind in sorted(budgets)}
    for path in cmake_files():
        for kind in re.findall(r"cy_add_test\([^)]*?KIND\s+(\w+)", path.read_text(encoding="utf-8"),
                               re.DOTALL):
            declared[kind] = declared.get(kind, 0) + 1
    empty = [kind for kind in sorted(budgets) if not declared.get(kind)]
    report.leg(not empty, "every kind in the taxonomy has at least one suite declared",
               "  ".join(f"{kind}={declared.get(kind, 0)}" for kind in sorted(budgets))
               + (f"\nno suite for: {', '.join(empty)}" if empty else ""))

    references = sorted((REPO_ROOT / "tests/render/references").glob("*.png"))
    report.leg(bool(references), "golden images are committed, not described",
               f"{len(references)} reference image(s) in tests/render/references/")

    # "a committed baseline and PER-BENCHMARK tolerances" — so the test is not that the file exists
    # but that it covers what the tree actually registers, in both directions. A benchmark with no
    # threshold cannot regress and a threshold with no benchmark is a threshold nobody runs.
    registered = set()
    for path in (REPO_ROOT / "benchmarks").rglob("*.cpp"):
        registered.update(re.findall(r'CY_BENCHMARK\(\s*"([^"]+)"', path.read_text(encoding="utf-8")))
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

    workflow = read(".github/workflows/ci.yml")
    absent = [job for job in TESTING_CI_JOBS if not re.search(rf"^  {re.escape(job)}:$", workflow,
                                                              re.MULTILINE)]
    report.leg(not absent, "the quality jobs the row is argued on are declared in ci.yml",
               f"expected: {', '.join(TESTING_CI_JOBS)}\n"
               f"not declared: {', '.join(absent) or 'none'}")
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
#: (the `CY_PROFILE_<p>_<suffix>` suffix in cmake/profiles.cmake, the justfile column, a label).
PROFILE_COLUMNS = (("CONFIG", 1, "CMake configuration"),
                   ("CARGO", 4, "Cargo profile"),
                   ("SLANG", 5, "slangc flags"))


def justfile_profile_table() -> dict[str, list[str]]:
    """`profile_table` in the justfile, one row per profile, fields left to right."""
    table = re.search(r"profile_table := '(.*?)'", read("justfile"), re.DOTALL)
    rows: dict[str, list[str]] = {}
    for line in (table.group(1).splitlines() if table else []):
        fields = [field.strip() for field in line.split("|")]
        if len(fields) >= 5:
            rows[fields[0]] = fields
    return rows


def profile_columns_disagreeing(rows: dict[str, list[str]], profiles_cmake: str) -> list[str]:
    """Where the justfile's table and cmake/profiles.cmake say different things, or say nothing.

    This is the comparison nothing in the tree makes end to end. `cmake/profiles.cmake` cross-checks
    itself against the justfile at CONFIGURE time and only when a caller supplies both, and neither
    file has ever been compared with the Cargo profile the editor is actually built with.
    """
    found = []
    for profile in PROFILES:
        fields = rows.get(profile, [])
        for suffix, index, label in PROFILE_COLUMNS:
            stated = re.search(rf'set\(CY_PROFILE_{profile}_{suffix}\s+"?([^"\n)]+?)"?\s*\)',
                               profiles_cmake)
            expected = fields[index] if len(fields) > index else None
            if stated is None or expected is None:
                found.append(f"{profile}/{label}: not stated in both files")
            elif " ".join(stated.group(1).split()) != " ".join(expected.split()):
                found.append(f"{profile}/{label}: cmake {stated.group(1).strip()!r} "
                             f"against justfile {expected!r}")
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


#: The four values `_cy_resolve_default` in cmake/features.cmake accepts. A fifth is a configure-time
#: error, which is a failure nobody sees until they configure with that option.
FEATURE_DEFAULTS = ("ON", "OFF", "DEVELOPMENT", "APPLE")


def feature_options() -> list[tuple[str, str]]:
    """`CY_FEATURE_OPTIONS`, one (name, default) per row. The table is data, so it is read as data."""
    return re.findall(r'"(CY_[A-Z0-9_]+)\|([A-Z]+)\|', read("cmake/features.cmake"))


def cargo_profiles(rows: dict[str, list[str]], cargo: str) -> tuple[list[str], list[str]]:
    """What `editor/Cargo.toml` declares, and which of the table's selections it does not.

    The leg nothing checked: the Cargo column has to name a profile Cargo actually has, or
    `just build-editor --profile release` selects one that does not exist.
    """
    declared = sorted(set(re.findall(r"^\[profile\.(\w+)\]$", cargo, re.MULTILINE)))
    selected = [rows[profile][4] for profile in PROFILES if len(rows.get(profile, [])) > 4]
    return declared, [name for name in selected if name not in declared]


def check_build_system_and_platforms() -> int:
    report = Report("build-system-and-platforms", "M4")
    rows = justfile_profile_table()
    profiles_cmake = read("cmake/profiles.cmake")
    cargo = read("editor/Cargo.toml")

    report.leg(tuple(sorted(rows)) == tuple(sorted(PROFILES)),
               "the justfile's profile table names exactly the four profiles",
               f"named: {', '.join(sorted(rows)) or 'none'}")

    listed = re.search(r"set\(CY_PROFILES\s+([^\n)]+)", profiles_cmake)
    report.leg(bool(listed) and tuple(listed.group(1).split()) == PROFILES,
               "cmake/profiles.cmake lists the same four, in the same order",
               f"CY_PROFILES = {listed.group(1).strip() if listed else 'not set'}")

    disagreeing = profile_columns_disagreeing(rows, profiles_cmake)
    report.leg(not disagreeing,
               "every profile means the same thing in CMake, Cargo and the shader toolchain",
               "\n".join(disagreeing)
               or "the CMake, Cargo and Slang columns agree between the justfile and profiles.cmake")

    declared, missing = cargo_profiles(rows, cargo)
    report.leg(not missing, "editor/Cargo.toml declares every profile the table selects",
               f"declared: {', '.join(declared) or 'none'}\n"
               f"selected but not declared: {', '.join(missing) or 'none'}")

    answered = subprocess.run(["just", "_profiles"], cwd=REPO_ROOT, capture_output=True,
                              text=True, check=False)
    report.leg(answered.returncode == 0 and tuple(answered.stdout.split()) == PROFILES,
               "the workflow itself answers with the four profiles when asked",
               f"just _profiles -> {' '.join(answered.stdout.split()) or answered.stderr.strip()}")

    options = feature_options()
    unrecognised = [name for name, default in options if default not in FEATURE_DEFAULTS]
    report.leg(bool(options) and not unrecognised,
               "the feature options are a table with a default the resolver accepts",
               f"{len(options)} option(s)\n"
               f"unrecognised default: {', '.join(unrecognised) or 'none'}")

    pinned = pins_outside_the_manifest()
    report.leg(not pinned, "no CMake file carries a pin the dependency manifest should own",
               "\n".join(pinned)
               or "every fetch reads its repository and commit from deps/manifest.toml")

    # And every gated dependency names a feature that exists, or `-D CY_X=OFF` excludes nothing
    # because there is no CY_X.
    known = {name for name, _ in options}
    unknown = [f"{entry.get('name')} -> {entry.get('feature', '')!r}" for entry in manifest_entries()
               if entry.get("optional") == "true" and entry.get("feature", "") not in known]
    report.leg(not unknown, "every optional dependency is gated by a feature option that exists",
               "\n".join(unknown) or "every `feature` in deps/manifest.toml is in CY_FEATURE_OPTIONS")

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

    summary = subprocess.run(["just", "--summary"], cwd=REPO_ROOT, capture_output=True, text=True,
                             check=False)
    recipes = summary.stdout.split()
    report.leg(summary.returncode == 0 and bool(recipes),
               "the recipe surface loads: every imported category file parses",
               f"{len(recipes)} public recipe(s)" if recipes else summary.stderr.strip()[:400])

    # The helper is how a refusal is recognised below. If it is gone, every recipe would read as
    # implemented and this check would pass over a workflow that refuses everywhere.
    report.leg("_not-implemented recipe task:" in read("justfile"),
               "the one shape an unimplemented recipe takes is still the shape this check looks for",
               "justfile defines `_not-implemented`")

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
