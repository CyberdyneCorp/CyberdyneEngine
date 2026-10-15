// SPDX-License-Identifier: MIT
#pragma once
// The editor's global illumination: the authored world lit by CyberGI, for the lighting panel's GI
// settings, volumes, classes and emitter overrides, the GI debug views in the viewport and the
// diagnostics report. Issue #35, the tooling half.
//
// ================================================================================================
// WHO DOES WHAT
// ================================================================================================
//
// The editor computes no illumination. Its `gi.*` commands write the world's document; the runtime
// host reads that document into an `IlluminationScene` (meshes, materials, lights, GI volumes, the
// world's GI settings, each object's class override and each material's emitter rule) and hands it
// here. This class is the one place that turns it into a `gi::IlluminationSystem`:
//
//   * GEOMETRY decides a rebuild. Cards (`gi::append_mesh_cards`) over every object's triangles in
//     its material's albedo and emission, and each mesh's own signed distance grid
//     (`gi::MeshDistanceField`, built once per mesh) placed by the object's transform. A movable
//     object is registered `Movable`, so the classification derives its class as a game's would.
//   * EVERYTHING ELSE IS APPLIED IN PLACE through the same calls a game makes through ABI 1.9:
//     `set_runtime_settings`, `set_volumes`, `set_object_class` and `set_emitter_rules`. So an
//     author changing a volume or putting a lamp out sees exactly what the game would see, and the
//     editor exercises the run-time path rather than a rebuild that hides it.
//
// The views are `gi::render_gi_debug_view` over a surface buffer this class traces itself: one
// primary ray a pixel through `lightmap_bake::MeshSceneTracer` over the same triangles, so a view
// shows the GI's state on the geometry the author placed. The report is `IlluminationSystem`'s own
// diagnostics — `profile()`, the emitter records, and `explain()` at a picked point.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/gi/debug_views.h>
#include <cy/rendering/gi/mesh_cards.h>
#include <cy/rendering/gi/system.h>
#include <cy/rendering/lightmap_bake/scene.h>

#include <memory>
#include <string>
#include <vector>

namespace cy::editor {

/// A mesh's triangles in its own space. Borrowed for the duration of `sync`.
struct IlluminationMesh {
    /// The host's name for it, which keys the distance grid cache: the same name is the same mesh.
    std::string key;
    Span<const Vec3> positions;
    Span<const u32> indices;
};

/// A material as the GI reads it: a card's albedo and emission.
struct IlluminationMaterial {
    /// The authored reference, which an emitter rule and the report name it by.
    std::string name;
    Vec3 albedo{0.5F, 0.5F, 0.5F};
    Vec3 emission{0.0F, 0.0F, 0.0F};
    f32 roughness = 0.8F;
};

/// One placed object.
struct IlluminationObject {
    /// The node's engine identity: the object's GI id, and the name the report gives it.
    u64 node = 0;
    u32 mesh = 0;
    u32 material = 0;
    Mat4 transform = Mat4::identity();
    bool movable = false;
    /// The class an author set, or none: derived from mobility and size.
    bool has_class = false;
    rendering::gi::IlluminationClass klass = rendering::gi::IlluminationClass::Static;
};

/// A material's emitter rule, by its index in `IlluminationScene::materials`.
struct IlluminationEmission {
    u32 material = 0;
    rendering::gi::EmitterOverride mode = rendering::gi::EmitterOverride::Automatic;
};

/// What the host read from the authored world.
struct IlluminationScene {
    Span<const IlluminationMesh> meshes;
    Span<const IlluminationMaterial> materials;
    Span<const IlluminationObject> objects;
    Span<const rendering::gi::GiLight> lights;
    Span<const rendering::gi::GiVolume> volumes;
    Span<const IlluminationEmission> emission;
    rendering::gi::GiRuntimeSettings settings{};
    rendering::gi::SkyTerm sky{};
};

/// A camera a view is drawn for.
struct IlluminationViewCamera {
    u32 width = 0;
    u32 height = 0;
    Vec3 eye{0.0F, 0.0F, 0.0F};
    /// World to clip, and its inverse: a pixel's ray is the inverse applied to the near and far
    /// planes of its clip-space column.
    Mat4 world_to_clip = Mat4::identity();
    Mat4 clip_to_world = Mat4::identity();
    /// Radiance is drawn as `x / (1 + x)` after this multiplier. Zero is automatic: the multiplier
    /// that draws the view's mean indirect diffuse at a third of white, whatever units the world's
    /// lights are in.
    f32 exposure = 0.0F;
};

/// One emitter as the report gives it.
struct IlluminationEmitterEntry {
    u64 node = 0;
    std::string material;
    rendering::gi::EmitterClass classification = rendering::gi::EmitterClass::CacheOnly;
    rendering::gi::EmitterOverride override_mode = rendering::gi::EmitterOverride::Automatic;
    u32 cards = 0;
    f32 area_m2 = 0.0F;
    f32 power = 0.0F;
};

/// One object's class as the report gives it.
struct IlluminationObjectEntry {
    u64 node = 0;
    rendering::gi::IlluminationClass klass = rendering::gi::IlluminationClass::Static;
    bool overridden = false;
};

/// What the editor's GI diagnostics show: the system's own report, never a second computation.
struct IlluminationReport {
    rendering::gi::GiRuntimeSettings settings{};
    rendering::gi::RadianceSource world_tier = rendering::gi::RadianceSource::None;
    f32 convergence = 0.0F;
    u32 cards = 0;
    u32 probes = 0;
    u32 volumes = 0;
    u32 lights = 0;
    /// Rebuilds since the preview was made, and in-place applications.
    u64 rebuilds = 0;
    u64 applications = 0;
    rendering::gi::GiProfile profile{};
    rendering::gi::EmissiveReport emissive{};
    std::vector<IlluminationEmitterEntry> emitters;
    std::vector<IlluminationObjectEntry> objects;
    /// The explained point, when a pick met the world.
    bool explained = false;
    Vec3 point{0.0F, 0.0F, 0.0F};
    Vec3 normal{0.0F, 1.0F, 0.0F};
    rendering::gi::IlluminationQuery query{};
};

/// The authored world lit by CyberGI for the editor: synced from the document, rebuilt only when
/// its geometry changed, drawn as any of the GI debug views and reported on with `explain()`.
class IlluminationPreview {
public:
    explicit IlluminationPreview(Allocator& allocator) noexcept;
    ~IlluminationPreview();

