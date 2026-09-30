#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mutation driver for the editor's lighting tools on the lightmap bake pipeline (#29, Lighting).

Each mutation replaces one exact snippet (which must occur once), runs the named tests, records
which failed, restores the file and verifies its md5. A mutation none of its tests notices is written
as SURVIVED and makes the driver exit non-zero. Output: falsification.txt beside this file.

Three kinds of target. A Rust target runs `cargo test` in the editor workspace with the tree's cargo
target directory and job pool, as `just build-editor-check` does. A C++ target rebuilds one test
executable in the engine's build tree (`CY_BUILD_DIR`, default build/dev) and runs the named doctest
case. A CLI target rebuilds `cy_build` and runs `tools/build/tests/test_lightmap_cli.py` over it.
The Rust cases that drive the real `cy_build` find the tree's build/dev one, so a mutation of the
tool is also seen from the editor's side.

Run from anywhere: python3 openspec/changes/add-editor-lighting-tools/evidence/mutate.py [name...]
Naming mutations runs only those and leaves falsification.txt alone.
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
BUILD = pathlib.Path(os.environ.get("CY_BUILD_DIR", ROOT / "build/dev"))

LIGHTING = "editor/crates/cy-editor-services/src/lighting.rs"
DESCRIPTION = "editor/crates/cy-editor-services/src/lightmap_description.rs"
LIGHTMAPS = "editor/crates/cy-editor-services/src/lightmaps.rs"
PANEL = "editor/crates/cy-editor-shell/src/panels/lighting.rs"
INSPECTOR = "editor/crates/cy-editor-shell/src/panels/inspector.rs"
TYPES = "src/servers/render/src/types.cpp"
BAKE = "src/rendering/lightmap_bake/src/bake.cpp"
PROBES = "src/rendering/lightmap_bake/src/probes.cpp"
PRODUCER = "tools/build/src/lightmap_producer.cpp"
CLI = "tools/build/src/main.cpp"

LIB_SERVICES = ("rust", "cy-editor-services", ["--lib"])
AUTHORED = ("rust", "cy-editor-services", ["--test", "the_bake_is_of_the_authored_world"])
LIB_VIEWPORT = ("rust", "cy-editor-viewport", ["--lib"])
PANELS_TEST = ("rust", "cy-editor-shell", ["--test", "new_panels_are_accessible"])
MCP_WIRE = ("rust", "cy-editor-mcp", ["--test", "a_session_over_the_wire"])
BAKE_SUITE = ("cmake", "cy_test_integration_render_lightmap_bake", [])
CONTENT_SUITE = ("cmake", "cy_test_integration_build_content", [])
CLI_SUITE = ("cli", "cy_build", [])

