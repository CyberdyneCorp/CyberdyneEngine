// The sky probe: the smallest shader that can photograph what `rendering::sky::compose_sky()`
// answered, through the same arrangement the engine's own sky is drawn with.
//
// COMPILED OFFLINE AND CHECKED IN, exactly as `conventions.slang`, `field_probe.slang` and
// `material_probe.slang` beside it are, and for the same reason: `CY_SHADER_SLANG` is off in
// Profile and Shipping, and a suite that needed a shader compiler would not run on the machines
// that most need to run it. `embed_spirv.py` beside this file regenerates the header; the
// invocation is at the bottom of this comment.
//
// ================================================================================================
// THE RADIANCE ARRIVES AS A VERTEX ATTRIBUTE, BECAUSE THAT IS WHERE THE ENGINE PUTS IT
// ================================================================================================
//
// `samples/10-world` and `samples/12-beauty` both draw the sky as triangles whose every vertex
// carries the radiance `compose_sky()` answered for its direction; the rasteriser interpolates and
// the fragment stage tone maps. There is no atmosphere evaluated in a shader anywhere in this
// engine, so a probe that computed one here would be photographing a second sky nothing ships.
//
// ================================================================================================
// THE TONE MAP IS `samples/10-world/shaders/world.slang`'s, CHARACTER FOR CHARACTER
// ================================================================================================
//
// Reinhard then the sRGB transfer function's gamma approximation: the simplest curve that is
// monotonic and has no free parameters. A reference image compared against a tone map with a knob
// in it would be a reference image of somebody's grade.
//
// `push.scale` is the exposure divide, which `samples/10-world` does on the processor and this
// probe does here so that the exposure is a declared input of the picture rather than a number
// folded into the vertex stream before anything can see it. `test_sky_times_of_day.cpp` also uses
// it to perturb the whole frame by one unit in the last place, which is how the comparison's
// tolerance was measured instead of chosen.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   slangc sky_frame.slang -target spirv -profile spirv_1_5 \
//          -entry skyFrameVertex   -stage vertex   -o sky_frame_vertex.spv
//   slangc sky_frame.slang -target spirv -profile spirv_1_5 \
//          -entry skyFrameFragment -stage fragment -o sky_frame_fragment.spv
//   python3 embed_spirv.py --source sky_frame.slang sky_frame_spirv.h \
//          kSkyFrameVertexSpirv=sky_frame_vertex.spv \
//          kSkyFrameFragmentSpirv=sky_frame_fragment.spv
//   clang-format -i sky_frame_spirv.h

struct SkyFramePush
{
    /// One over the frame's exposure times four — `samples/10-world/stage.cpp`'s own divisor, so
    /// the mean sky lands at a quarter of white with two stops of headroom above it for the sun's
    /// disc and the brightest cloud.
    float scale;
    float pad0;
    float pad1;
    float pad2;
};

[[vk::push_constant]] SkyFramePush push;

struct SkyFrameVertex
{
    /// Clip space directly. The grid IS the projection — see test_sky_times_of_day.cpp for why the
    /// dome is tessellated in the image rather than in latitude and longitude.
    float2 position : POSITION;
    /// Nits, as `compose_sky()` answers them. Not exposed, not clamped, not encoded.
    float3 radiance : COLOR0;
};

struct SkyFrameVaryings
{
    float4 position : SV_Position;
    float3 radiance : COLOR0;
};

[shader("vertex")]
SkyFrameVaryings skyFrameVertex(SkyFrameVertex vertex)
{
    SkyFrameVaryings varyings;
    varyings.radiance = vertex.radiance;
    varyings.position = float4(vertex.position, 0.0, 1.0);
    return varyings;
}

/// Reinhard, then the sRGB transfer function's gamma approximation.
float3 tonemap(float3 radiance)
{
    let mapped = radiance / (1.0 + radiance);
    return pow(saturate(mapped), 1.0 / 2.2);
}

[shader("fragment")]
float4 skyFrameFragment(SkyFrameVaryings varyings) : SV_Target
{
    return float4(tonemap(varyings.radiance * push.scale), 1.0);
}
