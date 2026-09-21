#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Embed checked-in MSL source for the GPU skinning pass.

The MSL source is emitted as adjacent C++ raw-string literals that concatenate at compile
time. MSVC's logical-source-line limit chokes on a single raw string that spans more than a
few thousand lines, and the skinning MSL is over a thousand — so we split at blank lines
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
        # Prefer to split at a blank line so a reader who opens the header does not see the
        # split fall in the middle of a statement.
        cut = end
        for candidate in range(end, min(end + SPLIT_LINES, len(lines))):
            if candidate < len(lines) and lines[candidate].strip() == "":
                cut = candidate + 1
                break
        out.append("".join(lines[start:cut]))
        start = cut
    return out


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <output.h> <module.metal>", file=sys.stderr)
        return 2
    source = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
    if ")cy_msl\"" in source:
        raise ValueError("MSL contains the raw-string delimiter")
    parts = chunks(source)
    body = "\n    ".join(f'R"cy_msl({part})cy_msl"' for part in parts)
    output = f'''#pragma once
// Compiled MSL for the GPU skinning dispatch. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::skinning {{

inline constexpr char kSkinVerticesMsl[] =
    {body};

}}  // namespace cy::rendering::skinning
'''
    pathlib.Path(sys.argv[1]).write_text(output, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
