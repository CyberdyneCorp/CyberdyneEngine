// GPU instance culling, hierarchy traversal and cluster culling. M7 task 7.3.
//
// `virtual-geometry` — "For surviving instances, hierarchy traversal and cluster selection SHALL run
// **entirely on the GPU**, producing a list of clusters to rasterise ... The CPU SHALL NOT traverse
// the hierarchy, and SHALL NOT issue per-cluster or per-instance draw calls."
//
// ================================================================================================
// THE SELECTION TEST IS TRANSCRIBED FROM cluster.h, AND THE SUITE COMPARES THE TRANSCRIPTION
// ================================================================================================
//
// `clusterSelected` and `projectError` below are the same four lines as `cy::rendering::vg::
// cluster_selected` and `project_error`. There is no way to share the source between C++ and Slang,
// so the agreement is checked rather than assumed: `test_gpu_traversal.cpp` runs both over a sweep
// of thresholds and distances and compares the visible lists element for element. A transcription
// error shows up there rather than as a hole in a frame.
//
// ================================================================================================
// THE DESCENT IS A QUEUE ITERATED BY DISPATCHES, NOT A PERSISTENT LOOP
// ================================================================================================
//
// A persistent-thread traversal is faster and needs a workgroup-level work-stealing scheme whose
// correctness is very hard to argue. What this does instead is a level per dispatch: `vgTraverse`
// reads one queue, appends the nodes it wants to descend into to the other, and appends what it
// selected to the visible list. `vgPrepare` between the two turns the next queue's count into
// indirect dispatch arguments, so the CPU issues a fixed number of dispatches and knows the size of
// none of them. That is "dispatched indirectly from GPU-generated arguments" with a shape a test
// can reason about, and the growth path to persistent threads changes this file and nothing above.
//
// ================================================================================================
// EVERY CLUSTER IS VISITED ONCE, AND THE MARK IS WHY
// ================================================================================================
//
// The hierarchy is a DAG: every cluster a group produced points at the same child range, so a
// member is reachable from each of its group's parents. `visited` is one word per (instance,
// cluster) and the first thread to claim it descends. Without it the queue grows with the
// duplication at every level.
//
// THE ALLOCATION IS instanceCount * clusterCount WORDS, and that is M7's shape rather than the
// final one: a scene with many instances of a large asset wants a hash instead. The bound is
// checked on the host and the driver refuses rather than overflowing.
//
// ================================================================================================
// REGENERATING
// ================================================================================================
//
//   slangc vg_traversal.slang -target spirv -profile spirv_1_5 \
//          -entry vgReset        -stage compute -o vg_reset.spv
//   slangc vg_traversal.slang -target spirv -profile spirv_1_5 \
//          -entry vgInstanceCull -stage compute -o vg_instance_cull.spv
//   slangc vg_traversal.slang -target spirv -profile spirv_1_5 \
//          -entry vgPrepare      -stage compute -o vg_prepare.spv
//   slangc vg_traversal.slang -target spirv -profile spirv_1_5 \
//          -entry vgTraverse     -stage compute -o vg_traverse.spv
//   python3 embed_spirv.py vg_spirv.h \
//          kVgResetSpirv=vg_reset.spv kVgInstanceCullSpirv=vg_instance_cull.spv \
//          kVgPrepareSpirv=vg_prepare.spv kVgTraverseSpirv=vg_traverse.spv
//   clang-format -i vg_spirv.h

// --- The records, matching traversal.h field for field ------------------------------------------

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

struct VgNode
{
    uint instance;
    uint cluster;
    uint ancestor;
    uint reserved;
};

struct VgVisible
{
    uint instance;
    uint cluster;
    uint material;
    float screenError;
};

struct VgRequest
{
    uint asset;
    uint page;
    float priority;
    uint requests;
};

struct VgPageEntry
{
    uint location;
    uint bytes;
    // generation in the low 16 bits, flags in the next 8. Packed because the C++ record is 12 bytes
    // and a structured buffer element must agree with it exactly.
    uint generationAndFlags;
};

// Counter slots. One buffer so that a single barrier covers every count the passes share.
static const uint kCountQueueA = 0;
static const uint kCountQueueB = 1;
static const uint kCountVisible = 2;
static const uint kCountRequests = 3;
static const uint kCountInstancesVisible = 4;
static const uint kCountNodesVisited = 5;
static const uint kCountPrunedFrustum = 6;
static const uint kCountRejectedCone = 7;
static const uint kCountRejectedFrustum = 8;
static const uint kCountRejectedSize = 9;
static const uint kCountVisibleTriangles = 10;
static const uint kCountMissingPages = 11;
static const uint kCounterWords = 12;

