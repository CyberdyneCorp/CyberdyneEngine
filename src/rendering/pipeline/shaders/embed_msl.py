#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Embed checked-in MSL source modules for the frame pipeline."""

import pathlib
import sys


def frame_argument_buffers(source: str, name: str) -> str:
    """Keep Slang's compacted MSL slots aligned with the RHI's fixed set slots."""
    if name not in {
        "kFrameDepthVertexMsl", "kFrameDepthFragmentMsl",
        "kFrameForwardVertexMsl", "kFrameForwardFragmentMsl",
    }:
        return source
    view = "cyFrameView_0 [[buffer(0)]]" if name == "kFrameDepthFragmentMsl" else "cyFrameView_1 [[buffer(0)]]"
    if view not in source:
        raise ValueError(f"{name}: expected the frame view at Slang's compacted slot zero")
    source = source.replace(view, view.replace("buffer(0)", "buffer(1)"))
    if name == "kFrameForwardFragmentMsl":
        globals_set = "cyFrameGlobals_1 [[buffer(1)]]"
        if globals_set not in source:
            raise ValueError(f"{name}: expected the material table at Slang's compacted slot one")
        source = source.replace(globals_set, globals_set.replace("buffer(1)", "buffer(0)"))
    if name.endswith("VertexMsl"):
        push = "cyDraw_1 [[buffer(2)]]"
        if push not in source:
            raise ValueError(f"{name}: expected the draw push at Slang's compacted slot two")
        source = source.replace(push, "cyDraw_1 [[buffer(3)]]")
    return source


def main() -> int:
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <output.h> <name>=<module.metal> ...", file=sys.stderr)
        return 2
    lines = [
        "#pragma once\n",
        "// SPDX-License-Identifier: MIT\n",
        "// Compiled MSL for the rendering frame. GENERATED — do not edit by hand.\n\n",
        "#include <cy/core/base/types.h>\n\n",
        "namespace cy::rendering::pipeline {\n\n",
    ]
    for item in sys.argv[2:]:
        name, raw_path = item.split("=", 1)
        path = pathlib.Path(raw_path)
        source = frame_argument_buffers(path.read_text(encoding="utf-8"), name)
        if ")cy_msl\"" in source:
            raise ValueError(f"{path}: source contains the raw-string delimiter")
        lines.append(f"/// {path.name}, {len(source.encode('utf-8'))} bytes.\n")
        lines.append(f'inline constexpr char {name}[] = R"cy_msl({source})cy_msl";\n\n')
    lines.append("}  // namespace cy::rendering::pipeline\n")
    pathlib.Path(sys.argv[1]).write_text("".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
