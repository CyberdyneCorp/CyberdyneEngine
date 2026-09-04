// samples/03-first-light — the whole shading the milestone's artefact needs, in one module.
//
// Two entry points and one resource layout between them: the shadow pass rasterises depth from the
// sun, the forward pass shades against it. Both read the same `FrameConstants` and the same
// per-object push block, so there is exactly one description of "where is this object" and one of
// "where is the sun", and the two passes cannot disagree about either.
//
// ================================================================================================
// EVERY POSITION HERE IS CAMERA-RELATIVE, AND THERE IS NOWHERE TO PUT A WORLD ONE
// ================================================================================================
//
// design.md §3: "Positions reach the GPU relative to the camera", with the first draw rather than
// when precision breaks. So the push block's model matrix is OBJECT-TO-CAMERA-RELATIVE: its
// translation column is the object's origin minus the camera's, subtracted on the CPU in f64 and
// handed down as an f32 that is small because the difference is small. The vertex shader never sees
// a world coordinate, and `viewProjection` is relative-to-clip rather than world-to-clip.
//
// That is what makes `render.golden`'s second case possible: the same scene a million units from
// the origin produces the same image as the scene at the origin, because nothing in this file ever
// held a number big enough to lose the detail.
//
// ================================================================================================
// REVERSED-Z, INCLUDING IN THE SHADOW MAP
// ================================================================================================
//
// `core-math`: depth is [0, 1] with near at 1 and far at 0, cleared to 0, compared GreaterOrEqual.
// The shadow map obeys the same convention — it is a depth buffer like any other — so the occluder
// nearest the light holds the LARGEST value, and a fragment is lit when its own light-space depth
// is at least the stored one. `SampleCmpLevelZero` against a sampler whose compare op is
// GreaterOrEqual is exactly that sentence, which is why the bias is ADDED to the reference: it
// moves the fragment towards the light, which is the direction that removes acne under this
// convention and the direction that would cause it under the other one.
//
// ================================================================================================
// FOUR EXPLICIT ROWS INSTEAD OF float4x4
// ================================================================================================
//
// The same decision, for the same reason, as tests/render/shaders/conventions.slang: a matrix in a
// constant block has a layout that is a property of the compiler's flags rather than of this file,
// and `mul(M, v)` against `mul(v, M)` silently transposes the result. Rows and dot products have no
// convention for the two sides to disagree about, and the C++ writes them out of `cy::Mat4`
// explicitly at the one place that happens.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   slangc first_light.slang -target spirv -profile spirv_1_5 \
//          -entry shadowVertex   -stage vertex   -o first_light_shadow_vertex.spv
//   slangc first_light.slang -target spirv -profile spirv_1_5 \
//          -entry forwardVertex  -stage vertex   -o first_light_forward_vertex.spv
//   slangc first_light.slang -target spirv -profile spirv_1_5 \
//          -entry forwardFragment -stage fragment -o first_light_forward_fragment.spv
//   python3 embed_spirv.py first_light_spirv.h \
//          kFirstLightShadowVertexSpirv=first_light_shadow_vertex.spv \
//          kFirstLightForwardVertexSpirv=first_light_forward_vertex.spv \
//          kFirstLightForwardFragmentSpirv=first_light_forward_fragment.spv
//   clang-format -i first_light_spirv.h

struct FrameConstants
{
    // Camera-relative to clip. See the header comment for why these are rows.
    float4 viewProjectionRow0;
    float4 viewProjectionRow1;
    float4 viewProjectionRow2;
    float4 viewProjectionRow3;
    // Camera-relative to the sun's clip space, for the shadow lookup and for the shadow pass.
    float4 lightViewProjectionRow0;
    float4 lightViewProjectionRow1;
    float4 lightViewProjectionRow2;
    float4 lightViewProjectionRow3;
    // xyz: the unit direction TOWARDS the sun. w: the depth bias, in light-clip units.
    float4 sunDirectionAndBias;
    // rgb: the sun's colour times its intensity. w: the ambient term.
    float4 sunColorAndAmbient;
    // x: 1 when the shadow map is to be sampled, 0 when the sun casts no shadow. y: the normal
    // offset applied before the shadow lookup, in camera-relative units. zw: unused.
    float4 shadowControl;
};

// Set 0 is the global, per-frame set — `shader-system`'s fixed convention, which this sample
// follows even though it has only one set: kSetGlobal is 0 and the first thing a reader of this
// file should not have to wonder about is which set is which.
[[vk::binding(0, 0)]] ConstantBuffer<FrameConstants> frame;
[[vk::binding(1, 0)]] Texture2D<float4> albedoTexture;
[[vk::binding(2, 0)]] SamplerState albedoSampler;
[[vk::binding(3, 0)]] Texture2D<float> shadowMap;
// A COMPARISON sampler, not an ordinary one: the hardware does the depth test and the bilinear
// filter over its RESULT, which is what makes a 2x2 tap a soft edge rather than four hard ones.
[[vk::binding(4, 0)]] SamplerComparisonState shadowSampler;

