#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Embed checked-in MSL source modules for the frame pipeline."""

import pathlib
import sys


def argument_buffers(source: str, name: str) -> str:
    if name != "kParticleVertexMsl":
        return source
    source = source.replace("uint3 dimensions_0;", "packed_uint3 dimensions_0;")
    particle = "cyParticlePass_1 [[buffer(0)]]"
    frame = "cyFrameView_1 [[buffer(1)]]"
    if particle not in source or frame not in source:
        raise ValueError(f"{name}: expected Slang's compacted particle and frame slots")
    return source.replace(particle, "cyParticlePass_1 [[buffer(2)]]")


def main() -> int:
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <output.h> <name>=<module.metal> ...", file=sys.stderr)
        return 2
    lines = [
        "#pragma once\n",
        "// SPDX-License-Identifier: MIT\n",
        "// Compiled MSL for the particle renderer. GENERATED — do not edit by hand.\n\n",
        "#include <cy/core/base/types.h>\n\n",
        "namespace cy::rendering::particles {\n\n",
    ]
    for item in sys.argv[2:]:
        name, raw_path = item.split("=", 1)
        path = pathlib.Path(raw_path)
        source = argument_buffers(path.read_text(encoding="utf-8"), name)
        if ")cy_msl\"" in source:
            raise ValueError(f"{path}: source contains the raw-string delimiter")
        lines.append(f"/// {path.name}, {len(source.encode('utf-8'))} bytes.\n")
        lines.append(f'inline constexpr char {name}[] = R"cy_msl({source})cy_msl";\n\n')
    lines.append("}  // namespace cy::rendering::particles\n")
    pathlib.Path(sys.argv[1]).write_text("".join(lines), encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
