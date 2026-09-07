#!/usr/bin/env python3
"""Source-level layering checks for CyberdyneEngine.

Task 1.3.3. The second half of the layer enforcement: `cmake/module.cmake` sees every link a target
declares, and this sees every `#include` a translation unit writes, including one that reaches
upward through an include path CMake was never told about.

Six checks, all of them cheap enough to run on every pull request:

  includes      a file may include a header at its own layer or below, never above
  sdl           no SDL header appears outside platform/ — design.md §4
  gpuapi        no Vulkan, Slang or SPIR-V header appears outside src/backends/ — the same rule as
                `sdl`, for the same reason, and M3's task 2.3.1
  thirdparty    no Jolt or miniaudio header appears outside the backend that owns it — the same
                rule again, and M4's tasks 4.2.2 and 4.3.4
  barriers      no barrier-emitting call appears outside the render graph and the RHI — M3's task
                2.2.4, the structural half of "a pass has no API to emit a barrier"
  targets       no bare add_library or add_executable, because that is a target that opted out of
                the configure-time check (task 1.3.2)

Exit status is 0 when the tree is clean and 1 when it is not. Violations are printed one per line as
`path:line: <check>: <message>`, which is both readable and what CI annotation tooling expects.

Governed by: engine-architecture (Layered architecture), project-and-plugins (Architectural layering
is enforced), build-system-and-platforms (Platform porting surface).
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

# --- The layer table ------------------------------------------------------------------------------
#
# design.md §1 and the table in engine-architecture. Kept identical to cmake/module.cmake by hand;
# there are ten rows and they have not changed since the specification was written.

LAYER_OF_NAME = {
    "core": 0,
    "ecs": 1,
    "servers": 2,
    # src/save/ is layer 2: it depends on layer 0 alone and is consumed by world partition,
    # gameplay and the editor's play mode. See src/save/CMakeLists.txt.
    "save": 2,
    "backends": 3,
    "platform": 3,
    "scene": 4,
    "rendering": 4,
    # CyberWorld: the spatial and persistence layer above the ECS (M6 section 3). It declares
    # LAYER scene in cy_add_module() for the reason src/gameplay/ does — there is no `world` rung
    # in `engine-architecture`'s table — and it is named here so that the include check covers it
    # rather than skipping it as a directory outside the stack.
    "world": 4,
    "runtime": 5,
    "abi": 6,
    "editor": 7,
    "tools": 7,
}

# Repository-relative directory prefixes, matched longest first. tests/, benchmarks/ and samples/ are
# consumers of the whole engine rather than parts of the stack, so they sit at the top and may
# include anything.
DIRECTORY_LAYERS = {
    "src/core": 0,
    "src/ecs": 1,
    "src/servers": 2,
    "src/save": 2,
    "src/backends": 3,
    "platform": 3,
    "src/scene": 4,
    "src/rendering": 4,
    "src/world": 4,
    "src/runtime": 5,
    "src/abi": 6,
    "editor": 7,
    "tools": 7,
    "tests": 7,
    "benchmarks": 7,
    "samples": 7,
}

LAYER_NAMES = {0: "core", 1: "ecs", 2: "servers", 3: "backends", 4: "scene/rendering",
               5: "runtime", 6: "abi", 7: "editor/tools"}

SOURCE_SUFFIXES = {".h", ".hpp", ".hh", ".hxx", ".inl", ".ipp", ".c", ".cc", ".cpp", ".cxx",
                   ".m", ".mm"}

# Anchored to the start of a line so that a `//`-commented include is not a finding. A #include
# inside a /* block comment */ still is; that is a known and acceptable false positive, and the fix
# is to delete the dead include rather than to teach this a C++ lexer.
INCLUDE_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]*[<"]([^>"\n]+)[>"]', re.MULTILINE)

BARE_TARGET_RE = re.compile(r'^[ \t]*(add_library|add_executable)[ \t]*\(', re.MULTILINE)

# --- The graphics-API rule (task 2.3.1) -------------------------------------------------------------
#
# The same rule as `sdl` and for the same reason: a third-party API's types live beneath an
# engine-owned interface, and a second backend arrives later. `rhi-and-render-graph` puts the RHI at
# layer 3 and the render graph above it, so a Vulkan type reaching layer 4 would mean the graph could
# not be compiled without a Vulkan SDK — and the null backend, which is what makes rendering testable
# in continuous integration, would stop being a reference for what the RHI requires.
#
# Matched by the include's leading directory or by its file name, so `SDL3/SDL_vulkan.h` — SDL's own
# forward declarations, which platform/desktop-sdl3 legitimately uses to build a surface — is not a
# finding. It is an SDL header and the `sdl` check already governs where it may appear.
GPU_API_DIRECTORIES = frozenset({"vulkan", "volk", "vk_video", "spirv", "spirv-tools",
                                 "spirv_cross", "SPIRV", "SPIRV-Reflect", "glslang", "slang"})
GPU_API_FILES = frozenset({"volk.h", "volk.c", "vk_mem_alloc.h", "vulkan.h", "vulkan.hpp",
                           "vulkan_core.h", "spirv_reflect.h", "spirv.h", "spirv.hpp", "slang.h",
                           "slang-com-ptr.h", "slang-com-helper.h"})

# Where a graphics API may be named. Only the backend that owns it.
GPU_API_ROOTS = ("src/backends/",)

# --- The simulation-library rule (M4, tasks 4.2.2 and 4.3.4) ----------------------------------------
#
# The third statement of the same rule, and the one M4 needed: `physics` requires "no Jolt type
# SHALL appear above src/backends/" and `audio` requires that no backend type appear in any engine
# or game-facing header outside the module that owns it. Both were true when M4 closed and both were
# true only because `cy::dep::jolt` and `cy::dep::miniaudio` are PRIVATE dependencies, so the
# libraries' include directories are not inherited and the include does not resolve elsewhere.
#
# That is a real structural guarantee, but it is not the same guarantee. A link-time accident — a
# PUBLIC dependency, a target that names the library directly, an include directory added by hand —
# turns it off silently, and the first symptom is that the engine no longer builds without a physics
# SDK. `sdl` and `gpuapi` exist because the same argument was made about them. So does this.
#
# Per library rather than one shared root: a miniaudio header inside src/backends/physics-jolt/ is
# as wrong as one inside src/servers/, and a rule that named only `src/backends/` would accept it.
#
# Each row is the library's name, the one directory it may be named from, and the engine-owned
# interface it sits beneath — the third is in the diagnostic because "why" is what a reader needs.
JOLT = ("Jolt", "src/backends/physics-jolt/", "cy::physics::PhysicsServer")
MINIAUDIO = ("miniaudio", "src/backends/audio-miniaudio/", "cy::audio::AudioBackend")

THIRD_PARTY_DIRECTORIES = {"Jolt": JOLT}
THIRD_PARTY_FILES = {"Jolt.h": JOLT, "miniaudio.h": MINIAUDIO}

# --- The barrier rule (task 2.2.4) ------------------------------------------------------------------
#
# THE M3 INVARIANT, AS A BUILD FAILURE. A pass declares what it reads and what it writes and has no
# API to emit a barrier; the render graph computes every barrier, layout transition and queue-family
# ownership transfer from those declarations. design.md §2: this is a property of the thirtieth pass,
# and the thirtieth pass obeys it because the first one did.
#
# The C++ side of the rule is a passkey — rhi::GraphBarrierKey's constructor is private and
# cy::rendering::GraphExecutor is its only friend — and this is the other side, because a passkey
# cannot stop somebody declaring a class of that name, and because a backend-level barrier call
# (vkCmdPipelineBarrier2) bypasses the RHI's types entirely.
#
# The symbols below are the whole barrier-emitting surface. A file outside the two permitted roots
# that names one is a file that is emitting synchronisation by hand.
BARRIER_SYMBOLS = ("record_barriers", "barrier_recorder", "GraphBarrierKey", "BarrierRecorder",
                   "vkCmdPipelineBarrier", "vkCmdWaitEvents", "vkCmdSetEvent")

BARRIER_RE = re.compile(r'\b(' + "|".join(BARRIER_SYMBOLS) + r')\b')

# Where a barrier may be emitted: the interface that declares it, the one implementation that reaches
# it, and — from M7 — the viewport transport's publisher.
#
# THE THIRD ROOT IS NOT A WEAKENING OF THE RULE, and it is worth saying why rather than leaving a
# reader to wonder. The rule is about PASSES: a pass declares what it reads and what it writes, and
# the render graph derives every transition from those declarations, so a pass that emitted a
# barrier by hand would be a pass the graph could not reason about. `src/backends/viewport/` is not a
# pass and has no graph. It owns its own `VkDevice` and its own queue, for one purpose — handing
# images to another process — and the two transitions it records are the protocol's own: an image
# goes to TRANSFER_DST to be written and back to SHADER_READ_ONLY_OPTIMAL because that is the layout
# the editor is promised every image in. There is nothing there for a graph to derive them from, and
# there is no renderer code in that directory for the rule to protect.
BARRIER_ROOTS = ("src/backends/rhi/", "src/rendering/graph/", "src/backends/viewport/")

# A line whose first non-space character starts a comment. Prose that names the barrier API — a
# design note, a header comment explaining why the rule exists — is not a barrier call, and a gate
# that reported one would be a gate people work around by not writing the comment.
COMMENT_LINE_RE = re.compile(r'^[ \t]*(//|/\*|\*|#)')

# Where the bare-target check applies: the engine tree, plus the top-level CMakeLists.txt. cmake/ is
# out of scope because it *is* the build system — it declares cy_add_module() itself, the
# cy_compile_options interface every module links, and the cy::dep::<name> shims that wrap targets
# this project does not own. A target declared there is build machinery, not an engine module.
TARGET_CHECK_ROOTS = ("src", "platform", "modules", "editor", "tools", "tests", "benchmarks",
                      "samples")

# Files inside that scope permitted to declare a target directly, each with the reason. Paths are
# relative to the scanned root. This table is the only opt-out, and adding a row to it is a reviewed
# change to this file — which is the point.
BARE_TARGET_EXEMPT = {
    "CMakeLists.txt":
        "the task 0.2 profile spike, which predates cy_add_module(); remove both at task 3.6.1",
}

# Directories that are not the engine tree: build output, version control, editor caches, and the
# layer checker's own fixtures, which contain deliberate violations.
EXCLUDED_DIRS = {".git", ".cache", "node_modules", "target", "__pycache__"}
EXCLUDED_PATHS = {"tools/layercheck/fixtures"}


@dataclass(frozen=True)
class Violation:
    path: str
    line: int
    check: str
    message: str

    def __str__(self) -> str:
        return f"{self.path}:{self.line}: {self.check}: {self.message}"


def directory_layer(relative: str) -> int | None:
    """The layer a repository-relative path belongs to, or None if it is outside the stack."""
    parts = PurePosixPath(relative).parts
    for depth in (2, 1):
        prefix = "/".join(parts[:depth])
        if prefix in DIRECTORY_LAYERS:
            return DIRECTORY_LAYERS[prefix]
    return None


def include_layer(include: str) -> int | None:
    """The layer an include path names, by its leading directory component.

    Textual rather than filesystem-based on purpose: it gives the right answer for a header that has
    not been written yet, and does not depend on how the include directories happen to be set up.
    `cy/` and `src/` are stripped so that all three spellings of the same header agree.
    """
    parts = PurePosixPath(include.replace("\\", "/")).parts
    while parts and parts[0] in ("cy", "src"):
        parts = parts[1:]
    return LAYER_OF_NAME.get(parts[0]) if parts else None


def is_sdl_include(include: str) -> bool:
    path = PurePosixPath(include.replace("\\", "/"))
    return path.parts[0].startswith("SDL") or path.name.startswith("SDL")


def is_gpu_api_include(include: str) -> bool:
    """Whether an include names a Vulkan, Slang or SPIR-V header.

    An SDL header is never one, even when it is spelled SDL_vulkan.h: those are SDL's own forward
    declarations, they name no Vulkan type the engine can use, and where they may appear is already
    the `sdl` check's business.
    """
    if is_sdl_include(include):
        return False
    path = PurePosixPath(include.replace("\\", "/"))
    return path.parts[0] in GPU_API_DIRECTORIES or path.name in GPU_API_FILES


def third_party_library_of(include: str) -> tuple[str, str, str] | None:
    """The library an include names, the directory it may be named from, and its interface.

    Matched by the include's leading directory or by its file name, the way `gpuapi` is, so
    `<Jolt/Physics/Body/Body.h>` and `"Jolt.h"` are both the same finding.
    """
    path = PurePosixPath(include.replace("\\", "/"))
    if path.parts and path.parts[0] in THIRD_PARTY_DIRECTORIES:
        return THIRD_PARTY_DIRECTORIES[path.parts[0]]
    return THIRD_PARTY_FILES.get(path.name)


def line_of(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def walk(root: Path, names: set[str], suffixes: set[str]):
    """Yield (absolute path, repository-relative path) for the tree's own files.

    Excluded directories are pruned rather than filtered, so a build tree next to the sources costs
    nothing to skip however large it has grown. That is most of what keeps this cheap enough to run
    on every pull request.
    """
    for parent, directories, files in os.walk(root):
        prefix = PurePosixPath(Path(parent).relative_to(root).as_posix())
        directories[:] = sorted(d for d in directories if not is_excluded(prefix / d))
        for name in sorted(files):
            if name in names or Path(name).suffix in suffixes:
                yield Path(parent) / name, (prefix / name).as_posix().removeprefix("./")


def is_excluded(directory: PurePosixPath) -> bool:
    relative = directory.as_posix().removeprefix("./")
    if directory.name in EXCLUDED_DIRS or directory.name.startswith("build"):
        return True
    return relative in EXCLUDED_PATHS


def check_source_file(relative: str, text: str) -> list[Violation]:
    """The include-shaped rules for one file: the layer of each include, SDL, and the graphics APIs."""
    own_layer = directory_layer(relative)
    in_platform = relative.startswith("platform/")
    in_gpu_backend = relative.startswith(GPU_API_ROOTS)
    violations = []

    for match in INCLUDE_RE.finditer(text):
        include = match.group(1)
        line = line_of(text, match.start())

        if is_sdl_include(include) and not in_platform:
            violations.append(Violation(relative, line, "sdl",
                f"includes '{include}'. No SDL header appears outside platform/ — SDL sits beneath "
                f"the engine-owned Platform and DisplayServer interfaces, and a second backend "
                f"lands at M11 (design.md §4)."))
            continue

        if is_gpu_api_include(include) and not in_gpu_backend:
            violations.append(Violation(relative, line, "gpuapi",
                f"includes '{include}'. No Vulkan, Slang or SPIR-V header appears outside "
                f"src/backends/ — the RHI's synchronisation vocabulary is engine-owned "
                f"(cy/backends/rhi/types.h) precisely so that the render graph above it needs no "
                f"graphics SDK, and so that Metal and D3D12 are a directory rather than a rewrite "
                f"(rhi-and-render-graph, 'Backend roadmap')."))
            continue

        library = third_party_library_of(include)
        if library is not None and not relative.startswith(library[1]):
            name, owner, interface = library
            violations.append(Violation(relative, line, "thirdparty",
                f"includes '{include}'. No {name} header appears outside {owner} — {name} sits "
                f"beneath {interface}, which is what lets a trivial implementation of that "
                f"interface run the same suites in continuous integration, and what makes a second "
                f"backend a directory rather than a rewrite (physics, audio, "
                f"thirdparty-dependencies)."))
            continue

        target_layer = include_layer(include)
        if own_layer is None or target_layer is None or target_layer <= own_layer:
            continue
        violations.append(Violation(relative, line, "includes",
            f"a file at layer {own_layer} ({LAYER_NAMES[own_layer]}) includes '{include}' at "
            f"layer {target_layer} ({LAYER_NAMES[target_layer]}). A lower layer never depends on a "
            f"higher one."))

    return violations


def check_barriers(relative: str, text: str) -> list[Violation]:
    """Task 2.2.4: no barrier-emitting call outside the render graph and the RHI.

    Comment lines are skipped. Prose that names the barrier API — the header comment that explains
    why the rule exists, this file's own tables — is not a barrier call, and a gate that reported one
    would be a gate people work around by not writing the comment.
    """
    if relative.startswith(BARRIER_ROOTS):
        return []
    violations = []
    for number, line in enumerate(text.splitlines(), start=1):
        if COMMENT_LINE_RE.match(line):
            continue
        match = BARRIER_RE.search(line)
        if match is None:
            continue
        violations.append(Violation(relative, number, "barriers",
            f"names '{match.group(1)}'. Barriers, image layout transitions and queue-family "
            f"ownership transfers are COMPUTED by the render graph from the reads and writes a pass "
            f"declares; a pass has no API to emit one (rhi-and-render-graph, 'No manual barriers in "
            f"user-facing code'). The barrier-emitting surface lives in "
            f"{' and '.join(BARRIER_ROOTS)} and nowhere else. Declare the resource use instead."))
    return violations


def in_target_check_scope(relative: str) -> bool:
    return relative == "CMakeLists.txt" or relative.startswith(
        tuple(root + "/" for root in TARGET_CHECK_ROOTS))


def check_cmake_file(relative: str, text: str) -> list[Violation]:
    """Task 1.3.2: no target is declared outside cy_add_module()."""
    if relative in BARE_TARGET_EXEMPT or not in_target_check_scope(relative):
        return []
    return [
        Violation(relative, line_of(text, match.start()), "targets",
            f"bare {match.group(1)}(). Every engine target is declared through cy_add_module(NAME "
            f"<n> LAYER <layer> ...), which records its layer and refuses at configure time to link "
            f"a target above it. A bare target opts out of that check.")
        for match in BARE_TARGET_RE.finditer(text)
    ]


def read(path: Path) -> str:
    return path.read_text(encoding="utf-8", errors="replace")


def run(root: Path, checks: set[str]) -> tuple[list[Violation], int]:
    violations: list[Violation] = []
    scanned = 0

    if {"includes", "sdl", "gpuapi", "thirdparty", "barriers"} & checks:
        for path, relative in walk(root, names=set(), suffixes=SOURCE_SUFFIXES):
            scanned += 1
            text = read(path)
            violations += [v for v in check_source_file(relative, text) if v.check in checks]
            if "barriers" in checks:
                violations += check_barriers(relative, text)

    if "targets" in checks:
        for path, relative in walk(root, names={"CMakeLists.txt"}, suffixes={".cmake"}):
            scanned += 1
            violations += check_cmake_file(relative, read(path))

    violations.sort(key=lambda v: (v.path, v.line))
    return violations, scanned


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2],
                        help="tree to check (default: the repository root)")
    parser.add_argument("--check", action="append",
                        choices=["includes", "sdl", "gpuapi", "thirdparty", "barriers",
                                 "targets", "all"],
                        help="run only this check; repeatable (default: all)")
    parser.add_argument("-q", "--quiet", action="store_true", help="print nothing when clean")
    args = parser.parse_args(argv)

    selected = set(args.check or ["all"])
    checks = ({"includes", "sdl", "gpuapi", "thirdparty", "barriers", "targets"}
              if "all" in selected
              else selected)

    root = args.root.resolve()
    if not root.is_dir():
        print(f"layercheck: not a directory: {root}", file=sys.stderr)
        return 2

    started = time.monotonic()
    violations, scanned = run(root, checks)
    elapsed_ms = (time.monotonic() - started) * 1000

    for violation in violations:
        print(violation, file=sys.stderr)

    if violations:
        print(f"\nlayercheck: {len(violations)} violation(s) in {scanned} files "
              f"({elapsed_ms:.0f} ms)", file=sys.stderr)
        return 1
    if not args.quiet:
        print(f"layercheck: clean — {scanned} files, {', '.join(sorted(checks))} "
              f"({elapsed_ms:.0f} ms)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
