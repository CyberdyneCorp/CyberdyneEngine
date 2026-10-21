#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Recompile the temporal upscaler's stages and rewrite the two committed headers.

Run from anywhere; the paths are resolved from this file. `--slangc` is the compiler, normally the
one a Development build stages at <build>/Development/bin/slangc.

    python3 src/rendering/upscale/shaders/regenerate.py --slangc build/dev/Development/bin/slangc

which runs, from the repository root, for each entry point below:

    slangc src/rendering/upscale/shaders/<file>.slang -I src/rendering/shaders \
           -entry <entry> -stage <stage> -target spirv -profile spirv_1_5 -o <entry>.spv
    slangc ... -target metal -o <entry>.metal

then embed_spirv.py and embed_msl.py into ../src/upscale_spirv.h and ../src/upscale_msl.h, and
clang-format over both. It is src/rendering/motion_blur/shaders/regenerate.py for this module's
stages, with a vertex and a fragment entry point beside the dispatches. The #line directives in the
MSL name repository-relative paths, because slangc is run from the repository root: the committed
text is then the same on every machine that regenerates it.
"""

import argparse
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[3]
SHADERS = pathlib.Path("src/rendering/shaders")
# (file stem, entry point, stage, SPIR-V array, MSL array)
ENTRIES = (
    ("upscale_reconstruct", "cyUpscaleReconstruct", "compute", "kUpscaleReconstructSpirv",
     "kUpscaleReconstructMsl"),
    ("upscale_sharpen", "cyUpscaleSharpen", "compute", "kUpscaleSharpenSpirv",
     "kUpscaleSharpenMsl"),
    ("upscale_copy", "cyUpscaleCopy", "compute", "kUpscaleCopySpirv", "kUpscaleCopyMsl"),
    ("upscale_resample", "cyUpscaleResampleVertex", "vertex", "kUpscaleResampleVertexSpirv",
     "kUpscaleResampleVertexMsl"),
    ("upscale_resample", "cyUpscaleResampleFragment", "fragment", "kUpscaleResampleFragmentSpirv",
     "kUpscaleResampleFragmentMsl"),
)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--slangc", required=True)
    parser.add_argument("--clang-format", default="clang-format")
    args = parser.parse_args()
    source_dir = HERE.relative_to(ROOT)
    with tempfile.TemporaryDirectory() as scratch:
        out = pathlib.Path(scratch)
        spirv, msl = [], []
        for stem, entry, stage, spirv_name, msl_name in ENTRIES:
            source = str(source_dir / f"{stem}.slang")
            common = [args.slangc, source, "-I", str(SHADERS), "-entry", entry, "-stage", stage]
            subprocess.run(common + ["-target", "spirv", "-profile", "spirv_1_5", "-o",
                                     str(out / f"{entry}.spv")], cwd=ROOT, check=True)
            subprocess.run(common + ["-target", "metal", "-o", str(out / f"{entry}.metal")],
                           cwd=ROOT, check=True)
            spirv.append(f"{spirv_name}={out / f'{entry}.spv'}")
            msl.append(f"{msl_name}={out / f'{entry}.metal'}")
        spirv_header = HERE.parent / "src" / "upscale_spirv.h"
        msl_header = HERE.parent / "src" / "upscale_msl.h"
        subprocess.run([sys.executable, str(HERE / "embed_spirv.py"), str(spirv_header)] + spirv,
                       check=True)
        subprocess.run([sys.executable, str(HERE / "embed_msl.py"), str(msl_header)] + msl,
                       check=True)
        subprocess.run([args.clang_format, "-i", str(spirv_header), str(msl_header)], cwd=ROOT,
                       check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
