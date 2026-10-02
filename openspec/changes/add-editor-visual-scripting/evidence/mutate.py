#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mutation driver for the gameplay graph editor (#29, visual scripting).

Each mutation replaces one exact snippet (which must occur once), runs the named tests, records
which failed, restores the file and verifies its md5. Rust mutations run `cargo test` through the
tree's target directory and job pool; C++ mutations rebuild one CTest executable in the Development
build tree (`build/dev`, or CY_BUILD_DIR) with CY_JOBS jobs and run it. A mutation none of its
tests notices is written as SURVIVED and makes the driver exit non-zero.

Run from anywhere: python3 openspec/changes/add-editor-visual-scripting/evidence/mutate.py
Pass mutation names to run only those; the record then goes to falsification-<first name>.txt.
Pass --check to verify every snippet occurs exactly once without building anything.
The snippets of c02, c03, r06, r13 and r16 were restated for the code as
`add-visual-scripting-debugger` left it (the same mutations, in today's spelling).
Output: falsification.txt beside this file. Every C++ target is rebuilt unmutated at the end.
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

EVENTS = "src/graph/src/event_script.cpp"
SCRIPT = "src/graph/src/lower_script.cpp"
NATIVE = "src/graph/src/lower_script_native.cpp"
HOST = "src/game_backend/src/graph_behaviours.cpp"
SERVICE = "src/editor_backend/src/script_service.cpp"
MATERIAL = "src/editor_backend/src/material_service.cpp"
RUNTIME = "samples/05b-editor-window/runtime/graph_runtime.cpp"
GRAPH_RS = "editor/crates/cy-editor-services/src/script_graph.rs"
REQUESTS_RS = "editor/crates/cy-editor-services/src/script_requests.rs"
COMMANDS_RS = "editor/crates/cy-editor-services/src/script_commands.rs"
EDITOR_RS = "editor/crates/cy-editor-services/src/editor.rs"
BUILTIN_RS = "editor/crates/cy-editor-services/src/builtin.rs"
CANVAS_RS = "editor/crates/cy-editor-interface/src/specialised/script.rs"
AUTHORING_RS = "editor/crates/cy-editor-interface/src/specialised/script_authoring_commands.rs"
DOMAINS_RS = "editor/crates/cy-editor-interface/src/specialised/mod.rs"
PANEL_RS = "editor/crates/cy-editor-shell/src/panels/script_graph.rs"

SERVICES_LIB = ("rust", "cy-editor-services", ["--lib"])
INTERFACE_LIB = ("rust", "cy-editor-interface", ["--lib"])
SHELL_LIB = ("rust", "cy-editor-shell", ["--lib"])
PANELS = ("rust", "cy-editor-shell", ["--test", "new_panels_are_accessible"])
MCP = ("rust", "cy-editor-mcp", ["--test", "a_session_over_the_wire"])
COMPILER = ("cpp", "cy_test_integration_graph_compiler", [])
GRAPH_HOST = ("cpp", "cy_test_integration_game_backend_graph", [])
ENGINE_SCRIPT = ("cpp", "cy_test_integration_editor_backend_script", [])
TWIN = ("cpp", "cy_test_smoke_editor_graph_equivalence", [])

ACCEPTANCE = "a_gameplay_graph_is_authored_compiled_and_run_in_play_over_mcp_and_undoes"

# (name, file, before, after, target, test filter)
MUTATIONS = [
    # --- The compiler -------------------------------------------------------------------------
    ("c01_every_handler_starts_at_the_first_block", EVENTS,
     "        handler.block = blocks[index];", "        handler.block = blocks[0];",
     COMPILER, "event graph: each event*"),
    ("c02_a_new_event_keeps_the_old_wait", SCRIPT,
     "    state.set_resume_block(kNoBlock);\n    state.clear_pause();\n    return run_blocks(program, "
     "state, host, RunStart{start, 0}, instruction_budget, debug);",
     "    state.clear_pause();\n    return run_blocks(program, state, host, RunStart{start, 0}, "
     "instruction_budget, debug);",
     COMPILER, "event graph: each event*"),
    ("c03_native_handler_starts_at_the_entry", NATIVE,
     "    return run_steps(program, state, host, program.block_starts()[start], "
     "instruction_budget,\n                     debug);",
     "    return run_steps(program, state, host, program.entry_step(), instruction_budget, debug);",
     COMPILER, "event graph: each event*"),
    ("c04_any_name_of_the_kind_is_declared", EVENTS,
     "        if (!declared.family && declared.name == name) {",
     "        if (!declared.family) {",
     COMPILER, "event graph: an undeclared*"),
    ("c05_a_name_of_another_kind_is_accepted", EVENTS,
     "        if (declared.kind != kind) {", "        if (declared.kind != kind && false) {",
     COMPILER, "event graph: an undeclared*"),
    ("c06_capabilities_unchecked", EVENTS,
     "        !has_capability(graph.granted(), declared->capability)) {",
     "        false) {",
     COMPILER, "event graph: a call outside*"),
    ("c07_pin_types_unchecked", EVENTS,
     "    if (source->type == target->type || registry.converts(source->type, target->type)) {",
     "    if (true) {",
     COMPILER, "event graph: a wire between*"),
    ("c08_duplicate_events_accepted", EVENTS,
     "        if (earlier != nullptr) {", "        if (false) {",
     COMPILER, "event graph: a graph with no event*"),
    ("c09_unreachable_nodes_unreported", EVENTS,
     '        report(sink, Severity::Warning, "script.node.unreachable", node.key,',
     '        (void)sink;\n        (void)node.key;\n        if (false) report(sink, '
     'Severity::Warning, "script.node.unreachable", node.key,',
     COMPILER, "event graph: a node no event reaches*"),
    ("c10_listing_loses_the_node", EVENTS,
     "static_cast<unsigned long long>(site != nullptr ? site->node : 0)",
     "static_cast<unsigned long long>(site != nullptr ? 0 : 0)",
     COMPILER, "event graph: the listing*"),
    ("c11_native_call_drops_its_arguments", NATIVE,
     "    frame.registers[step.dst] = frame.host->call(external, arguments_of(step, frame));",
     "    frame.registers[step.dst] = frame.host->call(external, {});",
     GRAPH_HOST, "graph behaviours: the bytecode and native*"),
    # --- The host -------------------------------------------------------------------------------
    ("c12_cue_plays_at_the_origin", HOST,
     "    play.position[0] = position.x;", "    play.position[0] = 0.0F;",
     GRAPH_HOST, "graph behaviours: an ordered unit*"),
    ("c13_arrival_is_immediate", HOST,
     "    return loaded.waits[index].verb == Verb::Arrived && !current_->moving;",
     "    return loaded.waits[index].verb == Verb::Arrived;",
     GRAPH_HOST, "graph behaviours: an ordered unit*"),
    ("c14_the_step_ignores_the_frame", HOST,
     "    const f32 step = speed * dt;", "    const f32 step = speed + (dt * 0.0F);",
     GRAPH_HOST, "graph behaviours: an ordered unit*"),
    ("c15_a_waiting_unit_ignores_new_orders", HOST,
     "        if (handler == nullptr) {",
     "        if (handler == nullptr || instance.status == GraphInstanceStatus::Waiting) {",
     GRAPH_HOST, "graph behaviours: a newer order*"),
    ("c16_any_cue_binds", HOST,
     "            } else if (audio_->find_cue(binding.cue_name.c_str(), binding.cue) != "
     "CY_RESULT_OK) {",
     "            } else if (false) {",
     GRAPH_HOST, "graph behaviours: a cue the project lacks*"),
    # --- The service ------------------------------------------------------------------------------
    ("c17_catalogue_offers_the_tick_entry", SERVICE,
     'constexpr std::string_view kExcludedTypes[] = {"script.entry"};',
     'constexpr std::string_view kExcludedTypes[] = {"script.none"};',
     ENGINE_SCRIPT, "editor script: the catalogue*"),
    ("c18_diagnostics_lose_their_severity", SERVICE,
     "        out.u8v(severity_of(diagnostic.severity))",
     "        out.u8v(static_cast<u8>(severity_of(diagnostic.severity) & 0U))",
     ENGINE_SCRIPT, "editor script: a misspelled function*"),
    ("c19_raise_drops_its_arguments", SERVICE,
     "        play->raise(entity, Name::intern(event), Span<const f32>(arguments, count));",
     "        play->raise(entity, Name::intern(event), Span<const f32>());",
     ENGINE_SCRIPT, "editor script: the editor's raise*"),
    ("c20_service_does_not_route_script", MATERIAL,
     "        result = dispatch_script(*session, operation, scripts_);",
     '        result = dispatch_script(*session, std::string_view("script.none"), scripts_);',
     ENGINE_SCRIPT, "editor script: the material service*"),
    # --- Play -----------------------------------------------------------------------------------
    ("c21_play_attaches_nothing", RUNTIME,
     "        if (reference.empty()) {", "        if (true) {",
     TWIN, "*Swift twin*"),
    ("c22_the_graph_rounds_differently", HOST,
     "    position.x += dx / distance * step;", "    position.x += dx * step / distance;",
     TWIN, "*Swift twin*"),
    ("c23_a_muted_node_skips_the_external_check", EVENTS,
     "        if (const ExternalUse* use = external_use(node.type); use != nullptr) {",
     "        if (const ExternalUse* use = external_use(node.type); use != nullptr && !node.muted) {",
     COMPILER, "event graph: a muted node*"),
    # --- The editor -----------------------------------------------------------------------------
    ("r01_floats_not_written_as_the_engine_writes_them", GRAPH_RS,
     '    trim_fraction(&format!("{wide:.decimals$}"))', '    format!("{value}")',
     SERVICES_LIB, "floats_are_written_as_c_writes_them"),
    ("r02_properties_not_in_name_order", GRAPH_RS,
     "    properties.sort_by(|a, b| a.name.cmp(&b.name));\n", "",
     SERVICES_LIB, "a_graph_built_in_any_order_writes_the_engines_order"),
    ("r03_empty_name_not_the_zero_tuple", GRAPH_RS,
     '            Self::Text(_) => out.push_str("(0, 0, 0, 0, 0)"),',
     '            Self::Text(_) => out.push_str("\\"\\""),',
     SERVICES_LIB, "a_graph_built_in_any_order_writes_the_engines_order"),
    ("r04_raise_drops_its_arguments", GRAPH_RS,
     "    out.u32(u32::try_from(count).unwrap_or(3));",
     "    out.u32(0);",
     SERVICES_LIB, "the_raise_is_the_bytes_the_engines_suite_submits"),
    ("r05_save_not_undoable", EDITOR_RS,
     '                kind: format!("{}{reference}", crate::script_graph::DOMAIN_PREFIX),',
     '                kind: format!("unrecorded:{reference}"),',
     MCP, ACCEPTANCE),
    ("r06_undo_does_not_restore_the_file", BUILTIN_RS,
     "    for (reference, source) in script_graphs {\n        let _ = project.put_source(",
     "    for (reference, source) in script_graphs {\n        let _ = (",
     MCP, ACCEPTANCE),
    ("r07_added_nodes_lack_the_engines_defaults", AUTHORING_RS,
     "                    canvas.set_property_by_identity(node, identity, default)?;",
     "                    let _ = (node, identity, default);",
     MCP, ACCEPTANCE),
    ("r08_attach_is_a_no_op", COMMANDS_RS,
     "        document.add_component(node, component, vec![(field, value)])",
     "        {\n            let _ = (component, field, value);\n            Ok(())\n        }",
     MCP, ACCEPTANCE),
    ("r09_raise_names_no_entity", COMMANDS_RS,
     "                crate::mirror::engine_identity(entity),", "                0,",
     MCP, ACCEPTANCE),
    ("r10_a_report_forgets_what_it_compiled", REQUESTS_RS,
     "(queued.source.clone(), report)", "(String::new(), report)",
     PANELS, "the_gameplay_graph_panel_offers_the_engines_events"),
    ("r11_capture_drops_unknown_nodes", CANVAS_RS,
     "        .filter(|node| catalogue.get(&node.type_name).is_none())",
     "        .filter(|_| false)",
     INTERFACE_LIB, "a_node_the_catalogue_lacks_is_kept_with_its_wires"),
    ("r12_catalogue_never_marked_ready", DOMAINS_RS,
     "        self.script_catalogue_ready = true;", "        self.script_catalogue_ready = false;",
     PANELS, "the_gameplay_graph_panel_offers_the_engines_events"),
    ("r13_a_diagnostic_row_selects_nothing", PANEL_RS,
     "        let _ = canvas.select([key]);", "        let _ = key;",
     PANELS, "an_engine_diagnostic_is_on_its_node_and_selects_it"),
    ("r14_the_palette_is_empty", PANEL_RS,
     "graph_canvas::node_palette(ui, entries, true)",
     "graph_canvas::node_palette(ui, Vec::new(), true)",
     PANELS, "gameplay_graph_gestures_are_the_registered_script_commands"),
    ("r15_compile_asked_every_frame", PANEL_RS,
     "        || asked\n", "",
     PANELS, "a_graph_the_engine_has_not_compiled_is_sent_to_it_once"),
    ("r17_opening_refuses_a_value_the_palette_would", CANVAS_RS,
     "        Some(declared) => canvas.restore_property_by_identity(",
     "        Some(declared) => canvas.set_property_by_identity(",
     INTERFACE_LIB, "a_value_the_palette_would_refuse_still_opens_and_is_kept"),
    ("r16_panel_invokes_an_unregistered_command", PANEL_RS,
     '        "script.debug.inspect",\n    ];', '        "script.reload",\n    ];',
     SHELL_LIB, "every_scaffolded_tool_is_an_undoable_mcp_peer_of_its_panel"),
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
        "Mutations that turn the gameplay graph editor's tests red (#29, visual scripting),",
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
