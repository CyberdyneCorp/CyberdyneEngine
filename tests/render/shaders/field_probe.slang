// The field probe: the smallest shader that can answer "does the device sampler agree with the
// processor's" — and it answers it through the binding path `environment-fields` actually requires.
//
// COMPILED OFFLINE AND CHECKED IN, exactly as `conventions.slang` beside it is, and for the same
// reason: `CY_SHADER_SLANG` is off in Profile and Shipping, and a suite that needed a shader
// compiler would not run on the machines that most need to run it. `embed_spirv.py` beside this
// file is how the header is regenerated; the invocation is at the bottom of this comment.
//
// ================================================================================================
// TWO ENTRY POINTS, BECAUSE THE REQUIREMENT HAS TWO HALVES
// ================================================================================================
//
// `environment-fields` — "CPU and GPU access" — asks for two separable things: that a shader
// produce "the same value for the same position and resolution" as CPU code, and that GPU access be
// "through BINDLESS RESOURCES reachable from the GPU scene, so a shader can sample a field without
// per-draw binding".
//
//   sampleFieldBound     one buffer bound directly. This is what the AGREEMENT is measured on, and
//                        it is the half that would still be worth having on a device with no
//                        descriptor indexing.
//   sampleFieldBindless  the same sample through `cyFieldSampleScene`, which indexes the global
//                        table `cy/field.slang` declares at set 0, binding 3. The suite dispatches
//                        BOTH over the same positions and requires them to agree BIT FOR BIT: the
//                        binding path may not change the answer, and a test that only ran one of
//                        them would leave "without per-draw binding" as a claim about a declaration.
//
// ================================================================================================
// WHY THE ANSWER IS WRITTEN OUT AS FOUR RAW FLOATS AND A FLAG
// ================================================================================================
//
// The comparison is BIT-EXACT. M11.a's spike measured why that matters: mutating the `UNorm16`
// decode's divisor from 65535 to 65536 changed every value in the report and left EVERY FIELD
// INSIDE ITS DECLARED PRECISION, so a tolerance bound could not fail on it. A probe that reduced
// its four components to a colour, a magnitude or a mean would fold exactly that class of defect
// away before the processor ever saw it.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   slangc field_probe.slang -I ../../../src/rendering/shaders -target spirv -profile spirv_1_5 \
//          -entry sampleFieldBound    -stage compute -fvk-use-entrypoint-name -o field_probe_bound.spv
//   slangc field_probe.slang -I ../../../src/rendering/shaders -target spirv -profile spirv_1_5 \
//          -entry sampleFieldBindless -stage compute -fvk-use-entrypoint-name -o field_probe_bindless.spv
//   python3 embed_spirv.py field_probe_spirv.h \
//          kFieldProbeBoundSpirv=field_probe_bound.spv \
//          kFieldProbeBindlessSpirv=field_probe_bindless.spv
//   clang-format -i field_probe_spirv.h

import cy.field;

struct FieldProbePush
{
    /// Positions to sample. A thread past this does nothing, so the dispatch may be rounded up to
    /// a whole workgroup without writing past the end of the answer buffer.
    uint count;
    /// Which slot of the bindless table `sampleFieldBindless` reads. Ignored by the bound entry.
    uint slot;
    uint pad0;
    uint pad1;
};

[[vk::push_constant]]
ConstantBuffer<FieldProbePush> push;

/// SET 1, the per-pass set of the descriptor-set convention: set 0 is per-frame and is where
/// `cy/field.slang`'s bindless table lives, so a probe that put its own buffers there would be
/// claiming a slot the convention reserves.
[[vk::binding(0, 1)]] StructuredBuffer<uint> probeImage;
[[vk::binding(1, 1)]] StructuredBuffer<float> probePositions;
[[vk::binding(2, 1)]] RWStructuredBuffer<float> probeAnswers;

/// Three floats in, four floats and a flag out. The flag is `CyFieldSample::resolved`, and it is
/// written because "the tile said the default" and "there was no tile" are the two answers a run
/// that sampled outside the written tiles cannot tell apart — which is the failure mode the sky's
/// own round trip was hiding behind.
void writeAnswer(uint index, CyFieldSample answered)
{
    probeAnswers[(index * 5u) + 0u] = answered.value.x;
    probeAnswers[(index * 5u) + 1u] = answered.value.y;
    probeAnswers[(index * 5u) + 2u] = answered.value.z;
    probeAnswers[(index * 5u) + 3u] = answered.value.w;
    probeAnswers[(index * 5u) + 4u] = answered.resolved ? 1.0 : 0.0;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void sampleFieldBound(uint3 thread : SV_DispatchThreadID)
{
    const uint index = thread.x;
    if (index >= push.count)
    {
        return;
    }
    CyFieldImage image;
    image.words = probeImage;
    writeAnswer(index, cyFieldSample(image, probePositions[(index * 3u) + 0u],
                                     probePositions[(index * 3u) + 1u],
                                     probePositions[(index * 3u) + 2u]));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void sampleFieldBindless(uint3 thread : SV_DispatchThreadID)
{
    const uint index = thread.x;
    if (index >= push.count)
    {
        return;
    }
    writeAnswer(index, cyFieldSampleScene(push.slot, probePositions[(index * 3u) + 0u],
                                          probePositions[(index * 3u) + 1u],
                                          probePositions[(index * 3u) + 2u]));
}