struct ObjectPush
{
    // Object-to-camera-relative, as three rows of a 3x4 affine matrix. The fourth row is implicit.
    float4 modelRow0;
    float4 modelRow1;
    float4 modelRow2;
    // rgb: the material's base colour, multiplied with the albedo texture. w: unused.
    float4 baseColor;
};

[[vk::push_constant]]
ConstantBuffer<ObjectPush> object;

struct VertexInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float3 normal : NORMAL;
    [[vk::location(2)]] float2 uv : TEXCOORD0;
};

struct ForwardVertexOutput
{
    float4 clip : SV_Position;
    [[vk::location(0)]] float3 positionRelativeToCamera : TEXCOORD1;
    [[vk::location(1)]] float3 normal : TEXCOORD2;
    [[vk::location(2)]] float2 uv : TEXCOORD3;
};

struct DepthOnlyOutput
{
    float4 clip : SV_Position;
};

// The object's position, in the space the camera is the origin of.
float3 toCameraRelative(float3 objectPosition)
{
    let point = float4(objectPosition, 1.0);
    return float3(dot(object.modelRow0, point), dot(object.modelRow1, point),
                  dot(object.modelRow2, point));
}

// The object's normal. The sample's transforms are rotation and uniform scale only, so the
// upper-left 3x3 is its own inverse-transpose up to a scale that normalising removes. A transform
// with non-uniform scale would need the inverse transpose passed in, and this comment is the place
// that would say so.
float3 rotateNormal(float3 objectNormal)
{
    return normalize(float3(dot(object.modelRow0.xyz, objectNormal),
                            dot(object.modelRow1.xyz, objectNormal),
                            dot(object.modelRow2.xyz, objectNormal)));
}

float4 transform(float4 row0, float4 row1, float4 row2, float4 row3, float3 relative)
{
    let point = float4(relative, 1.0);
    return float4(dot(row0, point), dot(row1, point), dot(row2, point), dot(row3, point));
}

[shader("vertex")]
DepthOnlyOutput shadowVertex(VertexInput input)
{
    DepthOnlyOutput output;
    output.clip = transform(frame.lightViewProjectionRow0, frame.lightViewProjectionRow1,
                            frame.lightViewProjectionRow2, frame.lightViewProjectionRow3,
                            toCameraRelative(input.position));
    return output;
}

[shader("vertex")]
ForwardVertexOutput forwardVertex(VertexInput input)
{
    ForwardVertexOutput output;
    output.positionRelativeToCamera = toCameraRelative(input.position);
    output.normal = rotateNormal(input.normal);
    output.uv = input.uv;
    output.clip = transform(frame.viewProjectionRow0, frame.viewProjectionRow1,
                            frame.viewProjectionRow2, frame.viewProjectionRow3,
                            output.positionRelativeToCamera);
    return output;
}

// How much of the sun reaches this point: 1 fully lit, 0 fully shadowed.
float sunVisibility(float3 positionRelativeToCamera, float3 normal)
{
    if (frame.shadowControl.x < 0.5)
    {
        return 1.0;
    }
    // The normal offset. Moving the lookup point along the surface normal is what stops a surface
    // lit at a grazing angle from shadowing itself; a constant depth bias alone cannot, because the
    // error it corrects grows with the slope.
    let offset = positionRelativeToCamera + (normal * frame.shadowControl.y);
    let lightClip = transform(frame.lightViewProjectionRow0, frame.lightViewProjectionRow1,
                              frame.lightViewProjectionRow2, frame.lightViewProjectionRow3, offset);
    if (lightClip.w <= 0.0)
    {
        return 1.0;
    }
    let ndc = lightClip.xyz / lightClip.w;
    // Clip x and y are [-1, 1] and the texture coordinate is [0, 1], with y flipped because the
    // image's first row is the top one — the same flip the colour target's readback assumes.
    let uv = float2((ndc.x * 0.5) + 0.5, 0.5 - (ndc.y * 0.5));
    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0)
    {
        return 1.0;
    }
    // ADDED, not subtracted: see the reversed-Z paragraph in the header comment.
    let reference = ndc.z + frame.sunDirectionAndBias.w;
    return shadowMap.SampleCmpLevelZero(shadowSampler, uv, reference);
}

[shader("fragment")]
float4 forwardFragment(ForwardVertexOutput input) : SV_Target
{
    let normal = normalize(input.normal);
    let albedo = albedoTexture.Sample(albedoSampler, input.uv).rgb * object.baseColor.rgb;
    let lambert = max(dot(normal, frame.sunDirectionAndBias.xyz), 0.0);
    let visibility = sunVisibility(input.positionRelativeToCamera, normal);
    let lit = albedo * ((frame.sunColorAndAmbient.rgb * lambert * visibility) +
                        frame.sunColorAndAmbient.w);
    // The target is Rgba8Unorm rather than a floating-point one, so the transfer function is applied
    // here. A real frame's tonemapping is a post-process pass and `rendering-forward-clustered`'s
    // thirteenth stage; this is a sample, and the sample says which corner it cut.
    let encoded = pow(saturate(lit), 1.0 / 2.2);
    return float4(encoded, 1.0);
}
