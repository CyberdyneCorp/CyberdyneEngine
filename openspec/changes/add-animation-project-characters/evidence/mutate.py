#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mutation driver for the animation panel's project characters and bake (#29, #112's gaps).

Each mutation replaces one exact snippet (which must occur once), runs the named tests, records
which failed, restores the file and verifies its md5. Rust mutations run `cargo test` through the
tree's target directory and job pool; C++ mutations rebuild one CTest executable in the Development
build tree (`build/dev`, or CY_BUILD_DIR) with CY_JOBS jobs and run it. A mutation none of its
tests notices is written as SURVIVED and makes the driver exit non-zero.

Run from anywhere: python3 openspec/changes/add-animation-project-characters/evidence/mutate.py
Pass mutation names to run only those; the record then goes to falsification-<first name>.txt.
Pass --check to verify every snippet occurs exactly once without building anything.
Output: falsification.txt beside this file. Every C++ target is rebuilt unmutated at the end.
The Swift mutations (p05, p06) need a Swift toolchain, which `smoke.editor_animation_events` needs.
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

CLIP_H = "src/animation/include/cy/animation/clip.h"
CHARACTER = "src/editor_backend/src/animation_character.cpp"
PREVIEW = "src/editor_backend/src/animation_preview.cpp"
RIG = "src/editor_backend/src/animation_rig.cpp"
SERVICE = "src/editor_backend/src/animation_service.cpp"
PLAY = "samples/05b-editor-window/runtime/play_animation.cpp"
SCRIPTS = "samples/05b-editor-window/runtime/script_runtime.cpp"
GRAPH_RS = "editor/crates/cy-editor-services/src/animation_graph.rs"
REQUESTS_RS = "editor/crates/cy-editor-services/src/animation_requests.rs"
CHARACTER_RS = "editor/crates/cy-editor-services/src/animation_character.rs"
EDITOR_RS = "editor/crates/cy-editor-services/src/editor.rs"

SERVICES_LIB = ("rust", "cy-editor-services", ["--lib"])
MCP = ("rust", "cy-editor-mcp", ["--test", "a_session_over_the_wire"])
CLIPS = ("cpp", "cy_test_unit_animation", [])
ENGINE = ("cpp", "cy_test_integration_editor_backend_animation", [])
PLAY_SUITE = ("cpp", "cy_test_integration_editor_window_play_animation", [])
SWIFT = ("cpp", "cy_test_smoke_editor_animation_events", [])

ACCEPTANCE = "an_imported_character_is_chosen_previewed_baked_and_undone_over_mcp"

