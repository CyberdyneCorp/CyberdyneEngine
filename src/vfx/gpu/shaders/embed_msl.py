#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Embed checked-in MSL source modules for the VFX GPU scheduler.

Usage: embed_msl.py [--title <what the MSL is>] <output.h> <name>=<module.metal> ...

`--title` replaces "the VFX GPU scheduler" in the header's first comment line; the iOS sample's
spark-plume kernel is embedded by this script too.
"""

import pathlib
import sys


def main() -> int:
    arguments = sys.argv[1:]
    title = "the VFX GPU scheduler"
    if len(arguments) > 1 and arguments[0] == "--title":
        title, arguments = arguments[1], arguments[2:]
    if len(arguments) < 2:
        print(f"usage: {sys.argv[0]} [--title <text>] <output.h> <name>=<module.metal> ...",
              file=sys.stderr)
        return 2
    lines = [
        "// SPDX-License-Identifier: MIT\n",
        "#pragma once\n",
        f"// Compiled MSL for {title}. GENERATED — do not edit by hand.\n\n",
        "namespace cy::vfx::gpu {\n\n",
    ]
    for item in arguments[1:]:
        name, raw_path = item.split("=", 1)
        source = pathlib.Path(raw_path).read_text(encoding="utf-8")
        if ")cy_msl\"" in source:
            raise ValueError("MSL contains the raw-string delimiter")
        lines.append(f'inline constexpr char {name}[] = R"cy_msl({source})cy_msl";\n\n')
    lines.append("}  // namespace cy::vfx::gpu\n")
    pathlib.Path(arguments[0]).write_text("".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
