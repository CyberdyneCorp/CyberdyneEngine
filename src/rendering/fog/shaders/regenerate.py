#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Recompile fog_march.slang and rewrite the two committed headers.

Run from anywhere; the paths are resolved from this file. `--slangc` is the compiler, normally the
one a Development build stages at <build>/Development/bin/slangc.

    python3 src/rendering/fog/shaders/regenerate.py --slangc build/dev/Development/bin/slangc

src/rendering/contact_shadows/shaders/regenerate.py is the same script for that module's dispatch.
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
# (output stem, entry point, SPIR-V name, MSL name, defines): the froxel texture the frame reads, and
# the atmosphere-table variant a consumer of `cy/aerial_perspective.slang` binds instead.
MODULES = (
    ("volumetric_fog", "cyVolumetricFog", "kVolumetricFogSpirv", "kVolumetricFogMsl", ()),
    ("volumetric_fog_table", "cyVolumetricFog", "kVolumetricFogTableSpirv",
     "kVolumetricFogTableMsl", ("-D", "CY_FOG_AIR_TABLE=1")),
)
SOURCE = "fog_march.slang"


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--slangc", required=True)
    parser.add_argument("--clang-format", default="clang-format")
    args = parser.parse_args()
    source_dir = HERE.relative_to(ROOT)
    with tempfile.TemporaryDirectory() as scratch:
        out = pathlib.Path(scratch)
        spirv, msl = [], []
        for stem, entry, spirv_name, msl_name, defines in MODULES:
            source = str(source_dir / SOURCE)
            common = [args.slangc, source, "-I", str(SHADERS), *defines, "-entry", entry, "-stage",
                      "compute"]
            subprocess.run(common + ["-target", "spirv", "-profile", "spirv_1_5", "-o",
                                     str(out / f"{stem}.spv")], cwd=ROOT, check=True)
            subprocess.run(common + ["-target", "metal", "-o", str(out / f"{stem}.metal")],
                           cwd=ROOT, check=True)
            spirv.append(f"{spirv_name}={out / f'{stem}.spv'}")
            msl.append(f"{msl_name}={out / f'{stem}.metal'}")
        spirv_header = HERE.parent / "src" / "fog_spirv.h"
        msl_header = HERE.parent / "src" / "fog_msl.h"
        subprocess.run([sys.executable, str(HERE / "embed_spirv.py"), str(spirv_header)] + spirv,
                       check=True)
        subprocess.run([sys.executable, str(HERE / "embed_msl.py"), str(msl_header)] + msl,
                       check=True)
        subprocess.run([args.clang_format, "-i", str(spirv_header), str(msl_header)], cwd=ROOT,
                       check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
