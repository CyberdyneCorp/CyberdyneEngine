// The visibility buffer and its material resolve. M7 task 7.4.
//
// `virtual-geometry` — "Visibility buffer and material resolve": "the geometry pass writes only what
// is needed to identify a surface — instance identifier, primitive identifier, and sufficient
// information to recover barycentrics — and material evaluation happens in a later pass", with
// resolve classifying "pixels by material into bins and evaluat[ing] each bin", and reconstructing
// attributes "from the identified primitive rather than interpolating them through the geometry
// pass".
//
// ================================================================================================
// TWO WORDS PER PIXEL, AND NO BARYCENTRICS AMONG THEM
// ================================================================================================
//
// A pixel carries a STABLE SURFACE IDENTITY and the triangle within it, and nothing else. That is
// enough to identify the surface — the identity names the instance and the cluster, the cluster
// names its page and its geometry — and the barycentrics are RECOVERED in the resolve by
// projecting the identified triangle and solving for the pixel centre. Storing them would be four
// more bytes per pixel to save an arithmetic the resolve has to do anyway to get a derivative.
//
// The identity is `instance * clusterStride + cluster`, which is the traversal's own DAG mark
// index. It used to be the index of the visible-cluster record instead, and that index is
// ATOMIC-APPEND ORDER: it names the same surface by a different number on every run. See the
// comment on `kTrianglePayloadBits` for why that mattered and what it cost to remove.
//
// ================================================================================================
// TWO RASTERISERS, ONE VISIBILITY BUFFER
// ================================================================================================
//
// `virtual-geometry` says visible clusters "SHALL be rasterised through a **hardware** path by
// default" and that the architecture "SHALL permit a compute rasterisation path for micro-triangles
// ... cluster dispatch, output format, and the visibility buffer SHALL not assume hardware
// rasterisation exclusively". M7 shipped the compute path: a workgroup per visible cluster, a
// bounding-box scan per triangle, and an atomic depth test into a buffer (`vgVisRaster`).
//
// M11.c task 4.3 adds the hardware path, and it is what the forward frame runs: `vgVisHwVertex` and
// `vgVisHwFragment` draw the SAME visible records, decode the SAME cluster payload through the SAME
// `decodeVertex`, and write the SAME `(identity << 8) | triangle` word — into a colour target the
// forward frame's `virtual geometry` stage owns, with the frame's own depth buffer doing the depth
// test the compute path does with a 64-bit atomic. `vgVisHwUnpack` turns that target into the
// `uint2` visibility buffer, and everything downstream — the classification, the bins, the resolve
// — is written against the FORMAT and does not know which rasteriser filled it.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   slangc vg_visbuffer.slang -target spirv -profile spirv_1_5 \
//          -entry vgVisClear    -stage compute -o vg_vis_clear.spv
//   slangc vg_visbuffer.slang -target spirv -profile spirv_1_5 \
//          -entry vgVisPrepare  -stage compute -o vg_vis_prepare.spv
//   slangc vg_visbuffer.slang -target spirv -profile spirv_1_5 \
//          -entry vgVisRaster   -stage compute -o vg_vis_raster.spv
//   slangc vg_visbuffer.slang -target spirv -profile spirv_1_5 \
//          -entry vgVisUnpack   -stage compute -o vg_vis_unpack.spv
//   slangc vg_visbuffer.slang -target spirv -profile spirv_1_5 \
//          -entry vgVisClassify -stage compute -o vg_vis_classify.spv
//   slangc vg_visbuffer.slang -target spirv -profile spirv_1_5 \
//          -entry vgVisScan     -stage compute -o vg_vis_scan.spv
//   slangc vg_visbuffer.slang -target spirv -profile spirv_1_5 \
//          -entry vgVisScatter  -stage compute -o vg_vis_scatter.spv
//   slangc vg_visbuffer.slang -target spirv -profile spirv_1_5 \
//          -entry vgVisResolve  -stage compute -o vg_vis_resolve.spv
//   slangc vg_visbuffer.slang -target spirv -profile spirv_1_5 \
//          -entry vgVisHwPrepare -stage compute -o vg_vis_hw_prepare.spv
//   slangc vg_visbuffer.slang -target spirv -profile spirv_1_5 \
//          -entry vgVisHwUnpack  -stage compute -o vg_vis_hw_unpack.spv
//   slangc vg_visbuffer.slang -target spirv -profile spirv_1_5 \
//          -entry vgVisHwVertex   -stage vertex   -o vg_vis_hw_vertex.spv
//   slangc vg_visbuffer.slang -target spirv -profile spirv_1_5 \
//          -entry vgVisHwFragment -stage fragment -o vg_vis_hw_fragment.spv
//   python3 embed_spirv.py vg_visbuffer_spirv.h \
//          kVgVisClearSpirv=vg_vis_clear.spv kVgVisPrepareSpirv=vg_vis_prepare.spv \
//          kVgVisRasterSpirv=vg_vis_raster.spv kVgVisUnpackSpirv=vg_vis_unpack.spv \
//          kVgVisClassifySpirv=vg_vis_classify.spv kVgVisScanSpirv=vg_vis_scan.spv \
//          kVgVisScatterSpirv=vg_vis_scatter.spv kVgVisResolveSpirv=vg_vis_resolve.spv \
//          kVgVisHwPrepareSpirv=vg_vis_hw_prepare.spv kVgVisHwUnpackSpirv=vg_vis_hw_unpack.spv \
//          kVgVisHwVertexSpirv=vg_vis_hw_vertex.spv kVgVisHwFragmentSpirv=vg_vis_hw_fragment.spv
//   clang-format -i vg_visbuffer_spirv.h

