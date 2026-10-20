// SPDX-License-Identifier: MIT
#pragma once
// The editor-window runtime's side of global illumination. Issue #35, the tooling half.
//
// ================================================================================================
// WHO DOES WHAT
// ================================================================================================
//
// The engine's `IlluminationPreview` (src/editor_backend) lights the world with CyberGI, draws its
// debug views and writes its report; `IlluminationService` answers `illumination.report`. This
// file only reads the `.cyworld` the editor has open into an `IlluminationScene` and carries the
// results to the frame:
//
//   * the GI view the editor's viewport asked for (`GizmoIntent::debug_view`), drawn for the
//     frame's camera at a fraction of its resolution and composited by the forward frame
//     (`AuthoredFrame::set_gi_overlay`), so every one of `gi/debug_views.h`'s nineteen views is a
//     view mode of the editor's own viewport;
//   * the GI volumes' boxes over the frame, so a volume the transform gizmo moves and scales is
//     seen where it is.
//
// The GI is kept in step lazily: a document change marks it stale, and the next view or report
// brings it up to date — rebuilt when the geometry changed, applied in place otherwise.
//
// ================================================================================================
// THE AUTHORED NAMES (the document schema the editor's `gi.*` commands write)
// ================================================================================================
//
//   GiSettings  mode, quality, tier (text: the engine's names, lower case), hardware_ray_tracing
//               (bool), budget_ms (float). One per world; absent is the engine's defaults.
//   GiVolume    priority (int), blend_distance, weight (float), sets (int: the
//               `gi::gi_volume_bit` mask), quality, tier (text), probe_density, update_rate,
//               diffuse_intensity, specular_intensity, far_field (float). The box is the node's
//               transform applied to the unit cube about its origin: the translation is its centre
//               and the scale its size, so the transform gizmo places and sizes it.
//   GiObject    class (text: automatic, static, slow-dynamic, fast-dynamic, transient) on a
//               placed object.
//   GiEmission  material (text: the drawn material's reference), mode (text: automatic,
//               cache-only, promote, off).
//
// A MeshRenderer is an object of the GI; one with a RigidBody or a KinematicBody moves. Its
// material is its imported slot 0, else the renderer's material: a cooked standard material gives
// its base colour and emission, anything else the frame's default grey.

#include <cy/core/base/expected.h>
#include <cy/core/math/shapes.h>
#include <cy/core/memory/array.h>
#include <cy/editor/illumination_preview.h>
#include <cy/editor/illumination_service.h>
#include <cy/game_backend/illumination_backend.h>
#include <cy/gameplay/play/session.h>
#include <cy/import/mesh.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/servers/render/model.h>
#include <cy/servers/render/types.h>

#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "overlay.h"

namespace cy::sample::editor_window {

/// The GI the frame is lit by on the device (`AuthoredFrame::set_global_illumination`): the
/// system the editor's `IlluminationPreview` keeps, where its clock stands, and the height its
/// distance-field view slices at.
struct AuthoredGi {
    rendering::gi::IlluminationSystem* system = nullptr;
    /// Bumped when the system is rebuilt: the device half is made again over the new one.
    u64 generation = 0;
    /// The system's frame, which its caches' ages are counted in.
    u64 frame = 0;
    f32 field_slice_height = 0.0F;
    /// Whether the Lit view takes the GI (`lights_frame`). False resolves it only for a GI view,
    /// and the pixels the view leaves clear are the frame as it was.
    bool composite = true;
};

/// One GI volume as the document places it, for the frame's overlay.
struct GiVolumeBox {
    u64 node = 0;
    Aabb bounds{};
};

class AuthoredIllumination final : public editor::IlluminationRuntime {
public:
    AuthoredIllumination(Allocator& allocator, std::string project) noexcept;
    ~AuthoredIllumination() override;

    AuthoredIllumination(const AuthoredIllumination&) = delete;
    AuthoredIllumination& operator=(const AuthoredIllumination&) = delete;

    /// The world the document syncs into. Borrowed; null closes it.
    void bind(const scene::serialization::World* world) noexcept;
    /// The document changed: the next view or report brings the GI up to date.
    void world_changed() noexcept { stale_ = true; }

    /// The view and eye frame `frame` was rendered with, for `illumination.report`'s pick.
    void record_frame(u64 frame, const render::View& view, Vec3 eye);

