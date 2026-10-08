// SPDX-License-Identifier: MIT
#pragma once
// The engine frame drawn from the editor's authoritative .cyworld.

#include <cy/backends/rhi/device.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/asset_id.h>
#include <cy/rendering/assembly/frame_assembly.h>
#include <cy/rendering/pipeline/frame_recorder.h>
#include <cy/rendering/pipeline/material_textures.h>
#include <cy/servers/render/server.h>
#include <cy/world/coordinates.h>
#if defined(CY_EDITOR_WINDOW_HAS_VFX)
#    include <cy/rendering/particles/particle_renderer.h>
#    include <cy/vfx/runtime.h>
#endif

#include "scene.h"
#include "world_view.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cy::vfx {
class SimulationWorld;
}

namespace cy::terrain {
struct RegionSnapshot;
}

namespace cy::sample::editor_window {

class WindFieldPreview;

/// The subset of a compiled material the authored frame's standard-material path can represent.
struct GraphColour {
    Vec4 value;
    std::string parameter;
    bool vertex = false;
};

/// Extract the supported surface colour. Vertex roots are allowed only for the scene path that
/// also prepares their compiled pipeline variants.
[[nodiscard]] Expected<GraphColour, Error> graph_diffuse_colour(std::string_view source,
                                                                Allocator& allocator,
                                                                bool allow_vertex = false) noexcept;

struct LightMarker {
    u64 identity = 0;
    render::LightKind kind = render::LightKind::Point;
    Vec3 position;
    Vec3 forward;
};

struct CameraMarker {
    u64 identity = 0;
    Vec3 position;
    Vec3 forward;
};

/// A skinned character the engine evaluated, for the frame to draw: its mesh in the bind pose and
/// this frame's skinning matrices. The animation panel's preview (#29) is one: the matrices are
/// `editor::AnimationPreview::skinning_matrices()`, so the frame draws exactly the pose the engine
/// evaluated.
struct SkinnedPreview {
    /// Names the mesh. A new identity replaces the mesh; the same one keeps it uploaded.
    u64 mesh_identity = 0;
    Span<const Vec3> positions;
    Span<const Vec3> normals;
    /// Four joint indices and four weights per vertex.
    Span<const u16> joints;
    Span<const f32> weights;
    Span<const u32> indices;
    /// One matrix per joint: `model * inverse bind`.
    Span<const Mat4> matrices;
    /// Where the character stands in the world.
    Vec3 position;
};

class AuthoredFrame {
public:
    AuthoredFrame(Allocator& allocator, rhi::Device& device) noexcept;
    ~AuthoredFrame();

    AuthoredFrame(const AuthoredFrame&) = delete;
    AuthoredFrame& operator=(const AuthoredFrame&) = delete;

