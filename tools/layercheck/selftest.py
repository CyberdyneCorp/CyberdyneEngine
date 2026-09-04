#!/usr/bin/env python3
"""Negative tests for the layer enforcement.

Task 1.3.4. Each fixture under fixtures/ is a tree the enforcement must reject; `legal/` is the one
it must accept. This runs them and asserts the outcome, including that a rejection *says* what it
rejected — a message that names neither target is a failure the specification calls out by name
("the build SHALL fail naming both modules and their layers").

A fixture that stops failing means the enforcement stopped firing. Fix the enforcement; the fixture
is the evidence, not the problem.

Run directly, or through `just quality-layers`, which runs it after checking the tree.
"""

from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIXTURES = HERE / "fixtures"
LAYERCHECK = HERE / "layercheck.py"


@dataclass(frozen=True)
class Case:
    fixture: str
    kind: str                       # "configure" or "check"
    must_pass: bool
    expect: tuple[str, ...] = ()    # substrings the diagnostic must contain
    checks: tuple[str, ...] = ()    # --check arguments, for kind "check"
    why: str = ""


CASES = (
    Case("upward-link", "configure", False, why="a layer 0 target links a layer 4 target",
         expect=("Layering violation", "'fx_core'", "layer 0", "'fx_scene'", "layer 4")),
    Case("upward-link-deferred", "configure", False,
         why="the same link, with the layer 4 target declared after it",
         expect=("Layering violation", "'fx_core'", "layer 0", "'fx_scene'", "layer 4")),
    Case("upward-include", "check", False, checks=("includes",),
         why="a file under src/core/ includes a header from src/scene/",
         expect=("src/core/allocator.cpp", "includes:", "layer 0", "scene/node.h", "layer 4")),
    Case("sdl-above-platform", "check", False, checks=("sdl",),
         why="a file outside platform/ includes <SDL3/SDL.h>",
         expect=("src/runtime/host.cpp", "sdl:", "SDL3/SDL.h", "platform/")),
    Case("gpuapi-above-backends", "check", False, checks=("gpuapi",),
         why="a file above src/backends/ includes <vulkan/vulkan.h>",
         expect=("src/rendering/graph/pass.cpp", "gpuapi:", "vulkan/vulkan.h", "src/backends/")),
    Case("barrier-outside-graph", "check", False, checks=("barriers",),
         why="a render pass emits its own barrier instead of declaring a resource use",
         expect=("src/rendering/passes/depth_prepass.cpp", "barriers:", "record_barriers",
                 "src/rendering/graph/")),
    Case("bare-target", "check", False, checks=("targets",),
         why="a target is declared without cy_add_module()",
         expect=("src/core/CMakeLists.txt", "targets:", "add_library", "cy_add_module")),
    Case("legal", "configure", True, why="downward links only"),
    Case("legal", "check", True,
         why="downward includes, SDL under platform/, no graphics API above it, no barrier outside "
             "the graph, no bare targets"),
)


@dataclass
class Result:
    case: Case
    ok: bool
    detail: str = ""
    output: str = field(default="", repr=False)


def configure(fixture: Path) -> tuple[int, str]:
    """Configure a fixture in a throwaway build tree. Fixtures are never built."""
    with tempfile.TemporaryDirectory(prefix="cy-layercheck-") as build:
        done = subprocess.run(
            ["cmake", "-S", str(fixture), "-B", build, "--no-warn-unused-cli"],
            capture_output=True, text=True, check=False)
    return done.returncode, done.stdout + done.stderr


def check(fixture: Path, checks: tuple[str, ...]) -> tuple[int, str]:
    argv = [sys.executable, str(LAYERCHECK), "--root", str(fixture)]
    for name in checks:
        argv += ["--check", name]
    done = subprocess.run(argv, capture_output=True, text=True, check=False)
    return done.returncode, done.stdout + done.stderr


def run_case(case: Case) -> Result:
    fixture = FIXTURES / case.fixture
    if not fixture.is_dir():
        return Result(case, False, f"no such fixture: {fixture}")

    if case.kind == "configure":
        code, output = configure(fixture)
    else:
        code, output = check(fixture, case.checks)

    if case.must_pass and code != 0:
        return Result(case, False, f"expected success, exited {code}", output)
    if not case.must_pass and code == 0:
        return Result(case, False, "expected failure, but it passed — the check did not fire",
                      output)

    missing = [text for text in case.expect if text not in output]
    if missing:
        return Result(case, False, f"diagnostic does not mention: {', '.join(missing)}", output)
    return Result(case, True, output=output)


def report(result: Result) -> None:
    case = result.case
    expectation = "passes" if case.must_pass else "fails"
    status = "ok  " if result.ok else "FAIL"
    print(f"{status} {case.fixture} [{case.kind}] {expectation}: {case.why}")
    if result.ok:
        return
    print(f"     {result.detail}", file=sys.stderr)
    for line in result.output.strip().splitlines():
        print(f"     | {line}", file=sys.stderr)


def main() -> int:
    if not shutil.which("cmake"):
        print("selftest: cmake is not on PATH", file=sys.stderr)
        return 2

    started = time.monotonic()
    results = [run_case(case) for case in CASES]
    for result in results:
        report(result)

    failed = [result for result in results if not result.ok]
    elapsed = time.monotonic() - started
    print(f"\nselftest: {len(results) - len(failed)}/{len(results)} passed ({elapsed:.1f} s)")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