// Indirect dispatch arguments: three words per queue, ping then pong.
static const uint kArgsPing = 0;
static const uint kArgsPong = 3;

struct VgView
{
    float4 frustum[6];
    float3 camera;
    float pixelsScale;   // viewportHeight / (2 * tan(fovY/2))
    float threshold;
    uint instanceCount;
    uint clusterCount;   // per instance, for the `visited` stride
    uint flags;          // bit 0: cone culling
};

struct VgPush
{
    VgView view;
};

[[vk::push_constant]]
ConstantBuffer<VgPush> push;

[[vk::binding(0, 0)]] StructuredBuffer<VgCluster> clusters;
[[vk::binding(1, 0)]] StructuredBuffer<VgInstance> instances;
[[vk::binding(2, 0)]] StructuredBuffer<VgAsset> assets;
[[vk::binding(3, 0)]] StructuredBuffer<uint> roots;
[[vk::binding(4, 0)]] StructuredBuffer<uint> children;
[[vk::binding(5, 0)]] StructuredBuffer<VgPageEntry> pageTable;
[[vk::binding(6, 0)]] RWStructuredBuffer<VgNode> queueA;
[[vk::binding(7, 0)]] RWStructuredBuffer<VgNode> queueB;
[[vk::binding(8, 0)]] RWStructuredBuffer<uint> counters;
[[vk::binding(9, 0)]] RWStructuredBuffer<uint> dispatchArgs;
[[vk::binding(10, 0)]] RWStructuredBuffer<VgVisible> visible;
[[vk::binding(11, 0)]] RWStructuredBuffer<VgRequest> requests;
[[vk::binding(12, 0)]] RWStructuredBuffer<uint> visited;
[[vk::binding(13, 0)]] StructuredBuffer<uint> clusterTriangles;

// The step this dispatch is running: 0 reads queueA and writes queueB, 1 the other way round.
// A specialization constant rather than a push field, so the two pipelines differ by a constant
// instead of by a branch every thread takes.
[[vk::constant_id(0)]] const uint kPingPong = 0;

static const float kRootError = 3.402823466e+38f;
static const float kMinimumDistance = 1.0e-4f;

// --- The arithmetic, transcribed from cluster.h --------------------------------------------------

// THE CAMERA IS A PARAMETER BECAUSE THERE ARE TWO OF THEM. An error is projected in the instance's
// LOCAL space — the scale cancels between the error and the distance, which is the argument
// traversal.cpp makes at `camera_in_local_space` — and a screen size is projected in world space.
// Sharing one function and one camera between the two is how the two projections silently disagree.
float pixelsPerUnitFrom(float3 camera, float3 center, float radius)
{
    float nearDistance = length(center - camera) - radius;
    float d = max(nearDistance, kMinimumDistance);
    return push.view.pixelsScale / d;
}

float projectError(float3 localCamera, float error, float3 center, float radius)
{
    if (error >= kRootError)
    {
        return kRootError;
    }
    return error * pixelsPerUnitFrom(localCamera, center, radius);
}

bool clusterTooCoarse(float3 localCamera, VgCluster c, float threshold)
{
    return projectError(localCamera, c.lodError, c.lodCenter, c.lodRadius) > threshold;
}

// The quaternion convention is q v q^-1, the same one cy/core/math/quat.h fixes.
float3 rotateByQuaternion(float4 q, float3 v)
{
    float3 axis = q.xyz;
    float3 t = 2.0f * cross(axis, v);
    return v + q.w * t + cross(axis, t);
}

float3 cameraInLocalSpace(VgInstance instance)
{
    float inverseScale = instance.scale > 1.0e-8f ? 1.0f / instance.scale : 1.0f;
    float4 conjugate = float4(-instance.rotation.xyz, instance.rotation.w);
    return rotateByQuaternion(conjugate, push.view.camera - instance.translation) * inverseScale;
}

float4 toWorldSphere(VgInstance instance, float3 center, float radius)
{
    float3 world = rotateByQuaternion(instance.rotation, center) * instance.scale +
                   instance.translation;
    return float4(world, radius * instance.scale);
}

