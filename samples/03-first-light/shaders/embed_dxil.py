#!/usr/bin/env python3
"""Embed named DXIL files in one deterministic C++ header."""

from pathlib import Path
import sys


def main() -> int:
    if len(sys.argv) < 3:
        print("usage: embed_dxil.py OUT NAME=FILE [NAME=FILE ...]", file=sys.stderr)
        return 2
    lines = [
        "#pragma once",
        "// Compiled DXIL for samples/03-first-light. GENERATED — do not edit by hand.",
        "// clang-format off",
        "#include <cy/core/base/types.h>",
        "namespace cy::sample::first_light {",
    ]
    for assignment in sys.argv[2:]:
        name, raw_path = assignment.split("=", 1)
        data = Path(raw_path).read_bytes()
        lines.append(f"inline constexpr u8 {name}[] = {{")
        for start in range(0, len(data), 12):
            chunk = ", ".join(f"0x{byte:02X}U" for byte in data[start : start + 12])
            lines.append(f"    {chunk},")
        lines.append("};")
    lines.append("}  // namespace cy::sample::first_light")
    Path(sys.argv[1]).write_bytes(("\n".join(lines) + "\n").encode("utf-8"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
