#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Embed checked-in MSL source modules for the frame pipeline.

A string literal has a length limit, concatenation included: clang rejects one over 65,536 bytes
(-Woverlength-strings, an error under -Werror) and MSVC one over 65,535, or a single piece over
16,380. The forward fragment outgrew that. A module longer than CHUNK_BYTES is therefore emitted as
line-aligned chunks that a consteval function copies into one array, which is not a literal and has
no such limit. It keeps its name and type (a reference to a char array), so `sizeof(k) - 1` and the
decay to `const char*` at the call sites are unchanged. Shorter modules stay one literal.
"""

import pathlib
import re
import sys

CHUNK_BYTES = 16000

JOIN_HELPER = """\
namespace detail {

/// One MSL module assembled from several raw-string chunks. See embed_msl.py for why.
template <std::size_t Size>
struct MslText {
    char text[Size];
};

template <std::size_t... Sizes>
consteval MslText<(Sizes + ...) - sizeof...(Sizes) + 1> join_msl(const char (&... chunks)[Sizes]) {
    MslText<(Sizes + ...) - sizeof...(Sizes) + 1> joined{};
    std::size_t at = 0;
    for (const char* chunk : {static_cast<const char*>(chunks)...}) {
        for (; *chunk != '\\0'; ++chunk) {
            joined.text[at++] = *chunk;
        }
    }
    return joined;
}

}  // namespace detail

"""


def chunks(source: str) -> list[str]:
    """Split at line ends into pieces of at most CHUNK_BYTES UTF-8 bytes."""
    pieces, current, size = [], [], 0
    for line in source.splitlines(keepends=True):
        length = len(line.encode("utf-8"))
        if length > CHUNK_BYTES:
            raise ValueError(f"a {length}-byte line cannot be embedded in {CHUNK_BYTES}-byte chunks")
        if size + length > CHUNK_BYTES:
            pieces.append("".join(current))
            current, size = [], 0
        current.append(line)
        size += length
    pieces.append("".join(current))
    return pieces


def embed(name: str, source: str) -> str:
    if len(source.encode("utf-8")) <= CHUNK_BYTES:
        return f'inline constexpr char {name}[] = R"cy_msl({source})cy_msl";\n\n'
    joined = ",\n    ".join(f'R"cy_msl({piece})cy_msl"' for piece in chunks(source))
    return (f"inline constexpr auto {name}Text = detail::join_msl(\n    {joined});\n"
            f"inline constexpr const auto& {name} = {name}Text.text;\n\n")


def frame_argument_buffers(source: str, name: str) -> str:
    """Keep Slang's compacted MSL slots aligned with the RHI's fixed set slots."""
    # The CPU's GpuLight stores each float3 tightly beside a scalar. Metal's
    # float3 occupies a 16-byte slot, so the generated storage struct must use
    # packed_float3 or every field after the first vector is read at the wrong
    # offset (including the light kind and intensity).
    if "struct Light_0" in source:
        for field in ("positionRelativeToCamera_0", "direction_0", "color_0"):
            original = f"float3 {field};"
            if original not in source:
                raise ValueError(f"{name}: missing Light.{field}")
            source = source.replace(original, f"packed_float3 {field};")
    if name not in {
        "kFrameDepthVertexMsl", "kFrameDepthFragmentMsl",
        "kFrameForwardVertexMsl", "kFrameForwardFragmentMsl", "kFrameShadowVertexMsl",
        "kFrameSkinnedDepthVertexMsl", "kFrameSkinnedForwardVertexMsl",
    }:
        return source
    # Slang emits uint3 for the grid dimensions. Metal pads that field to 16
    # bytes, while FrameViewData stores three adjacent u32 values (12 bytes).
    source = source.replace("uint3 dimensions_0;", "packed_uint3 dimensions_0;")
    view = "cyFrameView_0 [[buffer(0)]]" if name == "kFrameDepthFragmentMsl" else "cyFrameView_1 [[buffer(0)]]"
    if view not in source:
        raise ValueError(f"{name}: expected the frame view at Slang's compacted slot zero")
    source = source.replace(view, view.replace("buffer(0)", "buffer(1)"))
    if name == "kFrameForwardFragmentMsl":
        globals_set = "cyFrameGlobals_1 [[buffer(1)]]"
        if globals_set not in source:
            raise ValueError(f"{name}: expected the material table at Slang's compacted slot one")
        source = source.replace(globals_set, globals_set.replace("buffer(1)", "buffer(0)"))
        # Metal argument-buffer texture arrays must be direct members with the
        # resource IDs assigned by the RHI descriptor-set layout.
        wrapped = "_Array_default_Texture2D128_0 textures_0;"
        sampled = r"\(&((?:kernelContext_\d+))->cyFrameGlobals_0->textures_0\)->data_0\["
        if wrapped not in source or re.search(sampled, source) is None:
            raise ValueError(f"{name}: expected Slang's wrapped texture array")
        source = source.replace(wrapped, "array<texture2d<float, access::sample>, 128> textures_0 [[id(1)]];")
        source = source.replace("CyGlobalsData_0 constant* globals_0;", "CyGlobalsData_0 constant* globals_0 [[id(0)]];")
        source = source.replace("sampler sampler_0;", "sampler sampler_0 [[id(129)]];")
        source = re.sub(sampled, r"\1->cyFrameGlobals_0->textures_0[", source)
        source = source.replace(
            "CyFrameGlobalSet_default_0 constant* cyFrameGlobals_1 [[buffer(0)]]",
            "CyFrameGlobalSet_default_0 constant& cyFrameGlobals_1 [[buffer(0)]]",
        )
        source = source.replace("cyFrameGlobals_0 = cyFrameGlobals_1;",
                                "cyFrameGlobals_0 = &cyFrameGlobals_1;")
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
    ]
    modules = []
    for item in sys.argv[2:]:
        name, raw_path = item.split("=", 1)
        path = pathlib.Path(raw_path)
        source = frame_argument_buffers(path.read_text(encoding="utf-8"), name)
        if ")cy_msl\"" in source:
            raise ValueError(f"{path}: source contains the raw-string delimiter")
        modules.append(f"/// {path.name}, {len(source.encode('utf-8'))} bytes.\n")
        modules.append(embed(name, source))
    chunked = any("detail::join_msl" in module for module in modules)
    if chunked:
        lines.append("#include <cstddef>\n#include <initializer_list>\n\n")
    lines.append("namespace cy::rendering::pipeline {\n\n")
    if chunked:
        lines.append(JOIN_HELPER)
    lines.extend(modules)
    lines.append("}  // namespace cy::rendering::pipeline\n")
    pathlib.Path(sys.argv[1]).write_text("".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
