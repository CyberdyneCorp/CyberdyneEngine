#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile `cy/bloom.slang` and write the checked-in `bloom_spirv.h` and `bloom_msl.h`.

The same arrangement as the frame's own modules (`embed_spirv.py`, `embed_msl.py`): `CY_SHADER_SLANG`
is off in Profile and Shipping, so the compiled modules are committed and this script is how they
are regenerated reproducibly rather than by pasting numbers. It runs slangc itself because bloom's
four entry points share one invocation shape and one Metal slot rule.

THE METAL SLOT RULE. Slang compacts Metal buffer indices to the resources an entry point uses — the
push block at buffer(0) and set 2's argument buffer at buffer(1) — while the RHI binds a pipeline's
sets at their own indices and its push constants immediately after them (`rhi-metal`'s
`push_constant_index = set_count`). Bloom's layout has the frame's three sets, so the argument
buffer moves to buffer(2) and the push block to buffer(3). Every rewrite is checked, so a Slang that
compacts differently fails here rather than on a device.

Usage (from the repository root):
    python3 src/rendering/pipeline/shaders/embed_bloom.py <path to slangc>
    clang-format -i src/rendering/pipeline/shaders/bloom_spirv.h
"""

import pathlib
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parents[4]
SHADERS = ROOT / "src/rendering/shaders"
OUT = pathlib.Path(__file__).resolve().parent

# (C++ name stem, Slang entry point)
ENTRIES = [
    ("Prefilter", "bloomPrefilter"),
    ("Downsample", "bloomDownsample"),
    ("Upsample", "bloomUpsample"),
    ("Composite", "bloomComposite"),
]

PREAMBLE = """#pragma once
// SPDX-License-Identifier: MIT
// Compiled {kind} for bloom's four passes. GENERATED — do not edit by hand.
//
// Produced by src/rendering/pipeline/shaders/embed_bloom.py from
// src/rendering/shaders/cy/bloom.slang. The vertex stage is the frame's own `fullscreenVertex`
// (`frame_spirv.h`, `frame_msl.h`) and is not repeated here.

#include <cy/core/base/types.h>

namespace cy::rendering::pipeline {{

"""


def compile_entry(slangc: str, entry: str, target: list[str], out: pathlib.Path) -> None:
    command = [slangc, "cy/bloom.slang", "-I", ".", *target, "-entry", entry, "-stage",
               "fragment", "-o", str(out)]
    subprocess.run(command, cwd=SHADERS, check=True)


def metal_slots(source: str, entry: str) -> str:
    rewrites = [
        ("bloomSet_1 [[buffer(1)]]", "bloomSet_1 [[buffer(2)]]"),
        ("bloomPush_1 [[buffer(0)]]", "bloomPush_1 [[buffer(3)]]"),
    ]
    for original, replacement in rewrites:
        if original not in source:
            raise ValueError(f"{entry}: expected `{original}` in Slang's Metal output")
        source = source.replace(original, replacement)
    # Slang writes absolute #line paths; keep the header independent of whoever regenerated it.
    return source.replace(str(SHADERS) + "/", "src/rendering/shaders/")


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    slangc = str(pathlib.Path(argv[1]).resolve())
    spirv = [PREAMBLE.format(kind="SPIR-V")]
    msl = [PREAMBLE.format(kind="MSL")]
    with tempfile.TemporaryDirectory() as scratch:
        work = pathlib.Path(scratch)
        for stem, entry in ENTRIES:
            spv = work / f"{entry}.spv"
            metal = work / f"{entry}.metal"
            compile_entry(slangc, entry, ["-target", "spirv", "-profile", "spirv_1_5"], spv)
            compile_entry(slangc, entry, ["-target", "metal"], metal)

            data = spv.read_bytes()
            words = struct.unpack(f"<{len(data) // 4}I", data)
            spirv.append(f"/// {entry}.spv, {len(words)} words.\n")
            spirv.append(f"inline constexpr u32 kBloom{stem}Spirv[] = {{\n")
            for start in range(0, len(words), 6):
                row = ", ".join(f"0x{word:08X}U" for word in words[start:start + 6])
                spirv.append(f"    {row},\n")
            spirv.append("};\n\n")

            source = metal_slots(metal.read_text(encoding="utf-8"), entry)
            if ")cy_msl\"" in source:
                raise ValueError(f"{entry}: source contains the raw-string delimiter")
            msl.append(f"/// {entry}.metal, {len(source.encode('utf-8'))} bytes.\n")
            msl.append(f'inline constexpr char kBloom{stem}Msl[] = R"cy_msl({source})cy_msl";\n\n')
    for body in (spirv, msl):
        body.append("}  // namespace cy::rendering::pipeline\n")
    (OUT / "bloom_spirv.h").write_text("".join(spirv), encoding="utf-8")
    (OUT / "bloom_msl.h").write_text("".join(msl), encoding="utf-8")
    print(f"wrote {OUT / 'bloom_spirv.h'} and {OUT / 'bloom_msl.h'} — {len(ENTRIES)} entry points")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
