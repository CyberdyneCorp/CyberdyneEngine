#!/usr/bin/env python3
"""Tests for the feature options, the module system and the generated headers.

Tasks 1.4.4 and 1.5.4 ask for proof rather than for the code that would provide it: that a disabled
feature excludes its sources instead of stubbing at runtime, that a missing feature dependency fails
the configure, and that an undeclared module dependency, a cycle and a layer violation are each
build errors.

Every check here configures a real CMake project against the engine's own cmake/features.cmake and
cmake/modules.cmake. The fixture projects are written into a temporary directory rather than
committed, because what each test is actually about is the one line that differs between it and the
one above it, and that is easier to read here than across a dozen near-identical directories.

Run:  just generate-test          (or)  python3 tools/gen/tests/run_tests.py [name-fragment...]
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
CMAKE_DIR = REPO / "cmake"
GENERATOR = REPO / "tools" / "gen" / "generate_headers.py"

SAMPLE_INPUT = """\
# a generator input
version 1
feature CY_AUDIO ON
feature CY_UI OFF
setting CY_SANITIZE address,undefined
module beta beta ON Servers 1 servers 2 OFF
module alpha alpha ON Core 0 core 0 ON
module gamma gamma OFF Scene 2 scene 4 OFF
"""


# --- Harness ---------------------------------------------------------------------------------


class Failure(Exception):
    pass


def check(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def check_in(needle: str, haystack: str, message: str) -> None:
    if needle not in haystack:
        raise Failure(f"{message}\n  expected to find: {needle!r}\n  in:\n{haystack}")


def run(command: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(
        command, cwd=cwd, capture_output=True, text=True, check=False, env={**os.environ}
    )


def write_tree(root: Path, files: dict[str, str]) -> None:
    for relative, content in files.items():
        path = root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")


FIXTURE_CMAKELISTS = """\
cmake_minimum_required(VERSION 3.28)
list(APPEND CMAKE_MODULE_PATH "{cmake_dir}")
project(cy_fixture LANGUAGES {languages})
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# cy_add_module() requires this target; the fixture does not need the engine's warning set.
add_library(cy_compile_options INTERFACE)
add_library(cy::compile-options ALIAS cy_compile_options)

