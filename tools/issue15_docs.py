#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check that issue #15's authoring instructions point to shipped assets."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CHANGE = ROOT / "openspec/changes/implement-issue-15-graph-authoring"
SAMPLE = ROOT / "samples/05b-editor-window"

REQUIRED_FILES = (
    CHANGE / "proposal.md",
    CHANGE / "design.md",
    CHANGE / "tasks.md",
    CHANGE / "specs/vfx-system/spec.md",
    CHANGE / "specs/material-compiler/spec.md",
    CHANGE / "specs/editor-architecture/spec.md",
    SAMPLE / "project/worlds/issue15-sway.cyworld",
    SAMPLE / "project/materials/issue15_sway.cygraph",
    SAMPLE / "project/materials/issue15_sway.cymatcanvas",
    SAMPLE / "project/effects/issue15_two_emitters.cyvfxdoc",
    SAMPLE / "project/effects/shared_drag.cyvfxmodule",
    SAMPLE / "mcp_window.py",
    SAMPLE / "runtime/tests/references/issue15_two_emitters_metal.png",
    ROOT / "docs/design/images/issue15-vfx-mcp-preview.png",
)

REQUIRED_GUIDANCE = {
    ROOT / "editor/README.md": (
        "## Catalogue-driven material properties",
        "## VFX graph authoring status",
        "materials/issue15_sway.cymatcanvas",
        "vertex-geometry-unsupported",
        "effects/shared_drag.cyvfxmodule",
        "vfx.module.attach",
    ),
    SAMPLE / "README.md": (
        "## Vertex graph sine sway",
        "## VFX graph draft",
        "--world worlds/issue15-sway.cyworld",
        "--capture docs/design/images/issue15-sine-sway-mcp-preview.png",
        "project/effects/issue15_two_emitters.cyvfxdoc",
        "vfx.preview.load",
        "vfx.preview.step",
        "runtime/tests/references/issue15_two_emitters_metal.png",
    ),
}


def main() -> int:
    missing = [str(path.relative_to(ROOT)) for path in REQUIRED_FILES if not path.is_file()]
    for path, snippets in REQUIRED_GUIDANCE.items():
        if not path.is_file():
            missing.append(str(path.relative_to(ROOT)))
            continue
        contents = path.read_text(encoding="utf-8")
        missing.extend(
            f"{path.relative_to(ROOT)}: {snippet}"
            for snippet in snippets
            if snippet not in contents
        )
    for item in missing:
        print(f"missing issue #15 documentation: {item}")
    if missing:
        return 1
    print("issue #15 OpenSpec, guides, sample assets, and reference images are present")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
