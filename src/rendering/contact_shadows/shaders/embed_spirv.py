#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Turn the compiled contact shadow dispatch into the checked-in C++ header.

The pass must exist in a build with no shader compiler — `CY_SHADER_SLANG` is off in Profile and
Shipping — so contact_shadows.slang is compiled once and embedded; this script is how that is done
reproducibly rather than by pasting numbers. It is src/rendering/skinning/shaders/embed_spirv.py
with this module's namespace, kept separate for the reason that one gives.

The output is checked in and is subject to the formatting gate: run clang-format -i on it afterwards.
`regenerate.py` beside this file does all of it.

Usage:
    embed_spirv.py <output.h> <name>=<module.spv> [<name>=<module.spv> ...]
"""

import pathlib
import struct
import sys

HEADER = """// SPDX-License-Identifier: MIT
#pragma once
// Compiled SPIR-V for the contact shadow dispatch. GENERATED — do not edit by hand.
//
// Produced by src/rendering/contact_shadows/shaders/embed_spirv.py from contact_shadows.slang, whose
// header comment carries the slangc invocations. Checked in rather than compiled by the build
// because the pass must exist in a build with NO shader compiler at all.

#include <cy/core/base/types.h>

namespace cy::rendering::contact_shadows {

"""


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        sys.stderr.write(__doc__)
        return 2

    out = pathlib.Path(argv[1])
    body = [HEADER]
    for pair in argv[2:]:
        name, _, path = pair.partition("=")
        data = pathlib.Path(path).read_bytes()
        if len(data) % 4 != 0:
            sys.stderr.write(f"{path}: not a whole number of 32-bit words\n")
            return 1
        words = struct.unpack(f"<{len(data) // 4}I", data)
        body.append(f"/// {pathlib.Path(path).name}, {len(words)} words.\n")
        body.append(f"inline constexpr u32 {name}[] = {{\n")
        for start in range(0, len(words), 6):
            row = ", ".join(f"0x{word:08X}U" for word in words[start:start + 6])
            body.append(f"    {row},\n")
        body.append("};\n\n")
    body.append("}  // namespace cy::rendering::contact_shadows\n")
    out.write_text("".join(body))
    print(f"wrote {out} — {len(argv) - 2} module(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