    [[nodiscard]] Status initialize(u32 width, u32 height, const char* project,
                                    bool temporal = true, bool capture_motion = false) noexcept;
    [[nodiscard]] Status prepare_world(const scene::serialization::World& world) noexcept;
    /// The terrain the editor is authoring, as the engine's terrain module meshed it for
    /// `terrain.evaluate`, drawn at every node carrying `TerrainAuthoring`. Null draws none; a
    /// generation already uploaded costs nothing.
    [[nodiscard]] Status set_terrain(const terrain::RegionSnapshot* snapshot,
                                     u64 generation) noexcept;
    [[nodiscard]] Status preview(std::string_view reference,
                                 std::string_view canonical_graph) noexcept;
    /// Draw `preview` from the next frame on, posed by its matrices; null draws none. Refused on a
    /// device whose skinned pipelines have not run: only Vulkan has drawn them (issue #76).
    [[nodiscard]] Status set_skinned_preview(const SkinnedPreview* preview) noexcept;
    /// Whether this frame can draw a skinned preview at all.
    [[nodiscard]] bool skinned_preview_supported() const noexcept { return skinned_supported_; }
    /// The skinned draws the last frame recorded, over every pass that drew them.
    [[nodiscard]] u32 skinned_draws() const noexcept;
    [[nodiscard]] Status render(const scene::serialization::World& world,
                                const first_light::Camera& camera, bool editor_lighting = true,
                                const vfx::SimulationWorld* preview = nullptr,
                                std::optional<f32> time_seconds = std::nullopt,
                                const vfx::SimulationWorld* scene_vfx = nullptr) noexcept;
#if defined(CY_EDITOR_WINDOW_HAS_VFX)
    [[nodiscard]] const rendering::particles::ParticleReport& vfx_particle_report() const noexcept {
        return vfx_renderer_.report();
    }
    [[nodiscard]] Span<const rendering::particles::ParticleInstance> vfx_records() const noexcept {
        return vfx_records_.span();
    }
#endif
    [[nodiscard]] Span<const u32> pixels() const noexcept { return pixels_.span(); }
    /// Packed RG16 motion values from the prepass when temporal rendering is enabled.
    [[nodiscard]] Span<const u32> motion_texels() const noexcept { return motion_texels_.span(); }
    [[nodiscard]] Status publish(const first_light::Camera& camera,
                                 Array<render::GpuInstance>& instances,
                                 Array<render::DrawItem>& draws) const noexcept;
    [[nodiscard]] bool pivot_for(u64 identity, Vec3& pivot) const noexcept;
    /// Inspect the prior camera-relative placement bound for a graph material on this entity.
    [[nodiscard]] bool previous_material_transform(
        u64 identity, rendering::pipeline::InstanceTransform& out) const noexcept;
    [[nodiscard]] Span<const LightMarker> light_markers() const noexcept {
        return {light_markers_.data(), light_markers_.size()};
    }
    [[nodiscard]] Span<const CameraMarker> camera_markers() const noexcept {
        return {camera_markers_.data(), camera_markers_.size()};
    }
    [[nodiscard]] first_light::Camera framing(const first_light::Camera& fallback) const noexcept;
    [[nodiscard]] bool scene_camera(const scene::serialization::World& world, u64 identity,
                                    first_light::Camera& camera) const noexcept;

private:
    struct Mesh;
    struct Instance;
    struct Skinned;
    struct Readback;
    struct MaterialVariant;

    [[nodiscard]] Status resolve_meshes(const scene::serialization::World& world) noexcept;
    [[nodiscard]] Status load_mesh(const std::string& reference) noexcept;
    [[nodiscard]] Expected<u32, Error> material_slot(const scene::serialization::World& world,
                                                     const scene::serialization::WorldNode& node,
                                                     const std::string& reference) noexcept;
    [[nodiscard]] Expected<u32, Error> graph_material_slot(
        const scene::serialization::World& world, const scene::serialization::WorldNode& node,
        const std::string& reference, const std::string& key, u32 slot, bool new_slot) noexcept;
    [[nodiscard]] Expected<u32, Error> cooked_material_slot(const std::string& reference) noexcept;
    [[nodiscard]] Expected<rhi::BindlessIndex, Error> texture_slot(AssetId identity) noexcept;
    [[nodiscard]] Status upload_geometry() noexcept;
    [[nodiscard]] Status create_materials() noexcept;
    [[nodiscard]] Status create_material_variant_layout() noexcept;
    [[nodiscard]] Status prepare_graph_variant(u32 slot, std::string_view source) noexcept;
    [[nodiscard]] Status ensure_wind_field() noexcept;
    [[nodiscard]] Status bind_graph_variants(u32 frame_slot) noexcept;
    void release_graph_variant(MaterialVariant& variant) noexcept;
    [[nodiscard]] Status build_instances(const scene::serialization::World& world, Vec3 eye,
                                         bool editor_lighting = true) noexcept;
    [[nodiscard]] Status append_instance(const scene::serialization::World& world,
                                         const scene::serialization::WorldNode& node,
                                         const Mat4& matrix, Vec3 eye) noexcept;
    [[nodiscard]] Status update_previous_transform(u64 identity, const Mat4& matrix, Vec3 eye,
                                                   Span<const u32> materials) noexcept;
    [[nodiscard]] Status replace_skinned_mesh(const SkinnedPreview& preview) noexcept;
    [[nodiscard]] Status append_skinned_preview(Vec3 eye) noexcept;
    [[nodiscard]] const Mesh* skinned_mesh() const noexcept;
    [[nodiscard]] Status capture(u32 slot, const first_light::Camera& camera, bool editor_lighting,
                                 std::optional<f32> time_seconds) noexcept;
    void record_motion_capture(Readback& motion) noexcept;
    [[nodiscard]] Status copy_motion_readback(const Readback& motion) noexcept;
#if defined(CY_EDITOR_WINDOW_HAS_VFX)
    [[nodiscard]] Status prepare_vfx(u32 slot, const vfx::SimulationWorld* preview,
                                     const vfx::SimulationWorld* scene_vfx, Vec3 eye) noexcept;
#endif
    void release_geometry() noexcept;
    void sign_shading(const void* data, usize size) noexcept;
    template <class T>
    void sign_shading(const T& value) noexcept {
        sign_shading(&value, sizeof(value));
    }

