#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mutation driver for the #29 physics tools.

Each mutation keeps the file compiling (a helper left unused is an error under -Werror, so a call
is disabled rather than deleted) and replaces one exact snippet (which must occur once), runs the tests that should notice,
records which failed, restores the file and verifies its md5. A mutation none of its tests notices
is written as SURVIVED and makes the driver exit non-zero. Output: falsification.txt beside this file.

Rust mutations run `cargo test` in the tree's editor target directory through the job pool, as
`just build-editor-check` does. C++ mutations rebuild one test target in `build/dev` (whose Ninja
launcher is the machine-wide job slot) and run it through ctest; build the engine first.

Run from anywhere: python3 openspec/changes/add-editor-physics-tools/evidence/mutate.py [name...]
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

VIEWPORT = "editor/crates/cy-editor-viewport/src"
SERVICES = "editor/crates/cy-editor-services/src"
PANEL = "editor/crates/cy-editor-shell/src/panels/physics.rs"
RUNTIME = "samples/05b-editor-window/runtime/physics_overlay.cpp"


def cargo(crate, *target):
    return ("cargo", crate, list(target))


def ctest(kind, name):
    return ("ctest", kind, name)


VIEWPORT_LIB = cargo("cy-editor-viewport", "--lib")
SERVICES_LIB = cargo("cy-editor-services", "--lib")
JOINT_TESTS = cargo("cy-editor-services", "--test", "a_joint_is_a_transaction")
SHELL_LIB = cargo("cy-editor-shell", "--lib")
PANEL_TESTS = cargo("cy-editor-shell", "--test", "new_panels_are_accessible")
MCP_WIRE = cargo("cy-editor-mcp", "--test", "a_session_over_the_wire")

