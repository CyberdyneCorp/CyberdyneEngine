// SPDX-License-Identifier: MIT
// Compile probe for the descriptor model Metal actually needs. A runtime-sized global texture
// array is invalid MSL when emitted as a direct entry-point parameter; grouping the fixed-capacity
// table in a ParameterBlock asks Slang to emit one Metal argument buffer while retaining one
// descriptor set on SPIR-V.

struct BindlessTable
{
    Texture2D<float4> textures[16];
    SamplerState sampler;
};

ParameterBlock<BindlessTable> table;
RWTexture2D<float4> output;

[shader("compute")]
[numthreads(1, 1, 1)]
void argumentBufferProbe(uint3 id : SV_DispatchThreadID)
{
    output[id.xy] = table.textures[id.z].SampleLevel(table.sampler, float2(0.5), 0.0);
}
