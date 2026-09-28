#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Recompile the device GI dispatches and rewrite the two committed headers.

Run from anywhere; the paths are resolved from this file. `--slangc` is the compiler, normally the
one a Development build stages at <build>/Development/bin/slangc.

    python3 src/rendering/gi_gpu/shaders/regenerate.py --slangc build/dev/Development/bin/slangc

One module per ENTRY POINT rather than per file: gi_cards.slang holds two, and each is its own
pipeline. The #line directives in the MSL name repository-relative paths, because slangc is run from
the repository root: the committed text is then the same on every machine that regenerates it.
"""

import argparse
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[3]
MODULES = (
    ("gi_trace", "cyGiTraceRays", "kGiTraceRaysSpirv", "kGiTraceRaysMsl"),
    ("gi_cards", "cyGiShadeCards", "kGiShadeCardsSpirv", "kGiShadeCardsMsl"),
    ("gi_cards", "cyGiCommitCards", "kGiCommitCardsSpirv", "kGiCommitCardsMsl"),
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
            common = [args.slangc, source, "-I", str(source_dir), "-entry", entry, "-stage",
                      "compute"]
            subprocess.run(common + ["-target", "spirv", "-profile", "spirv_1_5", "-o",
                                     str(out / f"{entry}.spv")], cwd=ROOT, check=True)
            subprocess.run(common + ["-target", "metal", "-o", str(out / f"{entry}.metal")],
                           cwd=ROOT, check=True)
            spirv.append(f"{spirv_name}={out / f'{entry}.spv'}")
            msl.append(f"{msl_name}={out / f'{entry}.metal'}")
        spirv_header = HERE.parent / "src" / "gi_gpu_spirv.h"
        msl_header = HERE.parent / "src" / "gi_gpu_msl.h"
        subprocess.run([sys.executable, str(HERE / "embed_spirv.py"), str(spirv_header)] + spirv,
                       check=True)
        subprocess.run([sys.executable, str(HERE / "embed_msl.py"), str(msl_header)] + msl,
                       check=True)
        subprocess.run([args.clang_format, "-i", str(spirv_header), str(msl_header)], cwd=ROOT,
                       check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
