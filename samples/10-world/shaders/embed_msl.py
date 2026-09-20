#!/usr/bin/env python3
"""Embed checked-in MSL source modules for the world sample."""

import pathlib
import sys


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
        lines.append(f'inline constexpr char {name}[] = R"cy_msl({source})cy_msl";\n\n')
    lines.append("}  // namespace cy::sample::world\n")
    output.write_text("".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
