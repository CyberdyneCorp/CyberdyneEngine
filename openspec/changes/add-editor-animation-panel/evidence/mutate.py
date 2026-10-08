#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mutation driver for the editor's animation panel (#29 Animation, #76 stage 5).

Each mutation replaces one exact snippet (which must occur once), runs the named tests, records
which failed, restores the file and verifies its md5. Rust mutations run `cargo test` through the
tree's target directory and job pool; C++ mutations rebuild one CTest executable in the Development
build tree (`build/dev`, or CY_BUILD_DIR) with CY_JOBS jobs and run it. A mutation none of its
tests notices is written as SURVIVED and makes the driver exit non-zero.

Run from anywhere: python3 openspec/changes/add-editor-animation-panel/evidence/mutate.py
Pass mutation names to run only those; the record then goes to falsification-<first name>.txt.
Pass --check to verify every snippet occurs exactly once without building anything.
Output: falsification.txt beside this file. Every C++ target is rebuilt unmutated at the end.
The frame mutations (c16, c17) need a Vulkan device: without one their case skips and they survive.
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
BUILD = pathlib.Path(os.environ.get("CY_BUILD_DIR", ROOT / "build" / "dev"))

POSE = "src/graph/src/lower_pose.cpp"
SERVICE = "src/editor_backend/src/animation_service.cpp"
PREVIEW = "src/editor_backend/src/animation_preview.cpp"
MATERIAL = "src/editor_backend/src/material_service.cpp"
FRAME = "samples/05b-editor-window/runtime/authored_frame.cpp"
QUEUE = "samples/05b-editor-window/runtime/service_queue.cpp"
GRAPH_RS = "editor/crates/cy-editor-services/src/animation_graph.rs"
REQUESTS_RS = "editor/crates/cy-editor-services/src/animation_requests.rs"
EDITOR_RS = "editor/crates/cy-editor-services/src/editor.rs"
BUILTIN_RS = "editor/crates/cy-editor-services/src/builtin.rs"
CANVAS_RS = "editor/crates/cy-editor-interface/src/specialised/animation.rs"
AUTHORING_RS = "editor/crates/cy-editor-interface/src/specialised/animation_authoring_commands.rs"
DOMAINS_RS = "editor/crates/cy-editor-interface/src/specialised/mod.rs"
PANEL_RS = "editor/crates/cy-editor-shell/src/panels/animation.rs"
TIMELINE_RS = "editor/crates/cy-editor-shell/src/panels/timeline.rs"

SERVICES_LIB = ("rust", "cy-editor-services", ["--lib"])
INTERFACE_LIB = ("rust", "cy-editor-interface", ["--lib"])
SHELL_LIB = ("rust", "cy-editor-shell", ["--lib"])
PANELS = ("rust", "cy-editor-shell", ["--test", "new_panels_are_accessible"])
MCP = ("rust", "cy-editor-mcp", ["--test", "a_session_over_the_wire"])
COMPILER = ("cpp", "cy_test_integration_graph_compiler", [])
ENGINE = ("cpp", "cy_test_integration_editor_backend_animation", [])
FRAME_SUITE = ("cpp", "cy_test_smoke_editor_authored_frame_vulkan", [])
RUNTIME_UNIT = ("cpp", "cy_test_unit_editor_window_runtime", [])

ACCEPTANCE = "an_animation_graph_is_authored_previewed_and_undone_over_mcp"

