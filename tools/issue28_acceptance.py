#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the executable parts of issue #28's acceptance ledger.

Each acceptance criterion of the navigation authoring editor maps to probes: native doctest cases
run with `--no-skip` and an assertion floor, Cargo tests that must execute at least one test, and
the OpenSpec and documentation checks. A criterion is verified only when every probe passes and a
red mutation for it is recorded in the change's verification.md.

Before any native probe runs, the runner builds the probed test binaries in `build/dev`, so a probe
never reports on a binary older than its sources; a build failure fails every native probe.
`--no-build` skips that step for a tree the caller has just built. `--native-only` runs only the
native probes, which is what CI's test job runs after `just test-all` on a tree it has built.

`--check-docs` runs only the documentation check, which needs no build.
"""

from __future__ import annotations

import argparse
import os
from dataclasses import dataclass
from pathlib import Path
import re
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
CHANGE = "implement-issue-28-navigation-authoring"
ASSERTIONS = re.compile(r"\[doctest\] assertions:\s*(\d+)\s*\|")
CASES = re.compile(r"\[doctest\] test cases:\s*(\d+)\s*\|")
CARGO_PASSED = re.compile(r"test result: ok\.\s*(\d+) passed;")


@dataclass(frozen=True)
class Probe:
    name: str
    command: tuple[str, ...]
    min_assertions: int = 0


@dataclass(frozen=True)
class Criterion:
    key: str
    name: str
    probes: tuple[Probe, ...]
    red_mutation: str | None = None
    gap: str | None = None


def cargo(package: str, test: str) -> tuple[str, ...]:
    return (
        "cargo", "test", "--manifest-path", "editor/Cargo.toml", "-p", package,
        test, "--quiet",
    )


def native(binary: str, case: str) -> tuple[str, ...]:
    # doctest splits a filter on commas, so a probed case name must not contain one.
    return (f"build/dev/{binary}", f"--test-case={case}", "--no-skip")


SERVICE = "cy_test_integration_editor_backend_navigation"
RUNTIME = "cy_test_integration_editor_window_navigation"

CRITERIA = (
    Criterion(
        "bake", "A bake through the editor equals build_tile, tile by tile",
        (
            Probe(
                "service bake parity",
                native(SERVICE, "editor_backend: navigation bake equals build_tile tile by tile"),
                1000,
            ),
            Probe(
                "runtime test-map parity",
                native(RUNTIME, "editor runtime: baking the test map equals build_tile tile by tile"),
                40,
            ),
            Probe(
                "incremental rebuild after a surface shrink equals a fresh bake",
                native(
                    RUNTIME,
                    "editor runtime: shrinking the surface leaves the mesh equal to a fresh bake "
                    "of the shrunk map",
                ),
                40,
            ),
            Probe(
                "a stale restore is rebuilt whole",
                native(
                    SERVICE,
                    "editor_backend: an update after restoring a stale bake rebuilds every "
                    "changed tile*",
                ),
                1000,
            ),
            Probe(
                "a height-range change is rebuilt whole",
                native(
                    SERVICE,
                    "editor_backend: an edit that changes the geometry's height range rebuilds "
                    "every tile",
                ),
                1000,
            ),
        ),
        "S1, R1a, R1b, F2, F3, F4: perturb cell_size in the service's settings decode, bake "
        "mesh-local positions in the runtime seam, rebuild the dirty box without the surface "
        "region, or trust the dirty box after a stale restore or a height change; the per-tile "
        "digest checks fail.",
    ),
    Criterion(
        "overlay", "The overlay's walkable area matches the mesh (image test)",
        (
            Probe(
                "covered pixels",
                native(RUNTIME, "nav overlay covers the projected walkable polygons"),
                1000,
            ),
            Probe(
                "per-world toggle",
                native(RUNTIME, "nav overlay per-world toggle draws only the enabled world"),
                10,
            ),
            Probe(
                "the known test map through the frame's overlay call",
                native(
                    RUNTIME,
                    "editor runtime: the frame overlay over the baked test map covers its "
                    "polygons in their area colours",
                ),
                10000,
            ),
            Probe(
                "a polygon behind the camera is clipped",
                native(
                    RUNTIME,
                    "nav overlay clips a polygon at the near plane with the camera inside the tile",
                ),
                5,
            ),
        ),
        "R2, R7, F8, F9, F10: the sink skips every second polygon, ignores the world's flags or "
        "drops a polygon with a corner behind the camera, the palette shifts, or the frame call "
        "loses the eye; the coverage, colour and toggle checks fail.",
    ),
    Criterion(
        "obstacle", "An obstacle placed through the editor blocks a path; removing it restores it",
        (
            Probe(
                "service block and restore",
                native(
                    SERVICE,
                    "editor_backend: an obstacle added through the service blocks the path and "
                    "removing it restores it",
                ),
                200,
            ),
            Probe(
                "MCP add and remove reach the engine",
                cargo("cy-editor-mcp", "navigation_obstacle_add_and_remove_reach_the_engine_over_mcp"),
            ),
            Probe(
                "a document NavObstacle blocks the runtime's path",
                native(
                    RUNTIME,
                    "editor runtime: a NavObstacle added to the document blocks the path; "
                    "removing it restores the path",
                ),
                40,
            ),
        ),
        "S2, E7, F6: the service drops the seam's obstacles, the remove command records nothing, "
        "or the runtime misreads the NavObstacle's radius field; the blocked-path and removal "
        "checks fail.",
    ),
    Criterion(
        "history", "Bake, settings and component edits undo, redo and match over MCP",
        (
            Probe(
                "MCP undo and redo",
                cargo(
                    "cy-editor-mcp",
                    "navigation_settings_component_and_bake_edits_undo_and_redo_over_mcp",
                ),
            ),
            Probe(
                "MCP projection",
                cargo("cy-editor-mcp", "navigation_tools_are_projected_over_mcp"),
            ),
            Probe(
                "one bake transaction",
                cargo(
                    "cy-editor-services",
                    "completed_records_exactly_one_bake_transaction_and_undo_restores_the_identity",
                ),
            ),
            Probe(
                "one settings transaction",
                cargo("cy-editor-services", "a_settings_edit_is_one_entry_and_undo_redo_restore_it"),
            ),
            Probe(
                "desktop and MCP histories agree",
                cargo(
                    "cy-editor-shell",
                    "panel_gestures_record_the_history_the_same_gestures_record_over_mcp",
                ),
            ),
            Probe(
                "undoing the first bake unbakes the engine",
                native(
                    RUNTIME,
                    "editor runtime: undoing the first bake drops the engine's mesh and refuses "
                    "path queries",
                ),
                40,
            ),
            Probe(
                "an undone or reopened bake keeps its area costs",
                native(
                    SERVICE,
                    "editor_backend: a restored bake keeps its area costs in a new session and "
                    "on undo",
                ),
                1000,
            ),
        ),
        "E1, E2, E8, E9, P10, F1, F5: record the bake on every pump or never, send default "
        "settings, split a gesture, keep the mesh after the first bake is undone, or restore a "
        "bake without its area costs; the history-length, payload, refusal and cost checks fail.",
    ),
    Criterion(
        "stale", "A stale bake is detected after a geometry edit",
        (
            Probe(
                "service stale status",
                native(
                    SERVICE,
                    "editor_backend: navigation status reports a stale bake after a source change",
                ),
                1000,
            ),
            Probe(
                "runtime mesh move",
                native(
                    RUNTIME,
                    "editor runtime: moving a mesh marks the navigation bake stale; moving it "
                    "back clears the flag",
                ),
                40,
            ),
            Probe(
                "a missing sidecar is reported apart from a stale bake",
                native(
                    SERVICE,
                    "editor_backend: navigation status restores a saved bake into a new session",
                ),
                1000,
            ),
        ),
        "S3, R3a, R3b, M8b, F7: status reuses the saved fingerprint, the fingerprint ignores the "
        "geometry, or a missing sidecar fails the status; the stale checks fail.",
    ),
    Criterion(
        "docs", "The OpenSpec change validates with --strict, and the docs describe the editor",
        (
            Probe("strict OpenSpec", ("openspec", "validate", CHANGE, "--strict")),
            Probe("documentation", (sys.executable, "tools/issue28_acceptance.py", "--check-docs")),
        ),
        "D1, D2: drop a requirement's scenario from a spec delta, or a guidance line from a "
        "README; strict validation or the docs check fails.",
    ),
)


REQUIRED_FILES = (
    f"openspec/changes/{CHANGE}/proposal.md",
    f"openspec/changes/{CHANGE}/design.md",
    f"openspec/changes/{CHANGE}/tasks.md",
    f"openspec/changes/{CHANGE}/verification.md",
    "samples/05b-editor-window/runtime/tests/data/nav_test_map.cyworld",
    "docs/guides/navigation.md",
)

REQUIRED_GUIDANCE = {
    "editor/README.md": (
        "## Navigation authoring commands (issue #28)",
        "### The Navigation panel",
        "| Navigation |",
        "navigation.bake.status",
        "navigation.{surface,obstacle,area,link}.add",
        "python3 tools/issue28_acceptance.py",
    ),
    "samples/05b-editor-window/README.md": (
        "## Navigation overlay and baked navmesh sidecar",
        "### Baking the navmesh in the editor",
        "integration.editor_window_navigation",
    ),
    "src/navigation/README.md": ("bake_codec.h",),
    "src/editor_backend/README.md": ("## Navigation operations (issue #28)",),
    "tools/build/README.md": ("### The `navmesh` producer (issue #28)",),
    "docs/guides/README.md": ("[Navigation authoring](navigation.md)",),
    "docs/guides/navigation.md": (
        "| Navigation | Implemented |",
        "navigation.bake",
        "navigation.path.query",
        "just run-editor-live",
    ),
}


def missing_docs(root: Path) -> list[str]:
    """Name every required file or guidance line that is absent under `root`."""
    missing = [path for path in REQUIRED_FILES if not (root / path).is_file()]
    for path, snippets in REQUIRED_GUIDANCE.items():
        file = root / path
        if not file.is_file():
            missing.append(path)
            continue
        contents = file.read_text(encoding="utf-8")
        missing.extend(f"{path}: {snippet}" for snippet in snippets if snippet not in contents)
    return missing


def native_result(probe: Probe, output: str) -> tuple[bool, str]:
    cases = CASES.search(output)
    assertions = ASSERTIONS.search(output)
    if cases is None or int(cases.group(1)) != 1 or assertions is None:
        return False, "selected native case did not report one executed test"
    count = int(assertions.group(1))
    if count < probe.min_assertions:
        return False, f"only {count} assertions; needs {probe.min_assertions}"
    return True, "passed"


def probe_result(probe: Probe, result: subprocess.CompletedProcess[str]) -> tuple[bool, str]:
    if result.returncode != 0:
        return False, f"exit {result.returncode}"
    output = result.stdout + result.stderr
    if probe.command[0] == "cargo":
        executed = sum(int(match.group(1)) for match in CARGO_PASSED.finditer(output))
        if executed == 0:
            return False, "Cargo filter selected no tests"
    if probe.min_assertions:
        return native_result(probe, output)
    return True, "passed"


def run_probe(probe: Probe) -> bool:
    try:
        result = subprocess.run(
            probe.command, cwd=ROOT, text=True, capture_output=True, check=False
        )
    except OSError as error:
        print(f"  UNVERIFIED {probe.name}: {error}")
        return False
    valid, reason = probe_result(probe, result)
    print(f"  {'PASS' if valid else 'UNVERIFIED'} {probe.name}: {reason}")
    if not valid:
        output = (result.stdout + result.stderr).strip()
        if output:
            print("    " + output[-500:].replace("\n", "\n    "))
    return valid


def is_native(probe: Probe) -> bool:
    return probe.command[0].startswith("build/dev/")


def native_targets(selected: list[Criterion]) -> list[str]:
    """The test binaries the selected criteria probe, each once, in order."""
    targets: list[str] = []
    for item in selected:
        for probe in item.probes:
            target = probe.command[0].removeprefix("build/dev/")
            if is_native(probe) and target not in targets:
                targets.append(target)
    return targets


def build_command(targets: list[str]) -> tuple[str, ...]:
    jobs = max(1, (os.cpu_count() or 4) - 2)
    command = ["cmake", "--build", "build/dev", "--parallel", str(jobs)]
    for target in targets:
        command += ["--target", target]
    return tuple(command)


def build_native(targets: list[str]) -> bool:
    """Builds the probed binaries, so no probe reports on a binary older than its sources."""
    if not targets:
        return True
    try:
        result = subprocess.run(
            build_command(targets), cwd=ROOT, text=True, capture_output=True, check=False
        )
    except OSError as error:
        print(f"build: {error}")
        return False
    if result.returncode != 0:
        output = (result.stdout + result.stderr).strip()
        print("build: FAILED; every native probe is unverified")
        if output:
            print("    " + output[-1500:].replace("\n", "\n    "))
        return False
    return True


def run_criterion(criterion: Criterion, built: bool = True, native_only: bool = False) -> bool:
    print(f"{criterion.key}: {criterion.name}")
    results = []
    for probe in criterion.probes:
        if native_only and not is_native(probe):
            continue
        if is_native(probe) and not built:
            print(f"  UNVERIFIED {probe.name}: the probed binary did not build")
            results.append(False)
            continue
        results.append(run_probe(probe))
    passed = all(results) and bool(results)
    if native_only:
        return passed
    if criterion.red_mutation is None:
        print("  OPEN no recorded red mutation")
        passed = False
    if criterion.gap:
        print(f"  OPEN {criterion.gap}")
        passed = False
    return passed


def list_criteria(selected: list[Criterion]) -> int:
    for item in selected:
        print(f"{item.key}: {item.name}")
        for probe in item.probes:
            print("  " + " ".join(probe.command))
        if item.gap:
            print(f"  OPEN {item.gap}")
    return 0


def check_docs() -> int:
    missing = missing_docs(ROOT)
    for entry in missing:
        print(f"missing: {entry}")
    print("issue #28 documentation: " + ("incomplete" if missing else "complete"))
    return 1 if missing else 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--criterion", choices=[item.key for item in CRITERIA], action="append")
    parser.add_argument(
        "--list", action="store_true", help="show the criteria and probes without running them"
    )
    parser.add_argument(
        "--check-docs", action="store_true", help="check only the documentation and exit"
    )
    parser.add_argument(
        "--no-build", action="store_true", help="run the native probes on the binaries as built"
    )
    parser.add_argument(
        "--native-only", action="store_true", help="run only the native doctest probes"
    )
    args = parser.parse_args()
    if args.check_docs:
        return check_docs()
    selected = [item for item in CRITERIA if not args.criterion or item.key in args.criterion]
    if args.list:
        return list_criteria(selected)
    built = args.no_build or build_native(native_targets(selected))
    if args.native_only:
        selected = [item for item in selected if any(is_native(p) for p in item.probes)]
    results = [run_criterion(item, built, args.native_only) for item in selected]
    verdict = "native probes passed for" if args.native_only else "verified"
    print(f"{verdict} {sum(results)}/{len(results)} selected criteria")
    return 0 if all(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
