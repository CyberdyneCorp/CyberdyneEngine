#pragma once
// A skinned instance as one of the frame's draws. Issue #76 stage 3.
//
// `SkinnedScene` says where an instance's vertices are; `pipeline::DrawGeometry` is what the
// frame's geometry lookup answers for a draw. This is the one place the two meet, so a caller's
// lookup cannot get the halves of the double-buffered output the wrong way round.

#include <cy/backends/rhi/handles.h>
#include <cy/core/base/types.h>
#include <cy/rendering/pipeline/frame_recorder.h>
#include <cy/rendering/skinning/skinned_scene.h>

namespace cy::rendering::skinning {

/// The mesh's indices, and where its UVs are in the frame's rigid streams.
struct SkinnedDrawMesh {
    rhi::BufferHandle indices;
    bool wide_indices = false;
    u32 index_count = 0;
    u32 first_index = 0;
    /// The mesh's first vertex in the geometry source's UV streams, which skinning does not touch.
    i32 static_vertex_offset = 0;
};

/// Fill `out` with `instance`'s skinned draw: its current window, last frame's window when the
/// scene has one, and the mesh's indices and UVs. False — the lookup's "nothing to draw" — when the
/// instance was not skinned by the scene's last `declare`.
[[nodiscard]] bool skinned_draw_geometry(const SkinnedScene& scene, SkinnedInstance instance,
                                         const SkinnedDrawMesh& mesh,
                                         pipeline::DrawGeometry& out) noexcept;

}  // namespace cy::rendering::skinning