# (name, file, before, after, target, test filter)
MUTATIONS = [
    # --- The pose vocabulary -------------------------------------------------------------------
    ("c01_a_state_has_no_state_output", POSE,
     '    const PinDesc state_pins[] = {pin("pose", "pose", PinDirection::Input),\n'
     '                                  pin("state", "state", PinDirection::Output)};\n'
     '    if (Status added = register_node(registry, "pose.state", Span<const PinDesc>(state_pins, 2));',
     '    const PinDesc state_pins[] = {pin("pose", "pose", PinDirection::Input),\n'
     '                                  pin("state", "state", PinDirection::Output)};\n'
     '    if (Status added = register_node(registry, "pose.state", Span<const PinDesc>(state_pins, 1));',
     COMPILER, "graph_locomotion: the builder's graph validates*"),
    ("c02_unnamed_clocks_are_shared", POSE,
     "time != nullptr && !time->text.is_empty() ? time->text : own_clock(node)",
     "time != nullptr ? time->text : Name{}",
     COMPILER, "graph_pose: a clip that names no clock*"),
    # --- The service -------------------------------------------------------------------------------
    ("c03_a_cut_is_accepted", SERVICE,
     '    if (!float_property(graph, node.key, "duration", duration) || !(duration > 0.0F)) {',
     '    if (!float_property(graph, node.key, "duration", duration)) {',
     ENGINE, "editor animation: a zero-duration transition*"),
    ("c04_an_unwired_transition_is_accepted", SERVICE,
     '    if (source == nullptr || target == nullptr || !is(*source, "pose.state") ||',
     '    if (false && (source == nullptr || target == nullptr) && !is(*source, "pose.state") &&',
     ENGINE, "editor animation: what a compiled program cannot carry*"),
    ("c05_an_unknown_clip_is_accepted", SERVICE,
     "    if (preview != nullptr && known == nullptr) {",
     "    if (preview == nullptr && known != nullptr) {",
     ENGINE, "editor animation: what a compiled program cannot carry*"),
    ("c06_the_catalogue_offers_no_clips", SERVICE,
     "        out.u32v(static_cast<u32>(clips_of(preview).size()));",
     "        out.u32v(0);\n        return;",
     ENGINE, "editor animation: the catalogue offers*"),
    ("c07_the_service_routes_no_animation", MATERIAL,
     '    } else if (!session->cancelled && operation.starts_with("animation.")) {',
     '    } else if (!session->cancelled && operation.starts_with("animation.none")) {',
     ENGINE, "editor animation: the material service routes*"),
    # --- The preview ---------------------------------------------------------------------------------
    ("c08_the_machine_is_shown_at_its_start", PREVIEW,
     "    if (Status ran = run_machine(request.time, continuing ? previous : request.time); !ran) {",
     "    if (Status ran = run_machine(0.0F, continuing ? previous : request.time); !ran) {",
     ENGINE, "editor animation: the previewed machine*"),
    ("c09_a_clip_end_wraps_to_its_start", PREVIEW,
     "            focused.sample_unwrapped(time, skeleton_.retained(0), *cursor_, local_.span(), stats);",
     "            focused.sample(time, skeleton_.retained(0), *cursor_, local_.span(), stats);",
     ENGINE, "editor animation: a scrubbed clip*"),
    ("c10_the_parameters_never_reach_the_machine", PREVIEW,
     "        (void)instance_->set_parameter(*rig_, parameter.name, parameter.value);",
     "        (void)parameter;",
     ENGINE, "editor animation: the previewed machine*"),
    ("c11_authored_events_are_dropped", PREVIEW,
     "        if (event.clip != clip.name()) {",
     "        if (true) {",
     ENGINE, "editor animation: scrubbing forward*"),
    ("c12_a_scrub_fires_nothing", PREVIEW,
     "    if (continuing && time > previous) {",
     "    if (false) {",
     ENGINE, "editor animation: scrubbing forward*"),
    ("c13_a_playing_clip_never_wraps", PREVIEW,
     "                                : std::fmod(next, focused.duration());",
     "                                : next;",
     ENGINE, "editor animation: a playing preview*"),
    ("c14_the_focus_is_ignored", PREVIEW,
     "    if (request.focus != 0) {",
     "    if (false) {",
     ENGINE, "editor animation: a scrubbed clip*"),
    ("c15_the_compile_skips_validation", SERVICE,
     "    (void)graph::validate(*out.graph, registry, nullptr, out.sink);",
     "    (void)registry;",
     ENGINE, "editor animation: the acceptance graph*"),
    # --- The frame -----------------------------------------------------------------------------------
    ("c16_the_preview_is_not_drawn", FRAME,
     "    if (skinned_ == nullptr || !skinned_->visible || mesh == nullptr) {",
     "    if (true) {",
     FRAME_SUITE, "*animation preview*"),
    ("c17_a_new_pose_is_not_uploaded", FRAME,
     "    if (Status uploaded = skinned_->scene.upload_poses(preview->matrices, 0, bones); !uploaded) {",
     "    if (Status uploaded = skinned_->scene.upload_poses(preview->matrices, 0, skinned_->visible ? 0U : bones); !uploaded) {",
     FRAME_SUITE, "*animation preview*"),
    ("c18_a_busy_request_is_dropped", QUEUE,
     "    if (result == CY_RESULT_ALREADY_EXISTS) {\n        waiting_.push_back(std::move(waiting));\n"
     "        return CY_RESULT_OK;\n    }\n    return result;",
     "    return result;",
     RUNTIME_UNIT, "a request the service is too busy for*"),
    # --- The editor ----------------------------------------------------------------------------------
    ("r01_events_are_written_unsorted", GRAPH_RS,
     "    sorted.sort_by(|a, b| a.time.total_cmp(&b.time).then_with(|| a.name.cmp(&b.name)));\n",
     "",
     SERVICES_LIB, "events_read_and_write_as_the_engine_reads_them"),
    ("r02_the_preview_request_drops_its_parameters", GRAPH_RS,
     "    for (name, value) in &settings.parameters {",
     "    for (name, value) in settings.parameters.iter().take(0) {",
     MCP, ACCEPTANCE),
    ("r03_the_preview_does_not_follow_an_edit", EDITOR_RS,
     "            self.preview_follows(reference, Some(source));",
     "            let _ = source;",
     MCP, ACCEPTANCE),
    ("r04_the_preview_does_not_follow_an_undo", BUILTIN_RS,
     "        project.animation_graph_changed(&reference, source.as_deref());",
     "        let _ = source;",
     MCP, ACCEPTANCE),
    ("r05_a_graph_save_is_recorded_under_another_kind", EDITOR_RS,
     '                kind: format!("{prefix}{reference}"),',
     '                kind: format!("unrecorded:{reference}"),',
     MCP, ACCEPTANCE),
    ("r06_a_duplicate_event_is_placed", AUTHORING_RS,
     "                refuse_duplicate(&events, &name, time)?;",
     "",
     MCP, ACCEPTANCE),
    ("r07_a_moved_event_is_found_by_name_alone", AUTHORING_RS,
     "        .filter(|(_, event)| event.name == name && (event.time - time).abs() <= EVENT_TOLERANCE)",
     "        .filter(|(_, event)| event.name == name)",
     MCP, ACCEPTANCE),
    ("r08_a_key_drag_moves_from_where_it_ends", PANEL_RS,
     "                    .with(\"from\", Value::Float(event.time))",
     "                    .with(\"from\", Value::Float(seconds(to.max(0.0))))",
     SHELL_LIB, "each_timeline_gesture_is_the_event_command"),
    ("r09_a_graph_that_does_not_compile_is_previewed", PANEL_RS,
     "    if previewed || asked || !compiled || !panels.editor.runtime.is_connected() {",
     "    if previewed || asked || !panels.editor.runtime.is_connected() {",
     PANELS, "a_zero_duration_transition_is_refused_on_that_transition"),
    ("r10_the_mode_ignores_what_the_engine_previews", PANEL_RS,
     "        frame.inputs.animation.machine = state.focus == 0;",
     "        let _ = state;",
     PANELS, "the_animation_panel_offers_the_engines_pose_vocabulary"),
    ("r11_a_scrub_previews_the_state_machine", PANEL_RS,
     "            scrub_arguments(&target.reference, node, time),",
     "            scrub_arguments(&target.reference, 0, time),",
     PANELS, "a_click_on_the_timelines_ruler_scrubs"),
    ("r12_the_catalogue_is_never_ready", DOMAINS_RS,
     "        self.animation_catalogue_ready = true;",
     "        self.animation_catalogue_ready = false;",
     PANELS, "the_animation_panel_offers_the_engines_pose_vocabulary"),
    ("r13_any_catalogue_is_a_pose_catalogue", CANVAS_RS,
     "    if nodes.is_empty() || nodes.iter().any(|node| !node.name.starts_with(NODE_PREFIX)) {",
     "    if nodes.is_empty() {",
     INTERFACE_LIB, "a_catalogue_outside_the_pose_vocabulary_is_refused"),
    ("r14_an_event_lane_takes_no_key", TIMELINE_RS,
     "    kind.is_keyed() || kind == TrackKind::GameplayEvent",
     "    kind.is_keyed()",
     SHELL_LIB, "double_clicking_an_event_lane"),
    ("r15_a_queued_preview_is_not_replaced", REQUESTS_RS,
     "        self.settings = Some(settings);\n        self.previewed = Some(source.to_owned());\n"
     "        self.queue\n"
     "            .retain(|waiting| waiting.operation != PREVIEW_SET && waiting.operation != PREVIEW_GET);",
     "        self.settings = Some(settings);\n        self.previewed = Some(source.to_owned());",
     SERVICES_LIB, "a_newer_preview_replaces_one_still_queued"),
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
    command = ["cargo", "test", "--manifest-path", str(ROOT / "editor/Cargo.toml"), "--profile",
               "development", "--jobs", jobs, "-p", crate, *target, pattern]
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, env=env)
    text = result.stdout + result.stderr
    failed = sorted(set(re.findall(r"^test (\S+) \.\.\. FAILED$", text, re.MULTILINE)))
    summary = re.findall(r"^test result: .*$", text, re.MULTILINE)
    compiled = "could not compile" not in text
    return result.returncode, failed, summary[-1] if summary else "", compiled, text


