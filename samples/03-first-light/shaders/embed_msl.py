#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Embed checked-in MSL source modules for the first-light sample.

Usage:
    embed_msl.py <output.h> <name>=<module.metal> [<name>=<module.metal> ...]
"""

import pathlib
import sys


HEADER = """#pragma once
// SPDX-License-Identifier: MIT
// Compiled MSL for samples/03-first-light. GENERATED — do not edit by hand.
//
// Produced by samples/03-first-light/shaders/embed_msl.py from first_light.slang. The native
// artefacts are checked in so a shipping build does not need to link the Slang compiler.

#include <cy/core/base/types.h>

namespace cy::sample::first_light {

"""


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        sys.stderr.write(__doc__)
        return 2

    output = pathlib.Path(argv[1])
    body = [HEADER]
    for pair in argv[2:]:
        name, separator, path_text = pair.partition("=")
        if not separator or not name:
            sys.stderr.write(f"invalid module argument: {pair}\n")
            return 2
        path = pathlib.Path(path_text)
        source = path.read_text()
        if ")cy_msl\"" in source:
            sys.stderr.write(f"{path}: source contains the raw-string terminator\n")
            return 1
        body.append(f"/// {path.name}, {len(source.encode('utf-8'))} bytes.\n")
        body.append(f"inline constexpr char {name}[] = R\"cy_msl({source})cy_msl\";\n\n")
    body.append("}  // namespace cy::sample::first_light\n")
    output.write_text("".join(body))
    print(f"wrote {output} — {len(argv) - 2} module(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
