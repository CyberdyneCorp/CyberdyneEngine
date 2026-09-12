// The shading this artefact draws with: one vertex stage over a POSITION STREAM ALONE, and a
// fragment stage that recovers the surface normal from the screen-space derivatives of the world
// position. M8.d, samples/10-animated-character.
//
// ================================================================================================
// WHY THE NORMAL IS DERIVED IN THE FRAGMENT STAGE AND NOT READ FROM A STREAM
// ================================================================================================
//
// `cy::rendering::skinning::SkinPass` writes TWO streams: positions at stride 12 and normal-tangent
// frames at stride 8, the latter in the engine's cooked encoding — `render::PackedNormalTangent`,
// two 16-bit SNORM octahedral pairs. `rhi::Format` has no `Rgba16Snorm`; that is the gap
// `src/rendering/pipeline/frame_pipelines.h` records at length, and it is why the forward
// pipeline's own normal stream is `Rgba16Sfloat` carrying the same pair as half floats. Binding the
// dispatch's frame stream through a vertex input therefore needs a format this engine's RHI does
// not have yet.
//
// So this artefact does not bind it. It reads the one stream whose format is unambiguous — the
// positions the dispatch skinned — and computes a GEOMETRIC normal per pixel:
//
//     n = normalize(cross(ddx(world), ddy(world)))
//
// which is exact for the triangle being rasterised, costs no stream and cannot disagree with the
// vertices that were actually drawn. What it costs is smooth shading: this is FLAT shading, every
// triangle its own facet, and on a 14,600-vertex character that reads as faceting on curved
// surfaces. That is a stated property of this artefact's picture, not a defect in the skinning —
// the positions it shades are the compute pass's own output, unmodified.
//
// ================================================================================================
// WHY THE MATRIX IS FOUR EXPLICIT ROWS
// ================================================================================================
//
// tests/render/shaders/conventions.slang's argument, and this file only obeys it: a `float4x4` in a
// push-constant block has a matrix layout that is a property of the compiler's flags rather than of
// either side, and `mul(M, v)` against `mul(v, M)` silently transposes the result. Four rows and
// four dot products have no convention in between for the two sides to disagree about.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   slangc character.slang -target spirv -profile spirv_1_5 \
//          -entry characterVertex -stage vertex   -o character_vertex.spv
//   slangc character.slang -target spirv -profile spirv_1_5 \
//          -entry characterFragment -stage fragment -o character_fragment.spv
//   python3 embed_spirv.py character_spirv.h \
//          kCharacterVertexSpirv=character_vertex.spv \
//          kCharacterFragmentSpirv=character_fragment.spv
//   clang-format -i character_spirv.h

struct CharacterPush
{
    // Rows of the world-to-clip matrix. See the header comment.
    float4 row0;
    float4 row1;
    float4 row2;
    float4 row3;
    // rgb is the surface's base colour; a is unused and kept so the block stays 16-byte aligned.
    float4 color;
    // xyz is the direction light TRAVELS; w is the ambient term.
    float4 light;
    // xyz is the eye in world space, which is what turns a two-sided facet the right way round.
    float4 eye;
};

[[vk::push_constant]]
ConstantBuffer<CharacterPush> push;

struct VertexInput
{
    // Stride 12, `Rgb32Sfloat`: `render::VertexStream::Position` unquantised, which is exactly what
    // the skinning dispatch writes. Nothing repacks between the compute pass and this fetch.
    [[vk::location(0)]] float3 world : POSITION;
};

struct VertexOutput
{
    float4 clip : SV_Position;
    // Carried rather than reconstructed, because the fragment stage differentiates it.
    float3 world : TEXCOORD0;
};

[shader("vertex")]
VertexOutput characterVertex(VertexInput input)
{
    let point = float4(input.world, 1.0);
    VertexOutput output;
    output.clip = float4(dot(push.row0, point), dot(push.row1, point), dot(push.row2, point),
                         dot(push.row3, point));
    output.world = input.world;
    return output;
}

[shader("fragment")]
float4 characterFragment(VertexOutput input) : SV_Target
{
    // The facet's own normal. Its SIGN depends on the winding the rasteriser saw, so it is turned
    // towards the eye rather than trusted — the character's silhouette has back faces on it and a
    // mesh whose winding this artefact does not control would otherwise shade half of itself black.
    float3 normal = normalize(cross(ddx(input.world), ddy(input.world)));
    float3 toEye = normalize(push.eye.xyz - input.world);
    if (dot(normal, toEye) < 0.0)
    {
        normal = -normal;
    }

    let lambert = saturate(dot(normal, -push.light.xyz));
    // A second, dimmer light from below and behind, so the side the key light misses is readable
    // rather than black. A picture whose claim is "this geometry is deforming" must not hide half
    // the geometry.
    let fill = saturate(dot(normal, normalize(float3(0.4, 0.3, 0.9)))) * 0.25;
    let rim = pow(1.0 - saturate(dot(normal, toEye)), 3.0) * 0.35;

    float3 lit = push.color.rgb * (push.light.w + lambert) + fill + rim;
    return float4(lit, 1.0);
}
