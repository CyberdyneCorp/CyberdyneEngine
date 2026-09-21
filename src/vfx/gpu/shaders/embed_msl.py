#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Embed checked-in MSL source modules for the VFX GPU scheduler."""

import pathlib
import sys


def main() -> int:
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <output.h> <name>=<module.metal> ...", file=sys.stderr)
        return 2
    lines = [
        "#pragma once\n",
        "// Compiled MSL for the VFX GPU scheduler. GENERATED — do not edit by hand.\n\n",
        "namespace cy::vfx::gpu {\n\n",
    ]
    for item in sys.argv[2:]:
        name, raw_path = item.split("=", 1)
        source = pathlib.Path(raw_path).read_text(encoding="utf-8")
        if ")cy_msl\"" in source:
            raise ValueError("MSL contains the raw-string delimiter")
        lines.append(f'inline constexpr char {name}[] = R"cy_msl({source})cy_msl";\n\n')
    lines.append("}  // namespace cy::vfx::gpu\n")
    pathlib.Path(sys.argv[1]).write_text("".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