struct VgCluster
{
    float3 boundsMin;
    float lodError;
    float3 boundsMax;
    float parentError;
    float3 lodCenter;
    float lodRadius;
    float3 parentCenter;
    float parentRadius;
    float3 coneAxis;
    float coneCos;
    uint material;
    uint page;
    uint firstChild;
    uint childCount;
};

struct VgClusterGeometry
{
    uint payloadOffset;
    uint vertexCount;
    uint indexCount;
    uint reserved;
};

struct VgInstance
{
    float3 translation;
    float scale;
    float4 rotation;
    uint asset;
    uint materialOffset;
    float thresholdScale;
    uint layerMask;
};

struct VgAsset
{
    uint clusterBase;
    uint clusterCount;
    uint rootFirst;
    uint rootCount;
    uint pageBase;
    uint pageCount;
    uint childBase;
    uint reserved;
};

struct VgVisible
{
    uint instance;
    uint cluster;
    uint material;
    float screenError;
};

struct VgVisPush
{
    // Rows of the world-to-clip matrix. FOUR EXPLICIT ROWS rather than a float4x4, for the reason
    // tests/render/shaders/conventions.slang gives: a matrix in a push-constant block has a layout
    // that is a property of the compiler's flags, and `mul(M, v)` versus `mul(v, M)` silently
    // transposes. Four rows and four dot products leave nothing to disagree about.
    float4 row0;
    float4 row1;
    float4 row2;
    float4 row3;
    uint width;
    uint height;
    uint materialCount;
    uint vertexStride;
    uint normalOffset;  // 0xFFFFFFFF when the asset stores no normals
    uint uvOffset;      // 0xFFFFFFFF when the asset stores no UVs
    // 1 / (2^bits - 1) FOR EACH ATTRIBUTE, and they are separate numbers because the encoder's
    // declared bit depths are. A component is STORED in sixteen bits and QUANTISED to the asset's
    // own precision, so dividing a ten-bit normal by 65535 rather than by 1023 gives a value near
    // zero for every input — which decodes, through the octahedral fold, to the constant (0, 0, -1).
    // Measured: 2,529 of 2,529 pixels resolving to a normal exactly opposite the reference's.
    float positionScale;
    float normalScale;
    float uvScale;
    // The stride of the SURFACE IDENTITY: `instance * clusterStride + cluster`, where the stride is
    // the largest cluster count of any asset in the scene. It is `GpuScene::cluster_stride()`, the
    // same number the traversal uses for its DAG visit marks, so the two name a surface identically.
    uint clusterStride;
    // THE HARDWARE RASTERISER'S TWO NUMBERS, appended so that every compute entry point above reads
    // the block exactly as it did. `drawIndexCount` is three times the largest triangle count of
    // any cluster in the scene — the one draw's index count, every instance a visible cluster — and
    // `visibleCapacity` is the length of `visible`, which bounds the instance count a traversal that
    // overran its list can ask for.
    uint drawIndexCount;
    uint visibleCapacity;
};

[[vk::push_constant]]
ConstantBuffer<VgVisPush> push;

