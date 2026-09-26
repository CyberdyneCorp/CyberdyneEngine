// SPDX-License-Identifier: MIT
// The view probe: MSAA edges and multi-view layers on a device. M11.d tasks 5.1 and 5.2.
//
// COMPILED OFFLINE AND CHECKED IN, as conventions.slang is and for the same reason: the render
// suites must run in a build with no shader compiler.
//
// `SV_VulkanVertexID`, NOT `SV_VertexID`: Slang lowers the latter to `gl_VertexIndex -
// gl_BaseVertex`, which needs the `shaderDrawParameters` feature — see cy/particle.slang.
//
// ONE TRIANGLE, NO VERTEX BUFFER. Its three corners are constants chosen so that NO EDGE IS AXIS
// ALIGNED: an axis-aligned edge rasterises to whole pixels at every sample count, and the MSAA case
// would then compare two identical pictures and learn nothing.
//
// TWO VERTEX ENTRY POINTS, AND THE SPLIT IS THE WHOLE POINT. `viewVertex` reads `SV_ViewID`, which
// compiles to the SPIR-V `MultiView` capability — a module a device created WITHOUT the multiview
// feature must refuse. `baselineVertex` takes the view from the push block instead and carries no
// such capability, so the per-view baseline runs on a device that has no multi-view at all. Both
// feed the same fragment shader, which colours by view, so the two paths are comparable byte for
// byte.
//
// REGENERATING (slangc is built into any Development tree at <tree>/Development/bin/slangc):
//
//   slangc view_probe.slang -target spirv -profile spirv_1_5 \
//          -entry viewVertex -stage vertex -o view_vertex.spv
//   slangc view_probe.slang -target spirv -profile spirv_1_5 \
//          -entry baselineVertex -stage vertex -o baseline_vertex.spv
//   slangc view_probe.slang -target spirv -profile spirv_1_5 \
//          -entry viewFragment -stage fragment -o view_fragment.spv
//   python3 embed_spirv.py --source view_probe.slang view_probe_spirv.h \
//          kViewProbeMultiviewVertexSpirv=view_vertex.spv \
//          kViewProbeBaselineVertexSpirv=baseline_vertex.spv \
//          kViewProbeFragmentSpirv=view_fragment.spv
//   clang-format -i view_probe_spirv.h

struct ViewPush
{
    // One colour per view. The MSAA case draws view 0 only.
    float4 colors[2];
    // The baseline's view index. Zero for the multi-view path, which reads SV_ViewID instead. No
    // explicit padding after it: a `uint3` would be aligned to 16 and push the block to 60 bytes,
    // past the 48 the C++ side declares. The C++ struct pads itself to 48 instead.
    uint view;
};

[[vk::push_constant]]
ConstantBuffer<ViewPush> push;

struct VertexOutput
{
    float4 clip : SV_Position;
    nointerpolation uint view : VIEW_INDEX;
};

float4 corner(uint vertex)
{
    const float2 corners[3] = { float2(-0.8, -0.6), float2(0.7, -0.9), float2(0.1, 0.85) };
    return float4(corners[vertex % 3], 0.5, 1.0);
}

[shader("vertex")]
VertexOutput viewVertex(uint vertex : SV_VulkanVertexID, uint view : SV_ViewID)
{
    VertexOutput output;
    output.clip = corner(vertex);
    output.view = view;
    return output;
}

[shader("vertex")]
VertexOutput baselineVertex(uint vertex : SV_VulkanVertexID)
{
    VertexOutput output;
    output.clip = corner(vertex);
    output.view = push.view;
    return output;
}

[shader("fragment")]
float4 viewFragment(VertexOutput input) : SV_Target
{
    return push.colors[min(input.view, 1u)];
}
