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


def gate_coverage(root: pathlib.Path, workflows: list[pathlib.Path]) -> list[str]:
    """Permanent gates whose commands no workflow runs."""
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

    total = len(SELFTEST_CASES) + len(SELFTEST_LEGAL) + 3
    if failed:
        print(f"check-workflows selftest: {failed} of {total} cases failed", file=sys.stderr)
        return 1
    print(f"check-workflows selftest: {total}/{total} passed")
    return 0


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

    if violations or uncovered or drift:
        print("check-workflows: the workflows and the recipes disagree", file=sys.stderr)
        for violation in violations:
            print(violation.render(root), file=sys.stderr)
        for gap in uncovered:
            print(f"  {gap}\n      `just roadmap-gates` prints the declared set.", file=sys.stderr)
        for gap in drift:
            print(f"  {gap}\n      the pin is `llvm_pin_version` in the justfile.", file=sys.stderr)
        return 1

    print(
        f"check-workflows: clean — {len(workflows)} workflow(s), {total} command(s), "
        "every one a recipe or a tool install, every permanent gate run"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