# (name, file, before, after, target, test filter)
MUTATIONS = [
    # --- the editor ------------------------------------------------------------------------------
    ("r01_layer_bit_drifts", f"{VIEWPORT}/physics_view.rs",
     "    Contacts = 1 << 1,", "    Contacts = 1 << 7,",
     VIEWPORT_LIB, "the_layers_are_the_engines"),
    ("r02_colliders_planned_again", f"{VIEWPORT}/viewmode.rs",
     '    ("Navigation data", "navigation"),', '    ("Physics colliders", "physics"),',
     VIEWPORT_LIB, "physics_colliders_are_requestable_rather_than_planned"),
    ("r03_layers_not_sent", f"{SERVICES}/gizmo.rs",
     "        writer.u32(self.physics_overlays);", "        writer.u32(0);",
     SERVICES_LIB, "the_physics_layers_a_viewport_asks_for_survive_the_intent_wire"),
    ("r04_layers_not_read_from_the_viewport", f"{SERVICES}/gizmo.rs",
     "            physics_overlays: viewport.physics.bits(),", "            physics_overlays: 0,",
     SERVICES_LIB, "the_physics_layers_a_viewport_asks_for_survive_the_intent_wire"),
    ("r05_toggle_inverted", f"{SERVICES}/viewports.rs",
     '"toggle" => controls.get(layer.control()).as_deref() != Some("on"),',
     '"toggle" => controls.get(layer.control()).as_deref() == Some("on"),',
     SERVICES_LIB, "a_physics_layer_command_shows_hides_and_toggles"),
    ("r06_hide_all_hides_nothing", f"{SERVICES}/viewports.rs",
     "self.focused_mut().physics = PhysicsOverlays::NONE;", "let _ = PhysicsOverlays::NONE;",
     SERVICES_LIB, "a_physics_layer_command_shows_hides_and_toggles"),
    ("r07_reference_written_as_none", f"{SERVICES}/worldfile.rs",
     "            .get(entity)\n", "            .get(&0)\n",
     SERVICES_LIB, "an_entity_reference"),
    ("r08_reference_resolves_to_the_next_node", f"{SERVICES}/worldfile.rs",
     ".and_then(|index| created.get(index));", ".and_then(|index| created.get(index + 1));",
     SERVICES_LIB, "an_entity_reference"),
    ("r09_distance_range_unchecked", f"{SERVICES}/joints.rs",
     "if self.kind == JointKind::Distance && self.limit[0] > self.limit[1] {",
     "if self.kind == JointKind::Distance && self.limit[0] > self.limit[1] + 100.0 {",
     SERVICES_LIB, "what_the_engine_would_refuse_is_refused_when_it_is_authored"),
    ("r10_joined_to_itself", f"{SERVICES}/joints.rs",
     "    if node == subject {", "    if node == subject && false {",
     JOINT_TESTS, "a_joint_that_could_not_simulate_is_refused_and_records_nothing"),
    ("r11_kind_change_not_written", f"{SERVICES}/joints.rs",
     "if before.value(changed) != after.value(changed) {",
     "if before.value(changed) != after.value(changed) && changed != JointField::Kind {",
     JOINT_TESTS, "a_hinge_becoming_a_distance_joint_takes_a_span_rather_than_being_refused"),
    ("r12_distance_gets_no_span", f"{SERVICES}/joints.rs",
     "        after.limit = JointSpec::new(JointKind::Distance).limit;",
     "        let _ = JointSpec::new(JointKind::Distance).limit;",
     JOINT_TESTS, "a_hinge_becoming_a_distance_joint_takes_a_span_rather_than_being_refused"),
    ("r13_remove_forgets_its_values", f"{SERVICES}/joints.rs",
     "                        .map(|(field, value)| (*field, value.clone()))\n",
     "                        .map(|(field, _)| (*field, Value::Nil))\n",
     JOINT_TESTS, "removing_a_joint_is_undone_with_every_value_it_had"),
    ("r14_remove_is_irreversible", f"{SERVICES}/joints.rs",
     "             restores the joint with every value it had.\",\n"
     "            EffectClass::ReversibleMutation,",
     "             restores the joint with every value it had.\",\n"
     "            EffectClass::IrreversibleMutation,",
     SHELL_LIB, "every_scaffolded_tool_is_an_undoable_mcp_peer_of_its_panel"),
    ("r15_set_is_an_external_effect", f"{SERVICES}/joints.rs",
     "                 would refuse at play is refused now, naming the rule.\",\n"
     "                EffectClass::ReversibleMutation,",
     "                 would refuse at play is refused now, naming the rule.\",\n"
     "                EffectClass::ExternalEffect,",
     MCP_WIRE, "physics_authoring_is_an_undoable_mcp_peer_of_the_physics_panel"),
    ("r16_panel_names_an_unregistered_command", PANEL,
     '    "physics.joint.remove",\n];', '    "physics.joint.delete",\n];',
     SHELL_LIB, "every_scaffolded_tool_is_an_undoable_mcp_peer_of_its_panel"),
    ("r17_checkbox_sends_the_old_state", PANEL,
     'Arguments::new().with("state", Value::Text(on_off(on).to_string())),',
     'Arguments::new().with("state", Value::Text(on_off(!on).to_string())),',
     PANEL_TESTS, "the_physics_panel_toggles_an_engine_layer_through_its_registered_command"),
    ("r18_panel_shows_every_field", PANEL,
     "|| !spec.kind.uses(field)", "|| false",
     PANEL_TESTS, "a_joined_body_shows_the_fields_its_kind_reads"),
    ("r19_ragdoll_scope_unstated", PANEL,
     "diagnostics_area(ui, panels.shell, &diagnostics());",
     "diagnostics_area(ui, panels.shell, &[]);",
     PANEL_TESTS, "the_physics_panel_scopes_the_ragdoll_tool_in_its_diagnostics_area"),
    ("r20_add_names_the_wrong_kind", PANEL,
     '.with("kind", Value::Text(kind.keyword().to_string()))',
     '.with("kind", Value::Text(String::from("fixed")))',
     PANEL_TESTS, "a_selected_body_is_offered_a_joint"),
    ("r21_committed_every_frame_of_a_drag", PANEL,
     "        if finished\n", "        if true\n",
     SHELL_LIB, "a_dragged_field_is_committed_once_when_the_drag_is_released"),
    # --- the engine ------------------------------------------------------------------------------
    ("c01_reference_read_off_by_one", "src/scene/serialization/src/worldfile.cpp",
     "editor_node_identity(world.document(), *position + 1U).low",
     "editor_node_identity(world.document(), *position + 2U).low",
     ctest("unit", "scene_serialization"), None),
    ("c02_reference_written_as_identity", "src/scene/serialization/src/worldfile.cpp",
     "    return writer.word_u64(positions[index]);",
     "    return writer.word_u64(static_cast<u64>(value.integer));",
     ctest("unit", "scene_serialization"), None),
    ("c03_constraint_limits_not_drawn", "src/servers/physics/src/debug.cpp",
     "    draw_constraint_limits(description, frame_a, sink);",
     "    if (false) { draw_constraint_limits(description, frame_a, sink); }",
     ctest("unit", "physics_server"), None),
    ("c04_jolt_draws_no_constraint", "src/backends/physics-jolt/src/jolt_server.cpp",
     "            debug_draw_constraint(record.description, a, b, sink);",
     "            if (false) { debug_draw_constraint(record.description, a, b, sink); }",
     ctest("integration", "physics_jolt"), None),
    ("c05_intent_drops_the_layers", "src/servers/render/src/gizmo.cpp",
     "            intent.physics_overlays = overlays;", "            (void)overlays;",
     ctest("unit", "render_server"), None),
    ("c06_hinge_range_dropped", "src/gameplay/play/src/joints.cpp",
     "            out.limit = range;", "            out.limit = physics::AxisLimit{};",
     ctest("integration", "gameplay_joints"), None),
    ("c07_frame_b_in_the_wrong_space", "src/gameplay/play/src/joints.cpp",
     "    return inverse(*body_b) * anchor;", "    return anchor;",
     ctest("integration", "gameplay_joints"), None),
    ("c08_joints_never_attached", "src/gameplay/play/src/session.cpp",
     "    if (Status joined = attach_joints(); !joined) {",
     "    if (Status joined = ok(); !joined) {",
     ctest("integration", "gameplay_joints"), None),
    ("c09_bodiless_target_accepted", "src/gameplay/play/src/session.cpp",
     "(joint->target != 0 && body_b.is_null())", "(false && body_b.is_null())",
     ctest("integration", "gameplay_joints"), None),
    ("c10_overlay_ignores_the_camera", RUNTIME,
     "project_to_pixel(view_, from - eye_, start)", "project_to_pixel(view_, from, start)",
     ctest("unit", "editor_window_physics_overlay"), None),
    ("c11_joint_frame_scaled", RUNTIME,
     "    placement.scale = Vec3{1.0F, 1.0F, 1.0F};\n    return placement;",
     "    return placement;",
     ctest("unit", "editor_window_physics_overlay"), None),
    ("c12_joint_axis_not_drawn", RUNTIME,
     "    if (has_axis(description.type)) {", "    if (has_axis(description.type) && false) {",
     ctest("unit", "editor_window_physics_overlay"), None),
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


def run_cargo(env, crate, target, pattern):
    jobs = env.get("CY_JOBS", "4")
    command = ["cargo", "test", "--manifest-path", str(ROOT / "editor/Cargo.toml"), "--profile",
               "development", "--jobs", jobs, "-p", crate, *target, pattern]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, env=env)
    text = result.stdout + result.stderr
    failed = sorted(set(re.findall(r"^test (\S+) \.\.\. FAILED$", text, re.MULTILINE)))
    summary = re.findall(r"^test result: .*$", text, re.MULTILINE)
    compiled = "could not compile" not in text
    return result.returncode, failed, summary[-1] if summary else "", compiled, text


