// SPDX-License-Identifier: MIT
// What the temporal upscaler's dispatches share: the constants, the tone curve the reconstruction
// weighs colour in, the camera reprojection and the depth conversion. Included by
// upscale_reconstruct.slang and upscale_resample.slang.
//
// Every expression here has a C++ twin in src/rendering/upscale/src/upscale.cpp, written in the
// same order, because the suites compare the device's image against the host's.

/// `cy::rendering::upscale::UpscaleConstants`, word for word. 128 bytes: exactly the push-constant
/// size every backend guarantees.
struct CyUpscaleConstants
{
    /// This frame's clip space (unjittered NDC and depth) to last frame's, as four rows.
    float4 currentToPrevious0;
    float4 currentToPrevious1;
    float4 currentToPrevious2;
    float4 currentToPrevious3;
    /// x: the render extent, y: the output extent, each with the width in the low sixteen bits;
    /// z: unused, zero; w: the `kUpscale*` flags.
    uint4 extents;
    /// xy: the sample offset — render texel i's sample is at i + 0.5 + xy, unjittered.
    /// z: m22, w: m32 of the projection.
    float4 sample;
    /// x: the still history cap, y: the moving cap, z: the motion in output pixels at which the cap
    /// is the moving one, w: the depth tolerance.
    float4 history;
    /// x: the clip gamma when still, y: when moving, z: exposure, w: reactive strength.
    float4 control;
};

[[vk::push_constant]] CyUpscaleConstants cyUpscale;

uint2 cyUpscaleUnpack(uint packed)
{
    return uint2(packed & 0xFFFFu, packed >> 16);
}

uint2 cyUpscaleRenderExtent()
{
    return cyUpscaleUnpack(cyUpscale.extents.x);
}

uint2 cyUpscaleOutputExtent()
{
    return cyUpscaleUnpack(cyUpscale.extents.y);
}

uint cyUpscaleFlags()
{
    return cyUpscale.extents.w;
}

static const uint kCyUpscaleHistoryValid = 1;
static const uint kCyUpscaleVelocity = 2;
static const uint kCyUpscaleReactive = 4;
static const uint kCyUpscaleRejectDisocclusion = 8;
static const uint kCyUpscaleShadingChange = 16;
static const uint kCyUpscaleLocks = 32;

/// `kKernelFalloff` in upscale.cpp: exp(-2.29 d^2), one at the sample and a half at 0.55 pixels.
static const float kCyUpscaleKernelFalloff = 2.29;

/// `kSkyViewDepth` in upscale.cpp: the sky, far beyond any surface and finite.
static const float kCyUpscaleSkyDepth = 1.0e30;

float cyUpscaleLargest(float3 c)
{
    return max(c.r, max(c.g, c.b));
}

/// The reconstruction's tone curve, `x / (1 + max(x))` on the exposed colour. Invertible below.
float3 cyUpscaleTonemap(float3 c, float exposure)
{
    let exposed = c * exposure;
    return exposed * (1.0 / (1.0 + max(cyUpscaleLargest(exposed), 0.0)));
}

float3 cyUpscaleUntonemap(float3 t, float exposure)
{
    let headroom = max(1.0 - cyUpscaleLargest(t), 1.0e-4);
    return t * (1.0 / (exposure * headroom));
}

float3 cyUpscaleToYCoCg(float3 c)
{
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b, 0.5 * c.r - 0.5 * c.b,
                  -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}

float3 cyUpscaleFromYCoCg(float3 c)
{
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}

/// Reversed-Z depth to positive view depth in metres. Zero, the cleared far plane, is the sky.
float cyUpscaleViewDepth(float depth)
{
    let denominator = depth + cyUpscale.sample.z;
    return denominator > 0.0 ? min(cyUpscale.sample.w / denominator, kCyUpscaleSkyDepth)
                             : kCyUpscaleSkyDepth;
}

/// Where last frame's clip space put the unjittered point at `uv` and reversed-Z `depth`, divided
/// through.
float3 cyUpscalePreviousClip(float2 uv, float depth)
{
    let clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    let previous = float4(dot(cyUpscale.currentToPrevious0, clip),
                          dot(cyUpscale.currentToPrevious1, clip),
                          dot(cyUpscale.currentToPrevious2, clip),
                          dot(cyUpscale.currentToPrevious3, clip));
    let w = abs(previous.w) > 1.0e-8 ? previous.w : 1.0e-8;
    return previous.xyz / w;
}

/// The camera's motion of the unjittered point at `uv` and `depth`, in the prepass's convention:
/// normalised screen space, pointing at the past.
float2 cyUpscaleCameraMotion(float2 uv, float depth)
{
    let previous = cyUpscalePreviousClip(uv, depth);
    let history = float2(previous.x * 0.5 + 0.5, 0.5 - previous.y * 0.5);
    return history - uv;
}

/// The render texel whose sample is nearest the unjittered point `p`, in render pixels.
int2 cyUpscaleNearestTexel(float2 p)
{
    return int2(floor(p - cyUpscale.sample.xy));
}

int2 cyUpscaleClampTexel(int2 texel, uint2 extent)
{
    return clamp(texel, int2(0, 0), int2(extent) - int2(1, 1));
}

/// The output pixel's centre in normalised screen space.
float2 cyUpscaleOutputUv(uint2 pixel)
{
    return (float2(pixel) + 0.5) / float2(cyUpscaleOutputExtent());
}
