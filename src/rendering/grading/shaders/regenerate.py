#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Recompile the graded resolve and the three metering dispatches and rewrite the two headers.

Run from anywhere; the paths are resolved from this file. `--slangc` is the compiler, normally the
one a Development build stages at <build>/Development/bin/slangc.

    python3 src/rendering/grading/shaders/regenerate.py --slangc build/dev/Development/bin/slangc

The vertex stage is the shader library's own `fullscreenVertex`, compiled from cy/fullscreen.slang
here rather than copied, so the graded resolve's triangle is the frame resolve's triangle.

The #line directives in the MSL name repository-relative paths, because slangc is run from the
repository root: the committed text is then the same on every machine that regenerates it.
"""

import argparse
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[3]
SHADERS = pathlib.Path("src/rendering/shaders")
# (source, entry point, stage, SPIR-V name, MSL name)
MODULES = (
    (SHADERS / "cy" / "fullscreen.slang", "fullscreenVertex", "vertex",
     "kGradingVertexSpirv", "kGradingVertexMsl"),
    (None, "cyGradedResolve", "fragment", "kGradedResolveSpirv", "kGradedResolveMsl"),
    (None, "cyExposureClear", "compute", "kExposureClearSpirv", "kExposureClearMsl"),
    (None, "cyExposureHistogram", "compute", "kExposureHistogramSpirv", "kExposureHistogramMsl"),
    (None, "cyExposureAdapt", "compute", "kExposureAdaptSpirv", "kExposureAdaptMsl"),
)
SOURCES = {
    "cyGradedResolve": "graded_resolve.slang",
    "cyExposureClear": "exposure_clear.slang",
    "cyExposureHistogram": "exposure_histogram.slang",
    "cyExposureAdapt": "exposure_adapt.slang",
}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--slangc", required=True)
    parser.add_argument("--clang-format", default="clang-format")
    args = parser.parse_args()
    source_dir = HERE.relative_to(ROOT)
    with tempfile.TemporaryDirectory() as scratch:
        out = pathlib.Path(scratch)
        spirv, msl = [], []
        for source, entry, stage, spirv_name, msl_name in MODULES:
            path = str(source if source is not None else source_dir / SOURCES[entry])
            common = [args.slangc, path, "-I", str(SHADERS), "-entry", entry, "-stage", stage]
            subprocess.run(common + ["-target", "spirv", "-profile", "spirv_1_5", "-o",
                                     str(out / f"{entry}.spv")], cwd=ROOT, check=True)
            subprocess.run(common + ["-target", "metal", "-o", str(out / f"{entry}.metal")],
                           cwd=ROOT, check=True)
            spirv.append(f"{spirv_name}={out / f'{entry}.spv'}")
            msl.append(f"{msl_name}={out / f'{entry}.metal'}")
        spirv_header = HERE.parent / "src" / "grading_spirv.h"
        msl_header = HERE.parent / "src" / "grading_msl.h"
        subprocess.run([sys.executable, str(HERE / "embed_spirv.py"), str(spirv_header)] + spirv,
                       check=True)
        subprocess.run([sys.executable, str(HERE / "embed_msl.py"), str(msl_header)] + msl,
                       check=True)
        subprocess.run([args.clang_format, "-i", str(spirv_header), str(msl_header)], cwd=ROOT,
                       check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