[[vk::binding(0, 0)]] StructuredBuffer<VgVisible> visible;
[[vk::binding(1, 0)]] StructuredBuffer<VgCluster> clusters;
[[vk::binding(2, 0)]] StructuredBuffer<VgClusterGeometry> geometry;
[[vk::binding(3, 0)]] StructuredBuffer<VgInstance> instances;
[[vk::binding(4, 0)]] StructuredBuffer<VgAsset> assets;
[[vk::binding(5, 0)]] ByteAddressBuffer payload;
[[vk::binding(6, 0)]] RWStructuredBuffer<uint2> visbuffer;
[[vk::binding(7, 0)]] RWStructuredBuffer<uint64_t> depth;
[[vk::binding(8, 0)]] RWStructuredBuffer<uint> binCounts;
[[vk::binding(9, 0)]] RWStructuredBuffer<uint> binOffsets;
[[vk::binding(10, 0)]] RWStructuredBuffer<uint> binCursor;
[[vk::binding(11, 0)]] RWStructuredBuffer<uint> binPixels;
[[vk::binding(12, 0)]] RWStructuredBuffer<float4> resolved;
// The traversal's own counter buffer, read rather than copied: the visible count is a number the
// GPU produced and the CPU never learns, so the raster and the resolve are dispatched INDIRECTLY
// from arguments derived on the device. That is the same rule the traversal follows — "the CPU
// SHALL submit a small, bounded number of passes with indirect arguments" — applied one pass later.
[[vk::binding(13, 0)]] StructuredBuffer<uint> counters;
[[vk::binding(14, 0)]] RWStructuredBuffer<uint> visArgs;
// The hardware rasteriser's visibility target, copied out of the forward frame's `visibility`
// texture one word a texel. Read by `vgVisHwUnpack` and by nothing else.
[[vk::binding(15, 0)]] RWStructuredBuffer<uint> hwPayload;

// Counter slot 2 is the visible cluster count; see `vg_traversal.slang`.
static const uint kCountVisible = 2;
// visArgs[0..2] dispatch the raster, one group per visible cluster; [3..5] dispatch the resolve,
// one thread per binned pixel.
static const uint kArgsRaster = 0;
static const uint kArgsResolve = 3;
// visArgs[6..10] are the hardware rasteriser's `VkDrawIndexedIndirectCommand`: one draw, one
// instance per visible cluster.
static const uint kArgsDraw = 6;

static const uint kNoSurface = 0xFFFFFFFFu;
static const uint kFarDepth = 0xFFFFFFFFu;

/// DEPTH AND PAYLOAD IN ONE ATOMIC. The depth test and the payload store used to be two unordered
/// writes, so a farther fragment's store could land last and own a pixel the nearer one won. They
/// are one 64-bit value now: the depth key in the high half, the payload in the low half, settled by
/// a single `InterlockedMin`. Ordering by the packed value orders by depth first, and ties break by
/// payload — deterministically, rather than by whichever store the memory system retired last.
///
/// THE PAYLOAD IS A STABLE IDENTITY, NOT THE VISIBLE INDEX, and that is the second half of the same
/// defect. Pairing depth with the payload made "a farther fragment wins" impossible; it left the
/// case where two fragments tie on the depth key EXACTLY, where the minimum is decided by the
/// payload alone. The payload used to be `(visibleIndex << 8) | triangle` and `visibleIndex` is the
/// slot the traversal's `InterlockedAdd` happened to hand out, which permutes run to run — so an
/// exact tie was broken differently on each run. Measured on samples/07-fidelity at 1280x720: one
/// to three pixels of 921,593 moved between identical runs.
///
/// It is now `(identity << kTrianglePayloadBits) | triangle`, with
/// `identity = instance * push.clusterStride + cluster` — a pure function of the scene. An exact
/// tie is now broken towards the lower instance, and towards the lower cluster within it, on every
/// run and every device.
///
/// THE ARITHMETIC, because the substitution is not free. The atomic is 64 bits: 32 of depth key and
/// 32 of payload, of which 8 are the triangle. That leaves the identity the SAME 24 bits the
/// visible index had, so the scene bound changes from `visible_capacity <= 2^24` to
/// `instanceCount * clusterStride <= 2^24`. The traversal's own DAG marks are one word per
/// (instance, cluster) and it refuses above 2^26, so this bound is four times tighter than the one
/// already in the tree — `VisbufferPass::initialise` refuses above it by name rather than wrapping
/// a pixel onto a neighbouring cluster.
static const uint kTrianglePayloadBits = 8u;
static const uint kTrianglePayloadMask = (1u << kTrianglePayloadBits) - 1u;
static const uint64_t kFarPacked = 0xFFFFFFFFFFFFFFFFull;

// --- Decoding -----------------------------------------------------------------------------------
//
// Every 16-bit component sits at an EVEN byte offset — the encoder pads each cluster's geometry
// block to four bytes precisely so that this holds — so a component is always inside the word that
// contains its first byte and never straddles two.

