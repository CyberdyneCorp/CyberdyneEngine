// SPDX-License-Identifier: MIT
// CyberGI on the device: a batch of rays sphere traced against the uploaded distance field. Issue
// #35, stage 1.
//
// The software tier's intersection, `DistanceField::sphere_trace`, over the field as the device
// holds it: the page table and the brick pool `GpuGiScene::upload_field` wrote. Stage 3's tracing
// dispatches call the same `giSphereTrace`; this entry point is how stage 1 is held to the host
// trace ray for ray, and it is what a debug view of the field would dispatch.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   python3 src/rendering/gi_gpu/shaders/regenerate.py --slangc <build>/Development/bin/slangc
//
// which compiles this file (-entry cyGiTraceRays) and gi_cards.slang (-entry cyGiShadeCards,
// -entry cyGiCommitCards) for SPIR-V 1.5 and for Metal from the repository root, embeds them into
// ../src/gi_gpu_spirv.h and ../src/gi_gpu_msl.h, and runs clang-format over both.

#include "gi_gpu_common.slang"

[shader("compute")]
[numthreads(64, 1, 1)]
void cyGiTraceRays(uint3 thread : SV_DispatchThreadID) {
    const uint ray = thread.x;
    if (ray >= giDispatch.count) {
        return;
    }
    const float4 origin = giScene.rays[ray * 2u];
    const float4 direction = giScene.rays[(ray * 2u) + 1u];
    const GiTraceHit hit = giSphereTrace(origin.xyz, direction.xyz, origin.w, direction.w);
    giScene.hits[ray * 3u] =
        float4(hit.hit ? 1.0f : 0.0f, hit.t, hit.closest, hit.exhausted ? 1.0f : 0.0f);
    giScene.hits[(ray * 3u) + 1u] = float4(hit.position, 0.0f);
    giScene.hits[(ray * 3u) + 2u] = float4(hit.normal, 0.0f);
}
