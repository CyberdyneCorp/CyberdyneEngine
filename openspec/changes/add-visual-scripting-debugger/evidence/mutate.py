#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mutation driver for the gameplay graph Play debugger and hot reload (#84, #29).

Each mutation replaces one exact snippet (which must occur once), runs the named tests, records
which failed, restores the file and verifies its md5. Rust mutations run `cargo test` through the
tree's target directory and job pool; C++ mutations rebuild one CTest executable in the Development
build tree (`build/dev`, or CY_BUILD_DIR) with CY_JOBS jobs and run it. A mutation none of its
tests notices is written as SURVIVED and makes the driver exit non-zero.

`d07` is about Profile and Shipping, where the debugger is compiled out, so it runs against a
Shipping tree: CY_RELEASE_BUILD_DIR (default `build/release`), configured with `--profile release`.

Run from anywhere: python3 openspec/changes/add-visual-scripting-debugger/evidence/mutate.py
Pass mutation names to run only those; the record then goes to falsification-<first name>.txt.
Pass --check to verify every snippet occurs exactly once without building anything.
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
RELEASE = pathlib.Path(os.environ.get("CY_RELEASE_BUILD_DIR", ROOT / "build" / "release"))

SCRIPT = "src/graph/src/lower_script.cpp"
NATIVE = "src/graph/src/lower_script_native.cpp"
DEBUG = "src/graph/src/script_debug.cpp"
RELOAD = "src/graph/src/script_reload.cpp"
EVENTS = "src/graph/src/event_script.cpp"
HOST = "src/game_backend/src/graph_behaviours.cpp"
SERVICE = "src/editor_backend/src/script_service.cpp"
RUNTIME = "samples/05b-editor-window/runtime/graph_runtime.cpp"
REQUESTS_RS = "editor/crates/cy-editor-services/src/script_requests.rs"
DEBUG_RS = "editor/crates/cy-editor-services/src/script_debug.rs"
EDITOR_RS = "editor/crates/cy-editor-services/src/editor.rs"
BUILTIN_RS = "editor/crates/cy-editor-services/src/builtin.rs"
PANEL_RS = "editor/crates/cy-editor-shell/src/panels/script_graph.rs"

SERVICES_LIB = ("rust", "cy-editor-services", ["--lib"])
PANELS = ("rust", "cy-editor-shell", ["--test", "new_panels_are_accessible"])
MCP = ("rust", "cy-editor-mcp", ["--test", "a_session_over_the_wire"])
COMPILER = ("cpp", "cy_test_integration_graph_compiler", [])
COMPILER_SHIPPING = ("cpp-release", "cy_test_integration_graph_compiler", [])
GRAPH_HOST = ("cpp", "cy_test_integration_game_backend_graph", [])
ENGINE_SCRIPT = ("cpp", "cy_test_integration_editor_backend_script", [])
HOSTED = ("cpp", "cy_test_integration_editor_window_graph_debugger", [])

SESSION = "a_gameplay_graph_is_debugged_and_reloaded_in_play_over_mcp"