uint loadU16(uint byteOffset)
{
    uint word = payload.Load(byteOffset & ~3u);
    return (word >> ((byteOffset & 3u) * 8u)) & 0xFFFFu;
}

uint loadU8(uint byteOffset)
{
    uint word = payload.Load(byteOffset & ~3u);
    return (word >> ((byteOffset & 3u) * 8u)) & 0xFFu;
}

/// The surface an identity names, derived rather than looked up.
///
/// This is the same arithmetic `vgTraverse` does when it fills a `VgVisible` record — the cluster's
/// own material plus the instance's offset — so classify, scatter and resolve reach the same answer
/// the traversal would have written, without reading a list whose ORDER is not reproducible.
struct VgSurface
{
    uint instance;
    uint cluster;
    uint material;
};

VgSurface surfaceFromIdentity(uint identity)
{
    // `max(..., 1)` guards a scene with no clusters at all, where the stride is zero and the pass
    // has nothing to shade anyway. An integer divide by zero is undefined on a device.
    uint stride = max(push.clusterStride, 1u);
    VgSurface surface;
    surface.instance = identity / stride;
    surface.cluster = identity % stride;
    VgInstance instance = instances[surface.instance];
    VgAsset asset = assets[instance.asset];
    surface.material = clusters[asset.clusterBase + surface.cluster].material +
                       instance.materialOffset;
    return surface;
}

float3 rotateByQuaternion(float4 q, float3 v)
{
    float3 axis = q.xyz;
    float3 t = 2.0f * cross(axis, v);
    return v + q.w * t + cross(axis, t);
}

/// One decoded vertex of one cluster, in the asset's own space.
struct Vertex
{
    float3 position;
    float3 normal;
    float2 uv;
};

Vertex decodeVertex(VgCluster c, VgClusterGeometry g, uint slot)
{
    uint base = g.payloadOffset + (slot * push.vertexStride);
    float3 span = c.boundsMax - c.boundsMin;
    span = float3(span.x > 1.0e-12f ? span.x : 1.0f, span.y > 1.0e-12f ? span.y : 1.0f,
                  span.z > 1.0e-12f ? span.z : 1.0f);

    Vertex vertex;
    vertex.position =
        c.boundsMin + float3(float(loadU16(base + 0)) * push.positionScale * span.x,
                             float(loadU16(base + 2)) * push.positionScale * span.y,
                             float(loadU16(base + 4)) * push.positionScale * span.z);
    vertex.normal = float3(0.0f, 0.0f, 1.0f);
    if (push.normalOffset != 0xFFFFFFFFu)
    {
        // Octahedral, at the declared depth. The decode is the same fold cy/core's encoder applies
        // in reverse, and `test_visbuffer.cpp` compares it against the C++ one.
        float2 oct = float2(float(loadU16(base + push.normalOffset)) * push.normalScale,
                            float(loadU16(base + push.normalOffset + 2)) * push.normalScale) *
                         2.0f -
                     1.0f;
        float3 n = float3(oct.x, oct.y, 1.0f - abs(oct.x) - abs(oct.y));
        if (n.z < 0.0f)
        {
            float x = (1.0f - abs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f);
            float y = (1.0f - abs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f);
            n.x = x;
            n.y = y;
        }
        vertex.normal = normalize(n);
    }
    vertex.uv = float2(0.0f, 0.0f);
    if (push.uvOffset != 0xFFFFFFFFu)
    {
        vertex.uv = float2(float(loadU16(base + push.uvOffset)) * push.uvScale,
                           float(loadU16(base + push.uvOffset + 2)) * push.uvScale);
    }
    return vertex;
}

float4 toClip(float3 world)
{
    return float4(dot(push.row0, float4(world, 1.0f)), dot(push.row1, float4(world, 1.0f)),
                  dot(push.row2, float4(world, 1.0f)), dot(push.row3, float4(world, 1.0f)));
}

float3 toWorld(VgInstance instance, float3 local)
{
    return rotateByQuaternion(instance.rotation, local) * instance.scale + instance.translation;
}

// --- The passes ---------------------------------------------------------------------------------

[shader("compute")]
[numthreads(64, 1, 1)]
void vgVisClear(uint3 id : SV_DispatchThreadID)
{
    uint pixel = id.x;
    if (pixel < push.width * push.height)
    {
        visbuffer[pixel] = uint2(kNoSurface, 0u);
        depth[pixel] = kFarPacked;
        resolved[pixel] = float4(0.0f, 0.0f, 0.0f, 0.0f);
        binPixels[pixel] = kNoSurface;
    }
    if (pixel < push.materialCount)
    {
        binCounts[pixel] = 0u;
        binCursor[pixel] = 0u;
        binOffsets[pixel] = 0u;
    }
    if (pixel == push.materialCount)
    {
        binOffsets[pixel] = 0u;
    }
}

