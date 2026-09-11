// The frame's own shaders: what `src/rendering/pipeline/` records into `FrameAssembly`'s passes.
// M8.c task 1b.1.
//
// ================================================================================================
// WHY THIS FILE EXISTS
// ================================================================================================
//
// M8.b assembled a frame out of the renderer's own parts and stopped one layer short:
// `FrameAssembly` hands each pass's record callback to its CALLER and every caller in the tree
// supplied none. So the frame was declared, compiled, barriered and executed — and recorded nothing.
// This module is the shading half of the layer that closes that: the entry points a pass binds, and
// nothing else.
//
// It is CONTENT, not code: the compiled modules are staged next to the standard library and the
// pipeline layer consumes SPIR-V. Everything below imports the standard library rather than
// restating it — `cy.brdf`, `cy.light`, `cy.cluster`, `cy.material`, `cy.packing` — which is what
// makes "the engine supplies a shader standard library" a fact about a shipped frame rather than a
// directory of unread modules.
//
// ================================================================================================
// FOUR EXPLICIT ROWS INSTEAD OF float4x4, FOR THE REASON samples/03-first-light GIVES
// ================================================================================================
//
// A matrix in a constant block has a layout that is a property of the compiler's flags rather than
// of this file, and `mul(M, v)` against `mul(v, M)` silently transposes the result. Rows and dot
// products have no convention for the two sides to disagree about. `cy.view`'s `CyViewData` is the
// engine's per-view block and declares `float4x4`; this module deliberately does NOT import it, so
// that the frame's correctness does not depend on which matrix-layout flag the shader cook ran
// with. tests/render/shaders/conventions.slang and samples/03-first-light/shaders/ made the same
// decision for the same reason.
//
// ================================================================================================
// THE DESCRIPTOR SET CONVENTION IS THE ENGINE'S, NOT THIS FILE'S
// ================================================================================================
//
// `cy/backends/shader/reflection.h`: set 0 global, set 1 view, set 2 pass, set 3 draw.
//
//   set 0, binding 0   cyGlobals            imported from cy.globals — time, delta, exposure, wind
//   set 1, binding 0   cyFrame              this frame's view: rows, cluster grid, counts, offsets
//   set 1, binding 1   cyLights             `GpuLight`, which IS cy.light's `Light`, field for field
//   set 1, binding 2   cyClusterHeaders     `ClusterHeader{offset, count}` per (cluster, type)
//   set 1, binding 3   cyClusterIndices     the concatenated per-cluster lists
//   set 1, binding 4   cyDrawInstances      `GpuDrawInstance`, indexed by the draw's push constant
//   set 1, binding 5   cyInstances          the model-to-camera-relative rows, per GPU scene slot
//   set 1, binding 6   cyMaterialWords      the GPU material table, as words
//
// The per-draw index is a PUSH CONSTANT rather than a fourth set: it is one word, it changes per
// draw, and `rhi-and-render-graph`'s portability limit is 128 bytes — spending four of them here
// costs no descriptor traffic at all.
//
// ================================================================================================
// THE MATERIAL TABLE IS READ BY OFFSET, AND THE OFFSETS ARE DERIVED
// ================================================================================================
//
// M7's closing gate: "the material compiler fills a GPU material table nothing draws with". The
// opaque fragment shader below reads base colour, roughness, metallic and emission OUT OF THAT
// TABLE. It does not know where they are: `cyFrame.materialOffsets` carries the four word offsets,
// and the pipeline layer resolves them from the `MaterialProgram` the table was described with. A
// hardcoded offset here would be correct on the day it was written and wrong on a later day nobody
// can name, which is the same argument that made M1's reflection generated.
//
// ================================================================================================
// REGENERATING THE COMPILED MODULES
// ================================================================================================
//
//   slangc frame.slang -target spirv -profile spirv_1_5 -I <shaders dir> \
//          -entry cyDepthVertex    -stage vertex   -o frame_depth_vertex.spv
//   slangc ... -entry cyForwardVertex   -stage vertex   -o frame_forward_vertex.spv
//   slangc ... -entry cyForwardFragment -stage fragment -o frame_forward_fragment.spv
//   slangc ... -entry cyDepthFragment   -stage fragment -o frame_depth_fragment.spv
//
// `src/rendering/pipeline/shaders/embed_spirv.py` turns them into the committed header, for the
// reason samples/03-first-light's does: `CY_SHADER_SLANG` is off by default and a milestone
// artefact that only runs on a machine with a shader toolchain is one nobody can run.