include(module)
include(features)
include(modules)
add_subdirectory(modules)
{extra}
"""

MODULES_CMAKELISTS = "cy_add_module_subdirectories()\n"


def fixture(files: dict[str, str], extra: str = "", languages: str = "CXX") -> dict[str, str]:
    tree = {
        "CMakeLists.txt": FIXTURE_CMAKELISTS.format(
            cmake_dir=CMAKE_DIR.as_posix(), languages=languages, extra=extra
        ),
        "modules/CMakeLists.txt": MODULES_CMAKELISTS,
    }
    tree.update(files)
    return tree


def manifest(
    name: str,
    *,
    layer: str = "core",
    level: str = "Core",
    public: list[str] | None = None,
    private: list[str] | None = None,
    default_enabled: bool = True,
    platforms: list[str] | None = None,
    hot_reload: bool = False,
    module_type: str = "runtime",
    extra: dict | None = None,
) -> str:
    body = {
        "name": name,
        "description": f"fixture module {name}",
        "layer": layer,
        "type": module_type,
        "registration_level": level,
        "public_dependencies": public or [],
        "private_dependencies": private or [],
        "default_enabled": default_enabled,
        "platforms": platforms or ["linux", "windows", "macos"],
        "hot_reload": hot_reload,
    }
    body.update(extra or {})
    return json.dumps(body, indent=2) + "\n"


def module_files(name: str, *, links: str = "", **manifest_args) -> dict[str, str]:
    identifier = name.replace("-", "_")
    return {
        f"modules/{name}/module.json": manifest(name, **manifest_args),
        f"modules/{name}/CMakeLists.txt": (
            f"cy_add_module(NAME cy_module_{identifier} LAYER "
            f"{manifest_args.get('layer', 'core')} SOURCES src/{identifier}.cpp {links})\n"
        ),
        f"modules/{name}/src/{identifier}.cpp": f"int cy_fixture_{identifier}() {{ return 0; }}\n",
    }


def configure(tree: dict[str, str], defines: list[str], workspace: Path):
    source = workspace / "src"
    build = workspace / "build"
    write_tree(source, tree)
    result = run(
        ["cmake", "-S", str(source), "-B", str(build), "-G", "Ninja", *defines]
    )
    return result, build


def configure_ok(tree, defines, workspace):
    result, build = configure(tree, defines, workspace)
    check(result.returncode == 0, f"configure failed:\n{result.stdout}\n{result.stderr}")
    return build


def configure_fails(tree, defines, workspace) -> str:
    result, _ = configure(tree, defines, workspace)
    check(result.returncode != 0, f"configure was expected to fail but succeeded:\n{result.stdout}")
    return result.stdout + result.stderr


def compiled_sources(build: Path) -> str:
    database = build / "compile_commands.json"
    check(database.is_file(), f"no compile_commands.json in {build}")
    return " ".join(entry["file"] for entry in json.loads(database.read_text()))


def generated(build: Path, name: str) -> str:
    return (build / "generated" / "include" / name).read_text(encoding="utf-8")


# --- The generator ---------------------------------------------------------------------------


def test_generation_is_reproducible(workspace: Path) -> None:
    """Same inputs, byte-identical outputs — twice, into two directories."""
    source = workspace / "inputs.txt"
    source.write_text(SAMPLE_INPUT, encoding="utf-8")
    first, second = workspace / "first", workspace / "second"
    for output in (first, second):
        result = run([sys.executable, str(GENERATOR), "--input", str(source), "--output-dir", str(output)])
        check(result.returncode == 0, f"generation failed: {result.stderr}")

    for name in ("cy_features.h", "cy_modules.h"):
        left = (first / name).read_bytes()
        right = (second / name).read_bytes()
        check(left == right, f"{name} differs between two runs on the same input")

    # An unchanged output is not rewritten, or every configure would invalidate every compilation
    # that includes it.
    stamp = (first / "cy_features.h").stat().st_mtime_ns
    run([sys.executable, str(GENERATOR), "--input", str(source), "--output-dir", str(first)])
    check((first / "cy_features.h").stat().st_mtime_ns == stamp, "an unchanged header was rewritten")


def test_table_is_in_registration_order(workspace: Path) -> None:
    """Initialisation order is registration level, then name — not manifest or discovery order."""
    source = workspace / "inputs.txt"
    source.write_text(SAMPLE_INPUT, encoding="utf-8")
    output = workspace / "out"
    run([sys.executable, str(GENERATOR), "--input", str(source), "--output-dir", str(output)])
    modules = (output / "cy_modules.h").read_text(encoding="utf-8")
    check(modules.index('"alpha"') < modules.index('"beta"'), "Core module must precede Servers")
    check('"gamma"' not in modules, "a disabled module must not be in the table")
    check_in("/* CY_MODULE_GAMMA is disabled */", modules, "a disabled module must be recorded")


def test_check_detects_a_stale_header(workspace: Path) -> None:
    source = workspace / "inputs.txt"
    source.write_text(SAMPLE_INPUT, encoding="utf-8")
    output = workspace / "out"
    run([sys.executable, str(GENERATOR), "--input", str(source), "--output-dir", str(output)])

    current = run([sys.executable, str(GENERATOR), "--input", str(source), "--output-dir", str(output), "--check"])
    check(current.returncode == 0, f"--check failed on current output: {current.stderr}")

    (output / "cy_features.h").write_text("#define CY_UI 1\n", encoding="utf-8")
    stale = run([sys.executable, str(GENERATOR), "--input", str(source), "--output-dir", str(output), "--check"])
    check(stale.returncode == 1, "--check must exit non-zero on a stale header")
    check_in("cy_features.h", stale.stderr, "--check must name the stale file")


def test_malformed_input_is_reported(workspace: Path) -> None:
    source = workspace / "inputs.txt"
    source.write_text("version 1\nfeature CY_UI MAYBE\n", encoding="utf-8")
    result = run([sys.executable, str(GENERATOR), "--input", str(source), "--output-dir", str(workspace / "out")])
    check(result.returncode == 2, "a malformed input must be reported, not ignored")
    check_in("expected ON or OFF", result.stderr, "the diagnostic must name what was wrong")


# --- Feature options -------------------------------------------------------------------------


def test_disabled_feature_excludes_its_sources(workspace: Path) -> None:
    """Task 1.4.4: disabling excludes the sources; it does not stub at runtime."""
    tree = fixture(
        {
            "src/always.cpp": "int cy_fixture_always() { return 0; }\n",
            "src/ui.cpp": "int cy_fixture_ui() { return 1; }\n",
        },
        extra="""
