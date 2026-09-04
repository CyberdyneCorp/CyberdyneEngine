#!/usr/bin/env python3
"""Negative tests for the project graph.

Task 4.4. Each fixture under `fixtures/` is a project the graph validation **must** reject, plus one
it must accept. A fixture that stops failing means the check stopped firing — fix the check; the
fixture is the evidence, not the problem.

Two rejections have the milestone's name on them and are listed first: a project with a **cycle**,
and a project with an **undeclared dependency**. The remaining fixtures are the other rejections the
specification requires, kept here rather than in prose so that each one is a program's exit status.

A rejection must also *say* what it rejected. Every case names the substrings the diagnostic has to
contain — a message that named neither end of the collision would satisfy "the build fails" and fail
the requirement, which asks for the build to fail *naming both modules*.

The last case is not a rejection at all: `emit-header` must be byte-reproducible across runs and
across output directories, because cy_project.h is committed to nothing and compared to everything.

Run directly, or as the `integration.project_graph` test.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIXTURES = HERE / "fixtures"
PROJECT = HERE / "project.py"
ENGINE_VERSION = "0.0.0"


@dataclass(frozen=True)
class Case:
    fixture: str
    must_pass: bool
    expect: tuple[str, ...] = ()
    why: str = ""


CASES = (
    Case("cycle", False,
         why="three modules depend on one another in a ring",
         expect=("dependency cycle", "fx-alpha -> fx-beta -> fx-gamma -> fx-alpha",
                 "no initialisation order")),
    Case("undeclared-dependency", False,
         why="a module includes a header owned by a module it does not declare",
         expect=("modules/fx-scene/src/world.cpp", "'fx-scene'", "'fx-core'",
                 "fx/core/api.h", "declared dependencies")),
    Case("missing-module", False,
         why="a declared dependency names a module the project does not contain",
         expect=("'fx-scene'", "'fx-physics'", "not a module in this project")),
    Case("layer-violation", False,
         why="a core module depends on a scene module",
         expect=("layering violation", "'fx-core'", "layer core, 0", "'fx-scene'",
                 "layer scene, 4", "never upward")),
    Case("private-leak", False,
         why="a public header reaches a privately declared dependency",
         expect=("modules/fx-scene/include/fx/scene/world.h", "'fx-core'",
                 "private dependency", "not transitively exposed")),
    Case("editor-in-shipping", False,
         why="a shipping target reaches an editor module through the graph",
         expect=("shipping target 'client'", "'fx-editor'", "editor",
                 "not by a build flag")),
    Case("unknown-key", False,
         why="a mistyped manifest key is reported, not ignored",
         expect=("hot_relaod", "unknown key", "hot_reload")),
    Case("incompatible-plugin", False,
         why="a plugin declares an engine API range that excludes this engine",
         expect=("com.example.legacy", ">=9.0.0", "incompatible")),
    Case("malformed-json", False,
         why="a manifest that is not JSON reports where it stopped parsing",
         expect=("project.json:5", "not valid JSON")),
    Case("valid", True,
         why="every construct the schema allows, in a project that passes"),
)


@dataclass
class Result:
    name: str
    ok: bool
    detail: str = ""
    output: str = field(default="", repr=False)


def run(*arguments: str) -> tuple[int, str]:
    done = subprocess.run([sys.executable, str(PROJECT), *arguments],
                          capture_output=True, text=True, check=False)
    return done.returncode, done.stdout + done.stderr


def check_fixture(case: Case) -> Result:
    manifest = FIXTURES / case.fixture / "project.json"
    code, output = run("validate", "--manifest", str(manifest),
                       "--engine-version", ENGINE_VERSION)
    passed = code == 0
    if passed != case.must_pass:
        wanted = "accepted" if case.must_pass else "rejected"
        return Result(case.fixture, False,
                      f"must be {wanted} ({case.why}) but exited {code}", output)
    missing = [text for text in case.expect if text not in output]
    if missing:
        return Result(case.fixture, False,
                      "the diagnostic does not name: " + ", ".join(repr(t) for t in missing),
                      output)
    return Result(case.fixture, True, case.why, output)


def check_reproducible() -> Result:
    """The same manifest renders the same header, twice, into two different directories."""
    manifest = FIXTURES / "valid" / "project.json"
    outputs = []
    with tempfile.TemporaryDirectory(prefix="cy-project-") as first, \
         tempfile.TemporaryDirectory(prefix="cy-project-") as second:
        for directory in (first, second, first):
            target = Path(directory) / "generated" / "cy_project.h"
            code, output = run("emit-header", "--manifest", str(manifest),
                               "--engine-version", ENGINE_VERSION,
                               "--platform", "linux", "--output", str(target))
            if code != 0:
                return Result("emit-header", False, f"exited {code}", output)
            outputs.append(target.read_text(encoding="utf-8"))
        if outputs[0] != outputs[1]:
            return Result("emit-header", False,
                          "two build directories rendered different headers")
        if outputs[1] != outputs[2]:
            return Result("emit-header", False, "a second run rendered a different header")
        if "CY_PROJECT_MODULE_TABLE" not in outputs[0]:
            return Result("emit-header", False, "the header carries no module table")
    return Result("emit-header", True, "byte-identical across runs and build directories")


def check_platform_override() -> Result:
    """`platform_overrides` reaches the generated header: the same manifest renders a different
    setting, and a different module set, for a different platform."""
    manifest = FIXTURES / "valid" / "project.json"
    rendered: dict[str, str] = {}
    with tempfile.TemporaryDirectory(prefix="cy-project-") as directory:
        for platform in ("linux", "android"):
            target = Path(directory) / platform / "cy_project.h"
            code, output = run("emit-header", "--manifest", str(manifest),
                               "--engine-version", ENGINE_VERSION,
                               "--platform", platform, "--output", str(target))
            if code != 0:
                return Result("platform-override", False, f"exited {code}", output)
            rendered[platform] = target.read_text(encoding="utf-8")
    if '"renderer.profile", "mobile"' not in rendered["android"]:
        return Result("platform-override", False,
                      "the android header does not carry the platform layer's renderer.profile",
                      rendered["android"])
    if '"renderer.profile", "mobile"' in rendered["linux"]:
        return Result("platform-override", False,
                      "the linux header carries android's override", rendered["linux"])
    if '"fx-tools"' in rendered["android"]:
        return Result("platform-override", False,
                      "the android header carries a module the override disables",
                      rendered["android"])
    return Result("platform-override", True, "the platform layer overrides with no code change")


def configure(fixture: str) -> tuple[int, str]:
    """Configure a fixture in a throwaway build tree. Fixtures are configured, never built: the
    check under test runs at configure time and the sources exist only because CMake requires a
    target to have some."""
    with tempfile.TemporaryDirectory(prefix="cy-project-cmake-") as build:
        done = subprocess.run(
            ["cmake", "-S", str(FIXTURES / fixture), "-B", build, "--no-warn-unused-cli"],
            capture_output=True, text=True, check=False)
    return done.returncode, done.stdout + done.stderr


def check_configure(case: Case) -> Result:
    if shutil.which("cmake") is None:
        return Result(case.fixture, True, "skipped: cmake is not on PATH")
    code, output = configure(case.fixture)
    passed = code == 0
    if passed != case.must_pass:
        wanted = "configure" if case.must_pass else "fail to configure"
        return Result(case.fixture, False, f"must {wanted} ({case.why}) but exited {code}", output)
    missing = [text for text in case.expect if text not in output]
    if missing:
        return Result(case.fixture, False,
                      "the diagnostic does not name: " + ", ".join(repr(t) for t in missing),
                      output)
    return Result(case.fixture, True, case.why, output)


# Task 4.2's other half, and the only one that needs a real CMake run: cy_add_module() checks each
# link it is handed, and cy_project_check_target_graph() checks what will actually be linked. The
# violation fixture wires the two targets together with a bare target_link_libraries(), which the
# per-link check never sees.
CONFIGURE_CASES = (
    Case("target-graph-violation", False,
         why="a layer 0 target reaches a layer 4 target through a link cy_add_module never saw",
         expect=("Layering violation in the project graph", "'fx_core'", "layer 0",
                 "'fx_scene'", "layer 4", "transitive link")),  # CMake wraps the message
    Case("target-graph-legal", True,
         why="the same two targets, wired downward"),
)


def check_describe() -> Result:
    """`describe` reports registration order, which is what the runtime initialises in."""
    manifest = FIXTURES / "valid" / "project.json"
    code, output = run("describe", "--manifest", str(manifest),
                       "--engine-version", ENGINE_VERSION)
    if code != 0:
        return Result("describe", False, f"exited {code}", output)
    # "  <level> <layer> <name>[ -> deps]": the third field is the module.
    order = [line.split()[2] for line in output.splitlines()
             if line.startswith("  ") and "fx-" in line]
    expected = ["fx-core", "fx-scene", "fx-editor", "fx-tools"]
    if order != expected:
        return Result("describe", False, f"registration order is {order}, expected {expected}",
                      output)
    return Result("describe", True, "registration level, then name")


def main() -> int:
    results = [check_fixture(case) for case in CASES]
    results += [check_configure(case) for case in CONFIGURE_CASES]
    results += [check_reproducible(), check_platform_override(), check_describe()]

    width = max(len(result.name) for result in results)
    for result in results:
        mark = "ok  " if result.ok else "FAIL"
        print(f"{mark} {result.name:<{width}}  {result.detail}")
        if not result.ok and result.output:
            for line in result.output.splitlines():
                print(f"       | {line}")

    failed = [result for result in results if not result.ok]
    print(f"\n{len(results) - len(failed)} of {len(results)} project-graph checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
