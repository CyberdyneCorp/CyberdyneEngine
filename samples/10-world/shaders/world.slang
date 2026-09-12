// The shading samples/10-world draws with: one pipeline, two vertex bindings, one push block.
//
// ================================================================================================
// WHAT THIS SHADER IS AND IS NOT
// ================================================================================================
//
// IT IS NOT THE ENGINE'S MATERIAL SYSTEM. Nothing here is compiled by `cy::material`, nothing here
// samples a virtual texture page, and nothing here samples an `environment` field through
// `environment::build_field_image()`'s buffer layout. M10 shipped no `.slang` module for a field,
// for a terrain material, for a water surface or for a cloud march — six of this milestone's agents
// each recorded that gap against their own row, and this file closes none of them.
//
// WHAT IT IS: a Lambert term over a per-vertex colour, and the colour is the interesting half. Every
// colour this shader receives was computed on the PROCESSOR by the module that owns it — terrain's
// material layer composed with `environment::FieldStore::sample_deterministic()`'s wetness and snow,
// water's own foam and absorption, foliage's species tint, and for the sky dome the radiance
// `rendering::sky::compose_sky()` answered for that direction. The shader interpolates them and
// applies one light. That division is deliberate and it is what makes the picture honest: the
// modelling is the engine's, the pixels are a rasteriser's.
//
// ================================================================================================
// WHY THE MATRIX IS FOUR EXPLICIT ROWS
// ================================================================================================
//
// tests/render/shaders/conventions.slang's argument, which samples/09b-animated-character also only
// obeys: a `float4x4` in a push-constant block has a matrix layout that is a property of the
// compiler's flags rather than of either side, and `mul(M, v)` against `mul(v, M)` silently
// transposes the result. Four rows and four dot products have no convention in between for the two
// sides to disagree about.
//
// ================================================================================================
// ONE PIPELINE, AND THE SKY IS GEOMETRY
// ================================================================================================
//
// The sky is a dome of real triangles at a fixed radius around the eye, drawn first and depth-tested
// like everything else. That removes the second pipeline state a depth-write-disabled background
// pass would need, and it costs nothing: the dome's radius is inside the far plane and every piece
// of terrain is nearer than it. `push.eye.w` switches the fragment stage between lit and emissive,
// which is one branch over a uniform and therefore free.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   slangc world.slang -target spirv -profile spirv_1_5 \
//          -entry worldVertex -stage vertex   -o world_vertex.spv
//   slangc world.slang -target spirv -profile spirv_1_5 \
//          -entry worldFragment -stage fragment -o world_fragment.spv
//   python3 embed_spirv.py world_spirv.h \
//          kWorldVertexSpirv=world_vertex.spv \
//          kWorldFragmentSpirv=world_fragment.spv
//   clang-format -i world_spirv.h

struct WorldPush
{
    // Rows of the world-to-clip matrix. See the header comment.
    float4 row0;
    float4 row1;
    float4 row2;
    float4 row3;
    // xyz is the direction light TRAVELS; w is unused and keeps the block 16-byte aligned.
    float4 light;
    // xyz is the eye in world-relative space; w selects the fragment path: 0 lit, 1 emissive.
    float4 eye;
    // rgb is the sun's colour after the atmosphere and the clouds, already divided by the frame's
    // exposure; a is the specular strength, which is non-zero only for water.
    float4 sun;
    // rgb is the ambient term, which is the sky's own mean radiance divided by the same exposure;
    // a is unused.
    float4 ambient;
};

[[vk::push_constant]]
ConstantBuffer<WorldPush> push;

struct VertexInput
{
    // Binding 0, stride 24: position then normal, both `Rgb32Sfloat`. Static for terrain, rewritten
    // every frame for water and foliage.
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    // Binding 1, stride 12: the per-vertex colour every producer computed on the processor. A
    // SEPARATE binding because terrain's positions never change and its colours change every frame
    // — one buffer uploaded once and one uploaded per frame, rather than a hundred and fifty
    // thousand positions re-sent so that a snowfall can be seen.
    [[vk::location(2)]] float3 color : COLOR;
};

struct VertexOutput
{
    float4 clip : SV_Position;
    float3 world : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float3 color : TEXCOORD2;
};

[shader("vertex")]
VertexOutput worldVertex(VertexInput input)
{
    let point = float4(input.position, 1.0);
    VertexOutput output;
    output.clip = float4(dot(push.row0, point), dot(push.row1, point), dot(push.row2, point),
                         dot(push.row3, point));
    output.world = input.position;
    output.normal = input.normal;
    output.color = input.color;
    return output;
}

/// Reinhard, then the sRGB transfer function's gamma approximation.
///
/// A tone map is needed at all because the sky's radiance is in nits and spans four orders of
/// magnitude between a moonless midnight and the disc of the noon sun. The curve is the simplest one
/// that is monotonic and has no free parameters — this artefact is not claiming a grade, and
/// `rendering-post`'s real tone mapping is not in its link line.
float3 tonemap(float3 radiance)
{
    let mapped = radiance / (1.0 + radiance);
    return pow(saturate(mapped), 1.0 / 2.2);
}

[shader("fragment")]
float4 worldFragment(VertexOutput input) : SV_Target
{
    if (push.eye.w > 0.5)
    {
        // The sky dome: the colour IS the radiance `compose_sky()` answered, already divided by the
        // frame's exposure on the processor. Nothing is lit, because a sky is not a surface.
        return float4(tonemap(input.color), 1.0);
    }

    float3 normal = normalize(input.normal);
    float3 toEye = normalize(push.eye.xyz - input.world);
    if (dot(normal, toEye) < 0.0)
    {
        // Turned towards the eye rather than trusted. A terrain tile's winding is the mesher's and
        // a water patch's is the ocean surface's, and a picture whose claim is "this is a world"
        // must not answer a back-facing triangle by shading it black.
        normal = -normal;
    }

    let lambert = saturate(dot(normal, -push.light.xyz));
    float3 lit = input.color * (push.ambient.rgb + (push.sun.rgb * lambert));

    if (push.sun.a > 0.0)
    {
        // Water only. A Blinn-Phong lobe against the same sun, which is what puts the glitter path
        // on the sea under a low sun — and what makes the difference between the authoritative
        // surface and the rendered one visible at all, since the visual-only bands are exactly the
        // wavelengths this lobe picks up.
        let half = normalize(-push.light.xyz + toEye);
        let spec = pow(saturate(dot(normal, half)), 120.0) * push.sun.a;
        lit += push.sun.rgb * spec;
    }

    return float4(tonemap(lit), 1.0);
}
