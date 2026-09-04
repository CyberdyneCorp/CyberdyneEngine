// The reflection test fixture. Compiled to SPIR-V once and checked in as spirv_probe.h.
//
// It exists to exercise every branch of the SPIR-V reflection parser in one place: vertex inputs at
// three locations, a uniform buffer, a storage buffer, a separate texture and sampler, a
// runtime-sized bindless array, a storage image, a push-constant block, a specialization constant,
// and a compute entry point with a non-trivial workgroup size.
//
// REGENERATE WITH (from the repository root, with a Slang install):
//   slangc -target spirv -profile spirv_1_5 \
//          -entry probeVertex -entry probeFragment -entry probeCompute \
//          src/backends/shader/tests/fixtures/probe.slang -o probe.spv
//   python3 src/backends/shader/tests/fixtures/embed_spirv.py probe.spv ...
//
// It is checked in rather than compiled by the build for one reason: the test suite must run in a
// configuration with CY_SHADER_SLANG off, which is the default and is what continuous integration
// uses. A fixture that needed the compiler would make the reflection tests untestable in exactly
// the build where reflection matters most — the shipping one, which contains no compiler.

// Set 0 is global (per frame) by the descriptor set convention.
[[vk::binding(0, 0)]] ConstantBuffer<FrameConstants> frame;
[[vk::binding(1, 0)]] StructuredBuffer<float4> instanceData;
[[vk::binding(2, 0)]] Texture2D<float4> bindlessTextures[];

// Set 1 is per view.
[[vk::binding(0, 1)]] ConstantBuffer<ViewConstants> view;

// Set 2 is per pass.
[[vk::binding(0, 2)]] Texture2D<float4> passColor;
[[vk::binding(1, 2)]] SamplerState passSampler;
[[vk::binding(2, 2)]] RWTexture2D<float4> passStorage;

struct FrameConstants
{
    float timeSeconds;
    float exposureStops;
    float2 padding;
};

struct ViewConstants
{
    float4x4 relativeToClip;
    float4 cameraParameters;
};

struct PushConstants
{
    float4 tint;
    uint instanceOffset;
    uint materialIndex;
};

[[vk::push_constant]] PushConstants push;

/// A quality axis, as a specialization constant rather than a preprocessor permutation.
[SpecializationConstant]
const int kSampleCount = 4;

struct VertexOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float3 normal : NORMAL;
};

[shader("vertex")]
VertexOut probeVertex(float3 position: POSITION, float3 normal: NORMAL, float2 uv: TEXCOORD0)
{
    VertexOut output;
    let instance = instanceData[push.instanceOffset];
    output.position = mul(view.relativeToClip, float4(position + instance.xyz, 1.0));
    output.uv = uv;
    output.normal = normal;
    return output;
}

[shader("fragment")]
float4 probeFragment(VertexOut input) : SV_Target
{
    float4 total = float4(0.0);
    for (int index = 0; index < kSampleCount; ++index)
    {
        total += passColor.Sample(passSampler, input.uv + float2(index) * 0.001);
    }
    total += bindlessTextures[push.materialIndex].Sample(passSampler, input.uv);
    return total * push.tint * frame.exposureStops;
}

[shader("compute")]
[numthreads(8, 4, 2)]
void probeCompute(uint3 id: SV_DispatchThreadID)
{
    passStorage[id.xy] = instanceData[id.x] * frame.timeSeconds;
}
