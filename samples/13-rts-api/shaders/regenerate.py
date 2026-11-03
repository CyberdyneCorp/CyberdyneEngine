#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Recompile samples/13-rts-api's present pass and rewrite its two committed headers.

    python3 samples/13-rts-api/shaders/regenerate.py --slangc build/dev/Development/bin/slangc

runs, from the repository root, for each entry point of present.slang:

    slangc samples/13-rts-api/shaders/present.slang -entry <entry> -stage <stage> \
           -target spirv -profile spirv_1_5 -o <entry>.spv
    slangc ... -target metal -o <entry>.metal

and writes host/present_spirv.h and host/present_msl.h, then clang-format over both. Run from the
root so the #line directives in the MSL name repository-relative paths, the same on every machine.
tools/shaders/embedded_headers.toml's `rts_present` group runs this script, and
`just quality-shader-headers` fails when the headers are not what it produces.
"""

import argparse
import pathlib
import struct
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SOURCE = "present.slang"
# (entry point, stage, SPIR-V array, MSL array)
ENTRIES = (
    ("cyRtsPresentVertex", "vertex", "kPresentVertexSpirv", "kPresentVertexMsl"),
    ("cyRtsPresentFragment", "fragment", "kPresentFragmentSpirv", "kPresentFragmentMsl"),
)
PREAMBLE = """#pragma once
// SPDX-License-Identifier: MIT
// Compiled {kind} for samples/13-rts-api's present pass. GENERATED — do not edit by hand.
//
// Written by samples/13-rts-api/shaders/regenerate.py from present.slang.

#include <cy/core/base/types.h>

namespace sample::rts::present {{

"""
CLOSING = "}  // namespace sample::rts::present\n"
SPLIT_LINES = 300


def spirv_header(modules: list[tuple[str, pathlib.Path]]) -> str:
    body = [PREAMBLE.format(kind="SPIR-V")]
    for name, path in modules:
        data = path.read_bytes()
        words = struct.unpack(f"<{len(data) // 4}I", data)
        body.append(f"/// {path.name}, {len(words)} words.\n")
        body.append(f"inline constexpr cy::u32 {name}[] = {{\n")
        for start in range(0, len(words), 6):
            body.append("    " + ", ".join(f"0x{word:08X}U" for word in words[start:start + 6]))
            body.append(",\n")
        body.append("};\n\n")
    body.append(CLOSING)
    return "".join(body)


def chunks(source: str) -> list[str]:
    # MSVC's logical-source-line limit rejects one raw string spanning thousands of lines, so the
    # source is split into adjacent literals at a blank line about every SPLIT_LINES lines.
    lines = source.splitlines(keepends=True)
    out: list[str] = []
    start = 0
    while start < len(lines):
        cut = min(start + SPLIT_LINES, len(lines))
        for candidate in range(cut, min(cut + SPLIT_LINES, len(lines))):
            if lines[candidate].strip() == "":
                cut = candidate + 1
                break
        out.append("".join(lines[start:cut]))
        start = cut
    return out


def msl_header(modules: list[tuple[str, pathlib.Path]]) -> str:
    body = [PREAMBLE.format(kind="MSL")]
    for name, path in modules:
        source = path.read_text(encoding="utf-8")
        if ')cy_msl"' in source:
            raise ValueError(f"{path}: the source contains the raw-string delimiter")
        literal = "\n    ".join(f'R"cy_msl({part})cy_msl"' for part in chunks(source))
        body.append(f"/// {path.name}, {len(source.encode('utf-8'))} bytes.\n")
        body.append(f"inline constexpr char {name}[] =\n    {literal};\n\n")
    body.append(CLOSING)
    return "".join(body)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--slangc", required=True)
    parser.add_argument("--clang-format", default="clang-format")
    args = parser.parse_args()
    source = str(HERE.relative_to(ROOT) / SOURCE)
    with tempfile.TemporaryDirectory() as scratch:
        out = pathlib.Path(scratch)
        spirv, msl = [], []
        for entry, stage, spirv_name, msl_name in ENTRIES:
            common = [args.slangc, source, "-entry", entry, "-stage", stage]
            subprocess.run(common + ["-target", "spirv", "-profile", "spirv_1_5", "-o",
                                     str(out / f"{entry}.spv")], cwd=ROOT, check=True)
            subprocess.run(common + ["-target", "metal", "-o", str(out / f"{entry}.metal")],
                           cwd=ROOT, check=True)
            spirv.append((spirv_name, out / f"{entry}.spv"))
            msl.append((msl_name, out / f"{entry}.metal"))
        headers = {HERE.parent / "host" / "present_spirv.h": spirv_header(spirv),
                   HERE.parent / "host" / "present_msl.h": msl_header(msl)}
        for path, text in headers.items():
            path.write_text(text, encoding="utf-8")
        subprocess.run([args.clang_format, "-i"] + [str(path) for path in headers], cwd=ROOT,
                       check=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