# (name, file, before, after, target, test filter)
MUTATIONS = [
    ("r01_volume_edit_writes_nothing", LIGHTING,
     "            for (field, value) in grid.values(fields) {\n"
     "                document.set_field(volume, fields.component, field, value)?;\n",
     "            for (field, value) in grid.values(fields).into_iter().take(0) {\n"
     "                document.set_field(volume, fields.component, field, value)?;\n",
     LIB_SERVICES, "a_volume_is_placed_and_edited"),
    ("r02_mobility_ignores_the_choice", LIGHTING,
     "let value = Value::Text(mobility.keyword().to_string());",
     "let value = Value::Text(Mobility::Stationary.keyword().to_string());",
     MCP_WIRE, "lighting_authoring_is_undoable_over_mcp"),
    ("r03_resolution_not_written", LIGHTING,
     "(fields.scale, Value::Float(scale)),",
     "(fields.scale, Value::Float(1.0)),",
     LIB_SERVICES, "resolution_is_authored_per_object"),
    ("r04_description_drops_mobility", DESCRIPTION,
     "let mobility = scene.mobility_of(node).unwrap_or_default().keyword();",
     "let mobility = crate::lighting::Mobility::default().keyword();",
     AUTHORED, "what_the_author_changes"),
    ("r05_description_drops_resolution", DESCRIPTION,
     "            number(scene.resolution_of(node)),\n",
     "            number(1.0),\n",
     AUTHORED, "the_description_is_the_authored_world"),
    ("r06_description_drops_volumes", DESCRIPTION,
     "        .chain(volume_lines(document, binding, &scene))\n",
     "        .chain(Vec::<String>::new())\n",
     AUTHORED, "the_description_is_the_authored_world"),
    ("r07_occluder_not_written", DESCRIPTION,
     "        if !receives {\n            line.push_str(\" occluder\");\n",
     "        if !receives {\n            line.push_str(\"\");\n",
     AUTHORED, "the_description_is_the_authored_world"),
    ("r08_parents_ignored", DESCRIPTION,
     "    chain.iter().rev().fold(IDENTITY, |outer, node| {",
     "    chain.iter().take(1).fold(IDENTITY, |outer, node| {",
     LIB_SERVICES, "a_child_is_written_where_its_parents_put_it"),
    ("r09_tint_dropped", DESCRIPTION,
     "        let _ = write!(line, \" tint {}\", numbers(&tint));\n",
     "        let _ = (&mut line, tint);\n",
     LIB_SERVICES, "an_object_bakes_with_the_material_it_draws"),
    ("r10_slot_ignored", DESCRIPTION,
     "    let reference = slot.or(primary).unwrap_or_default().to_string();",
     "    let reference = primary.or(slot).unwrap_or_default().to_string();",
     LIB_SERVICES, "an_object_bakes_with_the_material_it_draws"),
    ("r11_disabled_light_baked", DESCRIPTION,
     "        field(document, node, LIGHT, \"enabled\"),\n        Some(Value::Bool(false))\n",
     "        field(document, node, LIGHT, \"enabled\"),\n        Some(Value::Bool(false)) if false\n",
     LIB_SERVICES, "a_disabled_light_is_not_baked"),
    ("r12_always_rewritten", DESCRIPTION,
     "    if std::fs::read(path).is_ok_and(|held| held == text.as_bytes()) {",
     "    if std::fs::read(path).is_ok_and(|held| held == text.as_bytes()) && false {",
     AUTHORED, "an_unchanged_world_rewrites_nothing"),
    ("r13_bake_skips_the_description", LIGHTMAPS,
     "    let rewritten = context.write_lightmap_description(&path, &text)?;",
     "    let rewritten = !text.is_empty();",
     AUTHORED, "a_bake_with_no_description"),
    ("r14_cached_misread", LIGHTMAPS,
     "        cached: optional(\"cached\") == 1,",
     "        cached: false,",
     AUTHORED, "the_engine_bakes_what_was_authored"),
    ("r15_probe_validity_misread", LIGHTMAPS,
     "                valid: floats[21] > 0.5,",
     "                valid: true,",
     LIB_SERVICES, "the_captured_probes_decode"),
    ("r16_inspector_rows_gone", INSPECTOR,
     "    super::lighting::inspector_rows(panels, ui);\n",
     "",
     PANELS_TEST, "a_light_offers_its_mobility"),
    ("r17_grid_keeps_a_stale_edit", PANEL,
     ".is_none_or(|(node, base, _)| node != volume.id || base != authored)",
     ".is_none_or(|(node, _, _)| node != volume.id)",
     PANELS_TEST, "the_selected_volume_grid_is_applied_as_one_command"),
    ("r18_finished_bake_not_collected", PANEL,
     "                lighting.baked = LightingForm::baked(panels.editor, &completion);\n",
     "                let _ = LightingForm::baked(panels.editor, &completion);\n",
     PANELS_TEST, "a_finished_bake_shows_what_it_made"),
    ("r19_engine_names_the_mode_differently", TYPES,
     '    "GiProbes",\n',
     '    "GIProbes",\n',
     LIB_VIEWPORT, "the_mode_list_matches_the_engines"),
    ("c01_interval_ignores_samples", BAKE,
     "    const u32 interval = lightmap_progress_interval(settings.trace.samples);",
     "    const u32 interval = kProgressTexels;",
     BAKE_SUITE, "a many-sample bake*"),
    ("c02_volumes_not_captured", BAKE,
     "            (void)volumes[index]->capture_all(context);\n",
     "            (void)volumes[index];\n",
     CONTENT_SUITE, "the description's volume*"),
    ("c03_capture_ignores_the_cancel", BAKE,
     "        if (progress != nullptr && progress->cancelled()) {\n            return cancelled(report);\n        }\n        if (volumes[index] != nullptr) {",
     "        if (volumes[index] != nullptr) {",
     BAKE_SUITE, "a capture cancelled between volumes*"),
    ("c04_probe_payload_drops_validity", PROBES,
     "        if (Status put = put_f32(out, probe.validity); !put) {",
     "        if (Status put = put_f32(out, 0.0F); !put) {",
     BAKE_SUITE, "captured probes round-trip*"),
    ("c05_light_id_ignored", PRODUCER,
     "            light.id = *id;\n",
     "            (void)*id;\n",
     CONTENT_SUITE, "the editor's description reads back*"),
    ("c06_instance_id_ignored", PRODUCER,
     "        instance.id = *id;\n",
     "        (void)*id;\n",
     CONTENT_SUITE, "the editor's description reads back*"),
    ("c07_occluder_ignored", PRODUCER,
     "            instance.receives_lightmap = false;\n",
     "            (void)instance;\n",
     CONTENT_SUITE, "the editor's description reads back*"),
    ("c08_mobility_word_ignored", PRODUCER,
     "        if (Status parsed = parse_mobility(line.word(at), light.mobility); !parsed) {",
     "        gi::LightMobility ignored{};\n"
     "        if (Status parsed = parse_mobility(line.word(at), ignored); !parsed) {",
     CONTENT_SUITE, "a light's mobility*"),
    ("c09_resolution_ignored", PRODUCER,
     "    instance.resolution_scale = number(line, 4);",
     "    instance.resolution_scale = 1.0F;",
     CONTENT_SUITE, "an object's resolution scale*"),
    ("c10_key_ignores_what_it_read", PRODUCER,
     "        inputs.upstreams.push_back(KeyedDigest{input.name, input.hash});",
     "        (void)input;",
     CONTENT_SUITE, "an unchanged level keys the same*"),
    ("c11_old_lights_renumbered", PRODUCER,
     "    light.id = level.lights.size() + 1U;",
     "    light.id = level.lights.size() + 7U;",
     CONTENT_SUITE, "a description written before*"),
    ("c12_empty_grid_accepted", PRODUCER,
     "    if (!(volume.settings.spacing_metres > 0.0F) || probes == 0U || probes > kMaxVolumeProbes ||",
     "    if (!(volume.settings.spacing_metres > 0.0F) || probes > kMaxVolumeProbes ||",
     CONTENT_SUITE, "a description written before*"),
    ("c13_single_output_takes_volumes", PRODUCER,
     "    if (report.volumes > 0U && node.outputs.size() < 2U) {",
     "    if (report.volumes > 0U && node.outputs.size() < 1U) {",
     CONTENT_SUITE, "a lightmap node with a probe output*"),
    ("c14_cooked_albedo_ignored", PRODUCER,
     "    out.albedo = Vec3{material.base_colour[0], material.base_colour[1], material.base_colour[2]};",
     "    out.albedo = Vec3{0.5F, 0.5F, 0.5F};",
     CLI_SUITE, ""),
    ("c15_tint_ignored", PRODUCER,
     "            material.albedo = cwise_mul(material.albedo, triple(line, 5));",
     "            (void)line;",
     CLI_SUITE, ""),
    ("c16_never_cached", CLI,
     "        !cached.empty()) {",
     "        cached.empty() && false) {",
     CLI_SUITE, ""),
    ("c17_stale_probes_kept", CLI,
     "        std::filesystem::remove(probes, ignored);",
     "        (void)probes;",
     CLI_SUITE, ""),
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


def run_cmake(env, executable, pattern):
    jobs = env.get("CY_JOBS", "4")
    build = subprocess.run(["cmake", "--build", str(BUILD), "--target", executable, "--parallel",
                            jobs], cwd=ROOT, capture_output=True, text=True, env=env)
    if build.returncode != 0:
        return build.returncode, [], "", False, build.stdout + build.stderr
    binary = next(BUILD.rglob(executable), None)
    if binary is None:
        return 1, [], "", False, f"{executable} was built but not found under {BUILD}"
    result = subprocess.run([str(binary), f"--test-case={pattern}"], cwd=ROOT,
                            capture_output=True, text=True, env=env, timeout=900)
    text = result.stdout + result.stderr
    failed = sorted(set(re.findall(r"TEST CASE:\s+(.+)$", text, re.MULTILINE)))
    # A case that holds many checks names the assertions the mutation broke, so a failure that
    # was already there is not mistaken for the mutation's.
    failed += sorted(set(re.findall(r"^(\S+:\d+): ERROR:", text, re.MULTILINE)))
    summary = re.findall(r"^\[doctest\] test cases:.*$", text, re.MULTILINE)
    return result.returncode, failed, summary[-1] if summary else "", True, text


def run_cli(env):
    jobs = env.get("CY_JOBS", "4")
    build = subprocess.run(["cmake", "--build", str(BUILD), "--target", "cy_build", "--parallel",
                            jobs], cwd=ROOT, capture_output=True, text=True, env=env)
    if build.returncode != 0:
        return build.returncode, [], "", False, build.stdout + build.stderr
    result = subprocess.run(
        [sys.executable, str(ROOT / "tools/build/tests/test_lightmap_cli.py"), "--cy-build",
         str(BUILD / "tools/build/cy_build"), "--cy-import",
         str(BUILD / "tools/import/cy_import_cli")],
        cwd=ROOT, capture_output=True, text=True, env=env, timeout=900)
    text = result.stdout + result.stderr
    failed = re.findall(r"^AssertionError.*$", text, re.MULTILINE) or (
        ["integration.build_lightmap_cli"] if result.returncode != 0 else [])
    last = text.strip().splitlines()[-1] if text.strip() else ""
    return result.returncode, failed, last, True, text


def run(env, target, pattern):
    kind, name, extra = target
    if kind == "rust":
        return run_rust(env, name, extra, pattern)
    if kind == "cli":
        return run_cli(env)
    return run_cmake(env, name, pattern)


def record(lines, name, relative, before, after, outcome):
    code, failed, summary, compiled, _ = outcome
    lines.append(f"== {name}: {relative}")
    for line in before.splitlines() or [""]:
        lines.append(f"<   {line.strip()}")
    for line in after.splitlines() or [""]:
        lines.append(f">   {line.strip()}")
    if not compiled:
        lines.append("DID NOT COMPILE")
        return False
    if code == 0 or not failed:
        lines.append(f"SURVIVED  {summary}")
        return False
    for test in failed:
        lines.append(f"FAILED  {test}")
    lines.append(summary)
    return True


def main(selected):
    env = cargo_env()
    lines = [
        "Mutations that turn the lighting tools' tests red (#29, Lighting, on #68's bake pipeline),",
        "Development profile, GCC 13. Each mutation was applied, its tests run, the file restored",
        "and md5-verified. Listed under each: the tests that failed. r* are editor (Rust)",
        "mutations, c* engine (C++ and cy_build) ones.",
        "",
    ]
    chosen = [m for m in MUTATIONS if not selected or m[0] in selected]
    survived = []
    for name, relative, before, after, target, pattern in chosen:
        path = ROOT / relative
        source = path.read_text(encoding="utf-8")
        if source.count(before) != 1:
            raise SystemExit(f"{name}: expected one occurrence of the snippet in {relative}, "
                             f"found {source.count(before)}")
        digest = md5(path)
        path.write_text(source.replace(before, after), encoding="utf-8")
        try:
            outcome = run(env, target, pattern)
        finally:
            path.write_text(source, encoding="utf-8")
        restored = md5(path) == digest
        if not record(lines, name, relative, before, after, outcome):
            survived.append(name)
            sys.stderr.write(outcome[4][-3000:])
        lines.append("RESTORED md5 ok" if restored else "RESTORE FAILED")
        lines.append("")
        print(f"{name}: {'survived' if name in survived else 'killed'}", flush=True)
        if not restored:
            raise SystemExit(f"{name}: {relative} was not restored")
    # Rebuild what the C++ mutations touched, so the tree's binaries are the restored sources'.
    for _, _, _, _, (kind, executable, _), _ in chosen:
        if kind in ("cmake", "cli"):
            subprocess.run(["cmake", "--build", str(BUILD), "--target", executable, "--parallel",
                            env.get("CY_JOBS", "4")], cwd=ROOT, capture_output=True, env=env)
    lines.append(f"{len(chosen) - len(survived)} of {len(chosen)} mutations killed.")
    if not selected:
        OUT.write_text("\n".join(lines) + "\n", encoding="utf-8")
    else:
        print("\n".join(lines))
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main(set(sys.argv[1:])))