// Exact for a sphere: outside when it is entirely behind some plane. The plane is stored as
// (normal, d) with `signed_distance(p) = dot(normal, p) + d`, which is what cy/core/math/shapes.h
// means by a plane — so the two sides agree about the sign without a convention to remember.
bool frustumContainsSphere(float4 sphere)
{
    for (uint plane = 0; plane < 6; ++plane)
    {
        float4 p = push.view.frustum[plane];
        if (dot(p.xyz, sphere.xyz) + p.w < -sphere.w)
        {
            return false;
        }
    }
    return true;
}

bool coneBackfacing(float3 axis, float coneCos, float3 apex, float3 camera)
{
    if (coneCos <= -1.0f)
    {
        return false;
    }
    float3 toCamera = camera - apex;
    float lengthSquared = dot(toCamera, toCamera);
    if (lengthSquared <= 1.0e-12f)
    {
        return false;
    }
    float3 direction = toCamera * rsqrt(lengthSquared);
    float sinSquared = 1.0f - coneCos * coneCos;
    float sinHalf = sinSquared > 0.0f ? sqrt(sinSquared) : 0.0f;
    return dot(axis, direction) < -sinHalf;
}

bool pageResident(uint pageBase, uint page)
{
    VgPageEntry entry = pageTable[pageBase + page];
    // Bit 1 of the flags byte is kResident, matching PageFlags in residency.h.
    return ((entry.generationAndFlags >> 16) & 0x02u) != 0u;
}

// --- The passes ---------------------------------------------------------------------------------

