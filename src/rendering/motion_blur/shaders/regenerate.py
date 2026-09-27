#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Recompile the three motion blur dispatches and rewrite the two committed headers.

Run from anywhere; the paths are resolved from this file. `--slangc` is the compiler, normally the
one a Development build stages at <build>/Development/bin/slangc.

    python3 src/rendering/motion_blur/shaders/regenerate.py --slangc build/dev/Development/bin/slangc

which runs, from the repository root, for each of tile_max, neighbour_max and gather:

    slangc src/rendering/motion_blur/shaders/motion_blur_<stem>.slang -I src/rendering/shaders \
           -entry <entry> -stage compute -target spirv -profile spirv_1_5 -o <stem>.spv
    slangc ... -target metal -o <stem>.metal

then embed_spirv.py and embed_msl.py into ../src/motion_blur_spirv.h and ../src/motion_blur_msl.h,
and clang-format over both. It is src/rendering/occlusion/shaders/regenerate.py for this module's
dispatches. The #line directives in the MSL name repository-relative paths, because slangc is run
from the repository root: the committed text is then the same on every machine that regenerates it.
"""

import argparse
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[3]
SHADERS = pathlib.Path("src/rendering/shaders")
MODULES = (
    ("motion_blur_tile_max", "cyMotionBlurTileMax", "kMotionBlurTileMaxSpirv",
     "kMotionBlurTileMaxMsl"),
    ("motion_blur_neighbour_max", "cyMotionBlurNeighbourMax", "kMotionBlurNeighbourMaxSpirv",
     "kMotionBlurNeighbourMaxMsl"),
    ("motion_blur_gather", "cyMotionBlurGather", "kMotionBlurGatherSpirv",
     "kMotionBlurGatherMsl"),
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
        for stem, entry, spirv_name, msl_name in MODULES:
            source = str(source_dir / f"{stem}.slang")
            common = [args.slangc, source, "-I", str(SHADERS), "-entry", entry, "-stage", "compute"]
            subprocess.run(common + ["-target", "spirv", "-profile", "spirv_1_5", "-o",
                                     str(out / f"{stem}.spv")], cwd=ROOT, check=True)
            subprocess.run(common + ["-target", "metal", "-o", str(out / f"{stem}.metal")],
                           cwd=ROOT, check=True)
            spirv.append(f"{spirv_name}={out / f'{stem}.spv'}")
            msl.append(f"{msl_name}={out / f'{stem}.metal'}")
        spirv_header = HERE.parent / "src" / "motion_blur_spirv.h"
        msl_header = HERE.parent / "src" / "motion_blur_msl.h"
        subprocess.run([sys.executable, str(HERE / "embed_spirv.py"), str(spirv_header)] + spirv,
                       check=True)
        subprocess.run([sys.executable, str(HERE / "embed_msl.py"), str(msl_header)] + msl,
                       check=True)
        subprocess.run([args.clang_format, "-i", str(spirv_header), str(msl_header)], cwd=ROOT,
                       check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