def run_cpp(env, executable, pattern):
    jobs = env.get("CY_JOBS", "4")
    build = subprocess.run(["ninja", "-C", str(BUILD), "-j", jobs, executable], cwd=ROOT,
                           capture_output=True, text=True)
    if build.returncode != 0:
        return build.returncode, [], "", False, build.stdout + build.stderr
    result = subprocess.run([str(BUILD / executable), f"--test-case={pattern}"], cwd=ROOT,
                            capture_output=True, text=True)
    text = result.stdout + result.stderr
    failed = sorted(set(re.findall(r"^TEST CASE:\s+(.*)$", text, re.MULTILINE)))
    summary = re.findall(r"^\[doctest\] test cases:.*$", text, re.MULTILINE)
    ran = re.search(r"test cases:\s+(\d+)", text)
    if ran is None or ran.group(1) == "0":
        return 0, [], "no test case matched the filter", True, text
    return result.returncode, failed, summary[-1] if summary else "", True, text


def record(lines, name, relative, before, after):
    lines.append(f"== {name}: {relative}")
    for line in before.splitlines():
        lines.append(f"<   {line.strip()}")
    for line in (after.splitlines() or [""]):
        lines.append(f">   {line.strip()}")


def check():
    bad = 0
    for name, relative, before, _after, _target, _pattern in MUTATIONS:
        count = (ROOT / relative).read_text(encoding="utf-8").count(before)
        if count != 1:
            print(f"{name}: {relative} holds the snippet {count} time(s)")
            bad += 1
    print(f"{len(MUTATIONS) - bad} of {len(MUTATIONS)} snippets occur exactly once")
    return 1 if bad else 0