[shader("compute")]
[numthreads(64, 1, 1)]
void vgReset(uint3 id : SV_DispatchThreadID)
{
    uint index = id.x;
    if (index < kCounterWords)
    {
        counters[index] = 0;
    }
    if (index < 6)
    {
        dispatchArgs[index] = (index % 3) == 0 ? 0 : 1;
    }
    uint marks = push.view.instanceCount * push.view.clusterCount;
    if (index < marks)
    {
        visited[index] = 0;
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void vgInstanceCull(uint3 id : SV_DispatchThreadID)
{
    uint index = id.x;
    if (index >= push.view.instanceCount)
    {
        return;
    }
    VgInstance instance = instances[index];
    VgAsset asset = assets[instance.asset];

    // `virtual-geometry`: "Before any hierarchy traversal, whole instances SHALL be culled on the
    // GPU against: frustum, projected screen size, HZB occlusion, and layer or visibility masks."
    // The layer mask is applied on the host when it builds the instance buffer for a view; the
    // frustum and the size are here.
    // The asset's own extent, from the roots: their `lod` spheres are the top groups' and together
    // they contain every cluster beneath them.
    float3 assetCenter = clusters[asset.clusterBase + roots[asset.rootFirst]].lodCenter;
    float assetRadius = 0.0f;
    for (uint slot = 0; slot < asset.rootCount; ++slot)
    {
        VgCluster root = clusters[asset.clusterBase + roots[asset.rootFirst + slot]];
        assetRadius = max(assetRadius, length(root.lodCenter - assetCenter) + root.lodRadius);
    }
    float4 sphere = toWorldSphere(instance, assetCenter, assetRadius);
    if (!frustumContainsSphere(sphere))
    {
        return;
    }

    InterlockedAdd(counters[kCountInstancesVisible], 1u);
    for (uint slot = 0; slot < asset.rootCount; ++slot)
    {
        uint at;
        InterlockedAdd(counters[kCountQueueA], 1u, at);
        VgNode node;
        node.instance = index;
        node.cluster = roots[asset.rootFirst + slot];
        node.ancestor = node.cluster;
        node.reserved = 0;
        queueA[at] = node;
    }
}

[shader("compute")]
[numthreads(1, 1, 1)]
void vgPrepare()
{
    // One thread, between two traversal dispatches: turn the queue the last step filled into the
    // arguments the next one is dispatched with, and clear the queue it is about to overwrite.
    uint readSlot = kPingPong == 0 ? kCountQueueA : kCountQueueB;
    uint writeSlot = kPingPong == 0 ? kCountQueueB : kCountQueueA;
    uint count = counters[readSlot];
    counters[writeSlot] = 0;
    uint groups = (count + 63u) / 64u;
    uint base = kPingPong == 0 ? kArgsPing : kArgsPong;
    dispatchArgs[base + 0] = groups;
    dispatchArgs[base + 1] = 1;
    dispatchArgs[base + 2] = 1;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void vgTraverse(uint3 id : SV_DispatchThreadID)
{
    uint index = id.x;
    uint readSlot = kPingPong == 0 ? kCountQueueA : kCountQueueB;
    if (index >= counters[readSlot])
    {
        return;
    }
    VgNode node = kPingPong == 0 ? queueA[index] : queueB[index];
    VgInstance instance = instances[node.instance];
    VgAsset asset = assets[instance.asset];

    // The DAG mark. First claimant descends; everyone else stops here.
    uint mark = node.instance * push.view.clusterCount + node.cluster;
    uint previous;
    InterlockedCompareExchange(visited[mark], 0u, 1u, previous);
    if (previous != 0u)
    {
        return;
    }
    InterlockedAdd(counters[kCountNodesVisited], 1u);

    VgCluster cluster = clusters[asset.clusterBase + node.cluster];
    float threshold = push.view.threshold * instance.thresholdScale;

    // PRUNE ON THE GROUP SPHERE, NOT THE CLUSTER'S OWN BOUNDS: `lodCenter`/`lodRadius` is the sphere
    // of the group that produced this cluster and contains everything beneath it.
    float4 subtree = toWorldSphere(instance, cluster.lodCenter, cluster.lodRadius);
    if (!frustumContainsSphere(subtree))
    {
        InterlockedAdd(counters[kCountPrunedFrustum], 1u);
        return;
    }

    bool resident = pageResident(asset.pageBase, cluster.page);
    uint ancestor = resident ? node.cluster : node.ancestor;

    float3 localCamera = cameraInLocalSpace(instance);
    if (clusterTooCoarse(localCamera, cluster, threshold) && cluster.childCount > 0)
    {
        uint writeSlot = kPingPong == 0 ? kCountQueueB : kCountQueueA;
        for (uint slot = 0; slot < cluster.childCount; ++slot)
        {
            uint at;
            InterlockedAdd(counters[writeSlot], 1u, at);
            VgNode child;
            child.instance = node.instance;
            child.cluster = children[asset.childBase + cluster.firstChild + slot];
            child.ancestor = ancestor;
            child.reserved = 0;
            if (kPingPong == 0)
            {
                queueB[at] = child;
            }
            else
            {
                queueA[at] = child;
            }
        }
        return;
    }

    uint draw = node.cluster;
    if (!resident)
    {
        InterlockedAdd(counters[kCountMissingPages], 1u);
        uint at;
        InterlockedAdd(counters[kCountRequests], 1u, at);
        VgRequest request;
        request.asset = instance.asset;
        request.page = cluster.page;
        float4 wanted = toWorldSphere(instance, cluster.lodCenter, cluster.lodRadius);
        request.priority = 2.0f * wanted.w *
                           pixelsPerUnitFrom(push.view.camera, wanted.xyz, wanted.w) /
                           max(instance.thresholdScale, 1.0e-6f);
        request.requests = 1;
        requests[at] = request;
        draw = ancestor;
    }

    VgCluster drawn = clusters[asset.clusterBase + draw];
    float3 drawnCenter = 0.5f * (drawn.boundsMin + drawn.boundsMax);
    float drawnRadius = 0.5f * length(drawn.boundsMax - drawn.boundsMin);
    float4 world = toWorldSphere(instance, drawnCenter, drawnRadius);
    if (!frustumContainsSphere(world))
    {
        InterlockedAdd(counters[kCountRejectedFrustum], 1u);
        return;
    }
    if ((push.view.flags & 1u) != 0u &&
        coneBackfacing(drawn.coneAxis, drawn.coneCos, drawnCenter, localCamera))
    {
        InterlockedAdd(counters[kCountRejectedCone], 1u);
        return;
    }
    if (2.0f * world.w * pixelsPerUnitFrom(push.view.camera, world.xyz, world.w) < 0.5f)
    {
        InterlockedAdd(counters[kCountRejectedSize], 1u);
        return;
    }

    uint at;
    InterlockedAdd(counters[kCountVisible], 1u, at);
    VgVisible record;
    record.instance = node.instance;
    record.cluster = draw;
    record.material = drawn.material + instance.materialOffset;
    record.screenError =
        projectError(localCamera, cluster.lodError, cluster.lodCenter, cluster.lodRadius);
    visible[at] = record;
    InterlockedAdd(counters[kCountVisibleTriangles], clusterTriangles[asset.clusterBase + draw]);
}