def run_ctest(env, kind, name):
    jobs = env.get("CY_JOBS", "4")
    target = f"cy_test_{kind}_{name}"
    built = subprocess.run(["cmake", "--build", str(BUILD), "--target", target, "-j", jobs],
                           cwd=ROOT, capture_output=True, text=True, env=env)
    if built.returncode != 0:
        return built.returncode, [], "", False, built.stdout + built.stderr
    result = subprocess.run(["ctest", "--test-dir", str(BUILD), "-R", f"^{kind}\\.{name}$",
                             "--output-on-failure"], cwd=ROOT, capture_output=True, text=True,
                            env=env)
    text = result.stdout + result.stderr
    failed = sorted(set(re.findall(r"TEST CASE:\s+(.+?)\s*$", text, re.MULTILINE)))
    summary = re.findall(r"^\[doctest\] test cases:.*$", text, re.MULTILINE)
    return result.returncode, failed, summary[-1] if summary else "", True, text


def restore_build(env, target):
    """Rebuild a restored C++ target so the next mutation starts from the real sources."""
    if target[0] == "ctest":
        subprocess.run(["cmake", "--build", str(BUILD), "--target",
                        f"cy_test_{target[1]}_{target[2]}", "-j", env.get("CY_JOBS", "4")],
                       cwd=ROOT, capture_output=True, text=True, env=env)


def run_one(env, mutation):
    name, relative, before, after, target, pattern = mutation
    path = ROOT / relative
    source = path.read_text(encoding="utf-8")
    if source.count(before) != 1:
        raise SystemExit(f"{name}: expected one occurrence of the snippet in {relative}, "
                         f"found {source.count(before)}")
    digest = md5(path)
    path.write_text(source.replace(before, after), encoding="utf-8")
    try:
        if target[0] == "cargo":
            outcome = run_cargo(env, target[1], target[2], pattern)
        else:
            outcome = run_ctest(env, target[1], target[2])
    finally:
        path.write_text(source, encoding="utf-8")
    restore_build(env, target)
    return outcome, md5(path) == digest


def main():
    env = cargo_env()
    wanted = set(sys.argv[1:])
    chosen = [mutation for mutation in MUTATIONS if not wanted or mutation[0] in wanted]
    lines = [
        "Mutations that turn the #29 physics tools' tests red, Development profile. Each mutation",
        "was applied, its tests run (cargo test for the editor, the one ctest suite for the engine),",
        "the file restored and md5-verified. Listed under each: the tests that failed.",
        "",
    ]
    survived = []
    for mutation in chosen:
        name, relative, before, after = mutation[:4]
        (code, failed, summary, compiled, text), restored = run_one(env, mutation)
        lines.append(f"== {name}: {relative}")
        lines.extend(f"<   {line.strip()}" for line in before.splitlines())
        lines.extend(f">   {line.strip()}" for line in after.splitlines() or [""])
        if not compiled:
            lines.append("DID NOT COMPILE")
            sys.stderr.write(text[-4000:])
            survived.append(name)
        elif code == 0 or not failed:
            lines.append(f"SURVIVED  {summary}")
            survived.append(name)
        else:
            lines.extend(f"FAILED  {test}" for test in failed)
            lines.append(summary)
        lines.append("RESTORED md5 ok" if restored else "RESTORE FAILED")
        lines.append("")
        print(f"{name}: {'survived' if name in survived else 'killed'}", flush=True)
        if not restored:
            raise SystemExit(f"{name}: {relative} was not restored")
    lines.append(f"{len(chosen) - len(survived)} of {len(chosen)} mutations killed.")
    output = OUT if not wanted else HERE / "falsification.partial.txt"
    output.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
