// Portable integer-only PCG candidate generation and density filtering.
//
// Regenerate the checked-in targets from the repository root:
//   slangc src/pcg/shaders/gpu_conformance.slang -target spirv -profile spirv_1_5 \
//     -entry pcgCandidates -stage compute -o /tmp/pcg_gpu.spv
//   slangc src/pcg/shaders/gpu_conformance.slang -target metal \
//     -entry pcgCandidates -stage compute -o /tmp/pcg_gpu.metal
//   python3 src/pcg/shaders/embed.py src/pcg/shaders/gpu_conformance_embedded.h \
//     /tmp/pcg_gpu.spv /tmp/pcg_gpu.metal

struct CandidateParameters
{
    uint seed;
    uint count;
    uint densityThreshold;
    uint reserved;
};

struct Candidate
{
    uint slot;
    uint x;
    uint z;
    uint accepted;
};

struct CandidateSet
{
    ConstantBuffer<CandidateParameters> parameters;
    RWStructuredBuffer<Candidate> candidates;
};

// One ParameterBlock per RHI descriptor set. Metal binds the set as an argument buffer at buffer
// index zero; Vulkan sees its members as set-zero bindings zero and one.
[[vk::binding(0, 0)]] ParameterBlock<CandidateSet> pcg;

uint candidateMix(uint value)
{
    value ^= value >> 16;
    value *= 0x7FEB352D;
    value ^= value >> 15;
    value *= 0x846CA68B;
    value ^= value >> 16;
    return value;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void pcgCandidates(uint3 invocation : SV_DispatchThreadID)
{
    const uint slot = invocation.x;
    if (slot >= pcg.parameters.count)
        return;

    const uint identity = candidateMix(pcg.parameters.seed ^ (slot * 0x9E3779B9));
    const uint second = candidateMix(identity ^ 0xA511E9B3);
    const uint density = candidateMix(identity + 0x63D83595);

    Candidate candidate;
    candidate.slot = slot;
    candidate.x = identity & 0xFFFF;
    candidate.z = second & 0xFFFF;
    candidate.accepted = (density & 0xFFFF) < pcg.parameters.densityThreshold ? 1 : 0;
    pcg.candidates[slot] = candidate;
}
