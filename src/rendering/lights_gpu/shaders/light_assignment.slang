// SPDX-License-Identifier: MIT
// The frame's clustered light lists, assigned on the device. `rendering-forward-clustered` —
// "Assignment SHALL run as a compute pass".
//
// ONE WORKGROUP A SCREEN TILE, ONE THREAD A DEPTH SLICE. The tile's threads load the lights 64 at a
// time into shared memory and test each against the TILE's four side planes once; a light whose
// bounding sphere misses the tile's frustum can reach no pixel of it and goes no further. The
// survivors, in ascending light order, are then tested by every slice against its own cluster —
// `assign_clusters`' test, transcribed: the sphere against the cluster's box, and a narrow spot's
// cone against the cluster's bounding sphere — and kept nearest-first up to the cluster's bound,
// a tie keeping the incumbent, the list written in ascending light order. That is
// `src/rendering/forward/src/cluster.cpp`'s `NearestKeptList`, so a cluster that does not overflow
// holds exactly the lights of the host's list that reach its tile, and `render.many_lights` holds
// the two to that.
//
// THE DECAL AND PROBE LISTS ARE THE HOST'S. A decal is an oriented box `assign_clusters` tests on
// the host; its header and its run of indices are copied through untouched, and the light lists
// are written after them, a fixed run of `maxLightsPerCluster` a cluster.
//
// Compiled by shaders/regenerate.py (SPIR-V 1.5 and MSL) into the checked-in headers.

static const uint kGroupSize = 64u;
/// The longest light list a cluster may keep on the device: the per-thread scratch's size.
static const uint kMaxListLength = 64u;

/// `lights_gpu::GpuClusterElement`, 48 bytes.
struct CyClusterElement
{
    float3 viewPosition;
    float radius;
    float3 viewDirection;
    float coneCos;
    uint payload;
    uint layerMask;
    uint2 reserved;
};

/// `lights_gpu::AssignmentConstants`, word for word.
struct CyAssignConstants
{
    uint4 dimensions;     // x, y, slices, max lights a cluster
    float4 planes;        // near, far, slice scale, slice bias
    float4 view;          // tan(fov_y / 2), aspect, 0, 0
    uint4 counts;         // elements, view layer mask, header stride, light base
    uint4 copies;         // host headers to copy (0 or all), host indices to copy, 0, 0
};

struct CyAssignSet
{
    StructuredBuffer<CyAssignConstants> constants;
    StructuredBuffer<CyClusterElement> elements;
    StructuredBuffer<uint2> hostHeaders;
    StructuredBuffer<uint> hostIndices;
    RWStructuredBuffer<uint2> headers;
    RWStructuredBuffer<uint> indices;
    /// `lights_gpu::AssignmentStatistics`: assignments dropped by a full cluster, assignments
    /// kept, and the longest list. Cleared by the host's pass before this one.
    RWStructuredBuffer<uint> statistics;
};

[[vk::binding(0, 0)]] ParameterBlock<CyAssignSet> cyAssignSet;

groupshared float4 gSphere[kGroupSize];
groupshared float4 gCone[kGroupSize];
groupshared uint gPayload[kGroupSize];
groupshared uint gVisible[kGroupSize];
groupshared uint gCompacted[kGroupSize];
groupshared uint gCompactedCount;