/// One workgroup per visible cluster; one thread per triangle, looping when a cluster has more
/// triangles than the group has threads.
///
/// REVERSED Z, and the depth test is an atomic MINIMUM on the bit pattern of a float in [0, 1] — a
/// non-negative float's ordering is its unsigned integer ordering, which is what makes an integer
/// atomic a depth test. The engine's clip volume is reversed, so NEAR is the LARGER depth and the
/// value compared is `1 - z`.
[shader("compute")]
[numthreads(64, 1, 1)]
void vgVisRaster(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID,
                 uint3 thread : SV_GroupThreadID)
{
    uint visibleIndex = group.x;
    if (visibleIndex >= counters[kCountVisible])
    {
        return;
    }
    VgVisible record = visible[visibleIndex];
    VgInstance instance = instances[record.instance];
    VgAsset asset = assets[instance.asset];
    uint clusterIndex = asset.clusterBase + record.cluster;
    VgCluster cluster = clusters[clusterIndex];
    VgClusterGeometry g = geometry[clusterIndex];
    uint triangles = g.indexCount / 3u;
    // The visible list is read HERE and nowhere downstream: what a pixel keeps is the identity of
    // the surface, not the slot this record happens to occupy. See `kTrianglePayloadBits`.
    uint identity = record.instance * max(push.clusterStride, 1u) + record.cluster;

    for (uint triangle = thread.x; triangle < triangles; triangle += 64u)
    {
        uint indexBase = g.payloadOffset + (g.vertexCount * push.vertexStride) + (triangle * 3u);
        float3 clip[3];
        float2 screen[3];
        bool behind = false;
        for (uint corner = 0; corner < 3; ++corner)
        {
            Vertex vertex = decodeVertex(cluster, g, loadU8(indexBase + corner));
            float4 position = toClip(toWorld(instance, vertex.position));
            if (position.w <= 1.0e-6f)
            {
                behind = true;
                break;
            }
            float3 ndc = position.xyz / position.w;
            clip[corner] = ndc;
            screen[corner] = float2((ndc.x * 0.5f + 0.5f) * float(push.width),
                                    (ndc.y * 0.5f + 0.5f) * float(push.height));
        }
        if (behind)
        {
            continue;
        }

        float area = (screen[1].x - screen[0].x) * (screen[2].y - screen[0].y) -
                     (screen[1].y - screen[0].y) * (screen[2].x - screen[0].x);
        if (abs(area) < 1.0e-9f)
        {
            continue;
        }
        float inverseArea = 1.0f / area;

        float2 low = min(screen[0], min(screen[1], screen[2]));
        float2 high = max(screen[0], max(screen[1], screen[2]));
        int minX = max(int(floor(low.x)), 0);
        int minY = max(int(floor(low.y)), 0);
        int maxX = min(int(ceil(high.x)), int(push.width) - 1);
        int maxY = min(int(ceil(high.y)), int(push.height) - 1);

        for (int y = minY; y <= maxY; ++y)
        {
            for (int x = minX; x <= maxX; ++x)
            {
                float2 centre = float2(float(x) + 0.5f, float(y) + 0.5f);
                float w0 = ((screen[1].x - centre.x) * (screen[2].y - centre.y) -
                            (screen[1].y - centre.y) * (screen[2].x - centre.x)) *
                           inverseArea;
                float w1 = ((screen[2].x - centre.x) * (screen[0].y - centre.y) -
                            (screen[2].y - centre.y) * (screen[0].x - centre.x)) *
                           inverseArea;
                float w2 = 1.0f - w0 - w1;
                if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f)
                {
                    continue;
                }
                float z = (w0 * clip[0].z) + (w1 * clip[1].z) + (w2 * clip[2].z);
                if (z < 0.0f || z > 1.0f)
                {
                    continue;
                }
                uint key = uint(saturate(1.0f - z) * 4294967040.0f);
                uint pixel = uint(y) * push.width + uint(x);
                uint payload = (identity << kTrianglePayloadBits) |
                               (triangle & kTrianglePayloadMask);
                uint64_t packed = (uint64_t(key) << 32) | uint64_t(payload);
                InterlockedMin(depth[pixel], packed);
            }
        }
    }
}

