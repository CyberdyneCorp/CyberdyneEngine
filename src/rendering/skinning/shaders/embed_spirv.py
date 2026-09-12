#!/usr/bin/env python3
"""Turn the compiled skinning dispatch into the checked-in C++ header.

The skinning pass must exist in a build with no shader compiler — `CY_SHADER_SLANG` is off in Profile
and Shipping, and a renderer whose skinning only exists where Slang was built is a renderer with two
different frames. So skin.slang is compiled once, by hand, and embedded; this script is how that
is done reproducibly rather than by pasting numbers.

It is the same script as tests/render/shaders/embed_spirv.py and
src/backends/shader/tests/fixtures/embed_spirv.py, kept separate for the reason those two are kept
separate from each other: they write different namespaces, and a shared one would need a flag nobody
remembers to pass. Sixty lines duplicated is cheaper than that parameter.

The output is checked in, so it is subject to the formatting gate like any other header: run
`just quality-format` (or clang-format -i on the file) after regenerating. This script does not shell
out to the formatter itself, because the formatter's version is pinned by the build's own tooling and
a second place that chose one would be a second place to keep in step.

Usage:
    embed_spirv.py <output.h> <name>=<module.spv> [<name>=<module.spv> ...]
"""

import pathlib
import struct
import sys

HEADER = """#pragma once
// Compiled SPIR-V for the GPU skinning dispatch. GENERATED — do not edit by hand.
//
// Produced by src/rendering/skinning/shaders/embed_spirv.py from skin.slang; that file's
// header comment carries the exact slangc invocations. Checked in rather than compiled by the build
// because the skinning pass must exist in a build with NO shader compiler at all — which is every
// Profile and Shipping build, and any build configured with -DCY_SHADER_SLANG=OFF.

#include <cy/core/base/types.h>

namespace cy::rendering::skinning {

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
    body.append("}  // namespace cy::rendering::skinning\n")
    out.write_text("".join(body))
    print(f"wrote {out} — {len(argv) - 2} module(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
