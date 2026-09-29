#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mutation driver for the editor audio tools (#29).

Each mutation replaces one exact snippet (which must occur once), runs the named tests, records
which failed, restores the file and verifies its md5. Rust mutations run `cargo test` through the
tree's target directory and job pool; C++ mutations rebuild one CTest executable in the Development
build tree (`build/dev`, or CY_BUILD_DIR) with CY_JOBS jobs and run it. A mutation none of its
tests notices is written as SURVIVED and makes the driver exit non-zero.

Run from anywhere: python3 openspec/changes/add-editor-audio-tools/evidence/mutate.py
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

AUDIO = "editor/crates/cy-editor-services/src/audio.rs"
EDITOR = "editor/crates/cy-editor-services/src/editor.rs"
BUILTIN = "editor/crates/cy-editor-services/src/builtin.rs"
COMMANDS = "editor/crates/cy-editor-services/src/audio_commands.rs"
PANEL = "editor/crates/cy-editor-shell/src/panels/audio_mixer.rs"
DOMAINS = "editor/crates/cy-editor-interface/src/specialised/mod.rs"
AUTHORING = "src/editor_backend/src/audio_authoring.cpp"
SERVICE = "src/editor_backend/src/material_service.cpp"
OVERLAY = "samples/05b-editor-window/runtime/overlay.cpp"

SERVICES_LIB = ("rust", "cy-editor-services", ["--lib"])
INTERFACE_LIB = ("rust", "cy-editor-interface", ["--lib"])
SHELL_LIB = ("rust", "cy-editor-shell", ["--lib"])
PANELS = ("rust", "cy-editor-shell", ["--test", "new_panels_are_accessible"])
MCP = ("rust", "cy-editor-mcp", ["--test", "a_session_over_the_wire"])
ENGINE_AUDIO = ("cpp", "cy_test_integration_editor_backend_audio", [])
OVERLAY_SUITE = ("cpp", "cy_test_integration_editor_window_overlay", [])
RUNTIME_UNIT = ("cpp", "cy_test_unit_editor_window_runtime", [])

