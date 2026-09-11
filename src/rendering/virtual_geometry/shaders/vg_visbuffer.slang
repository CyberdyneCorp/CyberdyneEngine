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
// A pixel carries the index of the VISIBLE CLUSTER RECORD and the triangle within it, and nothing
// else. That is enough to identify the surface — the record names the instance and the cluster, the
// cluster names its page and its geometry — and the barycentrics are RECOVERED in the resolve by
// projecting the identified triangle and solving for the pixel centre. Storing them would be four
// more bytes per pixel to save an arithmetic the resolve has to do anyway to get a derivative.
//
// ================================================================================================
// THE RASTERISER HERE IS THE COMPUTE ONE, AND THAT IS A STATED SCOPE RATHER THAN A PREFERENCE
// ================================================================================================
//
// `virtual-geometry` says visible clusters "SHALL be rasterised through a **hardware** path by
// default" and that the architecture "SHALL permit a compute rasterisation path for micro-triangles
// ... cluster dispatch, output format, and the visibility buffer SHALL not assume hardware
// rasterisation exclusively". What M7 ships is the compute path: a workgroup per visible cluster,
// a bounding-box scan per triangle, and an atomic depth test into a buffer.
//
// The hardware path is the same visibility buffer written by a vertex and fragment shader over the
// same records, and it lands when virtual geometry is wired into the forward frame's pass order.
// Said here rather than left to be discovered from a missing pipeline: everything downstream of the
// visibility buffer — the classification, the bins, the resolve — is written against the FORMAT and
// does not know which rasteriser filled it.
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
//          -entry vgVisClassify -stage compute -o vg_vis_classify.spv
//   slangc vg_visbuffer.slang -target spirv -profile spirv_1_5 \
//          -entry vgVisScan     -stage compute -o vg_vis_scan.spv
//   slangc vg_visbuffer.slang -target spirv -profile spirv_1_5 \
//          -entry vgVisScatter  -stage compute -o vg_vis_scatter.spv
//   slangc vg_visbuffer.slang -target spirv -profile spirv_1_5 \
//          -entry vgVisResolve  -stage compute -o vg_vis_resolve.spv
//   python3 embed_spirv.py vg_visbuffer_spirv.h \
//          kVgVisClearSpirv=vg_vis_clear.spv kVgVisPrepareSpirv=vg_vis_prepare.spv \
//          kVgVisRasterSpirv=vg_vis_raster.spv \
//          kVgVisClassifySpirv=vg_vis_classify.spv kVgVisScanSpirv=vg_vis_scan.spv \
//          kVgVisScatterSpirv=vg_vis_scatter.spv kVgVisResolveSpirv=vg_vis_resolve.spv
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
    uint reserved;
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
[[vk::binding(7, 0)]] RWStructuredBuffer<uint> depth;
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

// Counter slot 2 is the visible cluster count; see `vg_traversal.slang`.
static const uint kCountVisible = 2;
// visArgs[0..2] dispatch the raster, one group per visible cluster; [3..5] dispatch the resolve,
// one thread per binned pixel.
static const uint kArgsRaster = 0;
static const uint kArgsResolve = 3;

static const uint kNoSurface = 0xFFFFFFFFu;
static const uint kFarDepth = 0xFFFFFFFFu;

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
        depth[pixel] = kFarDepth;
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
                uint previous;
                InterlockedMin(depth[pixel], key, previous);
                if (key < previous)
                {
                    // A pixel whose depth this fragment won.
                    //
                    // KNOWN RACE, wider than coincident surfaces. The atomic settles `depth`, but
                    // the payload write below is a separate, unordered store, so the pair is not
                    // atomic together. Two fragments at DIFFERENT depths both take this branch when
                    // the far one runs its InterlockedMin first: far sees kFarDepth and wins its
                    // compare, near then lowers `depth` and wins its own. Both store, and whichever
                    // store lands last owns the pixel — which can be the FARTHER surface, leaving
                    // `depth` and `visbuffer` disagreeing.
                    //
                    // Measured on this scene at 1280x720: ~110-150 of 921,593 covered pixels (0.015%)
                    // change between two identical runs, and `materials_seen` flips between 4 and 5
                    // because a thin material's last pixels are the contested ones. Coverage and the
                    // visible-cluster count are bit-stable, so this is the payload pairing alone and
                    // not the traversal or the raster bounds.
                    //
                    // The fix is a single 64-bit atomic min over (key << 32 | payloadIndex) so depth
                    // and payload move together; it needs shader int64 atomics, which is why it is
                    // recorded in docs/roadmap/post-m9-tasks.md rather than done inline here.
                    visbuffer[pixel] = uint2(visibleIndex, triangle);
                }
            }
        }
    }
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
    uint material = visible[sample.x].material;
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
    uint material = visible[sample.x].material;
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
    VgVisible record = visible[sample.x];
    VgInstance instance = instances[record.instance];
    VgAsset asset = assets[instance.asset];
    uint clusterIndex = asset.clusterBase + record.cluster;
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
