#!/usr/bin/env python3
"""Task 1.6.5 — a disabled feature's dependency is not fetched, not built and not linked.

`thirdparty-dependencies` requires exclusion rather than a runtime stub, and the difference is only
observable at configure time: a stubbed dependency is still downloaded and still compiled. So this
test configures twice and looks at what the build system actually did.

    with CY_PROFILING off   Tracy is not declared, no source appears in the cache, and no
                            cy::dep::tracy target exists
    with CY_PROFILING on    all three of those become true

doctest under CY_BUILD_TESTS is checked the same way in the same pair of runs, since it costs
nothing extra and it is the other gated dependency the engine has.

The project configured here is a few lines that include cmake/dependencies.cmake and nothing else.
That is deliberate: the subject is the dependency logic, and a failure should not be able to come
from anywhere else in the engine tree.

    tools/deps/test_gating.py [--cache DIR] [--keep]

The first run downloads the ungated dependencies; later runs reuse the cache. The gated ones are
removed from the cache before the off run, so their absence afterwards means something.
"""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from manifest import REPO_ROOT, load

PROJECT = """\
cmake_minimum_required(VERSION 3.28)
project(cy_dependency_gating LANGUAGES C CXX)
list(APPEND CMAKE_MODULE_PATH "{cmake_dir}")
set(CY_DEPS_MANIFEST "{manifest}" CACHE FILEPATH "")
include(dependencies)

# What this configuration would link, as the build system sees it rather than as the log claims.
set(report "linked=${{CY_ENABLED_DEPENDENCIES}}\\n")
foreach(id IN LISTS CY_DEPENDENCIES)
    if(TARGET cy::dep::${{id}})
        string(APPEND report "target=${{id}}\\n")
    endif()
endforeach()
file(WRITE "{report}" "${{report}}")
"""


class Failure(Exception):
    """An assertion about the configured build that did not hold."""


def configure(project: Path, build: Path, cache: Path, report: Path, options: dict) -> str:
    command = [
        "cmake",
        "-S", str(project),
        "-B", str(build),
        "-G", "Ninja",
        f"-DCY_DEPS_CACHE={cache}",
        f"-DCY_GATING_REPORT={report}",
    ]
    command += [f"-D{name}={value}" for name, value in sorted(options.items())]
    finished = subprocess.run(command, capture_output=True, text=True, check=False)
    if finished.returncode != 0:
        raise Failure(f"configure failed with {' '.join(command)}\n{finished.stdout}\n{finished.stderr}")
    return finished.stdout


def read_report(report: Path) -> tuple[list[str], set[str]]:
    lines = report.read_text(encoding="utf-8").splitlines()
    linked = next(line[len("linked="):] for line in lines if line.startswith("linked="))
    targets = {line[len("target="):] for line in lines if line.startswith("target=")}
    return [name for name in linked.split(";") if name], targets


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)
    print(f"  ok  {message}")


def run(cache: Path, workspace: Path) -> None:
    dependencies = load()
    gated = {d.name: d.feature for d in dependencies if d.optional}
    ungated = [d.name for d in dependencies if not d.optional]
    if not gated:
        raise Failure("the manifest has no gated dependency, so there is nothing to test")

    # The gated dependencies must be absent from the cache before the off run, or their absence
    # afterwards would only mean that an earlier run had already downloaded them.
    for name in gated:
        for suffix in ("src", "build", "subbuild"):
            shutil.rmtree(cache / f"{name}-{suffix}", ignore_errors=True)

    report = workspace / "report.txt"
    project = workspace / "project"
    project.mkdir()
    (project / "CMakeLists.txt").write_text(
        PROJECT.format(cmake_dir=REPO_ROOT / "cmake", manifest=REPO_ROOT / "deps" / "manifest.toml",
                       report=report),
        encoding="utf-8",
    )

    print(f"configuring with {', '.join(f'{f}=OFF' for f in sorted(set(gated.values())))}")
    log = configure(project, workspace / "off", cache, report,
                    {feature: "OFF" for feature in gated.values()})
    linked, targets = read_report(report)

    for name, feature in sorted(gated.items()):
        expect(name not in linked, f"{name} is not linked when {feature} is off")
        expect(name not in targets, f"no cy::dep::{name} target exists when {feature} is off")
        expect(not (cache / f"{name}-src").exists(),
               f"{name} source was not fetched when {feature} is off")
        expect(f"dependency {name}: excluded" in log,
               f"the configure log says {name} was excluded, naming {feature}")
    for name in ungated:
        expect(name in linked, f"{name} is linked regardless, as an ungated dependency")

    print(f"configuring with {', '.join(f'{f}=ON' for f in sorted(set(gated.values())))}")
    configure(project, workspace / "on", cache, report,
              {feature: "ON" for feature in gated.values()})
    linked, targets = read_report(report)

    for name, feature in sorted(gated.items()):
        expect(name in linked, f"{name} is linked when {feature} is on")
        expect(name in targets, f"cy::dep::{name} exists when {feature} is on")
        expect((cache / f"{name}-src").exists(), f"{name} source was fetched when {feature} is on")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument(
        "--cache",
        type=Path,
        default=REPO_ROOT / "build" / "deps-gating-cache",
        help="FetchContent cache to reuse between runs (default: build/deps-gating-cache)",
    )
    parser.add_argument("--keep", action="store_true", help="keep the temporary build trees")
    arguments = parser.parse_args(argv)
    arguments.cache.mkdir(parents=True, exist_ok=True)

    workspace = Path(tempfile.mkdtemp(prefix="cy-gating-"))
    try:
        run(arguments.cache, workspace)
    except Failure as failure:
        print(f"\nFAIL: {failure}", file=sys.stderr)
        return 1
    finally:
        if arguments.keep:
            print(f"\nbuild trees kept in {workspace}")
        else:
            shutil.rmtree(workspace, ignore_errors=True)

    print("\nPASS: a disabled feature fetches, builds and links nothing")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