# (name, file, before, after, target, test filter)
MUTATIONS = [
    ("r01_mixer_admits_a_cycle", AUDIO,
     "        if self.has_cycle() {", "        if false && self.has_cycle() {",
     SERVICES_LIB, "an_edit_that_would_cycle_or_dangle_is_refused"),
    ("r02_mixer_writes_unit_gain", AUDIO,
     "                bus.volume,\n", "                1.0_f32,\n",
     SERVICES_LIB, "the_editors_mixer_is_the_text_the_engine_applies"),
    ("r03_state_ignores_engine_gain", AUDIO,
     "        volume: reader.f32()?,", "        volume: { let _ = reader.f32()?; 1.0 },",
     SERVICES_LIB, "the_engines_state_reply_decodes_to_what_its_mixer_applied"),
    ("r04_cues_listing_skips_directories", AUDIO,
     "            if !name.starts_with('.') && name != \"build\" {", "            if false {",
     PANELS, "the_audio_mixer_shows_the_engines_graph_and_levels"),
    ("r05_save_not_undoable", EDITOR,
     '                kind: format!("{}{reference}", crate::audio::DOMAIN_PREFIX),',
     '                kind: format!("unrecorded:{reference}"),',
     MCP, "the_mixer_is_an_undoable_mcp_peer_of_the_panel_and_reaches_the_engine"),
    ("r06_save_not_sent_to_engine", EDITOR,
     "        if is_mixer && self.runtime.is_connected() {",
     "        if false && is_mixer && self.runtime.is_connected() {",
     MCP, "the_mixer_is_an_undoable_mcp_peer_of_the_panel_and_reaches_the_engine"),
    ("r07_listener_ignores_camera", EDITOR,
     "            [forward.x, forward.y, forward.z],", "            [0.0, 0.0, 1.0],",
     MCP, "a_cue_and_a_spatial_source_preview_through_the_engine_over_mcp"),
    ("r08_undo_not_sent_to_engine", BUILTIN,
     "            let _ = project.audio_request(crate::audio::MIXER_APPLY, restored.into_bytes());",
     "            let _ = restored;",
     MCP, "the_mixer_is_an_undoable_mcp_peer_of_the_panel_and_reaches_the_engine"),
    ("r09_panel_flag_inverted", PANEL,
     '                    .with("enabled", Value::Bool(enabled)),',
     '                    .with("enabled", Value::Bool(!enabled)),',
     PANELS, "mixer_gestures_are_the_registered_audio_commands"),
    ("r10_meter_hides_engine_level", PANEL,
     '        Some(_) => format!("{:.1} dB", 20.0 * peak.log10()),',
     '        Some(_) => "no engine level".to_owned(),',
     PANELS, "the_audio_mixer_shows_the_engines_graph_and_levels"),
    ("r11_panel_invokes_an_unregistered_command", PANEL,
     '        "audio.source.preview",\n    ];', '        "audio.source.play",\n    ];',
     SHELL_LIB, "every_scaffolded_tool_is_an_undoable_mcp_peer_of_its_panel"),
    ("r12_source_range_irreversible", COMMANDS,
     'and its curve between, as one undoable transaction.",\n            '
     "EffectClass::ReversibleMutation,",
     'and its curve between, as one undoable transaction.",\n            '
     "EffectClass::IrreversibleMutation,",
     SHELL_LIB, "every_scaffolded_tool_is_an_undoable_mcp_peer_of_its_panel"),
    ("r13_mixer_opens_without_engine_vocabulary", DOMAINS,
     "            || (domain == Domain::AudioBusesAndMixing && self.audio_vocabulary.is_some())",
     "            || domain == Domain::AudioBusesAndMixing",
     INTERFACE_LIB, "the_audio_mixer_opens_only_on_the_engines_vocabulary"),
    ("c01_engine_ignores_bus_gain", AUTHORING,
     "    if (Status set = graph.set_volume(handle, bus.volume); !set) {",
     "    if (Status set = graph.set_volume(handle, 1.0F); !set) {",
     ENGINE_AUDIO, "*sets the engine*"),
    ("c02_engine_admits_a_cycle", AUTHORING,
     "    if (Status acyclic = check_acyclic(mixer); !acyclic) {",
     "    if (Status acyclic = check_acyclic(Mixer{}); !acyclic) {",
     ENGINE_AUDIO, "*refused whole*"),
    ("c03_removed_bus_keeps_its_voice", AUTHORING,
     "    stop_voices_on_dead_buses();", "    (void)0;",
     ENGINE_AUDIO, "*sets the engine*"),
    ("c04_preview_gain_is_not_the_curve", AUTHORING,
     "        report.gain = audio::attenuation_gain(attenuation, report.distance);",
     "        report.gain = 1.0F;",
     ENGINE_AUDIO, "*previewing a cue*"),
    ("c05_play_ignores_autoplay", AUTHORING,
     "        if (!source.enabled || !source.autoplay) {", "        if (!source.enabled) {",
     ENGINE_AUDIO, "*Play starts*"),
    ("c06_stop_leaves_play_sounding", AUTHORING,
     "    for (const ActiveVoice& active : play_voices_) {\n        (void)server_.stop(active.voice);",
     "    for (const ActiveVoice& active : play_voices_) {\n        (void)active;",
     ENGINE_AUDIO, "*Play starts*"),
    ("c07_play_ignores_project_mixer", AUTHORING,
     "    if (Status mixed = apply_project_mixer(); !mixed) {",
     "    if (Status mixed = ok(); !mixed) {",
     ENGINE_AUDIO, "*Play starts*"),
    ("c08_cues_not_named_for_swift", AUTHORING,
     "    if (Status named = adapter_.add_cue(Name::intern(stem_of(name)), *created); !named) {",
     "    if (Status named = adapter_.add_cue(Name::intern(std::string(stem_of(name)) + \".misnamed\"), *created);\n"
     "        !named) {",
     ENGINE_AUDIO, "*Play starts*"),
    ("c09_wav_rate_unchecked", AUTHORING,
     "        format.rate != AudioAuthoring::kSampleRate ||", "",
     ENGINE_AUDIO, "*WAV clip*"),
    ("c10_source_radii_unchecked", AUTHORING,
     "    if (!(marker.min_distance > 0.0F) || !(marker.max_distance > marker.min_distance) ||",
     "    if (!(marker.min_distance > 0.0F) ||",
     ENGINE_AUDIO, "*attenuation radii*"),
    ("c11_service_has_no_audio", SERVICE,
     "        result = dispatch_audio(*session, operation, audio_);",
     "        result = dispatch_audio(*session, operation, nullptr);",
     ENGINE_AUDIO, "*"),
    ("c12_silence_ring_not_dashed", OVERLAY,
     "    perimeter(canvas, x, y, outer_radius, 1.2F, colour, 4.0F);",
     "    perimeter(canvas, x, y, outer_radius, 1.2F, colour, 0.0F);",
     OVERLAY_SUITE, "*audio source*"),
    ("c13_ring_ignores_radius", OVERLAY,
     "        !render::project_to_pixel(view, offset + (normalize(right) * radius), edge)) {",
     "        !render::project_to_pixel(view, offset + (normalize(right) * (radius * 0.0F + 1.0F)),\n"
     "                                  edge)) {",
     RUNTIME_UNIT, "*radius is projected*"),
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


def main():
    env = cargo_env()
    lines = [
        "Mutations that turn the editor audio tools' tests red (#29), Development profile. Each",
        "mutation was applied, its tests run, the file restored and md5-verified. Listed under",
        "each: the tests (Rust) or test cases (C++) that failed.",
        "",
    ]
    survived = []
    rebuild = set()
    for name, relative, before, after, (kind, unit, target), pattern in MUTATIONS:
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
    lines.append(f"{len(MUTATIONS) - len(survived)} of {len(MUTATIONS)} mutations killed.")
    OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
