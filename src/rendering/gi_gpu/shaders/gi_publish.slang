// SPDX-License-Identifier: MIT
// CyberGI on the device, stage 4: the denoised signals as the two textures the forward pass
// samples. Issue #35.
//
//   cyGiPublish   the cascade's last buffer into the indirect diffuse and indirect specular
//                 targets, which `cy/frame.slang` reads through two slots of its texture table.
//
// A copy and nothing else: the arithmetic that decides a pixel is the resolve's and the
// denoiser's, which the suite compares as buffers; this is what the frame samples.

static const uint kGpuCounts = 0u;  // width, height, pixels, current slot

struct GiPublishSet {
    StructuredBuffer<uint4> constants;
    /// The cascade's last buffer: the diffuse signal's pixels, then the specular signal's.
    StructuredBuffer<float4> filtered;
    [format("rgba16f")] RWTexture2D<float4> diffuse;
    [format("rgba16f")] RWTexture2D<float4> specular;
};

[[vk::binding(0, 0)]] ParameterBlock<GiPublishSet> giPublish;

[shader("compute")]
[numthreads(8, 8, 1)]
void cyGiPublish(uint3 thread : SV_DispatchThreadID) {
    const uint4 counts = giPublish.constants[kGpuCounts];
    if (thread.x >= counts.x || thread.y >= counts.y) {
        return;
    }
    const uint pixel = (thread.y * counts.x) + thread.x;
    giPublish.diffuse[thread.xy] = float4(giPublish.filtered[pixel].xyz, 1.0f);
    giPublish.specular[thread.xy] = float4(giPublish.filtered[counts.z + pixel].xyz, 1.0f);
}