module frame;

import cy.brdf;
import cy.cluster;
import cy.globals;
import cy.light;
import cy.material;
import cy.packing;

// --- The per-view block -------------------------------------------------------------------------

/// What one assembled frame tells its passes. `cy::rendering::pipeline::FrameViewData` is this
/// struct's C++ half and the two are asserted equal, field offset for field offset.
public struct CyFrameData
{
    /// Camera-relative space to clip space. Reversed-Z: near maps to 1, far to 0. See the header
    /// comment for why these are rows and not a `float4x4`.
    public float4 relativeToClipRow0;
    public float4 relativeToClipRow1;
    public float4 relativeToClipRow2;
    public float4 relativeToClipRow3;
    /// Camera-relative space to VIEW space. Only the third row is ever read — the cluster slice
    /// needs a view-space depth and nothing else does — but all four are carried so a pass that
    /// wants a view-space normal has them without a second upload.
    public float4 relativeToViewRow0;
    public float4 relativeToViewRow1;
    public float4 relativeToViewRow2;
    public float4 relativeToViewRow3;

    /// The sky's irradiance contribution, and the ambient occlusion strength in `w`.
    public float4 ambientAndOcclusion;
    /// xy: the render extent in pixels. zw: its reciprocal.
    public float4 extentAndInverse;

    public ClusterGrid clusterGrid;

    /// x: how many lights `cyLights` holds. y: how many words one material block is.
    /// z: `kClusterElementTypeCount`, so the header stride is data rather than a constant repeated
    /// on two sides. w: 1 when the cluster lists were assigned, 0 when the frame had no grid — in
    /// which case the shading loop walks every light instead, which is the honest fallback and is
    /// what a frame with `cluster_count() == 0` actually gets.
    public uint4 counts;
    /// Word offsets into a material block: base colour, roughness, metallic, emission. Derived from
    /// the `MaterialProgram`; see the header comment.
    public uint4 materialOffsets;
};

/// `GpuDrawInstance` — 32 bytes, the per-draw indirection `first_instance` names.
public struct CyDrawInstance
{
    public uint instanceSlot;
    public uint material;
    public uint parameterOffset;
    public uint giAddress;
    public uint layerMask;
    public uint lodAndFade;
    public uint surface;
    public uint flags;
};

/// One instance's placement, model to CAMERA-RELATIVE. Never world space (design.md §3).
public struct CyInstanceTransform
{
    public float4 row0;
    public float4 row1;
    public float4 row2;
    /// rgb: a per-instance tint the material's base colour is multiplied by. a: unused.
    public float4 tint;
};

[[vk::binding(0, 1)]]
public ConstantBuffer<CyFrameData> cyFrame;
[[vk::binding(1, 1)]]
public StructuredBuffer<Light> cyLights;
[[vk::binding(2, 1)]]
public StructuredBuffer<uint2> cyClusterHeaders;
[[vk::binding(3, 1)]]
public StructuredBuffer<uint> cyClusterIndices;
[[vk::binding(4, 1)]]
public StructuredBuffer<CyDrawInstance> cyDrawInstances;
[[vk::binding(5, 1)]]
public StructuredBuffer<CyInstanceTransform> cyInstances;
[[vk::binding(6, 1)]]
public StructuredBuffer<uint> cyMaterialWords;

/// The one push constant: which draw is being recorded.
public struct CyDrawPush
{
    public uint drawIndex;
};

[[vk::push_constant]]
public ConstantBuffer<CyDrawPush> cyDraw;

// --- Shared helpers -------------------------------------------------------------------------

