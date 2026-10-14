// SPDX-License-Identifier: MIT
// CyberGI on the device, stage 4: the denoised signals back on the frame's pixels, as the two
// textures the forward pass samples. Issue #35.
//
//   cyGiPublish   `screen_upsample` for every pixel of the frame — the four grid samples about it
//                 weighted bilinearly, by depth and by normal, the one whose depth is nearest its
//                 own where none agrees, and at a divisor of one the pixel's own sample exactly —
//                 into the indirect diffuse and indirect specular targets `cy/frame.slang` reads
//                 through two slots of its texture table.

static const uint kGpuCounts = 0u;  // grid width, grid height, grid pixels, current slot
static const uint kGpuFull = 36u;   // frame width, frame height, grid divisor, flags (1: keep a copy)

/// `kUpsampleDepthTolerance` and `kUpsampleNormalSquarings`.
static const float kGpuDepthTolerance = 0.1f;
static const uint kGpuNormalSquarings = 3u;

struct GiPublishSet {
    StructuredBuffer<uint4> constants;
    /// The cascade's last buffer: the diffuse signal's grid samples, then the specular signal's.
    StructuredBuffer<float4> filtered;
    /// The resolve's surface slots: [world position, view depth], [normal, roughness] a grid cell.
    StructuredBuffer<float4> surfaces;
    /// [normal, view depth] a frame pixel.
    StructuredBuffer<float4> guidance;
    /// The upsampled signals a frame pixel, diffuse then specular, when the flag asks for a copy.
    RWStructuredBuffer<float4> published;
    [format("rgba16f")] RWTexture2D<float4> diffuse;
    [format("rgba16f")] RWTexture2D<float4> specular;
};

[[vk::binding(0, 0)]] ParameterBlock<GiPublishSet> giPublish;

/// `upsample_pixel` in screen_resolve.cpp, for one signal at `base` in the filtered buffer.
float3 gpuUpsample(uint2 pixel, float4 here, uint base) {
    const uint4 counts = giPublish.constants[kGpuCounts];
    const uint divisor = max(giPublish.constants[kGpuFull].z, 1u);
    const float gx = float(pixel.x) / float(divisor);
    const float gy = float(pixel.y) / float(divisor);
    const uint bx = min(uint(gx), counts.x - 1u);
    const uint by = min(uint(gy), counts.y - 1u);
    const float fx = clamp(gx - float(bx), 0.0f, 1.0f);
    const float fy = clamp(gy - float(by), 0.0f, 1.0f);
    float3 total = float3(0.0f, 0.0f, 0.0f);
    float weightTotal = 0.0f;
    float3 nearest = float3(0.0f, 0.0f, 0.0f);
    float nearestGap = 1.0e30f;
    for (uint j = 0u; j < 2u; ++j) {
        for (uint i = 0u; i < 2u; ++i) {
            const uint tx = min(bx + i, counts.x - 1u);
            const uint ty = min(by + j, counts.y - 1u);
            const uint tap = (ty * counts.x) + tx;
            const uint record = ((counts.w * counts.z) + tap) * 2u;
            const float tapDepth = giPublish.surfaces[record].w;
            if (tapDepth <= 0.0f) {
                continue;
            }
            const float3 value = giPublish.filtered[base + tap].xyz;
            const float gap = abs(tapDepth - here.w);
            if (gap < nearestGap) {
                nearestGap = gap;
                nearest = value;
            }
            const float bilinear = (i == 0u ? 1.0f - fx : fx) * (j == 0u ? 1.0f - fy : fy);
            float facing = max(0.0f, dot(giPublish.surfaces[record + 1u].xyz, here.xyz));
            for (uint squaring = 0u; squaring < kGpuNormalSquarings; ++squaring) {
                facing *= facing;
            }
            const float weight =
                bilinear * exp(-gap / ((kGpuDepthTolerance * here.w) + 1.0e-6f)) * facing;
            if (weight <= 0.0f) {
                continue;
            }
            total = total + (value * weight);
            weightTotal += weight;
        }
    }
    return weightTotal > 1.0e-6f ? total / weightTotal : nearest;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cyGiPublish(uint3 thread : SV_DispatchThreadID) {
    const uint4 full = giPublish.constants[kGpuFull];
    if (thread.x >= full.x || thread.y >= full.y) {
        return;
    }
    const uint4 counts = giPublish.constants[kGpuCounts];
    const uint pixel = (thread.y * full.x) + thread.x;
    const float4 here = giPublish.guidance[pixel];
    float3 diffuse = float3(0.0f, 0.0f, 0.0f);
    float3 specular = float3(0.0f, 0.0f, 0.0f);
    if (here.w > 0.0f) {
        if (max(full.z, 1u) == 1u) {
            // Every pixel is its own sample: the grid's values, exactly.
            diffuse = giPublish.filtered[pixel].xyz;
            specular = giPublish.filtered[counts.z + pixel].xyz;
        } else {
            diffuse = gpuUpsample(thread.xy, here, 0u);
            specular = gpuUpsample(thread.xy, here, counts.z);
        }
    }
    giPublish.diffuse[thread.xy] = float4(diffuse, 1.0f);
    giPublish.specular[thread.xy] = float4(specular, 1.0f);
    if ((full.w & 1u) != 0u) {
        giPublish.published[pixel] = float4(diffuse, 0.0f);
        giPublish.published[(full.x * full.y) + pixel] = float4(specular, 0.0f);
    }
}