    editor::IlluminationPreview* preview() noexcept override;
    [[nodiscard]] Expected<Ray, Error> pick_ray(u32 viewport, u64 frame, f32 x,
                                                f32 y) noexcept override;

    /// The GI view `mode` names for a frame of `width` by `height` from `view` at `eye`, drawn at
    /// one `divisor`-th of the resolution and repeated up to the frame's: `width * height` texels,
    /// alpha zero where the view draws nothing. Refused for a mode that is not a GI view.
    [[nodiscard]] Status render_view(render::DebugViewMode mode, const render::View& view, Vec3 eye,
                                     u32 width, u32 height, u32 divisor, Array<u32>& out) noexcept;

    /// Every GI volume's box, from the document.
    [[nodiscard]] Status volumes(std::vector<GiVolumeBox>& out) const noexcept;

    /// Whether the world asks for its frame to be lit by the GI: it holds the "Global
    /// Illumination" settings node, in a mode that admits the dynamic GI into a surface. A world
    /// without the node is drawn as it always was.
    [[nodiscard]] bool lights_frame() noexcept;
    /// One frame of the GI from `eye` — brought up to the document first — and what the device
    /// frame is lit by. False when the world has nothing for the GI to light.
    [[nodiscard]] bool advance(Vec3 eye, AuthoredGi& out) noexcept;
    /// What the device frame would be lit by now, without advancing the GI.
    [[nodiscard]] bool current(AuthoredGi& out) noexcept;
    /// ABI 1.9 for Play: an adapter over the GI the viewport is lit by, every object the GI holds
    /// bound by its Play entity and every material by its authored name, so a project's Swift
    /// game names what the author placed. Null when the world has nothing for the GI to light.
    [[nodiscard]] std::unique_ptr<game_backend::IlluminationAdapter> bind_play(
        const gameplay::PlaySession& play) noexcept;
    /// The GI's sky: a uniform dome of `radiance`, the frame's own flat ambient
    /// (`AuthoredFrame::ambient_radiance`), so the GI that replaces the ambient lights an open
    /// surface as the ambient did. Applied to the running GI in place; nothing is rebuilt.
    void set_sky_radiance(Vec3 radiance) noexcept;

    /// The scene `preview()` last read, for a test.
    [[nodiscard]] const editor::IlluminationScene& scene() const noexcept { return scene_; }
    [[nodiscard]] u32 mesh_failures() const noexcept { return mesh_failures_; }

private:
    struct LoadedMesh {
        import::MeshData data;
    };
    struct Frame {
        u64 identity = 0;
        render::View view;
        Vec3 eye{0.0F, 0.0F, 0.0F};
    };

    [[nodiscard]] Status gather() noexcept;
    [[nodiscard]] const LoadedMesh* mesh(const std::string& reference) noexcept;
    [[nodiscard]] u32 material(const std::string& reference) noexcept;

    Allocator* allocator_;
    std::string project_;
    const scene::serialization::World* world_ = nullptr;
    std::unique_ptr<editor::IlluminationPreview> preview_;
    std::map<std::string, std::unique_ptr<LoadedMesh>> meshes_;
    std::map<std::string, u32> mesh_index_;
    std::map<std::string, u32> material_index_;
    std::vector<editor::IlluminationMesh> scene_meshes_;
    std::vector<editor::IlluminationMaterial> scene_materials_;
    std::vector<editor::IlluminationObject> scene_objects_;
    std::vector<rendering::gi::GiLight> scene_lights_;
    std::vector<rendering::gi::GiVolume> scene_volumes_;
    std::vector<editor::IlluminationEmission> scene_emission_;
    editor::IlluminationScene scene_{};
    std::deque<Frame> frames_;
    u32 mesh_failures_ = 0;
    bool stale_ = true;
    /// Whether the last gather met the world's GI settings node.
    bool has_settings_ = false;
    /// The sky the GI is lit by: the preview's default until the frame names its ambient.
    rendering::gi::SkyTerm sky_{};
};

/// Draw every volume's box as its twelve edges over `canvas`, as `view` at `eye` sees them.
void draw_gi_volumes(const Canvas& canvas, const render::View& view, Vec3 eye,
                     Span<const GiVolumeBox> boxes) noexcept;

}  // namespace cy::sample::editor_window
