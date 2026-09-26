// Ground-truth ambient occlusion: the horizon search. `rendering-post-processing` — "Ambient
// occlusion".
//
// ================================================================================================
// WHAT THIS COMPUTES
// ================================================================================================
//
// Jimenez et al., "Practical Realtime Strategies for Accurate Indirect Occlusion" (2016): for each
// of `slices` directions in the image plane — whole-pixel lattice directions, see gtao_common.slang
// — march the depth buffer both ways out to `radius`
// metres, keep the highest horizon on each side, and integrate the COSINE-WEIGHTED visible arc
// between the two horizons in closed form against the normal projected into that slice.
//
// One departure, and it is what makes an unoccluded surface exactly unoccluded: the sum over slices
// is divided by the same slices' integral with both horizons at the tangent plane, rather than by
// the slice count. The two agree in expectation; this one is exactly 1 on an open plane with any
// slice count, where the slice-count form is 1 only on average over the slice directions and the
// filter has to average the error away. The horizon test is also biased down by `control.w` (a cosine),
// so the plane's own samples — which sit exactly on the tangent in exact arithmetic and a few ulps
// either side of it in a 32-bit depth buffer — never read as an occluder.
//
// The BENT NORMAL is the visibility-weighted mean of each slice's mid-horizon direction: the
// direction the unoccluded arc is centred on. It is written camera-relative, beside the visibility.
//
// ================================================================================================
// OUTPUT
// ================================================================================================
//
//   rgb  the bent normal, camera-relative, unnormalised after filtering (normalise to read it)
//   a    visibility in [0, 1]: 1 is unoccluded
//
// A sky pixel (cleared depth, 0) writes (0, 0, 0, 1).
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   python3 src/rendering/occlusion/shaders/regenerate.py --slangc <build>/Development/bin/slangc
//
// which runs, from the repository root, for this file and for gtao_filter.slang (-entry
// cyGtaoFilter):
//
//   slangc src/rendering/occlusion/shaders/gtao.slang -I src/rendering/shaders \
//          -entry cyGtao -stage compute -target spirv -profile spirv_1_5 -o gtao.spv
//   slangc ... -target metal -o gtao.metal
//
// then embed_spirv.py and embed_msl.py into ../src/occlusion_spirv.h and ../src/occlusion_msl.h,
// and clang-format over both.

#include "gtao_common.slang"

/// `cy::rendering::occlusion::GtaoConstants`, word for word. 128 bytes: exactly the push-constant
/// size every backend guarantees.
struct CyGtaoConstants
{
    /// Camera-relative to view space, the rotation's three rows. The prepass normals are
    /// camera-relative; the reconstruction is in view space.
    float4 viewRow0;
    float4 viewRow1;
    float4 viewRow2;
    /// x: P00, y: P11, z: m22, w: m32.
    float4 projection;
    /// xy: the extent in pixels, zw: its reciprocal.
    float4 extent;
    /// x: radius in metres, y: falloff multiplier, z: falloff offset, w: the final power.
    float4 radius;
    /// x: slices, y: steps per side, z: the largest screen radius in pixels, w: horizon bias.
    float4 control;
    /// xy: what to add to a pixel centre of the JITTERED depth to reach the unjittered screen
    /// position the projection above reconstructs — the frame's temporal jitter, undone. zw: unused.
    float4 jitter;
};

struct CyGtaoSet
{
    /// A depth texture on Metal (`depth2d`), which is what the prepass target IS there; SPIR-V
    /// ignores the distinction.
    DepthTexture2D depth;
    Texture2D<float4> normals;
    [format("rgba16f")] RWTexture2D<float4> output;
};

[[vk::binding(0, 0)]] ParameterBlock<CyGtaoSet> cyGtaoSet;
[[vk::push_constant]] CyGtaoConstants cyGtaoConstants;

float3 toView(float3 relative)
{
    return float3(dot(cyGtaoConstants.viewRow0.xyz, relative),
                  dot(cyGtaoConstants.viewRow1.xyz, relative),
                  dot(cyGtaoConstants.viewRow2.xyz, relative));
}

/// A pixel centre of the depth buffer and its view depth to a view-space position.
float3 viewPositionAt(float2 pixelCentre, float viewDepth)
{
    return cyGtaoViewPosition(cyGtaoConstants.projection, cyGtaoConstants.extent,
                              pixelCentre + cyGtaoConstants.jitter.xy, viewDepth);
}

/// The transpose: view space back to camera-relative.
float3 toRelative(float3 view)
{
    return cyGtaoConstants.viewRow0.xyz * view.x + cyGtaoConstants.viewRow1.xyz * view.y +
           cyGtaoConstants.viewRow2.xyz * view.z;
}

/// The closed-form cosine-weighted integral of one side of a slice, from the tangent-facing
/// horizon `h` to the view vector, against the projected normal at angle `n`.
float arcIntegral(float h, float n, float cosN, float sinN)
{
    return (cosN + 2.0 * h * sinN - cos(2.0 * h - n)) * 0.25;
}