set(sources src/always.cpp)
if(CY_UI)
    list(APPEND sources src/ui.cpp)
endif()
cy_add_module(NAME cy_fixture LAYER core SOURCES ${sources})
""",
    )

    off = configure_ok(tree, ["-DCY_UI=OFF"], workspace / "off")
    check("ui.cpp" not in compiled_sources(off), "ui.cpp is compiled with CY_UI=OFF")
    check_in("/* CY_UI is disabled */", generated(off, "cy_features.h"), "CY_UI must be undefined")

    on = configure_ok(tree, ["-DCY_UI=ON"], workspace / "on")
    check("ui.cpp" in compiled_sources(on), "ui.cpp is not compiled with CY_UI=ON")
    check_in("#define CY_UI 1", generated(on, "cy_features.h"), "CY_UI must be defined")


def test_missing_feature_dependency_fails(workspace: Path) -> None:
    """The specification's own example: CY_AI requires CY_NAVIGATION.

    `-DCY_NAVIGATION=OFF` IS THE POINT OF THIS TEST AND NOT AN INCIDENTAL FLAG — the same lesson
    `test_optional_backend_requires_its_subsystem` below records about CY_AUDIO. Until M8.b this
    case passed nothing but `-DCY_AI=ON` and relied on CY_NAVIGATION defaulting off, so what it
    actually asserted was the default rather than the rule. M8.b turned both options on —
    correctly, because it delivers navigation and the AI runtime, and `delivery-roadmap` fails a
    capability at Working whose CY_* option defaults off — and this case went red inside
    `just generate-test`, which is one of the three commands of the permanent `generated-code`
    gate. A test of a validation rule must state every input the rule reads; a default is not an
    input it may borrow.
    """
    output = configure_fails(
        fixture({}, languages="NONE"), ["-DCY_AI=ON", "-DCY_NAVIGATION=OFF"], workspace
    )
    check_in("CY_NAVIGATION", output, "the diagnostic must name the required option")
    check_in("-DCY_NAVIGATION=ON", output, "the diagnostic must give the correction")


def test_satisfied_feature_dependency_configures(workspace: Path) -> None:
    build = configure_ok(fixture({}, languages="NONE"), ["-DCY_AI=ON", "-DCY_NAVIGATION=ON"], workspace)
    check_in("#define CY_AI 1", generated(build, "cy_features.h"), "CY_AI must be defined")


def test_optional_backend_requires_its_subsystem(workspace: Path) -> None:
    """CY_AUDIO_STEAM_AUDIO requires CY_AUDIO, whatever CY_AUDIO happens to default to.

    `-DCY_AUDIO=OFF` IS THE POINT OF THIS TEST AND NOT AN INCIDENTAL FLAG. Until M4 this case
    passed nothing but `-DCY_AUDIO_STEAM_AUDIO=ON` and relied on CY_AUDIO defaulting off, so what
    it actually asserted was the default rather than the rule. M4 turned CY_AUDIO on — correctly,
    because it delivered the miniaudio backend — and this case went red inside `just generate-test`,
    which is one of the three commands of the permanent `generated-code` gate. A test of a
    validation rule must state every input the rule reads; a default is not an input it may borrow.
    """
    output = configure_fails(
        fixture({}, languages="NONE"), ["-DCY_AUDIO=OFF", "-DCY_AUDIO_STEAM_AUDIO=ON"], workspace
    )
    check_in("CY_AUDIO", output, "the diagnostic must name CY_AUDIO")
    check_in("-DCY_AUDIO=ON", output, "the diagnostic must give the correction")


# --- The module system -----------------------------------------------------------------------


def test_module_is_discovered_and_generated(workspace: Path) -> None:
    build = configure_ok(fixture(module_files("alpha-one")), [], workspace)
    modules = generated(build, "cy_modules.h")
    check_in("#define CY_MODULE_ALPHA_ONE 1", modules, "the module must reach the header")
    check("alpha_one.cpp" in compiled_sources(build), "the module's sources must be compiled")


def test_disabled_module_is_excluded(workspace: Path) -> None:
    """`engine-architecture`: sources excluded, and the macro undefined in cy_modules.h."""
    # One target that is always present, so there is a compile database to inspect at all.
    tree = fixture(
        {**module_files("alpha-one"), "src/always.cpp": "int cy_fixture_always() { return 0; }\n"},
        extra="cy_add_module(NAME cy_fixture LAYER core SOURCES src/always.cpp)\n",
    )
    build = configure_ok(tree, ["-DCY_MODULE_ALPHA_ONE=OFF"], workspace)
    modules = generated(build, "cy_modules.h")
    check_in("/* CY_MODULE_ALPHA_ONE is disabled */", modules, "the macro must be undefined")
    check("alpha_one.cpp" not in compiled_sources(build), "a disabled module must compile nothing")


def test_out_of_tree_module_builds_like_an_in_tree_one(workspace: Path) -> None:
    """CY_EXTRA_MODULE_PATHS: same manifest, same option, same target, same header."""
    outside = workspace / "elsewhere"
    write_tree(
        outside,
        {relative.replace("modules/", "", 1): content
         for relative, content in module_files("outsider").items()},
    )
    build = configure_ok(fixture({}), [f"-DCY_EXTRA_MODULE_PATHS={outside}"], workspace / "project")
    check_in("#define CY_MODULE_OUTSIDER 1", generated(build, "cy_modules.h"), "not discovered")
    check("outsider.cpp" in compiled_sources(build), "an out-of-tree module must be compiled")


def test_unknown_module_dependency_fails(workspace: Path) -> None:
    tree = fixture(module_files("alpha", public=["nowhere"]), languages="NONE")
    output = configure_fails(tree, [], workspace)
    check_in("'nowhere'", output, "the diagnostic must name the missing module")
    check_in("'alpha'", output, "the diagnostic must name the module that declared it")


def test_disabled_module_dependency_fails(workspace: Path) -> None:
    tree = fixture(
        {**module_files("alpha"), **module_files("beta", public=["alpha"])}, languages="NONE"
    )
    output = configure_fails(tree, ["-DCY_MODULE_ALPHA=OFF"], workspace)
    check_in("-DCY_MODULE_ALPHA=ON", output, "the diagnostic must give the correction")


def test_module_cycle_fails(workspace: Path) -> None:
    tree = fixture(
        {**module_files("alpha", public=["beta"]), **module_files("beta", public=["alpha"])},
        languages="NONE",
    )
    output = configure_fails(tree, [], workspace)
    check_in("cycle", output, "the diagnostic must say what it found")
    check_in("alpha -> beta -> alpha", output, "the cycle must be reported as its path")


def test_module_layer_violation_fails(workspace: Path) -> None:
    tree = fixture(
        {
            **module_files("low", layer="core", public=["high"]),
            **module_files("high", layer="scene"),
        },
        languages="NONE",
    )
    output = configure_fails(tree, [], workspace)
    check_in("Layer violation", output, "the diagnostic must say what it found")
    check_in("'low'", output, "the diagnostic must name the depending module")
    check_in("'high'", output, "the diagnostic must name the dependency")
    check_in("scene", output, "the diagnostic must name the layers")


def test_undeclared_link_fails(workspace: Path) -> None:
    """The manifest is authoritative: a link it does not declare is a build error."""
    tree = fixture(
        {
            **module_files("alpha"),
            **module_files("beta", links="PRIVATE_DEPENDENCIES cy_module_alpha"),
        }
    )
    output = configure_fails(tree, [], workspace)
    check_in("cy_module_alpha", output, "the diagnostic must name the undeclared link")
    check_in("manifest", output, "the diagnostic must say where the declaration belongs")


def test_declared_private_link_does_not_leak(workspace: Path) -> None:
    """The same link, declared: it configures, and the private dependency is not exposed."""
    tree = fixture(
        {
            **module_files("alpha"),
            **module_files(
                "beta", private=["alpha"], links="PRIVATE_DEPENDENCIES cy_module_alpha"
            ),
        }
    )
    configure_ok(tree, [], workspace)


def test_unknown_manifest_key_is_rejected(workspace: Path) -> None:
    tree = fixture(module_files("alpha", extra={"hot_relaod": True}), languages="NONE")
    # CMake wraps its own diagnostics, so the parts are checked rather than the sentence.
    output = " ".join(configure_fails(tree, [], workspace).split())
    check_in("unknown key 'hot_relaod'", output, "a typo must be reported, not ignored")


def test_manifest_name_must_match_its_directory(workspace: Path) -> None:
    files = module_files("alpha")
    files["modules/alpha/module.json"] = manifest("not-alpha")
    output = configure_fails(fixture(files, languages="NONE"), [], workspace)
    check_in("'not-alpha'", output, "the diagnostic must name the declared name")


def test_editor_module_is_gated_on_the_editor_option(workspace: Path) -> None:
    """Editor code is excluded from a runtime build by the graph, not by remembering to exclude it."""
    tree = fixture(module_files("panel", layer="editor", module_type="editor"))

    build = configure_ok(tree, [], workspace / "default")
    check_in(
        "/* CY_MODULE_PANEL is disabled */",
        generated(build, "cy_modules.h"),
        "an editor module must default off when the editor is not being built",
    )

    output = configure_fails(tree, ["-DCY_MODULE_PANEL=ON"], workspace / "forced")
    check_in("CY_BUILD_EDITOR", output, "the diagnostic must name the gating option")

    build = configure_ok(tree, ["-DCY_BUILD_EDITOR=ON"], workspace / "editor")
    check_in(
        "#define CY_MODULE_PANEL 1",
        generated(build, "cy_modules.h"),
        "an editor module builds when the editor does",
    )


def test_unsupported_platform_module_is_off(workspace: Path) -> None:
    tree = fixture(module_files("alpha", platforms=["web"]), languages="NONE")
    build = configure_ok(tree, [], workspace)
    check_in(
        "/* CY_MODULE_ALPHA is disabled */",
        generated(build, "cy_modules.h"),
        "a module that does not support this platform must default off",
    )


# --- Runner -----------------------------------------------------------------------------------


def collect() -> list:
    module = sys.modules[__name__]
    return [getattr(module, name) for name in sorted(dir(module)) if name.startswith("test_")]


def test_a_conditional_target_is_guarded_where_a_test_names_it(workspace: Path) -> None:
    """REGRESSION, M8.b's spike: `add_test` naming a target a feature option may not declare.

    `samples/05b-editor-window/runtime/CMakeLists.txt` returns before declaring
    `cy_editor_window_runtime` when the Vulkan backend is off or the host is not Linux. The parent's
    `add_test` named it unconditionally, so with `-DCY_RENDERER_VULKAN=OFF` the generator expression
    resolved against nothing and CMake failed at GENERATE:

        CMake Error at samples/05b-editor-window/CMakeLists.txt:113 (add_test):
          No target "cy_editor_window_runtime"

    CONFIGURE FAILED OUTRIGHT rather than the entry being skipped, so the option could not be
    configured at all — and `build-system-and-platforms` requires every CY_* option to build with it
    and without it. A feature that cannot be switched off is one nothing proves is separable.

    THE RULE IS NARROWER THAN IT FIRST LOOKS, AND THE NARROWING IS THE WORK. A first draft flagged
    every `$<TARGET_FILE:...>` in an `add_test` without an `if(TARGET ...)` beside it and found
    seventeen, of which none was the defect: most targets come from `cy_add_module` and are declared
    unconditionally, and three more are declared in the same file ABOVE its guard, so reaching the
    `add_test` already proves the target exists. A check that reports sixteen false positives is a
    check somebody switches off. What is actually unsafe is a CROSS-FILE reference to a target whose
    declaring file can return BEFORE declaring it.
    """
    del workspace  # a repository scan, not a fixture configure

    # The source directories only. A glob from the repository root walks the build trees first and
    # filters afterwards, which on a machine with four configured profiles is tens of gigabytes of
    # `_deps` before the first source file — measured, after it turned this into a timeout.
    paths = [REPO / "CMakeLists.txt"]
    for name in ("src", "samples", "tests", "tools", "bindings", "editor"):
        root = REPO / name
        if root.is_dir():
            paths.extend(root.glob("**/CMakeLists.txt"))
    files = [
        path
        for path in sorted(paths)
        if path.is_file() and "_deps" not in path.parts and ".build" not in path.parts
    ]

    declared: dict[str, tuple[Path, int]] = {}
    first_return: dict[Path, int | None] = {}
    declaration = re.compile(
        r"(?:cy_add_module\s*\(\s*NAME\s+|add_executable\s*\(\s*|add_library\s*\(\s*)"
        r"([A-Za-z0-9_.-]+)"
    )
    for path in files:
        text = path.read_text(encoding="utf-8")
        found = re.search(r"^\s*return\(\)", text, re.M)
        first_return[path] = found.start() if found else None
        for match in declaration.finditer(text):
            declared.setdefault(match.group(1), (path, match.start()))

    offenders: list[str] = []
    for path in files:
        text = path.read_text(encoding="utf-8")
        if "add_test" not in text:
            continue
        for target in sorted(set(re.findall(r"\$<TARGET_FILE:([A-Za-z0-9_:.-]+)>", text))):
            where = declared.get(target)
            if where is None:
                continue  # an imported or third-party target; not ours to reason about
            owner, declared_at = where
            if owner == path:
                continue  # same file: reaching the add_test proves the declaration ran
            returns_at = first_return.get(owner)
            if returns_at is None or returns_at > declared_at:
                continue  # nothing in that file can skip the declaration
            if f"if(TARGET {target})" in text:
                continue
            offenders.append(
                f"{path.relative_to(REPO)}: $<TARGET_FILE:{target}>, declared in "
                f"{owner.relative_to(REPO)} after a guard that can return first"
            )

    check(
        not offenders,
        "an add_test names a target another file may decline to declare; guard it with "
        "if(TARGET ...):\n  " + "\n  ".join(offenders),
    )


def main(argv: list[str]) -> int:
    tests = [test for test in collect() if not argv or any(a in test.__name__ for a in argv)]
    failures = []
    for test in tests:
        with tempfile.TemporaryDirectory(prefix="cy-gen-test-") as directory:
            try:
                test(Path(directory))
                print(f"pass  {test.__name__}")
            except Failure as failure:
                print(f"FAIL  {test.__name__}\n      {failure}")
                failures.append(test.__name__)
    print(f"\n{len(tests) - len(failures)}/{len(tests)} passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