# (name, file, before, after, target, test filter)
MUTATIONS = [
    # --- The compiled program's debugger ------------------------------------------------------
    ("d01_bytecode_probe_never_breaks", SCRIPT,
     "    if (debug->on_probe(site, state) != DebugVerdict::Break) {",
     "    if (true) {",
     COMPILER, "graph debugger: a breakpoint stops*"),
    ("d02_native_probe_never_breaks", NATIVE,
     "        if (frame.debug->on_probe(site, *frame.state) != DebugVerdict::Break) {",
     "        if (true) {",
     COMPILER, "graph debugger: a breakpoint stops*"),
    ("d03_native_resumes_past_the_next_instruction", NATIVE,
     "        frame.state->pause_at(site.block, site.offset + 1);",
     "        frame.state->pause_at(site.block, site.offset + 2);",
     COMPILER, "graph debugger: a breakpoint stops*"),
    ("d04_no_probe_on_the_event", DEBUG,
     "        if (entry != kInvalidNodeKey && entry != first_node) {",
     "        if (false) {",
     COMPILER, "graph debugger: a breakpoint on the event*"),
    ("d05_a_call_is_not_an_execution_node", DEBUG,
     "        case ScriptOp::Call:\n        case ScriptOp::EmitEvent:",
     "        case ScriptOp::EmitEvent:",
     COMPILER, "graph debugger: stepping*"),
    ("d06_a_pin_reads_nothing", DEBUG,
     "        reading.value = state.registers()[instruction.dst];",
     "        reading.value = Value{};",
     COMPILER, "graph debugger: pins*"),
    ("d07_instrumented_in_shipping", DEBUG,
     "    if constexpr (!kGraphDebuggerEnabled) {\n        (void)program;",
     "    if constexpr (false) {\n        (void)program;",
     COMPILER_SHIPPING, "graph debugger: compiled out*"),
    ("d08_variables_start_at_zero", SCRIPT,
     "            registers_[variable.reg] = variable.initial;",
     "            registers_[variable.reg] = Value{};",
     COMPILER, "graph reload: a running counter*"),
    ("d09_an_undeclared_variable_compiles", EVENTS,
     "        if (!declared) {\n            error(sink, name.is_empty()",
     "        if (false) {\n            error(sink, name.is_empty()",
     COMPILER, "graph variables: every misuse*"),
    # --- Migration ----------------------------------------------------------------------------
    ("m01_a_kept_variable_loses_its_value", RELOAD,
     "        target.registers()[next.reg] = read_variable(*previous, source);",
     "        (void)source;",
     COMPILER, "graph reload: a running counter*"),
    ("m02_a_type_change_is_migrated", RELOAD,
     "        if (previous == nullptr || previous->kind == next.kind) {",
     "        if (true) {",
     COMPILER, "graph reload: a variable that changed type*"),
    ("m03_a_wait_is_always_dropped", RELOAD,
     "            target.set_resume_block(point->resume);",
     "            (void)point;",
     COMPILER, "graph reload: a wait in progress*"),
    # --- Play ---------------------------------------------------------------------------------
    ("g01_a_breakpoint_stops_every_entity", HOST,
     "(!breakpoint.entity.valid() || breakpoint.entity == current_->entity)",
     "true",
     GRAPH_HOST, "graph debugger: a breakpoint stops the tick*"),
    ("g02_held_work_is_dropped", HOST,
     "            return resume_from(held.instance + 1);",
     "            return ok();",
     GRAPH_HOST, "graph debugger: the work a break held*"),
    ("g03_a_tick_runs_while_paused", HOST,
     "Status GraphBehaviours::update(f32 dt) noexcept {\n    if (paused()) {",
     "Status GraphBehaviours::update(f32 dt) noexcept {\n    if (false) {",
     GRAPH_HOST, "graph debugger: a breakpoint stops the tick*"),
    ("g04_step_over_stops_at_data_nodes", HOST,
     "(step_ == GraphStep::Into || site.executes)",
     "(step_ == GraphStep::Into || true)",
     GRAPH_HOST, "graph debugger: a step visits*"),
    ("g05_a_reload_is_never_applied", HOST,
     "    if (Status reloaded = apply_reloads(); !reloaded) {",
     "    if (Status reloaded = ok(); !reloaded) {",
     GRAPH_HOST, "graph reload: a running counter*"),
    ("g06_the_trace_is_not_kept", HOST,
     "        (void)trace_.push_back(entry);",
     "        (void)entry;",
     GRAPH_HOST, "graph debugger: a step visits*"),
    # --- The backend service ------------------------------------------------------------------
    ("s01_a_breakpoints_entity_is_not_resolved", SERVICE,
     "    if (entity != 0) {",
     "    if (false) {",
     ENGINE_SCRIPT, "editor script: the debugger stops Play*"),
    ("s02_watches_read_nothing", SERVICE,
     "            const graph::script::PinReading reading =\n"
     "                graphs->watch_pin(instance, watch.node, watch.pin);",
     "            const graph::script::PinReading reading{};",
     ENGINE_SCRIPT, "editor script: the debugger stops Play*"),
    ("s03_a_refused_reload_is_not_the_graphs_fault", SERVICE,
     "    if (!staged && sink.entries().empty()) {",
     "    if (!staged) {",
     ENGINE_SCRIPT, "editor script: a reload keeps*"),
    # --- The hosted runtime: what pauses ------------------------------------------------------
    ("u01_the_session_runs_under_a_break", RUNTIME,
     "            (void)play_->pause();",
     "            (void)0;",
     HOSTED, "a graph breakpoint pauses*"),
    ("u02_the_session_stays_paused", RUNTIME,
     "            (void)play_->resume();",
     "            (void)0;",
     HOSTED, "a graph breakpoint pauses*"),
    # --- The editor ---------------------------------------------------------------------------
    ("r01_breakpoints_are_not_sent_when_play_starts", REQUESTS_RS,
     "        if started && state.debugging {",
     "        if false && started && state.debugging {",
     MCP, SESSION),
    ("r02_saving_does_not_reload", EDITOR_RS,
     "        self.reload_running_graph(reference, Some(source));",
     "",
     MCP, SESSION),
    ("r03_undo_does_not_reload", BUILTIN_RS,
     "        project.script_graph_changed(&reference, source.as_deref());",
     "",
     MCP, SESSION),
    ("r04_graphs_play_does_not_run_are_reloaded", EDITOR_RS,
     "        if running && self.runtime.is_connected() {",
     "        if self.runtime.is_connected() {",
     MCP, SESSION),
    ("r05_a_watch_is_not_kept", EDITOR_RS,
     "            watch.pins.push(pair);",
     "            let _ = pair;",
     MCP, SESSION),
    ("r06_the_reason_is_misread", DEBUG_RS,
     '                0 => "breakpoint",',
     '                0 => "step",',
     SERVICES_LIB, "the_engines_paused_state_decodes"),
    ("r07_the_oldest_node_glows_brightest", DEBUG_RS,
     "        for entry in self.trace.iter().rev() {",
     "        for entry in self.trace.iter() {",
     SERVICES_LIB, "the_engines_paused_state_decodes"),
    ("r08_the_gutter_removes_the_wrong_breakpoint", PANEL_RS,
     '            format!("{:x}", breakpoint.entity)',
     "            String::new()",
     PANELS, "the_debuggers_controls_and_gutter_are_the_registered_debug_commands"),
    ("r09_step_over_steps_into", PANEL_RS,
     '("Step Over", "script.debug.step", Some("over"), paused)',
     '("Step Over", "script.debug.step", Some("into"), paused)',
     PANELS, "the_debuggers_controls_and_gutter_are_the_registered_debug_commands"),
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


def run_cpp(env, executable, pattern, tree=BUILD):
    jobs = env.get("CY_JOBS", "4")
    build = subprocess.run(["ninja", "-C", str(tree), "-j", jobs, executable], cwd=ROOT,
                           capture_output=True, text=True)
    if build.returncode != 0:
        return build.returncode, [], "", False, build.stdout + build.stderr
    result = subprocess.run([str(tree / executable), f"--test-case={pattern}"], cwd=ROOT,
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
        "Mutations that turn the Play debugger's and hot reload's tests red (#84, #29),",
        "Development profile (d07: Shipping). Each mutation was applied, its tests run, the file restored and",
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
                tree = RELEASE if kind == "cpp-release" else BUILD
                rebuild.add((tree, unit))
                code, failed, summary, compiled, text = run_cpp(env, unit, pattern, tree)
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
    for tree, unit in sorted(rebuild):
        subprocess.run(["ninja", "-C", str(tree), "-j", env.get("CY_JOBS", "4"), unit],
                       cwd=ROOT, check=True, capture_output=True)
    lines.append(f"{len(mutations) - len(survived)} of {len(mutations)} mutations killed.")
    out.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