/// THE UNPACK. `vgVisRaster` settles depth and payload together in one 64-bit atomic, so the
/// visibility buffer is a pure function of that buffer rather than something the raster races to
/// write. Splitting it out keeps every pass downstream — classify, scan, scatter, resolve — reading
/// the same `uint2` visibility buffer they always did, which is why this fix does not reach them.
///
/// One thread per pixel, no atomics, no contention: the answer is already decided.
[shader("compute")]
[numthreads(64, 1, 1)]
void vgVisUnpack(uint3 id : SV_DispatchThreadID)
{
    uint pixel = id.x;
    if (pixel >= push.width * push.height)
    {
        return;
    }
    uint64_t packed = depth[pixel];
    if (packed == kFarPacked)
    {
        visbuffer[pixel] = uint2(kNoSurface, 0u);
        return;
    }
    uint payload = uint(packed & 0xFFFFFFFFull);
    // x is the surface identity, y is the triangle within its cluster.
    visbuffer[pixel] = uint2(payload >> kTrianglePayloadBits, payload & kTrianglePayloadMask);
}

/// `virtual-geometry` — "Material resolve SHALL **classify pixels by material** into bins". This is
/// the count; `vgVisScan` turns the counts into offsets and `vgVisScatter` fills the bins.
[shader("compute")]
[numthreads(64, 1, 1)]
void vgVisClassify(uint3 id : SV_DispatchThreadID)
{
    uint pixel = id.x;
    if (pixel >= push.width * push.height)
    {
        return;
    }
    uint2 sample = visbuffer[pixel];
    if (sample.x == kNoSurface)
    {
        return;
    }
    uint material = surfaceFromIdentity(sample.x).material;
    if (material < push.materialCount)
    {
        InterlockedAdd(binCounts[material], 1u);
    }
}

/// The prefix sum, in one thread. The bin count is the number of distinct materials in a view,
/// which is hundreds rather than millions; a work-efficient parallel scan here would be more code
/// than the whole resolve and would save microseconds.
[shader("compute")]
[numthreads(1, 1, 1)]
void vgVisScan()
{
    uint running = 0;
    for (uint material = 0; material < push.materialCount; ++material)
    {
        binOffsets[material] = running;
        running += binCounts[material];
    }
    binOffsets[push.materialCount] = running;
    // The resolve's own dispatch arguments: one thread per binned pixel, from a total only the GPU
    // knows.
    visArgs[kArgsResolve + 0] = (running + 63u) / 64u;
    visArgs[kArgsResolve + 1] = 1u;
    visArgs[kArgsResolve + 2] = 1u;
}

/// One thread, before the raster: the visible count the traversal produced becomes the raster's
/// dispatch arguments, one workgroup per visible cluster.
[shader("compute")]
[numthreads(1, 1, 1)]
void vgVisPrepare()
{
    visArgs[kArgsRaster + 0] = counters[kCountVisible];
    visArgs[kArgsRaster + 1] = 1u;
    visArgs[kArgsRaster + 2] = 1u;
    visArgs[kArgsResolve + 0] = 0u;
    visArgs[kArgsResolve + 1] = 1u;
    visArgs[kArgsResolve + 2] = 1u;
}

// --- The hardware rasteriser ---------------------------------------------------------------------

/// `vgVisPrepare` for the hardware path: the same resolve arguments, and ONE indexed draw whose
/// instances are the visible clusters. The index buffer is `0 .. drawIndexCount - 1`, so an index is
/// a corner of a triangle slot and the vertex shader decodes the corner the cluster's own index list
/// names — the cluster's geometry never leaves the payload the compute path reads.
[shader("compute")]
[numthreads(1, 1, 1)]
void vgVisHwPrepare()
{
    uint count = min(counters[kCountVisible], push.visibleCapacity);
    visArgs[kArgsRaster + 0] = count;
    visArgs[kArgsRaster + 1] = 1u;
    visArgs[kArgsRaster + 2] = 1u;
    visArgs[kArgsResolve + 0] = 0u;
    visArgs[kArgsResolve + 1] = 1u;
    visArgs[kArgsResolve + 2] = 1u;
    visArgs[kArgsDraw + 0] = push.drawIndexCount;
    visArgs[kArgsDraw + 1] = count;
    visArgs[kArgsDraw + 2] = 0u;  // firstIndex
    visArgs[kArgsDraw + 3] = 0u;  // vertexOffset
    visArgs[kArgsDraw + 4] = 0u;  // firstInstance
}

struct VgHwVaryings
{
    float4 position : SV_Position;
    nointerpolation uint payload : VG_PAYLOAD;
};

