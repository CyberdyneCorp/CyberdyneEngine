// Cluster lookup for the forward-clustered pipeline. Part of the engine's Slang standard library.
//
// The grid is fixed in screen space with an exponential depth slicing, which is what keeps a
// cluster's depth extent proportional to its distance rather than making the near slices useless.
// REVERSED-Z: the depth buffer is [0,1] cleared to 0 and compared GreaterEqual (design.md §3), so
// the slice is derived from view-space depth rather than from the buffer value.

module cluster;

public struct ClusterGrid
{
    public uint3 dimensions;
    public uint maxLightsPerCluster;
    public float nearPlane;
    public float farPlane;
    public float sliceScale;  // dimensions.z / log(far / near)
    public float sliceBias;   // -dimensions.z * log(near) / log(far / near)
};

/// The depth slice a view-space depth falls in.
public uint clusterSliceOf(ClusterGrid grid, float viewDepth)
{
    let slice = log2(max(viewDepth, grid.nearPlane)) * grid.sliceScale + grid.sliceBias;
    return uint(clamp(slice, 0.0, float(grid.dimensions.z - 1)));
}

/// The cluster a pixel belongs to, from its pixel coordinate and view-space depth.
public uint3 clusterCoordOf(ClusterGrid grid, uint2 pixel, uint2 renderExtent, float viewDepth)
{
    let tile = uint2((float2(pixel) / float2(renderExtent)) * float2(grid.dimensions.xy));
    return uint3(min(tile, grid.dimensions.xy - 1), clusterSliceOf(grid, viewDepth));
}

/// The linear index of a cluster. Row-major over (x, y, slice), which is the order the assignment
/// compute shader writes them in — one ordering, stated once.
public uint clusterIndexOf(ClusterGrid grid, uint3 coord)
{
    return coord.x + grid.dimensions.x * (coord.y + grid.dimensions.y * coord.z);
}
