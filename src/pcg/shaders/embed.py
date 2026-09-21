#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Embed the GPU PCG conformance shader's SPIR-V and MSL outputs."""

from pathlib import Path
import struct
import sys


def words(data: bytes) -> str:
    values = struct.unpack(f"<{len(data) // 4}I", data)
    lines = []
    for offset in range(0, len(values), 8):
        lines.append("    " + ", ".join(f"0x{value:08X}U" for value in values[offset : offset + 8]) + ",")
    return "\n".join(lines)


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: embed.py <output.h> <shader.spv> <shader.metal>")
    output, spirv_path, msl_path = map(Path, sys.argv[1:])
    spirv = spirv_path.read_bytes()
    if len(spirv) % 4:
        raise SystemExit("SPIR-V byte length is not word aligned")
    msl = msl_path.read_text(encoding="utf-8")
    if ")cy_msl\"" in msl:
        raise SystemExit("MSL contains the raw-string delimiter")
    output.write_text(
        """#pragma once
// GENERATED from gpu_conformance.slang by embed.py. Do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::pcg::gpu {

inline constexpr u32 kCandidateSpirv[] = {
"""
        + words(spirv)
        + "\n};\n\ninline constexpr char kCandidateMsl[] = R\"cy_msl("
        + msl
        + ")cy_msl\";\n\n}  // namespace cy::pcg::gpu\n",
        encoding="utf-8",
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

