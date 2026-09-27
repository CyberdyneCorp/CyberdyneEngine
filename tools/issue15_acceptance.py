#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Run the executable parts of issue #15's acceptance ledger.

The runner treats a native test that only checked device availability as unverified.
Requirements without an executable proof stay open even when their neighbouring tests pass.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[1]
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
    return (f"build/dev/{binary}", f"--test-case={case}", "--no-skip")


CRITERIA = (
    Criterion(
        "palette", "VFX graph opens and matches the compiler registry",
        (
            Probe(
                "shared canvas",
                cargo(
                    "cy-editor-interface",
                    "vfx_editor_opens_on_the_shared_canvas_from_backend_nodes",
                ),
            ),
            Probe(
                "compiler parity",
                native(
                    "cy_test_unit_editor_backend",
                    "editor_backend: VFX palette equals the compiler registry",
                ),
                100,
            ),
        ),
        "Omit the last registered node from the engine catalogue; compiler parity fails.",
    ),
    Criterion(
        "sample", "Two CPU/GPU emitters save, reopen, cook and render a reference image",
        (
            Probe(
                "MCP two-emitter authoring",
                cargo(
                    "cy-editor-mcp",
                    "vfx_system_is_created_with_two_emitters_and_reopened_over_mcp",
                ),
            ),
            Probe(
                "exact sample authored over MCP",
                cargo(
                    "cy-editor-mcp",
                    "committed_two_emitter_sample_can_be_authored_through_mcp_commands",
                ),
            ),
            Probe(
                "committed sample reopens",
                cargo(
                    "cy-editor-interface",
                    "committed_two_emitter_sample_reopens_without_losing_stage_graphs",
                ),
            ),
            Probe(
                "engine cook",
                native("cy_test_integration_vfx", "the editor's two-emitter VFX draft cooks*"),
                5,
            ),
            Probe(
                "Metal reference image",
                native(
                    "cy_test_smoke_editor_authored_frame_metal",
                    "authored Metal viewport composites the engine VFX preview",
                ),
                5,
            ),
        ),
        "Mutate a reference texel away from an image edge; the golden comparison fails.",
    ),
    Criterion(
        "recook", "Live parameter edits avoid a recook; graph edits request one",
        (
            Probe(
                "engine live parameter",
                native(
                    "cy_test_integration_editor_backend_compile",
                    "editor_backend: VFX preview controls and live parameters use the engine world",
                ),
                10,
            ),
            Probe(
                "compile signature",
                cargo(
                    "cy-editor-interface",
                    "compile_signature_ignores_layout_and_live_values_but_tracks_graph_edits",
                ),
            ),
            Probe(
                "desktop submission",
                cargo("cy-editor-shell", "graph_edits_submit_a_new_cook_but_live_values_do_not"),
            ),
        ),
        "Fold the exposed live value into the signature; the signature test fails.",
    ),
    Criterion(
        "diagnostic", "A VFX error names and selects its offending node",
        (
            Probe(
                "compiler location",
                native(
                    "cy_test_unit_editor_backend",
                    "editor_backend: VFX compiler diagnostics name the authored node",
                ),
                10,
            ),
            Probe(
                "desktop navigation",
                cargo(
                    "cy-editor-shell",
                    "diagnostic_navigation_opens_the_scoped_stage_and_selects_its_node",
                ),
            ),
        ),
        "Encode node zero instead of the compiler's node; the location test fails.",
    ),
    Criterion(
        "displacement", "Vertex displacement, shadow and motion match CPU geometry",
        (
            Probe(
                "committed sine sway scene",
                native(
                    "cy_test_smoke_editor_authored_frame_metal",
                    "committed sine sway material cooks and renders in its authored scene",
                ),
                10,
            ),
            Probe(
                "native pixel comparison",
                native(
                    "cy_test_smoke_editor_authored_frame_metal",
                    "authored native frame renders a mesh and publishes its transformed bounds",
                ),
                10,
            ),
        ),
        gap="Run the native pixel comparison and record a mutation that makes its shadow or "
        "motion comparison fail.",
    ),
    Criterion(
        "unsupported", "Editor and cook refuse an assigned unsupported vertex path",
        (
            Probe(
                "engine author refusal",
                native(
                    "cy_test_unit_editor_backend",
                    "editor_backend: material authoring refuses unsupported assigned geometry",
                ),
                10,
            ),
            Probe(
                "material cook refusal",
                native(
                    "cy_test_integration_material_cook",
                    "material_cook: virtual geometry refuses a vertex offset without an artefact",
                ),
                1,
            ),
        ),
        "Allow VirtualGeometry in the compiler's path check; its named refusal test fails.",
        "Discover VirtualGeometry assignments from real scene or build assets and prove the "
        "editor forwards them.",
    ),
    Criterion(
        "history", "Both editors support undo/redo and MCP command parity",
        (
            Probe(
                "desktop VFX creation history",
                cargo(
                    "cy-editor-shell",
                    "desktop_vfx_creation_uses_saved_history_and_refuses_overwrite",
                ),
            ),
            Probe(
                "desktop VFX gesture history",
                cargo(
                    "cy-editor-shell",
                    "saved_vfx_canvas_edit_is_journaled_and_undoable_without_manual_save",
                ),
            ),
            Probe(
                "desktop VFX module history",
                cargo(
                    "cy-editor-shell",
                    "saved_vfx_module_edits_are_journaled_once_per_changed_frame",
                ),
            ),
            Probe(
                "VFX wire edits",
                cargo("cy-editor-mcp", "vfx_stage_wire_and_property_round_trip_over_mcp"),
            ),
            Probe(
                "VFX module wire edits",
                cargo("cy-editor-mcp", "vfx_module_canvas_round_trips_over_mcp"),
            ),
            Probe(
                "desktop material history",
                cargo(
                    "cy-editor-shell",
                    "desktop_material_save_and_mcp_share_one_undoable_transaction",
                ),
            ),
            Probe(
                "material wire edits",
                cargo(
                    "cy-editor-mcp",
                    "material_node_edits_round_trip_as_individual_mcp_transactions",
                ),
            ),
            Probe(
                "material draft history",
                cargo("cy-editor-mcp", "material_draft_gestures_undo_before_canonical_authoring"),
            ),
        ),
        "Remove node-connect command registration; the corresponding MCP edit test fails.",
    ),
    Criterion(
        "regressions", "Each discovered bug has a failing regression before its fix",
        (),
        gap="Audit bug fixes in PR #17 against the red-mutation entries in verification.md.",
    ),
    Criterion(
        "docs", "Documentation and OpenSpec reflect the finished feature",
        (
            Probe(
                "strict OpenSpec",
                ("openspec", "validate", "implement-issue-15-graph-authoring", "--strict"),
            ),
        ),
        gap="Update final editor and sample instructions when remaining vertex work is complete.",
    ),
)


