#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Recompile the device GI dispatches and rewrite the two committed headers.

Run from anywhere; the paths are resolved from this file. `--slangc` is the compiler, normally the
one a Development build stages at <build>/Development/bin/slangc.

    python3 src/rendering/gi_gpu/shaders/regenerate.py --slangc build/dev/Development/bin/slangc

One module per ENTRY POINT rather than per file: gi_cards.slang holds two, gi_radiance.slang six,
gi_hardware.slang three, gi_denoise.slang four and gi_screen_probes.slang six, and each is its
own pipeline. The #line directives in the MSL name repository-relative paths, because slangc is run from
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
    # Stage 3: the radiance cache and the tracing tiers. gi_hardware.slang's three are the ray
    # query tier; a device without ray queries never creates them.
    ("gi_radiance", "cyGiGatherProbes", "kGiGatherProbesSpirv", "kGiGatherProbesMsl"),
    ("gi_radiance", "cyGiSampleProbes", "kGiSampleProbesSpirv", "kGiSampleProbesMsl"),
    ("gi_radiance", "cyGiBuildScreenHzb", "kGiBuildScreenHzbSpirv", "kGiBuildScreenHzbMsl"),
    ("gi_radiance", "cyGiTraceScreen", "kGiTraceScreenSpirv", "kGiTraceScreenMsl"),
    ("gi_radiance", "cyGiTraceSoftware", "kGiTraceSoftwareSpirv", "kGiTraceSoftwareMsl"),
    ("gi_radiance", "cyGiTraceTiered", "kGiTraceTieredSpirv", "kGiTraceTieredMsl"),
    ("gi_hardware", "cyGiGatherProbesHardware", "kGiGatherProbesHardwareSpirv",
     "kGiGatherProbesHardwareMsl"),
    ("gi_hardware", "cyGiTraceHardware", "kGiTraceHardwareSpirv", "kGiTraceHardwareMsl"),
    ("gi_hardware", "cyGiTraceTieredHardware", "kGiTraceTieredHardwareSpirv",
     "kGiTraceTieredHardwareMsl"),
    # Stage 4: the per-pixel resolve, the denoiser's four stages, and the frame's two ends — the
    # prepass unpacked into surfaces and the denoised signals published as the frame's textures.
    # gi_resolve_hardware.slang's one is the ray query tier, created only where the device has them.
    ("gi_surfaces", "cyGiUnpackSurfaces", "kGiUnpackSurfacesSpirv", "kGiUnpackSurfacesMsl"),
    ("gi_resolve", "cyGiResolve", "kGiResolveSpirv", "kGiResolveMsl"),
    ("gi_resolve_hardware", "cyGiResolveHardware", "kGiResolveHardwareSpirv",
     "kGiResolveHardwareMsl"),
    ("gi_denoise", "cyGiReproject", "kGiReprojectSpirv", "kGiReprojectMsl"),
    ("gi_denoise", "cyGiDenoiseAccumulate", "kGiDenoiseAccumulateSpirv",
     "kGiDenoiseAccumulateMsl"),
    ("gi_denoise", "cyGiDenoiseVariance", "kGiDenoiseVarianceSpirv", "kGiDenoiseVarianceMsl"),
    ("gi_denoise", "cyGiDenoiseFilter", "kGiDenoiseFilterSpirv", "kGiDenoiseFilterMsl"),
    ("gi_publish", "cyGiPublish", "kGiPublishSpirv", "kGiPublishMsl"),
    # The denoiser with no cascade: the accumulation, and on the full grid the publish, in one.
    ("gi_temporal", "cyGiDenoiseTemporal", "kGiDenoiseTemporalSpirv", "kGiDenoiseTemporalMsl"),
    # The screen-probe gather: placement, the probes' rays, the probe-space filter and the
    # per-pixel integration.
    # gi_screen_probes_hardware.slang's two are the ray query tier.
    ("gi_screen_probes", "cyGiProbePlace", "kGiProbePlaceSpirv", "kGiProbePlaceMsl"),
    ("gi_screen_probes", "cyGiProbeRank", "kGiProbeRankSpirv", "kGiProbeRankMsl"),
    ("gi_screen_probes", "cyGiProbeTrace", "kGiProbeTraceSpirv", "kGiProbeTraceMsl"),
    ("gi_screen_probes", "cyGiProbeFilter", "kGiProbeFilterSpirv", "kGiProbeFilterMsl"),
    ("gi_screen_probes", "cyGiProbeIntegrate", "kGiProbeIntegrateSpirv", "kGiProbeIntegrateMsl"),
    ("gi_screen_probes", "cyGiProbeTraceNarrow", "kGiProbeTraceNarrowSpirv",
     "kGiProbeTraceNarrowMsl"),
    ("gi_screen_probes_hardware", "cyGiProbeTraceHardware", "kGiProbeTraceHardwareSpirv",
     "kGiProbeTraceHardwareMsl"),
    ("gi_screen_probes_hardware", "cyGiProbeIntegrateHardware", "kGiProbeIntegrateHardwareSpirv",
     "kGiProbeIntegrateHardwareMsl"),
    ("gi_screen_probes_hardware", "cyGiProbeTraceHardwareNarrow",
     "kGiProbeTraceHardwareNarrowSpirv", "kGiProbeTraceHardwareNarrowMsl"),
    # The debug views on the device: the exposure, the view, the leak suspects' discs, and the
    # view through ray queries.
    ("gi_debug_views", "cyGiDebugExposure", "kGiDebugExposureSpirv", "kGiDebugExposureMsl"),
    ("gi_debug_views", "cyGiDebugView", "kGiDebugViewSpirv", "kGiDebugViewMsl"),
    ("gi_debug_views", "cyGiDebugSuspects", "kGiDebugSuspectsSpirv", "kGiDebugSuspectsMsl"),
    ("gi_debug_views_hardware", "cyGiDebugViewHardware", "kGiDebugViewHardwareSpirv",
     "kGiDebugViewHardwareMsl"),
)
# The engine's shader library: gi_surfaces.slang imports `cy.packing` for the prepass's normal.
SHADERS = ROOT / "src" / "rendering" / "shaders"


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
            common = [args.slangc, source, "-I", str(source_dir), "-I",
                      str(SHADERS.relative_to(ROOT)), "-entry", entry, "-stage", "compute"]
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
