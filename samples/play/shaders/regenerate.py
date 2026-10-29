#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Recompile the play host's present shader and rewrite the two committed headers.

Run from anywhere; the paths are resolved from this file. `--slangc` is the compiler, normally the
one a Development build stages at <build>/Development/bin/slangc.

    python3 samples/play/shaders/regenerate.py --slangc build/dev/Development/bin/slangc

CHECKED IN RATHER THAN COMPILED BY THE BUILD, as CyberUI's are (src/ui/render/shaders/
regenerate.py, which this is): `CY_SHADER_SLANG` is off in Profile and Shipping, and a played game
must present in a build with no shader compiler at all. slangc runs from the repository root so the
#line directives in the MSL name repository-relative paths.

present.slang imports nothing. The MSL is embedded as Slang emits it; only the SPIR-V is exercised
on this host. DXIL is compiled and compared by `just build-shaders --strict` and not embedded.
"""

import argparse
import pathlib
import struct
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
SHADERS = pathlib.Path("src/rendering/shaders")
NAMESPACE = "cy::play"
# (source stem, entry point, stage, C++ name stem)
MODULES = (
    ("present", "cyPresentVertex", "vertex", "kPresentVertex"),
    ("present", "cyPresentFragment", "fragment", "kPresentFragment"),
)

PREAMBLE = """// SPDX-License-Identifier: MIT
#pragma once
// Compiled {kind} for the play host's present shader. GENERATED — do not edit by hand.
//
// Produced by samples/play/shaders/regenerate.py from present.slang. Checked in rather than
// compiled by the build because a played game must present in a build with no shader compiler.

#include <cy/core/base/types.h>

namespace {namespace} {{

"""


def spirv_header(modules: list[tuple[str, pathlib.Path]]) -> str:
    body = [PREAMBLE.format(kind="SPIR-V", namespace=NAMESPACE)]
    for name, path in modules:
        data = path.read_bytes()
        if len(data) % 4 != 0:
            raise ValueError(f"{path}: not a whole number of 32-bit words")
        words = struct.unpack(f"<{len(data) // 4}I", data)
        body.append(f"/// {path.name}, {len(words)} words.\n")
        body.append(f"inline constexpr u32 {name}[] = {{\n")
        for start in range(0, len(words), 6):
            row = ", ".join(f"0x{word:08X}U" for word in words[start:start + 6])
            body.append(f"    {row},\n")
        body.append("};\n\n")
    body.append(f"}}  // namespace {NAMESPACE}\n")
    return "".join(body)


def msl_header(modules: list[tuple[str, pathlib.Path]]) -> str:
    body = [PREAMBLE.format(kind="MSL", namespace=NAMESPACE)]
    for name, path in modules:
        source = path.read_text(encoding="utf-8")
        if ")cy_msl\"" in source:
            raise ValueError(f"{path}: source contains the raw-string delimiter")
        body.append(f"/// {path.name}, {len(source.encode('utf-8'))} bytes.\n")
        body.append(f'inline constexpr char {name}[] = R"cy_msl({source})cy_msl";\n\n')
    body.append(f"}}  // namespace {NAMESPACE}\n")
    return "".join(body)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--slangc", required=True)
    parser.add_argument("--clang-format", default="clang-format")
    args = parser.parse_args()
    source_dir = HERE.relative_to(ROOT)
    with tempfile.TemporaryDirectory() as scratch:
        out = pathlib.Path(scratch)
        spirv, msl = [], []
        for stem, entry, stage, name in MODULES:
            source = str(source_dir / f"{stem}.slang")
            common = [args.slangc, source, "-I", str(SHADERS), "-entry", entry, "-stage", stage]
            subprocess.run(common + ["-target", "spirv", "-profile", "spirv_1_5", "-o",
                                     str(out / f"{entry}.spv")], cwd=ROOT, check=True)
            subprocess.run(common + ["-target", "metal", "-o", str(out / f"{entry}.metal")],
                           cwd=ROOT, check=True)
            spirv.append((f"{name}Spirv", out / f"{entry}.spv"))
            msl.append((f"{name}Msl", out / f"{entry}.metal"))
        spirv_path = HERE.parent / "present_spirv.h"
        msl_path = HERE.parent / "present_msl.h"
        spirv_path.write_text(spirv_header(spirv), encoding="utf-8")
        msl_path.write_text(msl_header(msl), encoding="utf-8")
        subprocess.run([args.clang_format, "-i", str(spirv_path), str(msl_path)], cwd=ROOT,
                       check=True)
        print(f"wrote {spirv_path.relative_to(ROOT)} and {msl_path.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