/// The highest horizon cosine on one side of one slice. `lattice` is the slice's direction in
/// whole pixels; the taps are whole multiples of it.
float searchHorizon(int2 pixel, float3 position, float3 viewVector, int2 lattice,
                    float radiusPixels, float stepNoise, float lowHorizon)
{
    let steps = uint(cyGtaoConstants.control.y);
    let size = int2(cyGtaoConstants.extent.xy);
    // A float length: an integer `dot` lowers to SPIR-V's DotProductKHR, a capability Vulkan 1.3
    // devices need not enable.
    let latticeLength = length(float2(lattice));
    float horizon = lowHorizon;
    for (uint index = 0; index < steps; ++index)
    {
        let t = (float(index) + stepNoise) / float(steps);
        let multiple = max(int(floor(t * t * radiusPixels / latticeLength + 0.5)), 1);
        let tap = pixel + lattice * multiple;
        if (tap.x < 0 || tap.y < 0 || tap.x >= size.x || tap.y >= size.y)
        {
            continue;
        }
        let tapDepth = cyGtaoSet.depth.Load(int3(tap, 0));
        if (tapDepth <= 0.0)
        {
            continue;
        }
        let tapPosition =
            viewPositionAt(float2(tap) + 0.5, cyGtaoViewDepth(cyGtaoConstants.projection, tapDepth));
        let delta = tapPosition - position;
        let separation = sqrt(dot(delta, delta));
        if (separation <= 0.0)
        {
            continue;
        }
        let cosine = dot(delta, viewVector) / separation - cyGtaoConstants.control.w;
        let weight = saturate(separation * cyGtaoConstants.radius.y + cyGtaoConstants.radius.z);
        horizon = max(horizon, lerp(lowHorizon, cosine, weight));
    }
    return horizon;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void cyGtao(uint3 thread : SV_DispatchThreadID)
{
    let size = uint2(cyGtaoConstants.extent.xy);
    if (thread.x >= size.x || thread.y >= size.y)
    {
        return;
    }
    let pixel = int2(thread.xy);
    let depth = cyGtaoSet.depth.Load(int3(pixel, 0));
    if (depth <= 0.0)
    {
        cyGtaoSet.output[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    let pixelCentre = float2(pixel) + 0.5;
    let viewDepth = cyGtaoViewDepth(cyGtaoConstants.projection, depth);
    let position = viewPositionAt(pixelCentre, viewDepth);
    let viewVector = normalize(-position);
    let relativeNormal = cyGtaoNormal(cyGtaoSet.normals.Load(int3(pixel, 0)));
    let normal = normalize(toView(relativeNormal));

    let radiusPixels = min(cyGtaoConstants.radius.x * cyGtaoConstants.projection.y * 0.5 *
                               cyGtaoConstants.extent.y / viewDepth,
                           cyGtaoConstants.control.z);
    if (radiusPixels < 1.0)
    {
        // Smaller than a pixel on screen: nothing this buffer holds can occlude it.
        cyGtaoSet.output[pixel] = float4(relativeNormal, 1.0);
        return;
    }

    // The slices are `slices` of the eight lattice directions, evenly spaced and rotated per pixel
    // by the dither; the step jitter is the dither transposed, so the two are not correlated.
    let slices = uint(cyGtaoConstants.control.x);
    let rotation = cyGtaoDither(thread.xy) % (8u / slices);
    let stepNoise = (float(cyGtaoDither(thread.yx)) + 0.5) / 16.0;

    float visible = 0.0;
    float reference = 0.0;
    float3 bent = float3(0.0);
    for (uint slice = 0; slice < slices; ++slice)
    {
        let lattice = kCyGtaoDirections[rotation + slice * (8u / slices)];
        // Pixel space is y-down and view space y-up, so the image-plane direction flips its y.
        let direction = normalize(float2(lattice));
        let planeDirection = float3(direction.x, -direction.y, 0.0);
        let orthogonal = planeDirection - viewVector * dot(planeDirection, viewVector);
        let axis = normalize(cross(planeDirection, viewVector));
        let projected = normal - axis * dot(normal, axis);
        let projectedLength = sqrt(dot(projected, projected));
        if (projectedLength <= 1.0e-4)
        {
            continue;
        }
        let cosN = saturate(dot(projected, viewVector) / projectedLength);
        let n = (dot(orthogonal, projected) < 0.0 ? -1.0 : 1.0) * acos(cosN);
        let sinN = sin(n);

        let lowPositive = cos(n + kCyGtaoHalfPi);
        let lowNegative = cos(n - kCyGtaoHalfPi);
        let cosPositive = searchHorizon(pixel, position, viewVector, lattice, radiusPixels,
                                        stepNoise, lowPositive);
        let cosNegative = searchHorizon(pixel, position, viewVector, -lattice, radiusPixels,
                                        stepNoise, lowNegative);
        let hPositive = n + clamp(acos(clamp(cosPositive, -1.0, 1.0)) - n, -kCyGtaoHalfPi,
                                  kCyGtaoHalfPi);
        let hNegative = n + clamp(-acos(clamp(cosNegative, -1.0, 1.0)) - n, -kCyGtaoHalfPi,
                                  kCyGtaoHalfPi);

        let arc = arcIntegral(hNegative, n, cosN, sinN) + arcIntegral(hPositive, n, cosN, sinN);
        visible += projectedLength * arc;
        reference += projectedLength * (cosN + n * sinN);

        let middle = (hNegative + hPositive) * 0.5;
        let across = normalize(orthogonal);
        bent += (viewVector * cos(middle) + across * sin(middle)) * (projectedLength * arc);
    }

    float visibility = reference > 0.0 ? saturate(visible / reference) : 1.0;
    visibility = pow(visibility, cyGtaoConstants.radius.w);
    let bentLength = sqrt(dot(bent, bent));
    let bentRelative = bentLength > 1.0e-6 ? toRelative(bent / bentLength) : relativeNormal;
    cyGtaoSet.output[pixel] = float4(bentRelative, visibility);
}
