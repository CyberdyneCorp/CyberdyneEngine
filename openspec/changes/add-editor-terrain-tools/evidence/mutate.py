#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mutation driver for the terrain tools (#29, Terrain).

Each mutation replaces one exact snippet (which must occur once), rebuilds and runs the named tests,
records which failed, restores the file and verifies its md5. Rust mutations run `cargo test` through
the tree's job pool and target directory, as `just build-editor-check` does. C++ mutations rebuild the
named CTest suite's target in build/dev and run it. A mutation none of its tests notices is written
as SURVIVED and makes the driver exit non-zero. Output: falsification.txt beside this file.

Run: python3 openspec/changes/add-editor-terrain-tools/evidence/mutate.py [name-prefix ...]
Needs a configured `build/dev` (just build-engine). Honour the machine's job pool: check
`pgrep -f "[r]oadmap.py milestone"` first.
"""
import hashlib
import os
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[3]
OUT = HERE / "falsification.txt"
BUILD = ROOT / "build/dev"

PANEL = "editor/crates/cy-editor-shell/src/panels/terrain.rs"
COMMANDS = "editor/crates/cy-editor-services/src/terrain.rs"
ENGINE_VIEW = "editor/crates/cy-editor-services/src/terrain_engine.rs"
EDITOR = "editor/crates/cy-editor-services/src/editor.rs"
STACK = "src/terrain/src/stack.cpp"
MATERIAL = "src/terrain/src/material.cpp"
REGION = "src/terrain/src/region.cpp"
SERVICE = "src/editor_backend/src/terrain_service.cpp"
FRAME = "samples/05b-editor-window/runtime/authored_frame.cpp"

SHELL_LIB = ("rust", "cy-editor-shell", ["--lib"])
PANELS = ("rust", "cy-editor-shell", ["--test", "new_panels_are_accessible"])
SERVICES = ("rust", "cy-editor-services", ["--lib"])
MCP = ("rust", "cy-editor-mcp", ["--test", "a_session_over_the_wire"])
AUTHORING = ("cpp", "cy_test_integration_terrain_authoring", "integration.terrain_authoring")
BACKEND = ("cpp", "cy_test_integration_editor_backend_terrain",
           "integration.editor_backend_terrain")
VIEWPORT = ("cpp", "cy_test_smoke_editor_authored_frame_vulkan",
            "smoke.editor_authored_frame_vulkan")

# (name, file, before, after, suite, test filter for Rust)
MUTATIONS = [
    ("r01_panel_sends_layer_with_sculpt", PANEL,
     'if inputs.terrain_tool == "paint"\n', "if true\n",
     PANELS, "a_sculpt_stroke_with_a_layer_in_the_stack_names_no_layer_and_is_accepted"),
    ("r02_engine_resends_identical_stack", ENGINE_VIEW,
     "|| self.sent.as_deref() == Some(payload.as_slice())", "|| false",
     SERVICES, "each_changed_stack_is_sent_once"),
    ("r03_engine_sends_while_in_flight", ENGINE_VIEW,
     "if self.request.is_some() || self.sent", "if self.sent",
     SERVICES, "each_changed_stack_is_sent_once"),
    ("r04_paint_layer_off_by_one", ENGINE_VIEW,
     "u8::try_from(order + 1)", "u8::try_from(order)",
     MCP, "terrain_paint_over_mcp_requires_a_layer_and_a_hole_refuses_one"),
    ("r05_disabled_modifier_sent_enabled", ENGINE_VIEW,
     "writer.u8(u8::from(modifier.enabled));", "writer.u8(1);",
     SERVICES, "disabled_and_moved_modifiers_travel_in_stack_order"),
    ("r06_hole_requires_a_layer", COMMANDS,
     '    if tool != "paint" {\n        if text.is_empty() {',
     '    if tool != "paint" && tool != "hole" {\n        if text.is_empty() {',
     SERVICES, "a_hole_takes_no_layer_and_paint_requires_one"),
    ("r07_agent_brush_ignores_radius", COMMANDS,
     'radius: float("radius", defaults.radius),', "radius: defaults.radius,",
     SERVICES, "the_agent_brush_and_the_panel_stroke_record_the_same_modifier"),
    ("r08_pump_never_sends_terrain", EDITOR,
     "let wanted = self.edited_terrain_request();",
     "let wanted = self.edited_terrain_request().filter(|_| false);",
     MCP, "terrain_brushes_are_evaluated_by_the_engine_and_undo_over_mcp"),
    ("r09_heights_digest_blind", ENGINE_VIEW,
     "fnv(self.heights.iter().flat_map(|height| height.to_le_bytes()))",
     "fnv(self.heights.iter().take(0).flat_map(|height| height.to_le_bytes()))",
     SERVICES, "digests_distinguish_heights_and_weights_separately"),
    ("r10_reply_trailing_bytes_accepted", ENGINE_VIEW,
     "        if !reader.is_empty() {\n            return Err(Problem::new(\n"
     '                "read the engine\'s terrain",',
     "        if false {\n            return Err(Problem::new(\n"
     '                "read the engine\'s terrain",',
     SERVICES, "a_truncated_or_padded_reply_is_refused"),
    ("r11_panel_draws_holes_solid", PANEL,
     "if evaluation.holes[at] != 0 {", "if evaluation.holes[at] == 2 {",
     SHELL_LIB, "the_engine_surface_is_transparent_where_a_hole_is_cut"),
    ("r12_panel_hides_stale_navigation", PANEL,
     "if evaluation.stale.is_empty() {", "if true {",
     SHELL_LIB, "the_status_line_says_what_the_engine_evaluated_and_what_is_stale"),
    ("r13_panel_drops_the_hole_tool", PANEL,
     '"Flatten", "Paint", "Hole"];', '"Flatten", "Paint", "Cut"];',
     PANELS, "the_terrain_brush_offers_every_tool_the_engine_applies"),
    ("r14_engine_answer_discarded", ENGINE_VIEW,
     "                self.evaluation = Some(evaluation);\n",
     "                let _ = evaluation;\n",
     MCP, "terrain_brushes_are_evaluated_by_the_engine_and_undo_over_mcp"),
    ("c01_brush_reaches_past_its_radius", STACK,
     "brush_falloff(distance, modifier.radius, modifier.falloff)",
     "brush_falloff(distance, modifier.radius * 1.25F, modifier.falloff)",
     AUTHORING, None),
    ("c02_raise_digs", STACK,
     "return current + (kBrushReliefMetres * weight);",
     "return current - (kBrushReliefMetres * weight);",
     AUTHORING, None),
    ("c03_smooth_ignores_its_footprint", STACK,
     "const f32 weight = strength * brush_weight(modifier, sample_x(coord, grid, i), z);",
     "const f32 weight = strength + (0.0F * static_cast<f32>(z));",
     AUTHORING, None),
    ("c04_flatten_target_ignored", SERVICE,
     "decoded.modifier.height = target.value();",
     "decoded.modifier.height = 0.0F * target.value();",
     BACKEND, None),
    ("c05_paint_keeps_the_layers_beneath", MATERIAL,
     "(static_cast<f32>(texel.weight[slot]) / 255.0F) *\n                                           (1.0F - painted)};",
     "(static_cast<f32>(texel.weight[slot]) / 255.0F)};",
     AUTHORING, None),
    ("c06_hole_brush_cuts_nothing", STACK,
     "tile.set_hole(i, j, true);\n            } else {",
     "tile.set_hole(i, j, false);\n            } else {",
     AUTHORING, None),
    ("c07_region_forgets_open_quads", REGION,
     "out.rendered_hole_quads += report.hole_quads;",
     "out.rendered_hole_quads += 0U * report.hole_quads;",
     AUTHORING, None),
    ("c08_stale_never_marked", SERVICE,
     "    if (same_terrain) {\n        if (Status marked",
     "    if (false && same_terrain) {\n        if (Status marked",
     BACKEND, None),
    ("c09_removal_not_marked", SERVICE,
     "        if (differs(was, current)) {",
     "        if (false && differs(was, current)) {",
     BACKEND, None),
    ("c10_stale_regions_duplicated", SERVICE,
     "            known = known || (existing.min_x",
     "            known = false && (existing.min_x",
     BACKEND, None),
    ("c11_stale_region_not_clipped", SERVICE,
     "changed.push_back(clip(now.reach, extent))",
     "changed.push_back(now.reach)",
     BACKEND, None),
    ("c12_trailing_request_bytes_accepted", SERVICE,
     "    if (!reader.finished()) {", "    if (false) {",
     BACKEND, None),
    ("c13_viewport_ignores_the_terrain_root", FRAME,
     "if (reference.empty() && is_terrain_root(world, node)) {",
     "if (reference.empty() && is_terrain_root(world, node) && false) {",
     VIEWPORT, None),
]


def md5(path):
    return hashlib.md5(path.read_bytes()).hexdigest()


def cargo_env():
    env = dict(os.environ)
    target = subprocess.run(["just", "_editor-target-dir"], cwd=ROOT, capture_output=True,
                            text=True, check=True).stdout.strip()
    env["CARGO_TARGET_DIR"] = target
    pool = subprocess.run(["just", "_cargo-pool"], cwd=ROOT, capture_output=True, text=True,
                          check=True).stdout
    for name, value in re.findall(r"(\w+)='?([^'\s]*)'?", pool.replace("export ", "")):
        env[name] = value
    return env


def run_rust(env, crate, target, pattern):
    jobs = env.get("CY_JOBS", "4")
    command = ["cargo", "test", "--manifest-path", str(ROOT / "editor/Cargo.toml"), "--jobs", jobs,
               "-p", crate, *target, pattern]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, env=env)
    text = result.stdout + result.stderr
    failed = sorted(set(re.findall(r"^test (\S+) \.\.\. FAILED$", text, re.MULTILINE)))
    summary = re.findall(r"^test result: .*$", text, re.MULTILINE)
    compiled = "could not compile" not in text
    return result.returncode, failed, summary[-1] if summary else "", compiled, text


def run_cpp(env, target, suite):
    jobs = env.get("CY_JOBS", "4")
    built = subprocess.run(["cmake", "--build", str(BUILD), "--parallel", jobs, "--target", target],
                           cwd=ROOT, capture_output=True, text=True)
    if built.returncode != 0:
        return built.returncode, [], "", False, built.stdout + built.stderr
    result = subprocess.run(["ctest", "--test-dir", str(BUILD), "-R", f"^{re.escape(suite)}$",
                             "--output-on-failure"], cwd=ROOT, capture_output=True, text=True)
    text = result.stdout + result.stderr
    failed = sorted(set(re.findall(r"TEST CASE:\s+(.+)$", text, re.MULTILINE)))
    if result.returncode != 0 and not failed:
        failed = [suite]
    summary = re.findall(r"^\[doctest\] test cases:.*$", text, re.MULTILINE)
    return result.returncode, failed, summary[-1] if summary else "", True, text


def main(prefixes):
    env = cargo_env()
    chosen = [m for m in MUTATIONS if not prefixes or m[0].startswith(tuple(prefixes))]
    lines = [
        "Mutations that turn the terrain tools' tests red (#29, Terrain). Rust in the development",
        "profile, C++ in build/dev. Each mutation was applied, its tests rebuilt and run, the file",
        "restored and md5-verified. Listed under each: the tests or test cases that failed.",
        "",
    ]
    survived = []
    for name, relative, before, after, suite, pattern in chosen:
        path = ROOT / relative
        source = path.read_text(encoding="utf-8")
        if source.count(before) != 1:
            raise SystemExit(f"{name}: expected one occurrence of the snippet in {relative}, "
                             f"found {source.count(before)}")
        digest = md5(path)
        path.write_text(source.replace(before, after), encoding="utf-8")
        try:
            if suite[0] == "rust":
                code, failed, summary, compiled, text = run_rust(env, suite[1], suite[2], pattern)
            else:
                code, failed, summary, compiled, text = run_cpp(env, suite[1], suite[2])
        finally:
            path.write_text(source, encoding="utf-8")
        restored = md5(path) == digest
        lines.append(f"== {name}: {relative}")
        for line in before.splitlines():
            lines.append(f"<   {line.strip()}")
        for line in after.splitlines():
            lines.append(f">   {line.strip()}")
        if not compiled:
            lines.append("DID NOT COMPILE")
            sys.stderr.write(text[-4000:])
            survived.append(name)
        elif code == 0 or not failed:
            lines.append(f"SURVIVED  {summary}")
            survived.append(name)
        else:
            for test in failed:
                lines.append(f"FAILED  {test}")
            if summary:
                lines.append(summary)
        lines.append("RESTORED md5 ok" if restored else "RESTORE FAILED")
        lines.append("")
        print(f"{name}: {'survived' if name in survived else 'killed'}", flush=True)
        if not restored:
            raise SystemExit(f"{name}: {relative} was not restored")
    lines.append(f"{len(chosen) - len(survived)} of {len(chosen)} mutations killed.")
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
