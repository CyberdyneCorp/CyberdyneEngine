#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mutation driver for the specialised-editor scaffold (#29 Wave 0).

Each mutation replaces one exact snippet (which must occur once), runs the named Rust tests, records
which failed, restores the file and verifies its md5. A mutation none of its tests notices is written
as SURVIVED and makes the driver exit non-zero. Output: falsification.txt beside this file.

Run from anywhere: python3 openspec/changes/add-editor-specialised-scaffold/evidence/mutate.py
It uses the tree's cargo target directory and job pool, like `just build-editor-check`.
"""
import hashlib
import os
import pathlib
import re
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[3]
CRATES = ROOT / "editor/crates"
OUT = HERE / "falsification.txt"

SHELL = "cy-editor-shell/src/panels"
CANVAS = f"{SHELL}/graph_canvas.rs"
SCAFFOLD = f"{SHELL}/specialised.rs"
TERRAIN = f"{SHELL}/terrain.rs"
TIMELINE = f"{SHELL}/timeline.rs"
MODEL = "cy-editor-interface/src/specialised/timeline.rs"
SERVICES = "cy-editor-services/src/terrain.rs"

LIB_SHELL = ("cy-editor-shell", ["--lib"])
PANELS_TEST = ("cy-editor-shell", ["--test", "new_panels_are_accessible"])
LIB_MODEL = ("cy-editor-interface", ["--lib"])
MCP_WIRE = ("cy-editor-mcp", ["--test", "a_session_over_the_wire"])

# (name, file, before, after, target, test filter)
MUTATIONS = [
    ("m01_palette_grid_columns", CANVAS,
     "const PALETTE_COLUMNS: usize = 3;", "const PALETTE_COLUMNS: usize = 4;",
     LIB_SHELL, "palette_slots_fill_a_three_column_grid"),
    ("m02_palette_ignores_labels", CANVAS,
     "|| label.contains(&query)", "|| false",
     LIB_SHELL, "a_catalogue_palette_searches_names_and_labels"),
    ("m03_palette_drops_a_declared_type", CANVAS,
     ".filter_map(|name| canvas.catalogue().get(name))",
     ".filter_map(|name| canvas.catalogue().get(name)).skip(1)",
     LIB_SHELL, "every_openable_graph_domain_hosts_its_declared_node_palette"),
    ("m04_drag_release_not_reported", CANVAS,
     "} else if response.drag_stopped() && feedback.on_move.is_some() {",
     "} else if false && response.drag_stopped() && feedback.on_move.is_some() {",
     LIB_SHELL, "dragging_a_node_header_reports_moves_and_one_finished_gesture"),
    ("m05_connection_not_routed", CANVAS,
     "let result = if let Some(connect) = on_connect {",
     "let result = if let Some(connect) = on_connect.filter(|_| false) {",
     LIB_SHELL, "clicking_an_output_then_an_input_pin_routes_one_connection_to_the_host"),
    ("m06_parity_ignores_exclusion", SCAFFOLD,
     "if let Some(reason) = tool.exclusion {",
     "if let Some(reason) = tool.exclusion.filter(|_| false) {",
     LIB_SHELL, "an_excluded_or_irreversible_panel_command_is_refused"),
    ("m07_parity_admits_irreversible", SCAFFOLD,
     "EffectClass::Read | EffectClass::ReversibleMutation\n",
     "EffectClass::Read | EffectClass::ReversibleMutation | EffectClass::IrreversibleMutation\n",
     LIB_SHELL, "an_excluded_or_irreversible_panel_command_is_refused"),
    ("m08_parity_admits_unregistered", SCAFFOLD,
     "let Some(tool) = cy_editor_agent::tool::project_one(registry, command) else {",
     "let Some(tool) = cy_editor_agent::tool::project_one(registry, command) else {\n"
     "            continue;\n        };\n        let Some(tool) = Some(tool) else {",
     LIB_SHELL, "a_panel_command_nobody_registered_is_refused_by_name"),
    ("m09_terrain_names_an_unregistered_command", TERRAIN,
     '"terrain.modifier.move",\n    ];', '"terrain.modifier.reorder",\n    ];',
     LIB_SHELL, "every_scaffolded_tool_is_an_undoable_mcp_peer_of_its_panel"),
    ("m10_diagnostics_area_empty", SCAFFOLD,
     "        status(ui, shell, diagnostic.role, &diagnostic.message);",
     "        let _ = (&ui, shell, diagnostic);",
     PANELS_TEST, "a_terrain_refusal_is_shown_in_the_specialised_diagnostics_area"),
    ("m11_header_undo_is_redo", SCAFFOLD,
     '                    .push(Intent::Invoke("edit.undo".into(), Arguments::new()));',
     '                    .push(Intent::Invoke("edit.redo".into(), Arguments::new()));',
     PANELS_TEST, "the_terrain_tool_draws_in_the_specialised_frame_with_undo_over_its_transactions"),
    ("m12_terrain_empty_state_lost", TERRAIN,
     'if ui.button("Create terrain").clicked() {',
     'if ui.button("Create").clicked() {',
     PANELS_TEST, "the_terrain_tool_draws_in_the_specialised_frame_with_undo_over_its_transactions"),
    ("m13_terrain_layer_irreversible", SERVICES,
     '"Adds an ordered material layer with stable identity in one undoable transaction.",\n'
     "            EffectClass::ReversibleMutation,",
     '"Adds an ordered material layer with stable identity in one undoable transaction.",\n'
     "            EffectClass::IrreversibleMutation,",
     MCP_WIRE, "terrain_authoring_is_an_undoable_mcp_peer_of_the_terrain_panel"),
    ("m26_terrain_field_squeezed_again", TERRAIN,
     "                egui::Layout::top_down(egui::Align::Min),\n                |ui| {\n"
     "                    paint_field(",
     "                egui::Layout::left_to_right(egui::Align::Min),\n                |ui| {\n"
     "                    paint_field(",
     PANELS_TEST, "the_terrain_brush_field_fills_the_space_beside_the_controls"),
    ("m14_timeline_move_never_emitted", TIMELINE,
     "&& from.total_cmp(&to).is_ne()", "&& from.total_cmp(&to).is_eq()",
     LIB_SHELL, "dragging_a_key_is_one_move_on_release"),
    ("m15_timeline_move_inverse_wrong", TIMELINE,
     "                Ok(TimelineEdit::MoveKey {\n                    track,\n                    key,\n"
     "                    to: from,",
     "                Ok(TimelineEdit::MoveKey {\n                    track,\n                    key,\n"
     "                    to: to + 0.0 * from,",
     LIB_SHELL, "every_edit_is_undone_exactly_by_the_edit_it_answers"),
    ("m16_timeline_trim_inverse_is_new_range", TIMELINE,
     '                let (start, end) = before.expect("trim refuses a section the track lacks");',
     '                let _ = before;',
     LIB_SHELL, "every_edit_is_undone_exactly_by_the_edit_it_answers"),
    ("m17_timeline_escape_ignored", TIMELINE,
     "        view.drag = None;\n    }\n    navigate(",
     "        let _ = &view.drag;\n    }\n    navigate(",
     LIB_SHELL, "escape_mid_drag_cancels_the_gesture_and_records_nothing"),
    ("m18_timeline_zoom_loses_anchor", TIMELINE,
     "self.scroll = anchor - f64::from(anchor_x - left) / f64::from(self.pixels_per_second);",
     "self.scroll = anchor - f64::from(anchor_x - left) / 100.0;",
     LIB_SHELL, "zoom_keeps_the_time_under_the_pointer"),
    ("m19_timeline_add_key_at_zero", TIMELINE,
     "let value = surface.sample(track.id, time).unwrap_or(0.0);",
     "let value = 0.0;",
     LIB_SHELL, "double_clicking_a_keyed_lane_adds_one_key_at_the_sampled_value"),
    ("m20_timeline_delete_nothing", TIMELINE,
     "Selected::Key(track, key) => Some(TimelineEdit::RemoveKey { track, key }),",
     "Selected::Key(..) => None::<TimelineEdit>,",
     LIB_SHELL, "a_selected_key_is_removed_by_delete"),
    ("m21_timeline_trim_moves_start", TIMELINE,
     "Edge::End => *end = time,", "Edge::End => *start = time,",
     LIB_SHELL, "dragging_a_clip_end_is_one_trim_on_release"),
    ("m22_timeline_ruler_inert", TIMELINE,
     "if scrub.clicked() || scrub.dragged() {", "if false {",
     LIB_SHELL, "clicking_the_ruler_scrubs_without_an_edit"),
    ("m23_timeline_x_ignores_scroll", TIMELINE,
     "left + points((time - self.scroll) * f64::from(self.pixels_per_second))",
     "left + points(time * f64::from(self.pixels_per_second))",
     LIB_SHELL, "time_and_x_are_inverse_at_any_zoom_and_scroll"),
    ("m24_model_move_onto_another_key", MODEL,
     ".any(|other| other.id != key && same_time(other.time, time))",
     ".any(|other| other.id == key && same_time(other.time, time))",
     LIB_MODEL, "a_moved_key_keeps_its_identity_and_refuses_to_land_on_another"),
    ("m25_model_restore_twice", MODEL,
     ".any(|other| other.id == key.id || same_time(other.time, key.time))",
     ".any(|_| false)",
     LIB_MODEL, "a_removed_key_is_restored_with_the_same_identity"),
    ("m27_timeline_key_release_off_lane_lost", TIMELINE,
     "if !visible && !dragging {", "if !visible {",
     LIB_SHELL, "a_key_dragged_out_of_the_lanes_is_still_one_move_on_release"),
    ("m28_timeline_edge_release_off_lane_lost", TIMELINE,
     "|| body.left() > layout.right) && !dragging {", "|| body.left() > layout.right) {",
     LIB_SHELL, "a_clip_edge_dragged_out_of_the_lanes_is_still_one_trim_on_release"),
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


def run_tests(env, crate, target, pattern):
    jobs = env.get("CY_JOBS", "4")
    command = ["cargo", "test", "--manifest-path", str(ROOT / "editor/Cargo.toml"), "--profile",
               "development", "--jobs", jobs, "-p", crate, *target, pattern]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, env=env)
    text = result.stdout + result.stderr
    failed = sorted(set(re.findall(r"^test (\S+) \.\.\. FAILED$", text, re.MULTILINE)))
    summary = re.findall(r"^test result: .*$", text, re.MULTILINE)
    compiled = "could not compile" not in text
    return result.returncode, failed, summary[-1] if summary else "", compiled, text


def main():
    env = cargo_env()
    lines = [
        "Mutations that turn the specialised-editor scaffold's tests red (#29 Wave 0), Development",
        "profile. Each mutation was applied, its tests run, the file restored and md5-verified.",
        "Listed under each: the tests that failed.",
        "",
    ]
    survived = []
    for name, relative, before, after, (crate, target), pattern in MUTATIONS:
        path = CRATES / relative
        source = path.read_text(encoding="utf-8")
        if source.count(before) != 1:
            raise SystemExit(f"{name}: expected one occurrence of the snippet in {relative}, "
                             f"found {source.count(before)}")
        digest = md5(path)
        path.write_text(source.replace(before, after), encoding="utf-8")
        try:
            code, failed, summary, compiled, text = run_tests(env, crate, target, pattern)
        finally:
            path.write_text(source, encoding="utf-8")
        restored = md5(path) == digest
        lines.append(f"== {name}: editor/crates/{relative}")
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
            lines.append(summary)
        lines.append("RESTORED md5 ok" if restored else "RESTORE FAILED")
        lines.append("")
        print(f"{name}: {'survived' if name in survived else 'killed'}", flush=True)
        if not restored:
            raise SystemExit(f"{name}: {relative} was not restored")
    lines.append(f"{len(MUTATIONS) - len(survived)} of {len(MUTATIONS)} mutations killed.")
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