/// One vertex of one triangle slot of one visible cluster.
///
/// `SV_VulkanVertexID` AND `SV_VulkanInstanceID`, not `SV_VertexID` and `SV_InstanceID`: Slang
/// lowers the latter pair to `gl_*Index - gl_Base*`, which declares the `DrawParameters` capability,
/// which the Vulkan backend does not request. `src/rendering/pipeline/README.md` measured the same
/// thing for the forward frame's own shaders. The draw's first vertex and first instance are zero,
/// so the two spellings name the same numbers.
///
/// A SLOT PAST THE CLUSTER'S LAST TRIANGLE IS DROPPED BY CLIPPING, not by a branch in the fragment
/// shader: all three of its corners land at the same point outside the reversed-Z clip volume
/// (`z < 0`), so the triangle is degenerate and outside at once and produces no fragment. The same
/// holds for an instance past the visible count, which a traversal that overran its list reports.
///
/// Y IS NEGATED, and that is what keeps the two rasterisers' buffers byte-compatible. The RHI flips
/// the viewport so that clip `+y` is the TOP row of an image, which is the engine's Y-up convention;
/// `vgVisRaster` maps clip `+y` to the LAST row of its buffer, and every pass downstream —
/// `vgVisResolve`'s pixel centre, `reconstruct_surface` on the host — reconstructs in that
/// convention. Negating here puts row `r` of the target where row `r` of the compute buffer is. The
/// winding flips with it, which is why the pipeline culls nothing, exactly as `vgVisRaster` tests
/// both windings.
[shader("vertex")]
VgHwVaryings vgVisHwVertex(uint vertexId : SV_VulkanVertexID, uint instanceId : SV_VulkanInstanceID)
{
    VgHwVaryings output;
    output.position = float4(0.0f, 0.0f, -1.0f, 1.0f);
    output.payload = 0u;
    if (instanceId >= min(counters[kCountVisible], push.visibleCapacity))
    {
        return output;
    }
    VgVisible record = visible[instanceId];
    VgInstance instance = instances[record.instance];
    VgAsset asset = assets[instance.asset];
    uint clusterIndex = asset.clusterBase + record.cluster;
    VgCluster cluster = clusters[clusterIndex];
    VgClusterGeometry g = geometry[clusterIndex];
    uint triangle = vertexId / 3u;
    uint corner = vertexId - (triangle * 3u);
    if (triangle >= g.indexCount / 3u)
    {
        return output;
    }
    uint indexBase = g.payloadOffset + (g.vertexCount * push.vertexStride) + (triangle * 3u);
    Vertex vertex = decodeVertex(cluster, g, loadU8(indexBase + corner));
    float4 clip = toClip(toWorld(instance, vertex.position));
    output.position = float4(clip.x, -clip.y, clip.z, clip.w);
    // The payload `vgVisRaster` packs, COMPLEMENTED: the target clears to zero, and the complement
    // of zero is the "no surface" word. A payload of all ones would need identity 2^24 - 1 AND
    // triangle 255, and `ForwardVisibility::initialise` refuses a scene whose identity space
    // reaches 2^24 - 1, so a real payload is never stored as zero.
    uint identity = record.instance * max(push.clusterStride, 1u) + record.cluster;
    output.payload = ~((identity << kTrianglePayloadBits) | (triangle & kTrianglePayloadMask));
    return output;
}

/// The whole fragment stage: the depth test already happened, in the frame's depth buffer.
[shader("fragment")]
uint vgVisHwFragment(VgHwVaryings input) : SV_Target0
{
    return input.payload;
}

