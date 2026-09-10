// The picture: what the frame drew, projected by the engine's own matrices, plus the interface's
// own primitives and the menu's own instances. M8.b task 12.5.
//
// ================================================================================================
// WHY THIS IS A PROJECTION AND NOT A RASTERISATION, SAID WHERE A READER OF THE PICTURE WILL LOOK
// ================================================================================================
//
// `FrameAssembly` hands a pass's record callback to its CALLER — "it does not own the shaders or
// the pipelines" — and this sample supplies none. Writing them would mean a second renderer beside
// `samples/03-first-light`'s, with its own shaders, its own pipelines and its own drift.
//
// What is committed instead is the frame's own answer, drawn: every shape below is one item of the
// SORTED DRAW LIST the assembly produced, its bounds are the spatial index's, its silhouette is the
// mesh its `MeshRenderer` reference actually resolved to, and its corners are `projection * view`
// applied by `cy::Mat4`. Nothing here re-derives what is on screen; it reads it.
//
// That is the difference from M8.a's photograph, and it is the milestone's: M8.a drew an authored
// sphere as a unit box because the reference reached no renderer. Here the reference resolves to a
// handle, the handle reaches the draw list, and the draw list says which shape.

#include "internals.h"

#include "presentation.h"

#include <cmath>
#include <cstdio>