    IlluminationPreview(const IlluminationPreview&) = delete;
    IlluminationPreview& operator=(const IlluminationPreview&) = delete;

    /// Bring the GI to `scene`: rebuilt when its geometry, materials or lights changed, otherwise
    /// the settings, volumes, classes and emitter rules applied in place. Converges a rebuilt GI
    /// before returning, so the first view of it is settled.
    [[nodiscard]] Status sync(const IlluminationScene& scene) noexcept;

    /// `frames` frames of the GI from `eye`.
    void update(Vec3 eye, u32 frames) noexcept;

    /// One debug view for `camera`, `camera.width * camera.height` `Rgba8Unorm` texels from the
    /// top-left into `out`; alpha zero where the view draws nothing.
    [[nodiscard]] Status render_view(rendering::gi::GiDebugView view,
                                     const IlluminationViewCamera& camera,
                                     Array<u32>& out) noexcept;

    /// The nearest surface along a ray, on the GI's own triangles. False when it meets nothing.
    [[nodiscard]] bool pick(const Ray& ray, Vec3& point, Vec3& normal) const noexcept;

    /// The diagnostics, explaining the surface `pick` (when given) meets.
    [[nodiscard]] IlluminationReport report(const Ray* pick_ray) const noexcept;

    /// Whether a scene has been synced at all.
    [[nodiscard]] bool ready() const noexcept { return ready_; }
    [[nodiscard]] u64 rebuilds() const noexcept { return rebuilds_; }
    [[nodiscard]] u64 applications() const noexcept { return applications_; }
    [[nodiscard]] rendering::gi::IlluminationSystem& system() noexcept { return *system_; }
    [[nodiscard]] const rendering::gi::IlluminationSystem& system() const noexcept {
        return *system_;
    }

private:
    struct Fields;

    [[nodiscard]] Status rebuild(const IlluminationScene& scene) noexcept;
    [[nodiscard]] Status apply(const IlluminationScene& scene) noexcept;
    [[nodiscard]] Status build_tracer(const IlluminationScene& scene) noexcept;
    [[nodiscard]] Status add_objects(const IlluminationScene& scene) noexcept;
    [[nodiscard]] f32 automatic_exposure(
        Span<const rendering::gi::GiViewPixel> pixels) const noexcept;

    Allocator* allocator_;
    std::unique_ptr<rendering::gi::IlluminationSystem> system_;
    std::unique_ptr<rendering::lightmap_bake::MeshSceneTracer> tracer_;
    std::unique_ptr<Fields> fields_;
    std::vector<rendering::gi::GiLight> lights_;
    std::vector<std::string> material_names_;
    std::vector<u64> object_nodes_;
    std::vector<rendering::lightmap_bake::BakeMesh> bake_meshes_;
    std::vector<rendering::lightmap_bake::BakeMaterial> bake_materials_;
    std::vector<rendering::lightmap_bake::BakeInstance> bake_instances_;
    std::vector<Vec3> bake_positions_;
    std::vector<u32> bake_indices_;
    u64 geometry_digest_ = 0;
    u64 frame_ = 0;
    u64 rebuilds_ = 0;
    u64 applications_ = 0;
    Vec3 eye_{0.0F, 0.0F, 0.0F};
    /// The world's bounds at the last rebuild: the distance field view slices it half way up.
    Aabb bounds_{};
    bool ready_ = false;
};

}  // namespace cy::editor