# (name, file, before, after, target, test filter)
MUTATIONS = [
    # --- The runtime clip ----------------------------------------------------------------------
    ("c01_clearing_keeps_the_events", CLIP_H,
     "    void clear_events() noexcept { events_.clear(); }",
     "    void clear_events() noexcept {}",
     CLIPS, "clip: clearing the events*"),
    # --- The character -------------------------------------------------------------------------
    ("c02_a_clips_own_events_are_kept", CHARACTER,
     "        clip.set_name(source.name);\n        clip.clear_events();",
     "        clip.set_name(source.name);",
     ENGINE, "editor animation: a project clip's own events*"),
    ("c03_events_in_written_order", CHARACTER,
     "        return a.time < b.time;",
     "        return a.time < b.time && false;",
     ENGINE, "editor animation: events placed on the timeline fire on the imported clip"),
    ("c04_a_clip_keeps_its_stack_name", CHARACTER,
     "        clip.set_name(source.name);\n",
     "",
     ENGINE, "editor animation: a scrubbed imported clip*"),
    ("c05_a_foreign_clip_is_taken", CHARACTER,
     "    if (!animation::clip_matches_skeleton(decoded, joints.span(), skeleton_, offending)) {",
     "    if (!animation::clip_matches_skeleton(decoded, joints.span(), skeleton_, offending) && false) {",
     ENGINE, "editor animation: a clip cooked for another skeleton*"),
    ("c06_two_clips_of_one_name", CHARACTER,
     "        if (taken.name == wanted.name) {",
     "        if (taken.name == wanted.name && false) {",
     ENGINE, "editor animation: two clips of one name*"),
    ("c07_a_bare_skeleton_draws_nothing", CHARACTER,
     "        if (Status boxed = bone_boxes(); !boxed) {",
     "        if (Status boxed = ok(); !boxed) {",
     ENGINE, "editor animation: the imported mesh is the character's skin*"),
    ("c08_a_mesh_of_other_joints_is_taken", CHARACTER,
     "[this](const u16 joint) { return joint < skeleton_.joint_count(); }",
     "[](const u16 joint) { return joint < 0xFFFFU; }",
     ENGINE, "editor animation: two clips of one name*"),
    # --- The preview ---------------------------------------------------------------------------
    ("c09_a_project_clip_loops_as_cooked", PREVIEW,
     "        if (found != nullptr && character_->from_project()) {",
     "        if (found != nullptr && character_->from_project() && false) {",
     ENGINE, "editor animation: a project clip plays on or holds*"),
    ("c10_a_failed_character_replaces_the_one_that_played", PREVIEW,
     "    } else if (Status read = loaded->load(request, *source_); !read) {\n        return read;\n    }",
     "    } else {\n        (void)loaded->load(request, *source_);\n    }",
     ENGINE, "editor animation: a character that does not load*"),
    ("c11_a_new_character_keeps_the_preview", PREVIEW,
     "    stop();\n    state_ = AnimationPreviewState{};\n",
     "",
     ENGINE, "editor animation: a character that does not load*"),
    ("c12_the_mesh_is_not_uploaded_again", PREVIEW,
     "    ++mesh_generation_;\n",
     "",
     ENGINE, "editor animation: two clips of one name*"),
    # --- The bake ------------------------------------------------------------------------------
    ("c13_a_bake_checks_no_clip", RIG,
     "compile_animation_graph(&character, request.source, compilation)",
     "compile_animation_graph(nullptr, request.source, compilation)",
     ENGINE, "editor animation: a graph with an error bakes nothing*"),
    ("c14_a_bake_drops_the_events", RIG,
     "character.build_clips(compilation.events.span(), clips)",
     "character.build_clips({}, clips)",
     ENGINE, "editor animation: a bake cooks the authored events*"),
    ("c15_a_bake_keeps_the_cooked_loop", RIG,
     "        clip->set_loop_mode(reference.looping ? animation::LoopMode::Loop\n"
     "                                              : animation::LoopMode::None);\n",
     "",
     ENGINE, "editor animation: a baked clip holds or loops*"),
    ("c17_a_manifest_keeps_what_it_held", RIG,
     "    out.rig = Name{};\n    out.model = Name{};\n    out.skeleton = AssetId{};\n",
     "",
     ENGINE, "editor animation: a rig manifest reads back*"),
    ("c18_a_quote_is_written", RIG,
     "        return character == '\"' || character == '\\n' || character == '\\r';",
     "        return character == '\\n' || character == '\\r';",
     ENGINE, "editor animation: a rig manifest reads back*"),
    ("c19_the_mannequin_is_baked", SERVICE,
     "    if (arguments.request.skeleton.is_nil()) {",
     "    if (arguments.request.skeleton.is_nil() && false) {",
     ENGINE, "editor animation: a bake is refused*"),
    ("c20_any_rig_name", SERVICE,
     "    if (!rig_name(request.rig)) {",
     "    if (!rig_name(request.rig) && false) {",
     ENGINE, "editor animation: a bake is refused*"),
    ("c21_no_character_operation", SERVICE,
     '    if (operation == "animation.character.set") {',
     '    if (operation == "animation.character.none") {',
     ENGINE, "editor animation: an imported character plays its own clips*"),
    # --- Play ----------------------------------------------------------------------------------
    ("p01_no_frame_of_events", PLAY,
     "    return loaded_->adapter->begin_frame();",
     "    return ok();",
     PLAY_SUITE, "Play loads a baked rig*"),
    ("p02_the_rig_is_misnamed", PLAY,
     "loaded_->adapter->add_rig(name.c_str(), *added)",
     "loaded_->adapter->add_rig(\"rig\", *added)",
     PLAY_SUITE, "Play loads a baked rig*"),
    ("p03_a_manifest_reaches_anywhere", PLAY,
     "    return !path.empty() && path.front() != '/' && path.find(\"..\") == std::string_view::npos &&",
     "    return !path.empty() &&",
     PLAY_SUITE, "Play skips a rig that does not load*"),
    ("p04_the_tick_runs_no_animation", PLAY,
     "    if (Status ran = loaded_->system->run(1, seconds, nullptr); !ran) {",
     "    if (Status ran = loaded_->system->run(0, seconds, nullptr); !ran) {",
     PLAY_SUITE, "Play loads a baked rig*"),
    ("p05_no_on_update", SCRIPTS,
     "        runtime_->frame_update(dt);",
     "        (void)dt;",
     SWIFT, "a Swift behaviour receives the timeline's events*"),
    ("p06_swift_reaches_no_animation", SCRIPTS,
     "    host_.game.animation = animation_;",
     "    host_.game.animation = nullptr;",
     SWIFT, "a Swift behaviour receives the timeline's events*"),
    # --- The editor ----------------------------------------------------------------------------
    ("r01_the_character_is_always_wanted", REQUESTS_RS,
     "        self.character_sent.as_ref() != Some(choice)",
     "        self.character_sent.as_ref() != Some(choice) || true",
     SERVICES_LIB, "a_character_is_sent_once"),
    ("r02_the_vocabulary_is_kept", REQUESTS_RS,
     "                    // one's. The engine stopped its preview.\n                    self.catalogue_requested = false;",
     "                    // one's. The engine stopped its preview.",
     SERVICES_LIB, "a_character_is_sent_once"),
    ("r03_the_preview_goes_without_its_character", EDITOR_RS,
     "        self.ensure_animation_character(&settings.reference)?;",
     "",
     MCP, ACCEPTANCE),
    ("r04_an_undone_choice_is_not_shown", EDITOR_RS,
     "            self.character_follows(&graph);",
     "            let _ = graph;",
     MCP, ACCEPTANCE),
    ("r05_a_bake_keeps_stale_files", CHARACTER_RS,
     "    match std::fs::remove_dir_all(directory) {",
     "    match Ok::<(), std::io::Error>(()) {",
     MCP, ACCEPTANCE),
    ("r06_a_bake_writes_anywhere", GRAPH_RS,
     "            if path.is_empty() || !inside {",
     "            if path.is_empty() {",
     SERVICES_LIB, "a_bake_that_names_a_file_outside_its_rig_is_refused"),
    ("r07_a_mesh_is_a_character", GRAPH_RS,
     '        if name.starts_with("skeleton/")',
     '        if name.starts_with("mesh/")',
     SERVICES_LIB, "a_project_character_is_a_model"),
    ("r08_a_clip_keeps_its_prefix", GRAPH_RS,
     '            let leaf = entry.sub_asset.as_deref()?.strip_prefix("animation/")?;',
     '            let leaf = entry.sub_asset.as_deref().filter(|sub| sub.starts_with("animation/"))?;',
     SERVICES_LIB, "a_project_character_is_a_model"),
    ("r09_the_mannequin_is_sent_to_bake", CHARACTER_RS,
     "        if choice.is_mannequin() {",
     "        if choice.is_mannequin() && false {",
     MCP, ACCEPTANCE),
    ("r10_choosing_the_mannequin_keeps_the_file", CHARACTER_RS,
     "                    self.remove_graph_source(&file, DOMAIN_PREFIX, \"animation character\")?;",
     "",
     MCP, ACCEPTANCE),
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
        "Mutations that turn the project-character and bake tests red (#29 Animation, #112's gaps),",
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