/// `cluster_bounds`: the box containing the tile's frustum between the slice's two depths, in view
/// space. Row 0 is the top of the image.
void clusterBox(CyAssignConstants c, uint x, uint y, uint slice, out float3 boxMin,
                out float3 boxMax)
{
    let scale = c.planes.z != 0.0 ? c.planes.z : 1.0;
    let nearDepth = c.planes.x * exp2(float(slice) / scale);
    let farDepth = c.planes.x * exp2(float(slice + 1u) / scale);
    let tileMinX = (float(x) / float(c.dimensions.x)) * 2.0 - 1.0;
    let tileMaxX = (float(x + 1u) / float(c.dimensions.x)) * 2.0 - 1.0;
    let tileMaxY = 1.0 - (float(y) / float(c.dimensions.y)) * 2.0;
    let tileMinY = 1.0 - (float(y + 1u) / float(c.dimensions.y)) * 2.0;
    let tanX = c.view.x * c.view.y;
    let tanY = c.view.x;
    let minX = min(tileMinX * tanX * nearDepth, tileMinX * tanX * farDepth);
    let maxX = max(tileMaxX * tanX * nearDepth, tileMaxX * tanX * farDepth);
    let minY = min(tileMinY * tanY * nearDepth, tileMinY * tanY * farDepth);
    let maxY = max(tileMaxY * tanY * nearDepth, tileMaxY * tanY * farDepth);
    boxMin = float3(min(minX, maxX), min(minY, maxY), -farDepth);
    boxMax = float3(max(minX, maxX), max(minY, maxY), -nearDepth);
}

/// Whether a sphere reaches the tile's frustum: its four side planes, each through the eye. View
/// space looks down -Z, so a point at depth `d = -z` is inside the x slab when
/// `x0 tan d <= x <= x1 tan d`.
bool sphereInTile(CyAssignConstants c, uint x, uint y, float4 sphere)
{
    let tanX = c.view.x * c.view.y;
    let tanY = c.view.x;
    let x0 = ((float(x) / float(c.dimensions.x)) * 2.0 - 1.0) * tanX;
    let x1 = ((float(x + 1u) / float(c.dimensions.x)) * 2.0 - 1.0) * tanX;
    let y1 = (1.0 - (float(y) / float(c.dimensions.y)) * 2.0) * tanY;
    let y0 = (1.0 - (float(y + 1u) / float(c.dimensions.y)) * 2.0) * tanY;
    let p = sphere.xyz;
    let r = sphere.w;
    let left = float3(1.0, 0.0, x0);
    let right = float3(-1.0, 0.0, -x1);
    let bottom = float3(0.0, 1.0, y0);
    let top = float3(0.0, -1.0, -y1);
    return dot(p, left) >= -r * length(left) && dot(p, right) >= -r * length(right) &&
           dot(p, bottom) >= -r * length(bottom) && dot(p, top) >= -r * length(top);
}