def main():
    if "--check" in sys.argv[1:]:
        return check()
    env = cargo_env()
    chosen = sys.argv[1:]
    mutations = [m for m in MUTATIONS if not chosen or m[0] in chosen]
    out = OUT if not chosen else HERE / f"falsification-{chosen[0]}.txt"
    lines = [
        "Mutations that turn the animation panel's tests red (#29 Animation, #76 stage 5),",
        "Development profile. Each mutation was applied, its tests run, the file restored and",
        "md5-verified. Listed under each: the tests (Rust) or test cases (C++) that failed.",
        "",
    ]
    survived = []
    rebuild = set()
    for name, relative, before, after, (kind, unit, target), pattern in mutations:
        path = ROOT / relative
        source = path.read_text(encoding="utf-8")
        if source.count(before) != 1:
            raise SystemExit(f"{name}: expected one occurrence of the snippet in {relative}, "
                             f"found {source.count(before)}")
        digest = md5(path)
        path.write_text(source.replace(before, after), encoding="utf-8")
        try:
            if kind == "rust":
                code, failed, summary, compiled, text = run_rust(env, unit, target, pattern)
            else:
                rebuild.add(unit)
                code, failed, summary, compiled, text = run_cpp(env, unit, pattern)
        finally:
            path.write_text(source, encoding="utf-8")
        restored = md5(path) == digest
        record(lines, name, relative, before, after)
        if not compiled:
            lines.append("DID NOT COMPILE")
            print(f"{name}: did not compile", flush=True)
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
    for unit in sorted(rebuild):
        subprocess.run(["ninja", "-C", str(BUILD), "-j", env.get("CY_JOBS", "4"), unit],
                       cwd=ROOT, check=True, capture_output=True)
    lines.append(f"{len(mutations) - len(survived)} of {len(mutations)} mutations killed.")
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