def probe_result(probe: Probe, result: subprocess.CompletedProcess[str]) -> tuple[bool, str]:
    if result.returncode != 0:
        return False, f"exit {result.returncode}"
    output = result.stdout + result.stderr
    if probe.command[0] == "cargo" and sum(
        int(match.group(1)) for match in CARGO_PASSED.finditer(output)
    ) == 0:
        return False, "Cargo filter selected no tests"
    if probe.min_assertions:
        cases = CASES.search(output)
        assertions = ASSERTIONS.search(output)
        if cases is None or int(cases.group(1)) != 1 or assertions is None:
            return False, "selected native case did not report one executed test"
        count = int(assertions.group(1))
        if count < probe.min_assertions:
            return False, f"only {count} assertions; needs {probe.min_assertions}"
    return True, "passed"


def run_criterion(criterion: Criterion) -> bool:
    print(f"{criterion.key}: {criterion.name}")
    passed = True
    for probe in criterion.probes:
        try:
            result = subprocess.run(
                probe.command, cwd=ROOT, text=True, capture_output=True, check=False
            )
        except OSError as error:
            print(f"  UNVERIFIED {probe.name}: {error}")
            passed = False
            continue
        valid, reason = probe_result(probe, result)
        print(f"  {'PASS' if valid else 'UNVERIFIED'} {probe.name}: {reason}")
        if not valid:
            output = (result.stdout + result.stderr).strip()
            if output:
                print("    " + output[-500:].replace("\n", "\n    "))
            passed = False
    if criterion.red_mutation is None:
        print("  OPEN no recorded red mutation")
        passed = False
    if criterion.gap:
        print(f"  OPEN {criterion.gap}")
        passed = False
    if not criterion.probes:
        passed = False
    return passed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--criterion", choices=[item.key for item in CRITERIA], action="append")
    parser.add_argument(
        "--list", action="store_true", help="show the criteria and probes without running them"
    )
    args = parser.parse_args()
    selected = [item for item in CRITERIA if not args.criterion or item.key in args.criterion]
    if args.list:
        for item in selected:
            print(f"{item.key}: {item.name}")
            for probe in item.probes:
                print("  " + " ".join(probe.command))
            if item.gap:
                print(f"  OPEN {item.gap}")
        return 0
    results = [run_criterion(item) for item in selected]
    print(f"verified {sum(results)}/{len(results)} selected criteria")
    return 0 if all(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