/// `cone_intersects_sphere`.
bool coneReachesSphere(float3 apex, float3 axis, float coneCos, float range, float3 center,
                       float radius)
{
    let offset = center - apex;
    let distance = length(offset);
    if (distance > range + radius)
    {
        return false;
    }
    if (distance <= radius)
    {
        return true;
    }
    let coneSin = sqrt(max(1.0 - coneCos * coneCos, 0.0));
    let along = dot(offset, axis);
    let perpendicular = sqrt(max(distance * distance - along * along, 0.0));
    return coneCos * perpendicular - coneSin * along <= radius;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cyAssignLights(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID,
                    uint3 dispatch : SV_DispatchThreadID)
{
    let c = cyAssignSet.constants[0];
    let tileX = group.x;
    let tileY = group.y;
    let slice = thread.x;
    let slices = c.dimensions.z;
    let capacity = min(c.dimensions.w, kMaxListLength);
    let active = slice < slices && tileX < c.dimensions.x && tileY < c.dimensions.y;
    let stride = c.counts.z;

    // THE HOST'S LISTS, copied through: every index first, spread over the whole dispatch, and
    // each cluster's non-light headers by its own thread.
    let threads = c.dimensions.x * c.dimensions.y * kGroupSize;
    let linear = (group.y * c.dimensions.x + group.x) * kGroupSize + thread.x;
    for (uint index = linear; index < c.copies.y; index += threads)
    {
        cyAssignSet.indices[index] = cyAssignSet.hostIndices[index];
    }

    float3 boxMin = float3(0.0);
    float3 boxMax = float3(0.0);
    uint cluster = 0u;
    if (active)
    {
        clusterBox(c, tileX, tileY, slice, boxMin, boxMax);
        cluster = tileX + c.dimensions.x * (tileY + c.dimensions.y * slice);
        for (uint type = 1u; type < stride; ++type)
        {
            let header = cluster * stride + type;
            cyAssignSet.headers[header] =
                c.copies.x != 0u ? cyAssignSet.hostHeaders[header] : uint2(0u, 0u);
        }
    }
    let center = (boxMin + boxMax) * 0.5;
    let boxRadius = length((boxMax - boxMin) * 0.5);

    uint kept[kMaxListLength];
    float keptDistance[kMaxListLength];
    uint size = 0u;
    uint dropped = 0u;

    for (uint batch = 0u; batch < c.counts.x; batch += kGroupSize)
    {
        let mine = batch + thread.x;
        uint visible = 0u;
        if (mine < c.counts.x)
        {
            let element = cyAssignSet.elements[mine];
            gSphere[thread.x] = float4(element.viewPosition, element.radius);
            gCone[thread.x] = float4(element.viewDirection, element.coneCos);
            gPayload[thread.x] = element.payload;
            // Light channels: one AND, at assignment, and never per pixel.
            visible = (element.layerMask & c.counts.y) != 0u &&
                              sphereInTile(c, tileX, tileY,
                                           float4(element.viewPosition, element.radius))
                          ? 1u
                          : 0u;
        }
        gVisible[thread.x] = visible;
        GroupMemoryBarrierWithGroupSync();
        if (thread.x == 0u)
        {
            uint count = 0u;
            for (uint j = 0u; j < kGroupSize; ++j)
            {
                if (gVisible[j] != 0u)
                {
                    gCompacted[count] = j;
                    count += 1u;
                }
            }
            gCompactedCount = count;
        }
        GroupMemoryBarrierWithGroupSync();
        if (active)
        {
            for (uint k = 0u; k < gCompactedCount; ++k)
            {
                let j = gCompacted[k];
                let sphere = gSphere[j];
                let clamped = clamp(sphere.xyz, boxMin, boxMax);
                let gap = sphere.xyz - clamped;
                if (dot(gap, gap) > sphere.w * sphere.w)
                {
                    continue;
                }
                let cone = gCone[j];
                if (cone.w < 1.0 &&
                    !coneReachesSphere(sphere.xyz, cone.xyz, cone.w, sphere.w, center, boxRadius))
                {
                    continue;
                }
                let offset = sphere.xyz - center;
                let distance = dot(offset, offset);
                if (size < capacity)
                {
                    kept[size] = gPayload[j];
                    keptDistance[size] = distance;
                    size += 1u;
                    continue;
                }
                // Full: one assignment does not survive, the newcomer or the incumbent it displaces.
                dropped += 1u;
                uint furthest = 0u;
                for (uint i = 1u; i < size; ++i)
                {
                    if (keptDistance[i] > keptDistance[furthest])
                    {
                        furthest = i;
                    }
                }
                if (distance < keptDistance[furthest])
                {
                    kept[furthest] = gPayload[j];
                    keptDistance[furthest] = distance;
                }
            }
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (!active)
    {
        return;
    }
    // Ascending light order, so two runs write the same words however the drops fell out.
    for (uint i = 1u; i < size; ++i)
    {
        let value = kept[i];
        uint position = i;
        while (position > 0u && kept[position - 1u] > value)
        {
            kept[position] = kept[position - 1u];
            position -= 1u;
        }
        kept[position] = value;
    }
    let first = c.counts.w + cluster * c.dimensions.w;
    for (uint i = 0u; i < size; ++i)
    {
        cyAssignSet.indices[first + i] = kept[i];
    }
    cyAssignSet.headers[cluster * stride] = uint2(first, size);
    if (dropped != 0u)
    {
        InterlockedAdd(cyAssignSet.statistics[0], dropped);
    }
    if (size != 0u)
    {
        InterlockedAdd(cyAssignSet.statistics[1], size);
        InterlockedMax(cyAssignSet.statistics[2], size);
    }
}