/// The hardware path's unpack: the copied target, one word a texel, into the `uint2` visibility
/// buffer `vgVisUnpack` produces from the compute path's packed depth.
[shader("compute")]
[numthreads(64, 1, 1)]
void vgVisHwUnpack(uint3 id : SV_DispatchThreadID)
{
    uint pixel = id.x;
    if (pixel >= push.width * push.height)
    {
        return;
    }
    uint payload = ~hwPayload[pixel];
    if (payload == kNoSurface)
    {
        visbuffer[pixel] = uint2(kNoSurface, 0u);
        return;
    }
    visbuffer[pixel] = uint2(payload >> kTrianglePayloadBits, payload & kTrianglePayloadMask);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void vgVisScatter(uint3 id : SV_DispatchThreadID)
{
    uint pixel = id.x;
    if (pixel >= push.width * push.height)
    {
        return;
    }
    uint2 sample = visbuffer[pixel];
    if (sample.x == kNoSurface)
    {
        return;
    }
    uint material = surfaceFromIdentity(sample.x).material;
    if (material >= push.materialCount)
    {
        return;
    }
    uint slot;
    InterlockedAdd(binCursor[material], 1u, slot);
    binPixels[binOffsets[material] + slot] = pixel;
}

/// `virtual-geometry` — "The resolve pass SHALL reconstruct attributes — position, normal, tangent
/// frame, UVs — from the identified primitive rather than interpolating them through the geometry
/// pass."
///
/// One thread per BINNED PIXEL rather than per screen pixel: the dispatch walks the bins, so a
/// scene with thousands of materials evaluates each material once over its own run of pixels
/// instead of issuing a draw per material. What each bin's shading does is one evaluation of the
/// material program, and at M7 that is a placeholder — the normal and the UV, which is what the
/// suite can check against a CPU reconstruction. The material compiler's programs are section 6.
[shader("compute")]
[numthreads(64, 1, 1)]
void vgVisResolve(uint3 id : SV_DispatchThreadID)
{
    uint slot = id.x;
    if (slot >= binOffsets[push.materialCount])
    {
        return;
    }
    uint pixel = binPixels[slot];
    if (pixel == kNoSurface)
    {
        return;
    }
    uint2 sample = visbuffer[pixel];
    if (sample.x == kNoSurface)
    {
        return;
    }
    VgSurface surface = surfaceFromIdentity(sample.x);
    VgInstance instance = instances[surface.instance];
    VgAsset asset = assets[instance.asset];
    uint clusterIndex = asset.clusterBase + surface.cluster;
    VgCluster cluster = clusters[clusterIndex];
    VgClusterGeometry g = geometry[clusterIndex];

    uint indexBase = g.payloadOffset + (g.vertexCount * push.vertexStride) + (sample.y * 3u);

    // THREE NAMED VERTICES RATHER THAN AN ARRAY, and it is not a style choice. Writing
    // `Vertex corner[3]` from a loop with a dynamic index and reading it back by constant produced
    // a correct `corner[0]` and denormal garbage at 1 and 2 on this driver — the barycentrics were
    // right, the projected positions were right, and the blended normal came out at 1e-38. Three
    // locals have no array to lower and the defect cannot recur.
    Vertex v0 = decodeVertex(cluster, g, loadU8(indexBase + 0));
    Vertex v1 = decodeVertex(cluster, g, loadU8(indexBase + 1));
    Vertex v2 = decodeVertex(cluster, g, loadU8(indexBase + 2));

    float4 c0 = toClip(toWorld(instance, v0.position));
    float4 c1 = toClip(toWorld(instance, v1.position));
    float4 c2 = toClip(toWorld(instance, v2.position));
    float2 s0 = float2(((c0.x / max(c0.w, 1.0e-6f)) * 0.5f + 0.5f) * float(push.width),
                       ((c0.y / max(c0.w, 1.0e-6f)) * 0.5f + 0.5f) * float(push.height));
    float2 s1 = float2(((c1.x / max(c1.w, 1.0e-6f)) * 0.5f + 0.5f) * float(push.width),
                       ((c1.y / max(c1.w, 1.0e-6f)) * 0.5f + 0.5f) * float(push.height));
    float2 s2 = float2(((c2.x / max(c2.w, 1.0e-6f)) * 0.5f + 0.5f) * float(push.width),
                       ((c2.y / max(c2.w, 1.0e-6f)) * 0.5f + 0.5f) * float(push.height));

    // THE BARYCENTRICS ARE RECOVERED, NOT STORED. The pixel's centre against the projected
    // triangle: two words per pixel is what "sufficient information to recover barycentrics" buys.
    float2 centre = float2(float(pixel % push.width) + 0.5f, float(pixel / push.width) + 0.5f);
    float area = (s1.x - s0.x) * (s2.y - s0.y) - (s1.y - s0.y) * (s2.x - s0.x);
    float inverseArea = abs(area) > 1.0e-9f ? 1.0f / area : 0.0f;
    float w0 = ((s1.x - centre.x) * (s2.y - centre.y) - (s1.y - centre.y) * (s2.x - centre.x)) *
               inverseArea;
    float w1 = ((s2.x - centre.x) * (s0.y - centre.y) - (s2.y - centre.y) * (s0.x - centre.x)) *
               inverseArea;
    float w2 = 1.0f - w0 - w1;

    float3 blended = v0.normal * w0 + v1.normal * w1 + v2.normal * w2;
    float3 rotated = rotateByQuaternion(instance.rotation, blended);
    float lengthSquared = dot(rotated, rotated);
    float3 normal = lengthSquared > 1.0e-12f ? rotated * rsqrt(lengthSquared)
                                             : float3(0.0f, 0.0f, 1.0f);
    float2 uv = v0.uv * w0 + v1.uv * w1 + v2.uv * w2;
    resolved[pixel] = float4(normal * 0.5f + 0.5f, uv.x);
}
