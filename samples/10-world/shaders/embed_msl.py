#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Embed checked-in MSL source modules for the world sample.

The MSL sources are emitted as adjacent C++ raw-string literals that concatenate at compile
time. MSVC's logical-source-line limit chokes on a single raw string that spans more than a
few thousand lines; the world shaders are over a thousand each — so we split at blank lines
about every SPLIT_LINES lines. GCC/Clang treat the concatenated form identically to one
literal.
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
            if candidate < len(lines) and lines[candidate].strip() == "":
                cut = candidate + 1
                break
        out.append("".join(lines[start:cut]))
        start = cut
    return out


def emit_literal(name: str, source: str) -> str:
    parts = chunks(source)
    body = "\n    ".join(f'R"cy_msl({part})cy_msl"' for part in parts)
    return f'inline constexpr char {name}[] =\n    {body};\n\n'


def main() -> int:
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <output.h> <name>=<module.metal> ...", file=sys.stderr)
        return 2
    output = pathlib.Path(sys.argv[1])
    lines = [
        "#pragma once\n",
        "// SPDX-License-Identifier: MIT\n",
        "// Compiled MSL for samples/10-world. GENERATED — do not edit by hand.\n\n",
        "#include <cy/core/base/types.h>\n\n",
        "namespace cy::sample::world {\n\n",
    ]
    for item in sys.argv[2:]:
        name, raw_path = item.split("=", 1)
        path = pathlib.Path(raw_path)
        source = path.read_text(encoding="utf-8")
        if ")cy_msl\"" in source:
            raise ValueError(f"{path}: source contains the raw-string delimiter")
        lines.append(f"/// {path.name}, {len(source.encode('utf-8'))} bytes.\n")
        lines.append(emit_literal(name, source))
    lines.append("}  // namespace cy::sample::world\n")
    output.write_text("".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