    static Span<const rendering::DrawSurface> surfaces(const rendering::VisibleInstance& instance,
                                                       void* user) noexcept;
    static bool geometry(const render::DrawItem& item, const rendering::GpuDrawInstance& instance,
                         void* user, rendering::pipeline::DrawGeometry& out) noexcept;
    static bool select_material_pipeline(rendering::pipeline::FramePipelineKind kind,
                                         const render::DrawItem& item,
                                         const rendering::GpuDrawInstance& instance, void* user,
                                         rendering::pipeline::DrawPipelineSelection& out) noexcept;
    static void readback(const rendering::PassContext& context, void* user) noexcept;

    Allocator* allocator_;
    rhi::Device* device_;
    std::string project_;
    u32 width_ = 0;
    u32 height_ = 0;
    bool initialized_ = false;
    std::chrono::steady_clock::time_point time_origin_;
    f32 previous_frame_time_ = 0.0F;
    bool has_frame_time_ = false;
    bool has_previous_frame_ = false;
    bool history_cut_ = true;
    // What the frame shades with, apart from where things are and what time it is: TAA keeps its
    // history across a change of transform or time and must drop it across a change of lighting,
    // material or preview, or the new frame is blended with the old scene's pixels.
    u64 shading_signature_ = 0;
    u64 previous_shading_signature_ = 0;
    Vec3 previous_eye_;

    rendering::assembly::FrameAssembly assembly_;
    rendering::SpatialIndex index_;
    rendering::RenderGraph graph_;
    rendering::MaterialProgram material_program_;
    rendering::pipeline::FramePipelines pipelines_;
    rendering::pipeline::FrameBindings bindings_;
    rendering::pipeline::FrameRecorder recorder_;
#if defined(CY_EDITOR_WINDOW_HAS_VFX)
    rendering::particles::ParticleRenderer vfx_renderer_;
    Array<rendering::particles::ParticleInstance> vfx_records_;
    vfx::PublishReport vfx_published_;
#endif
    render::RenderServer texture_server_;
    rendering::pipeline::MaterialTextureTable texture_table_;
    Array<rendering::pipeline::InstanceTransform> transforms_;
    Array<render::LightDescription> lights_;
    Array<u32> pixels_;
    Array<u32> motion_texels_;

    std::vector<std::unique_ptr<Mesh>> meshes_;
    std::unique_ptr<Skinned> skinned_;
    bool skinned_supported_ = false;
    u64 terrain_generation_ = 0;
    std::vector<Instance> instances_;
    std::vector<std::pair<u64, Mat4>> current_models_;
    std::vector<std::pair<u64, Mat4>> previous_models_;
    std::vector<std::pair<u64, Vec3>> pivots_;
    std::vector<LightMarker> light_markers_;
    std::vector<CameraMarker> camera_markers_;
    std::vector<std::pair<std::string, u32>> material_slots_;
    std::vector<MaterialVariant> material_variants_;
    std::unique_ptr<WindFieldPreview> wind_;
    std::optional<std::pair<std::string, std::string>> preview_graph_;
    world::WorldVec3d field_camera_;
    rhi::BufferHandle wind_buffer_;
    rhi::DescriptorSetLayoutHandle material_set_layout_;
    rhi::PipelineLayoutHandle material_pipeline_layout_;
    std::vector<std::pair<AssetId, render::TextureHandle>> texture_handles_;
    rhi::BufferHandle positions_;
    rhi::BufferHandle normals_;
    rhi::BufferHandle uvs_;
    rhi::BufferHandle indices_;
    rhi::BufferHandle readback_;
    rhi::BufferHandle motion_readback_;
    u32 motion_row_length_ = 0;
    rhi::TextureHandle output_;
    rhi::TextureHandle shadow_color_;
    rhi::TextureHandle shadow_depth_;
    rhi::TextureViewHandle shadow_view_;
    rhi::BindlessIndex shadow_slot_ = rhi::kInvalidBindlessIndex;
    u32 material_offsets_[4]{};
    u32 base_color_texture_offset_ = rendering::pipeline::kNoMaterialTexture;
};

}  // namespace cy::sample::editor_window
