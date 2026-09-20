#!/usr/bin/env python3
"""Embed checked-in MSL source for the GPU skinning pass."""

import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(f"usage: {sys.argv[0]} <output.h> <module.metal>", file=sys.stderr)
        return 2
    source = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
    if ")cy_msl\"" in source:
        raise ValueError("MSL contains the raw-string delimiter")
    output = f'''#pragma once
// Compiled MSL for the GPU skinning dispatch. GENERATED — do not edit by hand.

#include <cy/core/base/types.h>

namespace cy::rendering::skinning {{

inline constexpr char kSkinVerticesMsl[] = R"cy_msl({source})cy_msl";

}}  // namespace cy::rendering::skinning
'''
    pathlib.Path(sys.argv[1]).write_text(output, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
