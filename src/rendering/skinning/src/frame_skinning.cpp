#include <cy/rendering/skinning/frame_skinning.h>

namespace cy::rendering::skinning {

bool skinned_draw_geometry(const SkinnedScene& scene, SkinnedInstance instance,
                           const SkinnedDrawMesh& mesh, pipeline::DrawGeometry& out) noexcept {
    SkinnedOutput skinned;
    if (!scene.output(instance, skinned)) {
        return false;
    }
    out = pipeline::DrawGeometry{};
    out.indices = mesh.indices;
    out.wide_indices = mesh.wide_indices;
    out.index_count = mesh.index_count;
    out.first_index = mesh.first_index;
    out.vertex_offset = static_cast<i32>(skinned.current_vertex);
    out.has_previous_vertices = skinned.has_previous;
    out.previous_vertex_offset = static_cast<i32>(skinned.previous_vertex);
    out.skinned_positions = skinned.positions;
    out.skinned_frames = skinned.frames;
    out.static_vertex_offset = mesh.static_vertex_offset;
    return true;
}

}  // namespace cy::rendering::skinning
