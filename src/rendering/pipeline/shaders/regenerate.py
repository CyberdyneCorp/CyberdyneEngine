#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Recompile the frame's shaders and rewrite frame_spirv.h and frame_msl.h.

Run from anywhere; the paths are resolved from this file. `--slangc` is the compiler, normally the
one a Development build stages at <build>/Development/bin/slangc:

    python3 src/rendering/pipeline/shaders/regenerate.py --slangc build/dev/Development/bin/slangc

It runs the slangc invocations `cy/frame.slang`'s header comment lists, for every entry point of
`cy/frame.slang` and `cy/fullscreen.slang`, then hands the modules to `embed_spirv.py` and
`embed_msl.py` — the embedders that patch Slang's MSL to the RHI's layout and split a module over
the string-literal limit into consteval-joined chunks. slangc runs from the repository root so the
#line directives in the MSL name repository-relative paths and the committed text is the same on
every machine. The frame's Metal build defines CY_FRAME_METAL and CY_MATERIAL_METAL_ARGUMENT_BUFFER,
as `cy/frame.slang`'s header asks; the fullscreen passes do not import cy.frame and take neither.
"""

import argparse
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[3]
SHADERS = pathlib.Path("src/rendering/shaders")
METAL_DEFINES = ["-D", "CY_FRAME_METAL=1", "-D", "CY_MATERIAL_METAL_ARGUMENT_BUFFER=1"]
# (source, entry point, stage, module file stem, C++ name stem, frame defines on Metal)
MODULES = (
    ("cy/frame.slang", "cyDepthVertex", "vertex", "DepthVertex", "kFrameDepthVertex", True),
    ("cy/frame.slang", "cyDepthFragment", "fragment", "DepthFragment", "kFrameDepthFragment",
     True),
    ("cy/frame.slang", "cyShadowVertex", "vertex", "ShadowVertex", "kFrameShadowVertex", True),
    ("cy/frame.slang", "cyShadowFragment", "fragment", "ShadowFragment", "kFrameShadowFragment",
     True),
    ("cy/frame.slang", "cyForwardVertex", "vertex", "ForwardVertex", "kFrameForwardVertex", True),
    ("cy/frame.slang", "cyForwardFragment", "fragment", "ForwardFragment",
     "kFrameForwardFragment", True),
    ("cy/frame.slang", "cySkinnedDepthVertex", "vertex", "SkinnedDepthVertex",
     "kFrameSkinnedDepthVertex", True),
    ("cy/frame.slang", "cySkinnedForwardVertex", "vertex", "SkinnedForwardVertex",
     "kFrameSkinnedForwardVertex", True),
    ("cy/fullscreen.slang", "fullscreenVertex", "vertex", "ResolveVertex", "kFrameResolveVertex",
     False),
    ("cy/fullscreen.slang", "fullscreenResolve", "fragment", "ResolveFragment",
     "kFrameResolveFragment", False),
    ("cy/fullscreen.slang", "temporalResolve", "fragment", "TemporalFragment",
     "kFrameTemporalFragment", False),
)


def compile_all(slangc: str, out: pathlib.Path) -> tuple[list[str], list[str]]:
    spirv, msl = [], []
    for source, entry, stage, stem, name, frame in MODULES:
        common = [slangc, str(SHADERS / source), "-I", str(SHADERS), "-entry", entry, "-stage",
                  stage]
        spv = out / f"{stem}.spv"
        metal = out / f"{stem}.metal"
        subprocess.run(common + ["-target", "spirv", "-profile", "spirv_1_5", "-o", str(spv)],
                       cwd=ROOT, check=True)
        defines = METAL_DEFINES if frame else []
        subprocess.run(common + defines + ["-target", "metal", "-o", str(metal)], cwd=ROOT,
                       check=True)
        spirv.append(f"{name}Spirv={spv}")
        msl.append(f"{name}Msl={metal}")
    return spirv, msl


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--slangc", required=True)
    parser.add_argument("--clang-format", default="clang-format")
    parser.add_argument("--keep", help="copy the compiled modules into this directory too")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as scratch:
        out = pathlib.Path(scratch)
        spirv, msl = compile_all(args.slangc, out)
        spirv_path = HERE / "frame_spirv.h"
        msl_path = HERE / "frame_msl.h"
        subprocess.run([sys.executable, str(HERE / "embed_spirv.py"), str(spirv_path)] + spirv,
                       cwd=ROOT, check=True)
        subprocess.run([sys.executable, str(HERE / "embed_msl.py"), str(msl_path)] + msl,
                       cwd=ROOT, check=True)
        subprocess.run([args.clang_format, "-i", str(spirv_path), str(msl_path)], cwd=ROOT,
                       check=True)
        if args.keep:
            keep = pathlib.Path(args.keep)
            keep.mkdir(parents=True, exist_ok=True)
            for module in out.iterdir():
                (keep / module.name).write_bytes(module.read_bytes())
        print(f"wrote {spirv_path.relative_to(ROOT)} and {msl_path.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
