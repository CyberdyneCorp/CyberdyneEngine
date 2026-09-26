// The shading samples/10-world draws with: one pipeline, two vertex bindings, one push block.
//
// ================================================================================================
// WHAT THIS SHADER IS AND IS NOT
// ================================================================================================
//
// IT IS NOT THE ENGINE'S MATERIAL SYSTEM. Nothing here is compiled by `cy::material` and nothing
// samples a virtual texture page. The compute module beside it samples environment field images and
// prepares the colour stream; this raster pipeline only consumes that stream.
//
// WHAT IT IS: a Lambert term over a per-vertex colour. Device compute passes produce terrain, sky,
// and water colours; the processor still produces foliage colours. This shader interpolates those
// streams and applies one light, attenuated per fragment by the cloud shadow field.
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
// CLOUD SHADOWS ATTENUATE THE SUN AND NOTHING ELSE
// ================================================================================================
//
// `atmosphere-sky-and-clouds` — "Cloud shadows": the coarse field is "consumed by terrain, foliage,
// water, and illumination". All four are drawn by the one lit path below, so the field is sampled
// once per fragment, at the fragment's own world position, through `cy/cloud_shadow.slang` — the
// same line of arithmetic every consumer of the field shares. The field `sky::CloudShadowField`
// writes is packed by `environment::build_field_image` and bound in set 0 beside where it sits.
//
// The DIRECT sun is what it multiplies: the Lambert term and water's specular lobe. The ambient
// term is the sky's own mean radiance, already computed from the same cloud deck, and the sky dome
// is emissive — neither is touched, because a cloud redistributes skylight rather than removing it
// and multiplying it again would darken an overcast day twice. `placement.z` of zero reads no field
// at all and multiplies the sun by exactly one, which is what keeps a frame with cloud shadows off
// byte-identical to the frame before they existed.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   slangc world.slang -I ../../../src/rendering/shaders -target spirv -profile spirv_1_5 \
//          -entry worldVertex -stage vertex   -o world_vertex.spv
//   slangc world.slang -I ../../../src/rendering/shaders -target spirv -profile spirv_1_5 \
//          -entry worldFragment -stage fragment -o world_fragment.spv
//   python3 embed_spirv.py world_spirv.h \
//          kWorldVertexSpirv=world_vertex.spv \
//          kWorldFragmentSpirv=world_fragment.spv
//   slangc world.slang -I ../../../src/rendering/shaders -target metal \
//          -entry worldVertex -stage vertex -o world_vertex.metal
//   slangc world.slang -I ../../../src/rendering/shaders -target metal \
//          -entry worldFragment -stage fragment -o world_fragment.metal
//   python3 embed_msl.py world_msl.h \
//          kWorldVertexMsl=world_vertex.metal \
//          kWorldFragmentMsl=world_fragment.metal
//   clang-format -i world_spirv.h world_msl.h

import cy.cloud_shadow;
import cy.field;

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

/// The cloud shadow field, and where it sits. Set 0, fragment stage only.
///
/// DECLARED BEFORE THE PUSH BLOCK, and the order is the Metal binding rather than taste: Slang
/// numbers Metal buffer slots in declaration order, and the Metal backend puts set N at buffer N
/// and the push block after the last set. The compute module beside this one declares its set
/// first for the same reason.
struct WorldCloudShadow
{
    /// `cy/field.slang`'s image of the `cloud-shadow` field at its regional level.
    StructuredBuffer<uint> field;
    /// One element. xy: metres from the world-relative origin to the image's origin corner, the f64
    /// subtraction `environment::image_local()` makes done once on the processor. z: 1 when this
    /// frame reads the field, 0 when cloud shadows are off. w: unused.
    StructuredBuffer<float4> placement;
};

[[vk::binding(0, 0)]]
ParameterBlock<WorldCloudShadow> cloudShadow;

[[vk::push_constant]]
ConstantBuffer<WorldPush> push;

struct VertexInput
{
    // Binding 0, stride 24: position then normal, both `Rgb32Sfloat`. Static for terrain, rewritten
    // every frame for water and foliage.
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    // Binding 1, stride 12: the per-vertex colour stream. A separate binding lets compute rewrite
    // terrain colours without resending a hundred and fifty thousand positions.
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

/// The fraction of the sun that reaches this world-relative position through the cloud deck.
float sunThroughClouds(float3 world)
{
    const float4 placement = cloudShadow.placement[0];
    if (placement.z < 0.5)
    {
        return 1.0;
    }
    CyFieldImage image;
    image.words = cloudShadow.field;
    return cyCloudShadowAtImage(image, world.x + placement.x, world.y, world.z + placement.y);
}

[shader("fragment")]
float4 worldFragment(VertexOutput input) : SV_Target
{
    if (push.eye.w > 0.5)
    {
        // The sky dome is already scene-linear output from its device producer. Nothing is lit,
        // because a sky is not a surface.
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

    // The sun reaching this surface: the atmosphere's share is already in `push.sun`, and the
    // cloud deck's is the field's, sampled here. See the header comment for why only this term.
    let direct = push.sun.rgb * sunThroughClouds(input.world);
    let lambert = saturate(dot(normal, -push.light.xyz));
    float3 lit = input.color * (push.ambient.rgb + (direct * lambert));

    if (push.sun.a > 0.0)
    {
        // Water only. A Blinn-Phong lobe against the same sun, which is what puts the glitter path
        // on the sea under a low sun — and what makes the difference between the authoritative
        // surface and the rendered one visible at all, since the visual-only bands are exactly the
        // wavelengths this lobe picks up.
        let half = normalize(-push.light.xyz + toEye);
        let spec = pow(saturate(dot(normal, half)), 120.0) * push.sun.a;
        lit += direct * spec;
    }

    return float4(tonemap(lit), 1.0);
}