namespace cy::sample::slice {
namespace {

/// One projected corner, in pixels. Returns false when the corner is behind the camera, which is
/// what makes a shape straddling the near plane get dropped rather than drawn inside out.
[[nodiscard]] bool project(const cy::Mat4& view_projection, Vec3 point, f32 width, f32 height,
                           f32& out_x, f32& out_y, f32& out_w) noexcept {
    const cy::Vec4 clip = view_projection * cy::Vec4{point.x, point.y, point.z, 1.0F};
    if (clip.w <= 1e-3F) {
        return false;
    }
    out_w = clip.w;
    out_x = (((clip.x / clip.w) * 0.5F) + 0.5F) * width;
    out_y = (1.0F - (((clip.y / clip.w) * 0.5F) + 0.5F)) * height;
    return true;
}

}  // namespace

Span<const ShotShape> Slice::shot_shapes() const noexcept {
    return shot_.span();
}

Status Slice::build_shot() noexcept {
    shot_.clear();
    if (presentation_ == nullptr || level_ == nullptr) {
        return cy::ok();
    }
    const cy::rendering::assembly::SceneIndex& index = presentation_->scene_index();
    const cy::Mat4 view_projection = presentation_->projection() * presentation_->view_matrix();
    const f32 width = static_cast<f32>(presentation_->viewport_width());
    const f32 height = static_cast<f32>(presentation_->viewport_height());

    for (const u64 stable_id : presentation_->drawn()) {
        const u32 slot = index.slot_of(stable_id);
        if (slot == cy::rendering::assembly::SceneIndex::kNoSlot) {
            continue;
        }
        const Aabb bounds = index.index().entry(slot).bounds;
        ShotShape shape;
        const cy::rendering::assembly::SceneIndex::Surface surface = index.surface_of(slot);
        shape.kind = kind_of(surface.mesh);
        // THE MATERIAL IS READ BACK OUT OF THE FRAME, not out of a table beside the geometry: this
        // is the handle `bind_render_assets` resolved, published in the snapshot and handed to the
        // draw list by the surface query. A picture coloured from the sample's own arrays would
        // look identical and would prove nothing.
        shape.material = surface.material.index();
        bool visible = true;
        f32 depth = 0.0F;
        for (u32 corner = 0; corner < 8U; ++corner) {
            const Vec3 point{(corner & 1U) != 0U ? bounds.max.x : bounds.min.x,
                             (corner & 2U) != 0U ? bounds.max.y : bounds.min.y,
                             (corner & 4U) != 0U ? bounds.max.z : bounds.min.z};
            f32 w = 0.0F;
            if (!project(view_projection, point, width, height, shape.corners[corner][0],
                         shape.corners[corner][1], w)) {
                visible = false;
                break;
            }
            depth += w;
        }
        if (!visible) {
            continue;
        }
        shape.depth = depth / 8.0F;
        // A cheap lambert against the frame's own sun direction, so the picture reads as a scene
        // rather than as a wireframe. The sun is the one the assembly was handed.
        shape.shade = 0.55F + (0.45F / (1.0F + (shape.depth * 0.02F)));
        if (Status pushed = shot_.push_back(shape); !pushed) {
            return pushed;
        }
    }
    return cy::ok();
}

MeshKind Slice::kind_of(cy::render::MeshHandle mesh) const noexcept {
    // THE SILHOUETTE COMES OUT OF THE FRAME, exactly as the colour above does, and it did not used
    // to. This function used to search the level's own `props` array for the entity and answer the
    // kind the sample had authored — which draws the same picture whatever the renderer resolved,
    // and would have drawn M8.a's sphere-as-a-box correctly while the defect was still there. The
    // chain now runs draw list → spatial slot → `surface_of(slot).mesh`, the handle
    // `bind_render_assets` produced, → the mesh asset that handle indexes → its shape. Found by
    // M8.b's closing gate, which read this file rather than the claim in the README beside it.
    const Level& level = *level_;
    const cy::u32 slot = mesh.index();
    if (mesh.is_null() || slot >= level.meshes.size()) {
        return MeshKind::Count;
    }
    return level.meshes[slot].kind;
}

Status Slice::shot_interface(Array<f32>& rects, Array<u32>& colours) const noexcept {
    rects.clear();
    colours.clear();
    if (presentation_ == nullptr) {
        return cy::ok();
    }
    const f32 scale = presentation_->interface_scale();
    for (const cy::ui::Primitive& primitive : presentation_->primitives()) {
        // Reference units to pixels — see `Presentation::interface_scale`.
        const f32 values[4] = {primitive.bounds.x * scale, primitive.bounds.y * scale,
                               primitive.bounds.width * scale, primitive.bounds.height * scale};
        for (const f32 value : values) {
            if (Status pushed = rects.push_back(value); !pushed) {
                return pushed;
            }
        }
        if (Status pushed = colours.push_back(primitive.colour); !pushed) {
            return pushed;
        }
    }
    return cy::ok();
}

Status Slice::shot_menu(Array<f32>& rects, Array<u32>& colours) const noexcept {
    rects.clear();
    colours.clear();
    if (presentation_ == nullptr) {
        return cy::ok();
    }
    for (const cy::rendering2d::Instance2D& instance : presentation_->menu_instances()) {
        const cy::rendering2d::Rect2D& box = instance.destination;
        const f32 values[4] = {box.x, box.y, box.width, box.height};
        for (const f32 value : values) {
            if (Status pushed = rects.push_back(value); !pushed) {
                return pushed;
            }
        }
        if (Status pushed = colours.push_back(instance.tint); !pushed) {
            return pushed;
        }
    }
    return cy::ok();
}

Status write_shot_data(const Slice& slice, const char* path) noexcept {
    std::FILE* out = std::fopen(path, "wb");
    if (out == nullptr) {
        return cy::fail(cy::ErrorCode::NotFound, "the shot data file could not be opened");
    }
    (void)std::fprintf(out, "viewport %u %u\n", 1600U, 900U);
    // Every number is widened DELIBERATELY: `printf` promotes a float to a double whatever the
    // call site says, and the engine builds with -Werror=double-promotion so that an implicit one
    // is a compile error. Saying it here is the difference between a cast that is meant and a
    // conversion nobody chose.
    for (const ShotShape& shape : slice.shot_shapes()) {
        (void)std::fprintf(out, "shape %s %s %.3f %.4f", mesh_kind_name(shape.kind),
                           material_kind_name(static_cast<MaterialKind>(shape.material)),
                           static_cast<double>(shape.depth), static_cast<double>(shape.shade));
        for (const auto& corner : shape.corners) {
            (void)std::fprintf(out, " %.2f %.2f", static_cast<double>(corner[0]),
                               static_cast<double>(corner[1]));
        }
        (void)std::fprintf(out, "\n");
    }

    Array<f32> rects(slice.allocator());
    Array<u32> colours(slice.allocator());
    if (Status read = slice.shot_interface(rects, colours); !read) {
        (void)std::fclose(out);
        return read;
    }
    for (cy::usize index = 0; index < colours.size(); ++index) {
        (void)std::fprintf(out, "hud %.2f %.2f %.2f %.2f %08x\n",
                           static_cast<double>(rects[index * 4U]),
                           static_cast<double>(rects[(index * 4U) + 1U]),
                           static_cast<double>(rects[(index * 4U) + 2U]),
                           static_cast<double>(rects[(index * 4U) + 3U]), colours[index]);
    }
    if (Status read = slice.shot_menu(rects, colours); !read) {
        (void)std::fclose(out);
        return read;
    }
    for (cy::usize index = 0; index < colours.size(); ++index) {
        (void)std::fprintf(out, "menu %.2f %.2f %.2f %.2f %08x\n",
                           static_cast<double>(rects[index * 4U]),
                           static_cast<double>(rects[(index * 4U) + 1U]),
                           static_cast<double>(rects[(index * 4U) + 2U]),
                           static_cast<double>(rects[(index * 4U) + 3U]), colours[index]);
    }
    (void)std::fclose(out);
    return cy::ok();
}

}  // namespace cy::sample::slice