public float4 transformToClip(float3 relative)
{
    let point = float4(relative, 1.0);
    return float4(dot(cyFrame.relativeToClipRow0, point), dot(cyFrame.relativeToClipRow1, point),
                  dot(cyFrame.relativeToClipRow2, point), dot(cyFrame.relativeToClipRow3, point));
}

public float3 transformToRelative(CyInstanceTransform instance, float3 modelPosition)
{
    let point = float4(modelPosition, 1.0);
    return float3(dot(instance.row0, point), dot(instance.row1, point), dot(instance.row2, point));
}

/// The rotation half of the placement, applied to a direction. No inverse transpose: the frame's
/// instances are rigid plus uniform scale, and `rendering-geometry-and-resources` says a
/// non-uniform scale is normalised at import rather than compensated for per pixel.
public float3 rotateToRelative(CyInstanceTransform instance, float3 direction)
{
    return normalize(float3(dot(instance.row0.xyz, direction), dot(instance.row1.xyz, direction),
                            dot(instance.row2.xyz, direction)));
}

public float viewDepthOf(float3 relative)
{
    let point = float4(relative, 1.0);
    // -Z forward (`core-math`), so a point in front of the camera has a negative view-space z and
    // the positive depth the cluster slice wants is its negation.
    return -dot(cyFrame.relativeToViewRow2, point);
}

float readMaterialFloat(uint material, uint wordOffset)
{
    return asfloat(cyMaterialWords[material * cyFrame.counts.y + wordOffset]);
}

float3 readMaterialFloat3(uint material, uint wordOffset)
{
    let base = material * cyFrame.counts.y + wordOffset;
    return float3(asfloat(cyMaterialWords[base]), asfloat(cyMaterialWords[base + 1]),
                  asfloat(cyMaterialWords[base + 2]));
}

/// The surface the frame shades, read OUT OF THE MATERIAL TABLE. See the header comment.
Surface surfaceOf(uint material, float3 tint)
{
    Surface surface = defaultSurface();
    surface.albedo = readMaterialFloat3(material, cyFrame.materialOffsets.x) * tint;
    surface.roughness = clamp(readMaterialFloat(material, cyFrame.materialOffsets.y), 0.02, 1.0);
    surface.metallic = saturate(readMaterialFloat(material, cyFrame.materialOffsets.z));
    surface.emission = readMaterialFloat3(material, cyFrame.materialOffsets.w);
    return surface;
}

// --- The depth prepass ------------------------------------------------------------------------
//
// POSITION ONLY. `render::kDepthPassStreams` is `stream_bit(Position)` and this shader is why that
// constant exists: binding the normal and UV streams for a pass that writes depth is exactly the
// bandwidth `rendering-geometry-and-resources` split the streams to avoid.

public struct CyDepthVertex
{
    public float4 position : SV_Position;
};

[shader("vertex")]
public CyDepthVertex cyDepthVertex(float3 modelPosition : POSITION)
{
    let draw = cyDrawInstances[cyDraw.drawIndex];
    let instance = cyInstances[draw.instanceSlot];
    CyDepthVertex output;
    output.position = transformToClip(transformToRelative(instance, modelPosition));
    return output;
}

[shader("fragment")]
public void cyDepthFragment(CyDepthVertex input)
{
    // Nothing. The prepass writes depth and the depth is written by the fixed-function stage; an
    // empty fragment shader is what makes the pipeline legal without a colour attachment.
}

// --- The forward pass ---------------------------------------------------------------------------

public struct CyForwardVertex
{
    public float4 position : SV_Position;
    public float3 relativePosition : TEXCOORD0;
    public float3 normal : TEXCOORD1;
    public float2 uv : TEXCOORD2;
    public nointerpolation uint drawIndex : TEXCOORD3;
};

[shader("vertex")]
public CyForwardVertex cyForwardVertex(float3 modelPosition : POSITION,
                                       float4 packedNormal : NORMAL, float2 uv : TEXCOORD0)
{
    let draw = cyDrawInstances[cyDraw.drawIndex];
    let instance = cyInstances[draw.instanceSlot];

    CyForwardVertex output;
    output.relativePosition = transformToRelative(instance, modelPosition);
    output.position = transformToClip(output.relativePosition);
    // `render::PackedNormalTangent` is two 16-bit signed normalised pairs: the octahedral normal in
    // xy and the octahedral tangent in zw. Only the normal is read here; a material that wants a
    // tangent frame reads zw the same way.
    output.normal = rotateToRelative(instance, decodeOctahedral(packedNormal.xy));
    output.uv = uv;
    output.drawIndex = cyDraw.drawIndex;
    return output;
}

