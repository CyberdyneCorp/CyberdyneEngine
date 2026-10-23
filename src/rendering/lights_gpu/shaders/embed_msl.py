#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Embed checked-in MSL source for the device light dispatches (src/rendering/lights_gpu).

Each module is emitted as adjacent C++ raw-string literals that concatenate at compile time, split at
a blank line about every SPLIT_LINES lines: MSVC's logical-source-line limit rejects one raw string
spanning thousands of lines. src/rendering/skinning/shaders/embed_msl.py does the same for the same
reason.

A literal also has a length limit, concatenation included: clang rejects one over 65,536 bytes
(-Woverlength-strings, an error under -Werror) and MSVC one over 65,535, or a single piece over
16,380. The per-pixel resolve's two modules outgrew that, so a module longer than JOIN_BYTES is
emitted as line-aligned pieces of at most CHUNK_BYTES that a consteval function copies into one
array — not a literal, so not limited — exactly as src/rendering/pipeline/shaders/embed_msl.py does
for the forward fragment. It keeps its name and its type (a reference to a char array), so
`sizeof(k) - 1` at the call sites is unchanged; every shorter module is emitted as before.

Usage:
    embed_msl.py <output.h> <name>=<module.metal> [<name>=<module.metal> ...]
"""

import pathlib
import sys

SPLIT_LINES = 300
JOIN_BYTES = 60000
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
    lines = source.splitlines(keepends=True)
    out: list[str] = []
    start = 0
    while start < len(lines):
        end = min(start + SPLIT_LINES, len(lines))
        cut = end
        for candidate in range(end, min(end + SPLIT_LINES, len(lines))):
            if lines[candidate].strip() == "":
                cut = candidate + 1
                break
        out.append("".join(lines[start:cut]))
        start = cut
    return out


def byte_chunks(source: str) -> list[str]:
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


def main() -> int:
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <output.h> <name>=<module.metal> ...", file=sys.stderr)
        return 2
    lines = [
        "#pragma once\n",
        "// SPDX-License-Identifier: MIT\n",
        "// Compiled MSL for the device light dispatches (src/rendering/lights_gpu). GENERATED — do not edit by hand.\n\n",
        "#include <cy/core/base/types.h>\n\n",
    ]
    modules = []
    joined_any = False
    for item in sys.argv[2:]:
        name, raw_path = item.split("=", 1)
        path = pathlib.Path(raw_path)
        source = path.read_text(encoding="utf-8")
        if ")cy_msl\"" in source:
            raise ValueError(f"{path}: source contains the raw-string delimiter")
        size = len(source.encode("utf-8"))
        modules.append(f"/// {path.name}, {size} bytes.\n")
        if size > JOIN_BYTES:
            joined_any = True
            pieces = ",\n    ".join(f'R"cy_msl({part})cy_msl"' for part in byte_chunks(source))
            modules.append(f"inline constexpr auto {name}Text = detail::join_msl(\n    {pieces});\n"
                           f"inline constexpr const auto& {name} = {name}Text.text;\n\n")
        else:
            body = "\n    ".join(f'R"cy_msl({part})cy_msl"' for part in chunks(source))
            modules.append(f"inline constexpr char {name}[] =\n    {body};\n\n")
    if joined_any:
        lines.append("#include <cstddef>\n#include <initializer_list>\n\n")
    lines.append("namespace cy::rendering::lights_gpu {\n\n")
    if joined_any:
        lines.append(JOIN_HELPER)
    lines.extend(modules)
    lines.append("}  // namespace cy::rendering::lights_gpu\n")
    pathlib.Path(sys.argv[1]).write_text("".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
