#!/usr/bin/env python3
"""Check that a workflow file does not duplicate logic a recipe already has — task 2.4.4.

`developer-workflow-and-just` lists "a continuous integration script that duplicates rather than
invokes recipes" among the forbidden patterns, and requires each forbidden pattern to be checkable.
This is that check.

Three rules, applied to every command in every `.github/workflows/*.yml`:

1. The command is a `just` recipe, or it provisions a tool. Provisioning is what continuous
   integration is allowed to add around the recipes — a runner arrives without the OpenSpec CLI, and
   installing it is not build logic. Everything else that is not `just` is a job doing a recipe's
   job. Checkout, caching and artefact upload are `uses:` steps, which are actions rather than
   commands and are not examined here.
2. No command reaches for a build, test or quality tool directly. `cmake`, `ctest`, `clang-tidy` and
   their neighbours each have a recipe; naming one in a workflow is the duplication itself.
3. Every recipe a workflow names exists. A workflow that invokes a recipe nobody kept is a job that
   fails for a reason unrelated to the change under test.
4. Every permanent merge gate declared in `tools/roadmap/gates.toml` is actually run. That file is
   the gate set as data, and it exists because "continuous integration, `just roadmap-gates` and the
   contributor documentation must name the same gates and three hand-maintained copies diverge".
   This is the check that stops the first of those three from drifting: a gate nobody runs is not a
   gate, and the divergence is silent in every other direction.

`--list` prints each workflow's jobs and the recipes they invoke: the gate set, read from the
workflows rather than from a document that can fall behind them.

Run through `just ci-check`. Exits 0 when clean, 1 naming every violation with its file and line.

The YAML is read with a deliberately small line scanner rather than a YAML library: this check runs
on every host in the matrix, and a gate that needs a package installed before it can run is a gate
that gets skipped. It only has to find `run:` scalars in files this repository writes.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys
import tempfile
import tomllib

# Commands a workflow may run that are not recipes. Each provisions a tool, and each is a command
# `just env-doctor` prints as the correction for that tool being absent — so this list stays honest
# by being the same set of answers, given to a runner instead of to a person.
PROVISIONING = (
    "sudo apt-get",
    "sudo apt",
    "brew install",
    "brew update",
    "npm install -g",
    "winget install",
    "choco install",
    "pipx install",
    # The reflection generator's frontend bindings. `reflect_gen.py` prints this exact command when
    # they are absent, and a runner needs the same answer a person gets.
    "pip install",
    "python3 -m pip install",
)

# Tools whose use in a workflow means the workflow is doing a recipe's job, and the recipe that
# already does it. The message names the replacement, because "this is forbidden" without the
# alternative is how a rule gets worked around instead of followed.
DUPLICATED_TOOLS = {
    "cmake": "just build-engine",
    "ctest": "just test-unit, test-integration, test-smoke or test-all",
    "ninja": "just build-engine",
    "clang-format": "just quality-format-check",
    "clang-tidy": "just quality-lint",
    "openspec": "just quality-specs",
    "cargo": "just build-editor",
    "slangc": "just build-shaders",
}

RUN_KEY = re.compile(r"^(?P<indent>\s*)(?:-\s+)?run:\s*(?P<inline>.*?)\s*$")
JOB_KEY = re.compile(r"^  (?P<name>[A-Za-z_][\w-]*):\s*$")
BLOCK_SCALAR = re.compile(r"^[|>][+-]?\d*$")
SEPARATORS = re.compile(r"&&|\|\||;")


class Violation:
    def __init__(self, path: pathlib.Path, line: int, message: str, fix: str) -> None:
        self.path, self.line, self.message, self.fix = path, line, message, fix

    def render(self, root: pathlib.Path) -> str:
        return (
            f"  {self.path.relative_to(root)}:{self.line}\n"
            f"      {self.message}\n"
            f"      {self.fix}"
        )


class Command:
    def __init__(self, line: int, text: str, job: str) -> None:
        self.line, self.text, self.job = line, text, job


def commands_in(path: pathlib.Path) -> list[Command]:
    """Every command line of every `run:` step, with the job it belongs to."""
    lines = path.read_text(encoding="utf-8").splitlines()
    found: list[Command] = []
    job = "?"
    index = 0
    while index < len(lines):
        job_match = JOB_KEY.match(lines[index])
        if job_match:
            job = job_match.group("name")
        match = RUN_KEY.match(lines[index])
        if not match:
            index += 1
            continue
        column = lines[index].index("run:")
        inline = match.group("inline")
        index += 1
        if not BLOCK_SCALAR.match(inline):
            if inline:
                found.append(Command(index, inline, job))
            continue
        # A block scalar runs until a line indented no further than the `run:` key itself.
        while index < len(lines):
            body = lines[index]
            if body.strip() and (len(body) - len(body.lstrip())) <= column:
                break
            if body.strip():
                found.append(Command(index + 1, body.strip(), job))
            index += 1
    return found


def known_recipes(root: pathlib.Path) -> set[str]:
    summary = subprocess.run(
        ["just", "--summary"], cwd=root, capture_output=True, text=True, check=True
    )
    return set(summary.stdout.split())


def check_segment(
    path: pathlib.Path, command: Command, segment: str, recipes: set[str]
) -> list[Violation]:
    """The three rules, applied to one command — or to one side of an `&&`."""
    stripped = segment.strip()
    if not stripped or stripped.startswith("#"):
        return []
    if stripped.startswith(PROVISIONING):
        return []

    words = stripped.split()
    tool = pathlib.PurePath(words[0]).name
    if tool in DUPLICATED_TOOLS:
        return [
            Violation(
                path,
                command.line,
                f"runs {tool} directly: `{stripped}`",
                f"That is a recipe's job. Invoke {DUPLICATED_TOOLS[tool]} instead, so the check "
                f"reproduces locally with the same command.",
            )
        ]
    if tool != "just":
        return [
            Violation(
                path,
                command.line,
                f"is neither a recipe nor a tool install: `{stripped}`",
                "A workflow invokes recipes. Add a recipe for this and call it.",
            )
        ]

    named = [word for word in words[1:] if not word.startswith("-")]
    if named and named[0] not in recipes:
        return [
            Violation(
                path,
                command.line,
                f"invokes recipe '{named[0]}', which does not exist",
                "`just --list` names the recipes that do.",
            )
        ]
    return []


def recipes_invoked(command: Command, recipes: set[str]) -> list[str]:
    invoked = []
    for segment in SEPARATORS.split(command.text):
        words = segment.split()
        if words and words[0] == "just":
            invoked += [word for word in words[1:] if word in recipes]
    return invoked


def list_gates(root: pathlib.Path, workflows: list[pathlib.Path], recipes: set[str]) -> int:
    for path in workflows:
        print(f"{path.relative_to(root)}")
        jobs: dict[str, list[str]] = {}
        for command in commands_in(path):
            jobs.setdefault(command.job, [])
            for recipe in recipes_invoked(command, recipes):
                if recipe not in jobs[command.job]:
                    jobs[command.job].append(recipe)
        for job, invoked in jobs.items():
            print(f"  {job:<12} {' '.join(invoked) if invoked else '(no recipes)'}")
    return 0


# The milestone ladder, for the rule below. Read from tools/roadmap/record.py rather than repeated
# here, because a second copy of the ladder is how M5.5 came to sort off the end of the first one.
def _ladder(root: pathlib.Path) -> tuple[str, ...]:
    record = root / "tools" / "roadmap" / "record.py"
    if not record.exists():
        return ()
    text = record.read_text(encoding="utf-8")
    start = text.find("MILESTONES = (")
    if start < 0:
        return ()
    end = text.find(")", start)
    body = text[start + len("MILESTONES = (") : end]
    return tuple(part.strip().strip('"\'') for part in body.split(",") if part.strip())


def _rung(ladder: tuple[str, ...], identifier: str) -> int:
    """Where a milestone sits. Off the ladder sorts last, as `criteria.rung` does."""
    return ladder.index(identifier) if identifier in ladder else len(ladder)


def gate_coverage(root: pathlib.Path, workflows: list[pathlib.Path]) -> list[str]:
    """Gates whose commands no workflow runs — permanent ones, and CLOSED MILESTONES.

    --- WHY MILESTONE GATES ARE CHECKED HERE, WHICH IS M6 TASK 10.9 ---------------------------------

    Until M6 this function skipped any gate whose class was not `permanent`, so six milestone gates
    sat at `state = "green"` — M0 through M5.5 — and NO CONTINUOUS-INTEGRATION JOB RAN ANY OF THEM.
    `ci.yml` named `roadmap-milestone` in a comment and nowhere else. `delivery-roadmap` puts a
    closed milestone's criteria into the permanent set the moment it closes and requires them to
    stay green; a set nothing runs cannot regress *visibly*, which is the same failure as M2's
    unpromoted `milestone-m1` gate one level up. It is also the unmet precondition that
    specification sets for reducing the audit at M9, so closing it at M6 is worth more than
    discovering it at M9.

    --- THE RULE, AND WHY IT IS NOT "EVERY GREEN GATE'S COMMAND APPEARS" ----------------------------

    The ledger has been FLAT since M5: `criteria.build_plan` merges the criteria of every green
    milestone gate BELOW the target with the target's own and runs each distinct check once. So one
    job running `just roadmap-milestone m5b` evaluates M0, M1, M2, M3, M4, M5 and M5.5 — and
    demanding seven separate commands would re-run `four-profiles` seven times, which is exactly the
    multiplication the flattening removed.

    A green milestone gate is therefore covered when a workflow runs the ledger of ANY milestone at
    or above its rung. Uncovered gates are reported naming the one command that would cover them
    all, so the fix is one job rather than one job per milestone.
    """
    declaration = root / "tools" / "roadmap" / "gates.toml"
    if not declaration.exists():
        return []
    gates = tomllib.loads(declaration.read_text(encoding="utf-8")).get("gate", [])

    invoked = {
        segment.strip()
        for path in workflows
        for command in commands_in(path)
        for segment in SEPARATORS.split(command.text)
    }
    uncovered = []
    for gate in gates:
        if gate.get("class") != "permanent":
            continue
        missing = [run for run in gate.get("runs", []) if run not in invoked]
        if missing:
            uncovered.append(
                f"gate '{gate['id']}' is declared permanent but no workflow runs: "
                + ", ".join(f"`{command}`" for command in missing)
            )

    ladder = _ladder(root)
    green = [gate for gate in gates
             if gate.get("class") == "milestone" and gate.get("state") == "green"]
    if green:
        # The highest rung any workflow actually evaluates. `just roadmap-milestone <id>` is the
        # only spelling a ledger has, so the command is matched rather than parsed loosely.
        def evaluates(identifier: str) -> bool:
            # `--ci`, `--list` and a profile flag may follow the id, so the command is matched on
            # its first three words rather than compared whole.
            prefix = f"just roadmap-milestone {identifier}"
            return any(text == prefix or text.startswith(prefix + " ") for text in invoked)

        evaluated = [identifier for identifier in ladder if evaluates(identifier)]
        reached = max((_rung(ladder, identifier) for identifier in evaluated), default=-1)
        gap = [gate for gate in green if _rung(ladder, gate.get("milestone", "")) > reached]
        if gap:
            newest = max(gap, key=lambda gate: _rung(ladder, gate.get("milestone", "")))
            names = ", ".join(f"'{gate['id']}'" for gate in gap)
            uncovered.append(
                f"{len(gap)} closed milestone gate(s) — {names} — are green and no workflow "
                f"evaluates them. One job running `just roadmap-milestone "
                f"{newest.get('milestone')}` covers all of them, because a ledger is flat and "
                "inherits every green milestone below it"
            )
    return uncovered


# The recipes that will not run without the pinned LLVM tooling, and so may not appear in a job that
# has not installed it. `env-doctor` is here because it requires the pin rather than merely using it.
NEEDS_PINNED_LLVM = ("just env-doctor", "just quality-format-check", "just quality-lint")

PINNED_INSTALL = re.compile(r"\b(clang-format|clang-tidy)==(?P<version>[0-9][0-9.]*)")
PIN_IN_JUSTFILE = re.compile(r"^llvm_pin_version\s*:=\s*'(?P<version>[^']+)'", re.MULTILINE)


def pinned_version(root: pathlib.Path) -> str | None:
    """The LLVM version the justfile pins, which is the one the workflows must install."""
    justfile = root / "justfile"
    if not justfile.exists():
        return None
    match = PIN_IN_JUSTFILE.search(justfile.read_text(encoding="utf-8"))
    return match.group("version") if match else None


def pin_drift(root: pathlib.Path, workflows: list[pathlib.Path]) -> list[str]:
    """The workflows install the version the justfile pins, in every job that needs it.

    `developer-workflow-and-just` requires continuous integration to use the same pinned versions as
    developers. The pin lives in the justfile, `just env-doctor` enforces it, and a workflow that
    installed a different version would produce a gate result nobody can reproduce — which is
    exactly the failure the pin exists to prevent, reintroduced one edit later.
    """
    pin = pinned_version(root)
    if pin is None:
        return ["the justfile declares no llvm_pin_version, so the workflows cannot be checked"]

    problems = []
    for path in workflows:
        installed: set[str] = set()
        needed: dict[str, tuple[int, str]] = {}
        for command in commands_in(path):
            for tool, version in PINNED_INSTALL.findall(command.text):
                if version != pin:
                    problems.append(
                        f"{path.name}:{command.line} installs {tool}=={version}, but the justfile "
                        f"pins {pin}"
                    )
                else:
                    installed.add(command.job)
            for recipe in NEEDS_PINNED_LLVM:
                if recipe in command.text and command.job not in needed:
                    needed[command.job] = (command.line, recipe)
        for job, (line, recipe) in needed.items():
            if job not in installed:
                problems.append(
                    f"{path.name}:{line} job '{job}' runs `{recipe}` without installing the pinned "
                    f"LLVM tooling (pip install clang-format=={pin} clang-tidy=={pin})"
                )
    return problems


# The documented Linux dependency set, and the check M9's closing gate had to write.
#
# SIXTY-THREE CI RUNS, NOT ONE OF THEM GREEN, AND THE CAUSE WAS FOUR PACKAGES. Every Linux job in
# ci.yml died at CONFIGURE — "Couldn't find dependency package for XCURSOR" — because the workflow
# installed `libx11-dev libxext-dev libwayland-dev libxkbcommon-dev` while README.md's own "System
# libraries SDL3 builds against" list, the one a human runs on a fresh clone, also names
# `libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev`. `cmake/dependencies.cmake` says which X11
# extensions the engine keeps on and why; SDL3 refuses to configure when one of them has no header.
#
# So the invariant is: THE RUNNER GETS WHAT THE DEVELOPER IS TOLD TO INSTALL. Compared over the X11
# packages only, which are the ones SDL3 turns into a hard configure failure — an audio or IME
# package that is missing degrades a feature, an X11 one stops the build.
README_APT = re.compile(r"^sudo apt(?:-get)? install -y (?P<packages>.+?)$", re.MULTILINE)
X11_PACKAGE = re.compile(r"^libx[a-z0-9.-]*-dev$")
# The one paragraph of README.md this compares against. Named by its own comment rather than by
# position, so that inserting a section above it does not silently change what is checked — and so
# that the Swift toolchain's apt line, which also installs a `libx*-dev` (libxml2-dev, which has
# nothing to do with X11), is not mistaken for it.
README_SECTION = "# System libraries SDL3 builds against"


def _packages(text: str) -> set[str]:
    """Every package named by an apt install line, continuations included."""
    found: set[str] = set()
    for line in text.replace("\\\n", " ").splitlines():
        match = README_APT.match(line.strip())
        if match:
            found.update(match.group("packages").split())
    return found


def documented_dependencies(root: pathlib.Path) -> set[str]:
    """The X11 development packages README.md tells a Linux developer to install."""
    readme = root / "README.md"
    if not readme.exists():
        return set()
    text = readme.read_text(encoding="utf-8")
    _, marker, rest = text.partition(README_SECTION)
    if not marker:
        return set()
    section = rest.split("```", 1)[0]
    return {p for p in _packages(section) if X11_PACKAGE.match(p)}


def system_dependencies(root: pathlib.Path, workflows: list[pathlib.Path]) -> list[str]:
    """Every Linux job installs the documented X11 set, or the difference is named here."""
    documented = documented_dependencies(root)
    if not documented:
        return ["README.md documents no X11 development packages, so the workflows cannot be "
                "checked against it"]

    problems = []
    for path in workflows:
        for command in commands_in(path):
            if "apt-get install" not in command.text and "apt install" not in command.text:
                continue
            installed = _packages(command.text)
            if not any(X11_PACKAGE.match(package) for package in installed):
                continue  # a step installing something else entirely, such as libclang
            missing = sorted(documented - installed)
            if missing:
                problems.append(
                    f"{path.name}:{command.line} installs {len(installed)} package(s) and omits "
                    f"{', '.join(missing)}, which README.md names as a system library SDL3 builds "
                    "against. Every Linux job failed at the SDL3 configure for exactly this reason "
                    "from M0 to M9"
                )
    return problems


# --- The check's own negative fixtures -------------------------------------------------------------
#
# A gate that has never been seen to fire is a gate nobody should trust. Each case below is a
# workflow step that must be rejected, and the last is one that must not be, so that a rule which
# stopped firing fails here rather than passing everything.
SELFTEST_CASES = (
    ("- run: cmake --build build/dev", "runs cmake directly"),
    ("- run: ctest --test-dir build/dev", "runs ctest directly"),
    ("- run: clang-tidy -p build/dev src/core/base/src/error.cpp", "runs clang-tidy directly"),
    ("- run: openspec validate --specs --strict", "runs openspec directly"),
    ("- run: ./scripts/build.sh", "is neither a recipe nor a tool install"),
    ("- run: just build-engine && ninja -C build/dev", "runs ninja directly"),
    ("- run: just build-everything", "invokes recipe 'build-everything', which does not exist"),
)

SELFTEST_LEGAL = (
    "- run: just build-all",
    "- run: npm install -g @fission-ai/openspec",
    "- run: |\n          sudo apt-get update\n          sudo apt-get install -y ninja-build",
)


def selftest(root: pathlib.Path) -> int:
    """Run the rules over deliberately bad steps, and over legal ones."""
    recipes = known_recipes(root)
    failed = 0
    with tempfile.TemporaryDirectory() as directory:
        scratch = pathlib.Path(directory) / "case.yml"

        def violations_of(step: str) -> list[Violation]:
            scratch.write_text(f"jobs:\n  case:\n    steps:\n      {step}\n", encoding="utf-8")
            found: list[Violation] = []
            for command in commands_in(scratch):
                for segment in SEPARATORS.split(command.text):
                    found.extend(check_segment(scratch, command, segment, recipes))
            return found

        for step, expected in SELFTEST_CASES:
            found = violations_of(step)
            if any(expected in violation.message for violation in found):
                print(f"ok   rejected: {step.strip()}")
            else:
                failed += 1
                messages = [violation.message for violation in found] or ["nothing"]
                print(f"fail {step.strip()}\n       expected {expected!r}, got {messages}",
                      file=sys.stderr)

        for step in SELFTEST_LEGAL:
            found = violations_of(step)
            if found:
                failed += 1
                print(f"fail accepted step was rejected: {step.strip()}\n"
                      f"       {[violation.message for violation in found]}", file=sys.stderr)
            else:
                print(f"ok   accepted: {step.splitlines()[0].strip()}")

        # The pin's own negative fixtures. A workflow that installs the wrong version, and one that
        # runs a gate needing the pinned tooling without installing it, must both be rejected; the
        # correct one must not be.
        pin = pinned_version(root) or "0.0.0"
        pin_cases = (
            (
                f"run: |\n          pip install clang-format==1.2.3 clang-tidy=={pin}\n"
                f"      - run: just quality-lint",
                "but the justfile pins",
            ),
            ("run: just quality-lint", "without installing the pinned LLVM tooling"),
        )
        for step, expected in pin_cases:
            scratch.write_text(
                f"jobs:\n  case:\n    steps:\n      - {step}\n", encoding="utf-8"
            )
            found = pin_drift(root, [scratch])
            if any(expected in problem for problem in found):
                print(f"ok   rejected: {expected}")
            else:
                failed += 1
                print(f"fail expected {expected!r}, got {found or ['nothing']}", file=sys.stderr)

        scratch.write_text(
            "jobs:\n  case:\n    steps:\n"
            f"      - run: pip install clang-format=={pin} clang-tidy=={pin}\n"
            "      - run: just quality-lint\n",
            encoding="utf-8",
        )
        found = pin_drift(root, [scratch])
        if found:
            failed += 1
            print(f"fail accepted workflow was rejected: {found}", file=sys.stderr)
        else:
            print("ok   accepted: a job that installs the pinned tooling before the gate")

        # --- M9's OWN NEGATIVE FIXTURE ------------------------------------------------------------
        #
        # THE DEFECT, RESTORED: a Linux step that installs a subset of the documented X11 set. That
        # was this repository's actual state from M0 to M9 — sixty-three continuous-integration
        # runs, not one green, every Linux leg dead at an SDL3 configure asking for XCURSOR.
        documented = sorted(documented_dependencies(root))
        if len(documented) < 2:
            failed += 1
            print("fail README.md documents fewer than two X11 packages to check against",
                  file=sys.stderr)
        else:
            scratch.write_text(
                "jobs:\n  case:\n    steps:\n"
                f"      - run: sudo apt-get install -y ninja-build {documented[0]}\n",
                encoding="utf-8",
            )
            found = system_dependencies(root, [scratch])
            if any("README.md names as a system library" in problem for problem in found):
                print(f"ok   rejected: a Linux job installing 1 of {len(documented)} documented "
                      "X11 packages")
            else:
                failed += 1
                print(f"fail a short package list was accepted: {found or ['nothing']}",
                      file=sys.stderr)

            scratch.write_text(
                "jobs:\n  case:\n    steps:\n"
                f"      - run: sudo apt-get install -y ninja-build {' '.join(documented)}\n",
                encoding="utf-8",
            )
            found = system_dependencies(root, [scratch])
            if found:
                failed += 1
                print(f"fail the documented list itself was rejected: {found}", file=sys.stderr)
            else:
                print("ok   accepted: a Linux job installing every documented X11 package")

        # --- M6 TASK 10.9's OWN NEGATIVE FIXTURE -------------------------------------------------
        #
        # THE DEFECT, RESTORED: a workflow that runs every permanent gate and no milestone ledger.
        # That was this repository's actual state from M0 to M6 — seven green milestone gates and no
        # job that evaluated one — and it passed this check, because the check skipped any gate whose
        # class was not `permanent`. It must not pass now.
        gate_set = tomllib.loads(
            (root / "tools" / "roadmap" / "gates.toml").read_text(encoding="utf-8")
        ).get("gate", [])
        permanent_commands = [
            command
            for gate in gate_set
            if gate.get("class") == "permanent"
            for command in gate.get("runs", [])
        ]
        newest_green = [
            gate.get("milestone", "")
            for gate in gate_set
            if gate.get("class") == "milestone" and gate.get("state") == "green"
        ]
        ladder = _ladder(root)
        newest = max(newest_green, key=lambda name: _rung(ladder, name), default="")

        def coverage_of(commands: list[str]) -> list[str]:
            body = "".join(f"      - run: {command}\n" for command in commands)
            scratch.write_text(f"jobs:\n  case:\n    steps:\n{body}", encoding="utf-8")
            return gate_coverage(root, [scratch])

        if newest:
            gaps = coverage_of(permanent_commands)
            if any("closed milestone gate" in gap for gap in gaps):
                print("ok   rejected: every permanent gate run and no milestone ledger evaluated")
            else:
                failed += 1
                print("fail a workflow that evaluates no closed milestone's criteria was accepted",
                      file=sys.stderr)

            gaps = coverage_of([*permanent_commands, f"just roadmap-milestone {newest} --ci"])
            if gaps:
                failed += 1
                print(f"fail the newest closed milestone's ledger did not cover the ones below it: "
                      f"{gaps}", file=sys.stderr)
            else:
                print(f"ok   accepted: one job running `just roadmap-milestone {newest}` covers "
                      f"every green milestone gate, because a ledger is flat")
        else:
            failed += 1
            print("fail no milestone gate is green, so the coverage rule cannot be tested",
                  file=sys.stderr)

    total = len(SELFTEST_CASES) + len(SELFTEST_LEGAL) + 5
    if failed:
        print(f"check-workflows selftest: {failed} of {total} cases failed", file=sys.stderr)
        return 1
    print(f"check-workflows selftest: {total}/{total} passed")
    return 0


def long_run_cancellation(root: pathlib.Path,
                          workflows: list[pathlib.Path]) -> list[str]:
    """A workflow that runs for hours on `main` must not cancel its own in-progress runs.

    MEASURED, NOT REASONED. On 2026-09-11 `gh run list` reported SIXTY-FIVE of sixty-six runs in
    this repository's history ending `cancelled` — no `success`, and no honest `failure` either,
    because no run lived long enough to reach a verdict. The median survived 115 minutes and the
    longest reached 14 hours 31 before the next push killed it.

    `cancel-in-progress: true` is ordinary good practice and is right for a pull request: it stops
    stale runs piling up on a branch somebody is pushing to repeatedly. It is wrong for a long
    workflow on a trunk that is pushed to many times a day, which is exactly this project — milestone
    workflows commit their agents' work mid-flight on purpose.

    The cost was not a red badge. `delivery-roadmap` makes "continuous integration has actually
    executed" a precondition for a milestone's audit ever shrinking, so this one setting is why every
    gate from M0 to M9 ran in full, by hand, for six to twelve hours each.

    The rule: if a workflow triggers on a push to a branch, its `cancel-in-progress` must not be the
    bare literal `true`. An expression that distinguishes the trunk from a branch is what this looks
    like when it is right, and an explicit `false` is also fine.
    """
    problems: list[str] = []
    for path in workflows:
        text = path.read_text(encoding="utf-8")
        if not re.search(r"^on:\s*$", text, re.M):
            continue
        # Only workflows that actually run on a push to a branch can be cancelled by the next push.
        if not re.search(r"^\s*push:\s*$", text, re.M):
            continue
        match = re.search(r"^\s*cancel-in-progress:\s*(.+?)\s*$", text, re.M)
        if match is None:
            continue
        value = match.group(1)
        if value == "true":
            problems.append(
                f"{path.relative_to(root)}: `cancel-in-progress: true` on a workflow that runs on "
                "push. A run that takes hours is then killed by the next commit, and this "
                "repository's first sixty-five runs all died that way without ever reaching a "
                "verdict. Make it an expression that spares the trunk, or false."
            )
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=pathlib.Path,
        default=pathlib.Path(__file__).resolve().parents[2],
        help="repository root (default: this file's repository)",
    )
    parser.add_argument(
        "--selftest",
        action="store_true",
        help="run the rules over deliberately bad steps, proving the check still fires",
    )
    parser.add_argument(
        "--list",
        action="store_true",
        help="print each job and the recipes it invokes instead of checking",
    )
    arguments = parser.parse_args()
    root = arguments.root.resolve()

    if arguments.selftest:
        return selftest(root)

    workflows = sorted((root / ".github" / "workflows").glob("*.y*ml"))
    if not workflows:
        print("check-workflows: no workflow files in .github/workflows/", file=sys.stderr)
        return 1

    recipes = known_recipes(root)
    if arguments.list:
        return list_gates(root, workflows, recipes)

    violations: list[Violation] = []
    total = 0
    for path in workflows:
        for command in commands_in(path):
            total += 1
            for segment in SEPARATORS.split(command.text):
                violations.extend(check_segment(path, command, segment, recipes))

    uncovered = gate_coverage(root, workflows)
    drift = pin_drift(root, workflows)
    system = system_dependencies(root, workflows)
    cancellation = long_run_cancellation(root, workflows)

    if violations or uncovered or drift or system or cancellation:
        print("check-workflows: the workflows and the recipes disagree", file=sys.stderr)
        for violation in violations:
            print(violation.render(root), file=sys.stderr)
        for gap in uncovered:
            print(f"  {gap}\n      `just roadmap-gates` prints the declared set.", file=sys.stderr)
        for gap in drift:
            print(f"  {gap}\n      the pin is `llvm_pin_version` in the justfile.", file=sys.stderr)
        for gap in system:
            print(f"  {gap}\n      README.md's list is the one a developer is told to run.",
                  file=sys.stderr)
        for gap in cancellation:
            print(f"  {gap}\n      measured: 65 of this repository's first 66 runs were "
                  "cancelled, none ever succeeded.", file=sys.stderr)
        return 1

    print(
        f"check-workflows: clean — {len(workflows)} workflow(s), {total} command(s), "
        "every one a recipe or a tool install, every permanent gate run, every closed "
        "milestone's criteria evaluated, every Linux job given the documented system "
        "libraries, and no long workflow cancelling its own runs on the trunk"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
