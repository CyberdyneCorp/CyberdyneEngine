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

#include "scene.h"
#include "world_view.h"

#include <memory>
#include <string>
#include <vector>

namespace cy::sample::editor_window {

class AuthoredFrame {
public:
    AuthoredFrame(Allocator& allocator, rhi::Device& device) noexcept;
    ~AuthoredFrame();

    AuthoredFrame(const AuthoredFrame&) = delete;
    AuthoredFrame& operator=(const AuthoredFrame&) = delete;

    [[nodiscard]] Status initialize(u32 width, u32 height, const char* project) noexcept;
    [[nodiscard]] Status prepare_world(const scene::serialization::World& world) noexcept;
    [[nodiscard]] Status render(const scene::serialization::World& world,
                                const first_light::Camera& camera) noexcept;
    [[nodiscard]] Span<const u32> pixels() const noexcept { return pixels_.span(); }
    [[nodiscard]] Status publish(const first_light::Camera& camera,
                                 Array<render::GpuInstance>& instances,
                                 Array<render::DrawItem>& draws) const noexcept;
    [[nodiscard]] bool pivot_for(u64 identity, Vec3& pivot) const noexcept;
    [[nodiscard]] first_light::Camera framing(const first_light::Camera& fallback) const noexcept;

private:
    struct Mesh;
    struct Instance;
    struct Readback;

    [[nodiscard]] Status resolve_meshes(const scene::serialization::World& world) noexcept;
    [[nodiscard]] Status load_mesh(const std::string& reference) noexcept;
    [[nodiscard]] Expected<u32, Error> material_slot(const std::string& reference) noexcept;
    [[nodiscard]] Expected<rhi::BindlessIndex, Error> texture_slot(AssetId identity) noexcept;
    [[nodiscard]] Status upload_geometry() noexcept;
    [[nodiscard]] Status create_materials() noexcept;
    [[nodiscard]] Status build_instances(const scene::serialization::World& world,
                                         Vec3 eye) noexcept;
    [[nodiscard]] Status append_instance(const scene::serialization::World& world,
                                         const scene::serialization::WorldNode& node,
                                         const Mat4& matrix, Vec3 eye) noexcept;
    [[nodiscard]] Status capture(u32 slot, const first_light::Camera& camera) noexcept;
    void release_geometry() noexcept;

    static Span<const rendering::DrawSurface> surfaces(const rendering::VisibleInstance& instance,
                                                       void* user) noexcept;
    static bool geometry(const render::DrawItem& item, const rendering::GpuDrawInstance& instance,
                         void* user, rendering::pipeline::DrawGeometry& out) noexcept;
    static void readback(const rendering::PassContext& context, void* user) noexcept;

    Allocator* allocator_;
    rhi::Device* device_;
    std::string project_;
    u32 width_ = 0;
    u32 height_ = 0;
    bool initialized_ = false;

    rendering::assembly::FrameAssembly assembly_;
    rendering::SpatialIndex index_;
    rendering::RenderGraph graph_;
    rendering::MaterialProgram material_program_;
    rendering::pipeline::FramePipelines pipelines_;
    rendering::pipeline::FrameBindings bindings_;
    rendering::pipeline::FrameRecorder recorder_;
    render::RenderServer texture_server_;
    rendering::pipeline::MaterialTextureTable texture_table_;
    Array<rendering::pipeline::InstanceTransform> transforms_;
    Array<render::LightDescription> lights_;
    Array<u32> pixels_;

    std::vector<std::unique_ptr<Mesh>> meshes_;
    std::vector<Instance> instances_;
    std::vector<std::pair<u64, Vec3>> pivots_;
    std::vector<std::pair<std::string, u32>> material_slots_;
    std::vector<std::pair<AssetId, render::TextureHandle>> texture_handles_;
    rhi::BufferHandle positions_;
    rhi::BufferHandle normals_;
    rhi::BufferHandle uvs_;
    rhi::BufferHandle indices_;
    rhi::BufferHandle readback_;
    rhi::TextureHandle output_;
    u32 material_offsets_[4]{};
    u32 base_color_texture_offset_ = rendering::pipeline::kNoMaterialTexture;
};

}  // namespace cy::sample::editor_window
