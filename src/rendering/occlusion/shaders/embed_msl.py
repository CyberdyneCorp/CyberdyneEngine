#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Embed checked-in MSL source for the ambient occlusion dispatches.

Each module is emitted as adjacent C++ raw-string literals that concatenate at compile time, split at
a blank line about every SPLIT_LINES lines: MSVC's logical-source-line limit rejects one raw string
spanning thousands of lines. src/rendering/skinning/shaders/embed_msl.py does the same for the same
reason.

Usage:
    embed_msl.py <output.h> <name>=<module.metal> [<name>=<module.metal> ...]
"""

import pathlib
import sys

SPLIT_LINES = 300


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


def main() -> int:
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <output.h> <name>=<module.metal> ...", file=sys.stderr)
        return 2
    lines = [
        "#pragma once\n",
        "// SPDX-License-Identifier: MIT\n",
        "// Compiled MSL for the ambient occlusion dispatches. GENERATED — do not edit by hand.\n\n",
        "#include <cy/core/base/types.h>\n\n",
        "namespace cy::rendering::occlusion {\n\n",
    ]
    for item in sys.argv[2:]:
        name, raw_path = item.split("=", 1)
        path = pathlib.Path(raw_path)
        source = path.read_text(encoding="utf-8")
        if ")cy_msl\"" in source:
            raise ValueError(f"{path}: source contains the raw-string delimiter")
        body = "\n    ".join(f'R"cy_msl({part})cy_msl"' for part in chunks(source))
        lines.append(f"/// {path.name}, {len(source.encode('utf-8'))} bytes.\n")
        lines.append(f"inline constexpr char {name}[] =\n    {body};\n\n")
    lines.append("}  // namespace cy::rendering::occlusion\n")
    pathlib.Path(sys.argv[1]).write_text("".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
