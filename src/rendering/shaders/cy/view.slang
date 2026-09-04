// The per-view constants. Part of the engine's Slang standard library.
//
// SET 1, per the descriptor set convention: view matrices, view constants and cluster buffers
// change per view and not per frame — a shadow pass and the main pass share set 0 and differ here.
//
// CAMERA-RELATIVE RENDERING (design.md §3). There is no world-space camera position in this block
// and no world-to-clip matrix: geometry reaches the GPU already relative to the camera, and the
// matrix here takes camera-relative space to clip space. That is the arrangement a scene a million
// units from the origin renders without jitter, and putting it here at the first draw is what stops
// every shader written after this one assuming world space.

module view;

import cy.cluster;

public struct CyViewData
{
    /// Camera-relative space to clip space. Reversed-Z: near maps to 1, far to 0.
    public float4x4 relativeToClip;
    public float4x4 clipToRelative;
    /// The camera's world position, as a double split into high and low halves. Shaders do not use
    /// it; it is here so a debug view can report where the camera actually is.
    public float3 cameraWorldHigh;
    public float nearPlane;
    public float3 cameraWorldLow;
    public float farPlane;
    public float2 renderExtent;
    public float2 inverseRenderExtent;
    public ClusterGrid clusterGrid;
};

[[vk::binding(0, 1)]]
public ConstantBuffer<CyViewData> cyView;
