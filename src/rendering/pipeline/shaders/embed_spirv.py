#!/usr/bin/env python3
"""Turn the frame's compiled shader modules into the checked-in C++ header.

`CY_SHADER_SLANG` is off by default, so an ordinary build has no Slang compiler to call. The frame's
Slang source — `src/rendering/shaders/cy/frame.slang` and the standard library's own
`cy/fullscreen.slang` — is therefore compiled once, by hand, and embedded; this script is how that
is done reproducibly rather than by pasting numbers.

It is the same script as samples/03-first-light/shaders/embed_spirv.py, kept separate for the reason
that one records: the copies write different namespaces, and a shared one would need a flag to say
which. Sixty lines duplicated is cheaper than a parameter nobody remembers.

`src/rendering/shaders/cy/frame.slang`'s header comment carries the exact slangc invocations, and
`just` has no recipe for them because `build-shaders` compiles a *project's* shaders rather than the
engine's own committed artefacts.

The output is checked in, so it is subject to the formatting gate like any other header: run
`just quality-format` (or clang-format -i on the file) after regenerating.

Usage:
    embed_spirv.py <output.h> <name>=<module.spv> [<name>=<module.spv> ...]
"""

import pathlib
import struct
import sys

HEADER = """#pragma once
// Compiled SPIR-V for the frame's own passes. GENERATED — do not edit by hand.
//
// Produced by src/rendering/pipeline/shaders/embed_spirv.py from
// src/rendering/shaders/cy/frame.slang and src/rendering/shaders/cy/fullscreen.slang; the first of
// those carries the exact slangc invocations in its header comment.
//
// CHECKED IN RATHER THAN COMPILED BY THE BUILD, for the reason samples/03-first-light's copy gives:
// `CY_SHADER_SLANG` is off by default and in Profile and Shipping, and a renderer that only draws on
// a machine with a shader toolchain is a renderer nobody can run. `src/backends/shader/`'s SPIR-V
// passthrough front end is the shipping path by the same argument.

#include <cy/core/base/types.h>

namespace cy::rendering::pipeline {

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
    body.append("}  // namespace cy::rendering::pipeline\n")
    out.write_text("".join(body))
    print(f"wrote {out} — {len(argv) - 2} module(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