/// The light loop, over the cluster this pixel falls in.
///
/// `counts.w` of zero means the frame declared no cluster grid — a view with no lights, or a grid
/// configured to nothing — and the loop then walks every light. That is a DECLARED fallback rather
/// than a silent one: it is slower and it is correct, and a frame that quietly shaded nothing
/// because its grid was empty is the failure this branch exists to make impossible.
float3 accumulateLights(Surface surface, float3 relativePosition, float3 normal, float3 viewDir,
                        uint2 pixel)
{
    float3 lit = float3(0.0);
    let lightCount = cyFrame.counts.x;

    // A DIRECTIONAL LIGHT IS NEVER IN A CLUSTER LIST, and this loop is why the frame is not black.
    // `FrameAssembly::build_lights` says so in as many words — "a directional light has an infinite
    // bound and is not clustered: it reaches every cluster, and putting it in every list would be
    // the whole grid holding one index" — so the cluster list carries the PUNCTUAL lights and the
    // sun is evaluated globally. The first version of this shader looped the cluster list alone; the
    // captured frame came back lit by two point lights and nothing else, which is how the omission
    // was found. There are a handful of directional lights in any frame, so the loop is short.
    for (uint global = 0; global < lightCount; ++global)
    {
        if (cyLights[global].kind != kLightDirectional)
        {
            continue;
        }
        let sunSample = evaluateLight(cyLights[global], relativePosition);
        lit += shadeSurfaceWithLight(surface, normal, viewDir, sunSample);
    }

    if (cyFrame.counts.w == 0 || cyFrame.clusterGrid.dimensions.z == 0)
    {
        for (uint index = 0; index < lightCount; ++index)
        {
            if (cyLights[index].kind == kLightDirectional)
            {
                continue;  // already accumulated above
            }
            let sample = evaluateLight(cyLights[index], relativePosition);
            lit += shadeSurfaceWithLight(surface, normal, viewDir, sample);
        }
        return lit;
    }

    let extent = uint2(cyFrame.extentAndInverse.xy);
    let coord = clusterCoordOf(cyFrame.clusterGrid, pixel, extent, viewDepthOf(relativePosition));
    let cluster = clusterIndexOf(cyFrame.clusterGrid, coord);
    // The headers are indexed `cluster * kClusterElementTypeCount + type`, and the light type is
    // zero. The stride comes from `counts.z` so the two sides cannot disagree about it.
    let header = cyClusterHeaders[cluster * cyFrame.counts.z];
    for (uint slot = 0; slot < header.y; ++slot)
    {
        let index = cyClusterIndices[header.x + slot];
        if (index >= lightCount || cyLights[index].kind == kLightDirectional)
        {
            continue;
        }
        let sample = evaluateLight(cyLights[index], relativePosition);
        lit += shadeSurfaceWithLight(surface, normal, viewDir, sample);
    }
    return lit;
}

[shader("fragment")]
public float4 cyForwardFragment(CyForwardVertex input)
{
    let pixel = uint2(input.position.xy);
    let draw = cyDrawInstances[input.drawIndex];
    let instance = cyInstances[draw.instanceSlot];
    let surface = surfaceOf(draw.material, instance.tint.rgb);

    let normal = normalize(input.normal);
    // Camera-relative, so the camera is at the origin and the direction to it is the negated
    // position. There is nowhere in this file to put a world-space camera, which is the point.
    let viewDir = normalize(-input.relativePosition);

    var color = accumulateLights(surface, input.relativePosition, normal, viewDir, pixel);
    color += surface.emission;
    color += surface.albedo * cyFrame.ambientAndOcclusion.rgb * surface.occlusion;
    return float4(color, surface.opacity);
}
