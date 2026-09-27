#include "renderer.h"

#include <cy/backends/rhi/access.h>
#include <cy/core/math/projection.h>
#include <cy/rendering/material/material.h>

#include "shaders/first_light_dxil.h"
#include "shaders/first_light_msl.h"
#include "shaders/first_light_spirv.h"

#include <cmath>
#include <cstring>

namespace cy::sample::first_light {
namespace {

using rendering::PassContext;
using rendering::ResourceId;
using rhi::Access;
using rhi::QueueKind;

/// The per-frame uniform block, matching `FrameConstants` in first_light.slang field for field.
/// Eleven `float4`s and nothing else: a block with a scalar in it would acquire a padding rule that
/// the two sides could disagree about, and a shader constant block is not the place to save
/// sixteen bytes.
struct FrameConstants {
    f32 view_projection[4][4] = {};
    f32 light_view_projection[4][4] = {};
    f32 sun_direction_and_bias[4] = {};
    f32 sun_color_and_ambient[4] = {};
    f32 shadow_control[4] = {};
};

static_assert(sizeof(FrameConstants) == 176,
              "first_light.slang's FrameConstants is eleven float4s");

/// The per-object push block, matching `ObjectPush`.
struct ObjectPush {
    f32 model[3][4] = {};
    f32 base_color[4] = {};
};

static_assert(sizeof(ObjectPush) <= rhi::kMaxPushConstantBytes,
              "the push block must fit the portability limit every backend guarantees");
static_assert(sizeof(ObjectPush) == kObjectPushBytes,
              "renderer.h publishes this size and the XR late-latch check asserts on it");

/// Write a `cy::Mat4` into four rows.
///
/// `Mat4` is COLUMN-major (`columns[4]`), so row `i` is the `i`th component of each column. Writing
/// it out here rather than memcpy'ing sixty-four bytes is what keeps the transposition explicit at
/// the one place it happens — the same decision, for the same reason, as
/// tests/render/shaders/conventions.slang.
void write_rows(f32 out[4][4], const Mat4& matrix) noexcept {
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            out[row][column] = matrix.columns[column][row];
        }
    }
}

}  // namespace

/// What the record callbacks need. A struct rather than captures, because `RecordFn` is a plain
/// function pointer: the engine has no exceptions and no per-frame allocation on this path.
struct Renderer::PassState {
    rendering::GraphExecutor* executor = nullptr;
    rhi::PipelineLayoutHandle layout;
    rhi::PipelineLayoutHandle material_layout;
    rhi::GraphicsPipelineHandle shadow_pipeline;
    rhi::GraphicsPipelineHandle forward_pipeline;
    rhi::DescriptorSetHandle descriptor_set;
    rhi::DescriptorSetHandle global_textures;
    const MaterialState* materials = nullptr;
    rhi::BufferHandle vertices;
    rhi::BufferHandle indices;
    rhi::BufferHandle checker_staging;
    rhi::BufferHandle readback;

    ResourceId albedo = rendering::kInvalidResource;
    ResourceId shadow = rendering::kInvalidResource;
    ResourceId color = rendering::kInvalidResource;
    ResourceId depth = rendering::kInvalidResource;

    const Object* objects = nullptr;
    const ObjectPush* pushes = nullptr;
    u32 object_count = 0;
    u32 width = 0;
    u32 height = 0;
    u32 draws = 0;
    u32 triangles = 0;
};

struct Renderer::MaterialState {
    struct Program {
        u64 artefact = 0;
        rhi::ShaderModuleHandle vertex;
        rhi::ShaderModuleHandle shadow_vertex;
        rhi::ShaderModuleHandle fragment;
        rhi::GraphicsPipelineHandle pipeline;
        rhi::GraphicsPipelineHandle shadow_pipeline;
        rhi::BufferHandle parameters;
        rhi::DescriptorSetHandle descriptor_set;
    };

    explicit MaterialState(Allocator& allocator) noexcept
        : programs(allocator), object_artefacts(allocator) {}

    [[nodiscard]] const Program* find(u64 artefact) const noexcept {
        for (const Program& program : programs) {
            if (program.artefact == artefact) {
                return &program;
            }
        }
        return nullptr;
    }

    [[nodiscard]] Program* find(u64 artefact) noexcept {
        return const_cast<Program*>(static_cast<const MaterialState*>(this)->find(artefact));
    }

    Array<Program> programs;
    Array<u64> object_artefacts;
};

namespace {

/// Draw every caster with the same material vertex deformation as the visible pass.
void draw_shadow_objects(const PassContext& context, Renderer::PassState& state) noexcept {
    const u64 offset = 0;
    context.commands->bind_vertex_buffers(0, Span<const rhi::BufferHandle>(&state.vertices, 1),
                                          Span<const u64>(&offset, 1));
    context.commands->bind_index_buffer(state.indices, 0, false);

    rhi::PipelineLayoutHandle layout = state.layout;
    const void* bound = nullptr;
    bool any_bound = false;
    for (u32 index = 0; index < state.object_count; ++index) {
        const u64 artefact = index < state.materials->object_artefacts.size()
                                 ? state.materials->object_artefacts[index]
                                 : 0;
        const auto* material = state.materials->find(artefact);
        if (!any_bound || material != bound) {
            if (material == nullptr) {
                context.commands->bind_graphics_pipeline(state.shadow_pipeline);
                context.commands->bind_descriptor_sets(
                    state.layout, 0,
                    Span<const rhi::DescriptorSetHandle>(&state.descriptor_set, 1));
                layout = state.layout;
            } else {
                context.commands->bind_graphics_pipeline(material->shadow_pipeline);
                const rhi::DescriptorSetHandle sets[] = {
                    state.global_textures, material->descriptor_set, state.descriptor_set};
                context.commands->bind_descriptor_sets(state.material_layout, 0, sets);
                layout = state.material_layout;
            }
            bound = material;
            any_bound = true;
        }
        const ObjectPush& push = state.pushes[index];
        context.commands->push_constants(
            layout, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
            Span<const u8>(reinterpret_cast<const u8*>(&push), sizeof(ObjectPush)));
        context.commands->draw_indexed(state.objects[index].index_count, 1,
                                       state.objects[index].first_index, 0, 0);
        ++state.draws;
        state.triangles += state.objects[index].index_count / 3U;
    }
}

/// Bind the pipeline and descriptor sets one object draws with, and answer the layout they belong
/// to. No `material_set` is the sample's own forward pipeline; one is a compiled material's
/// program, which also takes the global texture table.
rhi::PipelineLayoutHandle bind_forward_material(
    const PassContext& context, const Renderer::PassState& state,
    rhi::GraphicsPipelineHandle pipeline, const rhi::DescriptorSetHandle* material_set) noexcept {
    context.commands->bind_graphics_pipeline(pipeline);
    if (material_set == nullptr) {
        context.commands->bind_descriptor_sets(
            state.layout, 0, Span<const rhi::DescriptorSetHandle>(&state.descriptor_set, 1));
        return state.layout;
    }
    const rhi::DescriptorSetHandle sets[] = {state.global_textures, *material_set,
                                             state.descriptor_set};
    context.commands->bind_descriptor_sets(state.material_layout, 0, sets);
    return state.material_layout;
}

void draw_forward_objects(const PassContext& context, Renderer::PassState& state) noexcept {
    const u64 offset = 0;
    context.commands->bind_vertex_buffers(0, Span<const rhi::BufferHandle>(&state.vertices, 1),
                                          Span<const u64>(&offset, 1));
    context.commands->bind_index_buffer(state.indices, 0, false);

    // BIND ON A CHANGE OF MATERIAL, NOT PER OBJECT. The view travels in the descriptor-bound
    // uniform buffer and reaches every draw through memory, which is the late-latch seam
    // `render.xr_prerequisites` measures as one descriptor bind per pass. Rebinding per object
    // (as this function did from af2c5e1) records nine binds for the default scene's forward and
    // shadow passes and turned that check red; objects that share a material share one bind.
    rhi::PipelineLayoutHandle layout = state.layout;
    const void* bound = nullptr;
    bool any_bound = false;
    for (u32 index = 0; index < state.object_count; ++index) {
        const u64 artefact = index < state.materials->object_artefacts.size()
                                 ? state.materials->object_artefacts[index]
                                 : 0;
        const auto* material = state.materials->find(artefact);
        if (!any_bound || material != bound) {
            layout = material == nullptr
                         ? bind_forward_material(context, state, state.forward_pipeline, nullptr)
                         : bind_forward_material(context, state, material->pipeline,
                                                 &material->descriptor_set);
            bound = material;
            any_bound = true;
        }
        const ObjectPush& push = state.pushes[index];
        context.commands->push_constants(
            layout, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
            Span<const u8>(reinterpret_cast<const u8*>(&push), sizeof(ObjectPush)));
        context.commands->draw_indexed(state.objects[index].index_count, 1,
                                       state.objects[index].first_index, 0, 0);
        ++state.draws;
        state.triangles += state.objects[index].index_count / 3U;
    }
}

/// The checkerboard into the albedo texture. Declared only on the first frame; the graph derives
/// UNDEFINED to TRANSFER_DST here and TRANSFER_DST to SHADER_READ_ONLY before the forward pass,
/// from the two declarations and nothing else.
void record_upload(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<Renderer::PassState*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{kCheckerExtent, kCheckerExtent, 1};
    context.commands->copy_buffer_to_texture(state->checker_staging,
                                             state->executor->texture(state->albedo),
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

void record_shadow(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<Renderer::PassState*>(user);

    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, kShadowMapExtent, kShadowMapExtent};
    info.depth_attachment.view = state->executor->view(state->shadow);
    info.depth_attachment.load = rhi::LoadOp::Clear;
    info.depth_attachment.store = rhi::StoreOp::Store;
    // Reversed-Z clears to ZERO. A shadow map cleared to 1 looks plausible and shadows everything
    // outside the casters, which is the failure that reads as "the shadow bias is wrong".
    info.depth_attachment.clear = rhi::reversed_z_depth_clear();

    context.commands->begin_rendering(info);
    const auto extent = static_cast<f32>(kShadowMapExtent);
    context.commands->set_viewport(rhi::Viewport{0.0F, 0.0F, extent, extent, 0.0F, 1.0F});
    context.commands->set_scissor(rhi::Rect2D{0, 0, kShadowMapExtent, kShadowMapExtent});
    draw_shadow_objects(context, *state);
    context.commands->end_rendering();
}

void record_forward(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<Renderer::PassState*>(user);

    rhi::RenderAttachment color;
    color.view = state->executor->view(state->color);
    color.load = rhi::LoadOp::Clear;
    color.store = rhi::StoreOp::Store;
    // A sky the sample does not shade. `rendering-forward-clustered`'s sky pass is a stage of its
    // own and this is not it; the clear says so rather than pretending.
    color.clear.color[0] = 0.09F;
    color.clear.color[1] = 0.12F;
    color.clear.color[2] = 0.18F;
    color.clear.color[3] = 1.0F;

    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, state->width, state->height};
    info.color_attachments = Span<const rhi::RenderAttachment>(&color, 1);
    info.depth_attachment.view = state->executor->view(state->depth);
    info.depth_attachment.load = rhi::LoadOp::Clear;
    info.depth_attachment.store = rhi::StoreOp::Store;
    info.depth_attachment.clear = rhi::reversed_z_depth_clear();

    context.commands->begin_rendering(info);
    context.commands->set_viewport(rhi::Viewport{0.0F, 0.0F, static_cast<f32>(state->width),
                                                 static_cast<f32>(state->height), 0.0F, 1.0F});
    context.commands->set_scissor(rhi::Rect2D{0, 0, state->width, state->height});
    draw_forward_objects(context, *state);
    context.commands->end_rendering();
}

void record_readback(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<Renderer::PassState*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{state->width, state->height, 1};
    context.commands->copy_texture_to_buffer(state->executor->texture(state->color),
                                             state->readback,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

}  // namespace

Renderer::Renderer(Allocator& allocator, rhi::Device& device) noexcept
    : allocator_(&allocator), device_(&device), readback_(allocator) {
    void* storage = allocator.allocate(sizeof(MaterialState), alignof(MaterialState));
    if (storage != nullptr) {
        materials_ = new (storage) MaterialState(allocator);
    }
}

Renderer::~Renderer() {
    if (device_ == nullptr) {
        return;
    }
    rhi::Device& device = *device_;
    (void)device.wait_idle();
    destroy_material_runtime();
    if (!forward_pipeline_.is_null()) {
        device.destroy_graphics_pipeline(forward_pipeline_);
    }
    if (!shadow_pipeline_.is_null()) {
        device.destroy_graphics_pipeline(shadow_pipeline_);
    }
    if (!pipeline_layout_.is_null()) {
        device.destroy_pipeline_layout(pipeline_layout_);
    }
    if (!set_layout_.is_null()) {
        device.destroy_descriptor_set_layout(set_layout_);
    }
    if (!forward_fragment_.is_null()) {
        device.destroy_shader_module(forward_fragment_);
    }
    if (!forward_vertex_.is_null()) {
        device.destroy_shader_module(forward_vertex_);
    }
    if (!shadow_vertex_.is_null()) {
        device.destroy_shader_module(shadow_vertex_);
    }
    if (!shadow_view_.is_null()) {
        device.destroy_texture_view(shadow_view_);
    }
    if (!albedo_view_.is_null()) {
        device.destroy_texture_view(albedo_view_);
    }
    if (!shadow_map_.is_null()) {
        device.destroy_texture(shadow_map_);
    }
    if (!albedo_.is_null()) {
        device.destroy_texture(albedo_);
    }
    if (!shadow_sampler_.is_null()) {
        device.destroy_sampler(shadow_sampler_);
    }
    if (!albedo_sampler_.is_null()) {
        device.destroy_sampler(albedo_sampler_);
    }
    for (const rhi::BufferHandle buffer :
         {readback_buffer_, checker_staging_, constants_, indices_, vertices_}) {
        if (!buffer.is_null()) {
            device.destroy_buffer(buffer);
        }
    }
}

void Renderer::destroy_material_runtime() noexcept {
    if (materials_ == nullptr) {
        return;
    }
    for (const MaterialState::Program& program : materials_->programs) {
        if (!program.pipeline.is_null()) {
            device_->destroy_graphics_pipeline(program.pipeline);
        }
        if (!program.shadow_pipeline.is_null()) {
            device_->destroy_graphics_pipeline(program.shadow_pipeline);
        }
        if (!program.fragment.is_null()) {
            device_->destroy_shader_module(program.fragment);
        }
        if (!program.vertex.is_null()) {
            device_->destroy_shader_module(program.vertex);
        }
        if (!program.shadow_vertex.is_null()) {
            device_->destroy_shader_module(program.shadow_vertex);
        }
        if (!program.parameters.is_null()) {
            device_->destroy_buffer(program.parameters);
        }
    }
    if (checker_bindless_ != rhi::kInvalidBindlessIndex) {
        device_->release_bindless_index(checker_bindless_);
    }
    if (!material_pipeline_layout_.is_null()) {
        device_->destroy_pipeline_layout(material_pipeline_layout_);
    }
    if (!material_set_layout_.is_null()) {
        device_->destroy_descriptor_set_layout(material_set_layout_);
    }
    materials_->~MaterialState();
    allocator_->deallocate(materials_, sizeof(MaterialState), alignof(MaterialState));
    materials_ = nullptr;
}

Status Renderer::prepare(const Scene& scene, const RendererOptions& options) noexcept {
    if (materials_ == nullptr) {
        return fail(ErrorCode::OutOfMemory,
                    "first-light: material runtime state could not be allocated");
    }
    options_ = options;
    if (options_.width == 0 || options_.height == 0) {
        return fail(ErrorCode::InvalidArgument, "first-light: the viewport must not be empty");
    }
    if (Status created = create_shaders(); !created) {
        return created;
    }
    if (Status created = create_pipelines(); !created) {
        return created;
    }
    if (Status created = create_resources(scene); !created) {
        return created;
    }
    if (Status sized = materials_->object_artefacts.resize(scene.objects().size()); !sized) {
        return sized;
    }
    return upload_geometry(scene);
}

Status Renderer::create_shaders() noexcept {
    struct Request {
        const char* name;
        rhi::ShaderStage stage;
        const u32* words;
        usize spirv_bytes;
        const char* msl;
        usize msl_bytes;
        const char* msl_entry;
        const u8* dxil;
        usize dxil_bytes;
        rhi::ShaderModuleHandle* out;
    };
    const Request requests[3] = {
        {"first-light shadow vertex", rhi::ShaderStage::Vertex, kFirstLightShadowVertexSpirv,
         sizeof(kFirstLightShadowVertexSpirv), kFirstLightShadowVertexMsl,
         sizeof(kFirstLightShadowVertexMsl) - 1, "shadowVertex", kFirstLightShadowVertexDxil,
         sizeof(kFirstLightShadowVertexDxil), &shadow_vertex_},
        {"first-light forward vertex", rhi::ShaderStage::Vertex, kFirstLightForwardVertexSpirv,
         sizeof(kFirstLightForwardVertexSpirv), kFirstLightForwardVertexMsl,
         sizeof(kFirstLightForwardVertexMsl) - 1, "forwardVertex", kFirstLightForwardVertexDxil,
         sizeof(kFirstLightForwardVertexDxil), &forward_vertex_},
        {"first-light forward fragment", rhi::ShaderStage::Fragment,
         kFirstLightForwardFragmentSpirv, sizeof(kFirstLightForwardFragmentSpirv),
         kFirstLightForwardFragmentMsl, sizeof(kFirstLightForwardFragmentMsl) - 1,
         "forwardFragment", kFirstLightForwardFragmentDxil, sizeof(kFirstLightForwardFragmentDxil),
         &forward_fragment_},
    };
    const rhi::ShaderFormat native_format = device_->capabilities().native_shader_format();
    for (const Request& request : requests) {
        rhi::ShaderModuleDescription description;
        description.name = request.name;
        description.stage = request.stage;
        if (native_format == rhi::ShaderFormat::Msl) {
            description.entry_point = request.msl_entry;
            description.native =
                Span<const u8>(reinterpret_cast<const u8*>(request.msl), request.msl_bytes);
            description.native_format = rhi::ShaderFormat::Msl;
        } else if (native_format == rhi::ShaderFormat::Dxil) {
            description.entry_point = request.name;
            description.native = Span<const u8>(request.dxil, request.dxil_bytes);
            description.native_format = rhi::ShaderFormat::Dxil;
        } else {
            description.entry_point = "main";
            description.spirv = Span<const u32>(request.words, request.spirv_bytes / sizeof(u32));
        }
        Expected<rhi::ShaderModuleHandle, Error> module =
            device_->create_shader_module(description);
        if (!module.has_value()) {
            return make_unexpected(module.error());
        }
        *request.out = *module;
    }
    return ok();
}

Status Renderer::create_pipelines() noexcept {
    // Set 0, the global per-frame set. `shader-system` fixes the convention — 0 global, 1 view,
    // 2 pass, 3 draw — and this sample has one set because it has one view and one material path.
    const rhi::DescriptorBinding bindings[5] = {
        {0, rhi::DescriptorKind::UniformBuffer, 1,
         rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, false},
        {1, rhi::DescriptorKind::SampledTexture, 1, rhi::ShaderStage::Fragment, false},
        {2, rhi::DescriptorKind::Sampler, 1, rhi::ShaderStage::Fragment, false},
        {3, rhi::DescriptorKind::SampledTexture, 1, rhi::ShaderStage::Fragment, false},
        {4, rhi::DescriptorKind::Sampler, 1, rhi::ShaderStage::Fragment, false},
    };
    rhi::DescriptorSetLayoutDescription set_layout;
    set_layout.name = "first-light globals";
    set_layout.bindings = Span<const rhi::DescriptorBinding>(bindings, 5);
    Expected<rhi::DescriptorSetLayoutHandle, Error> layout =
        device_->create_descriptor_set_layout(set_layout);
    if (!layout.has_value()) {
        return make_unexpected(layout.error());
    }
    set_layout_ = *layout;

    const rhi::DescriptorBinding material_binding{0, rhi::DescriptorKind::UniformBuffer, 1,
                                                  rhi::ShaderStage::Fragment, false};
    rhi::DescriptorSetLayoutDescription material_set;
    material_set.name = "first-light compiled material parameters";
    material_set.bindings = {&material_binding, 1};
    auto created_material_set = device_->create_descriptor_set_layout(material_set);
    if (!created_material_set.has_value()) {
        return make_unexpected(created_material_set.error());
    }
    material_set_layout_ = *created_material_set;

    const rhi::PushConstantRange range{rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
                                       sizeof(ObjectPush)};
    rhi::PipelineLayoutDescription pipeline_layout;
    pipeline_layout.name = "first-light layout";
    pipeline_layout.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(&set_layout_, 1);
    pipeline_layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    Expected<rhi::PipelineLayoutHandle, Error> created =
        device_->create_pipeline_layout(pipeline_layout);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    pipeline_layout_ = *created;

    const rhi::DescriptorSetLayoutHandle material_sets[] = {device_->global_texture_table_layout(),
                                                            material_set_layout_, set_layout_};
    if (!material_sets[0].is_null()) {
        rhi::PipelineLayoutDescription material_pipeline_layout;
        material_pipeline_layout.name = "first-light compiled material layout";
        material_pipeline_layout.set_layouts = material_sets;
        material_pipeline_layout.push_constants = {&range, 1};
        auto created_material_layout = device_->create_pipeline_layout(material_pipeline_layout);
        if (!created_material_layout.has_value()) {
            return make_unexpected(created_material_layout.error());
        }
        material_pipeline_layout_ = *created_material_layout;
    }

    const rhi::VertexBinding binding{0, sizeof(Vertex), rhi::VertexInputRate::PerVertex};
    const rhi::VertexAttribute attributes[3] = {
        {0, 0, rhi::Format::Rgb32Sfloat, 0},
        {1, 0, rhi::Format::Rgb32Sfloat, 12},
        {2, 0, rhi::Format::Rg32Sfloat, 24},
    };

    // The shadow pipeline: a vertex stage, a depth attachment, and NO FRAGMENT SHADER. A depth-only
    // pipeline is the ordinary shape for a shadow pass, and it is spelled here as the absence of a
    // handle rather than as a shader that writes nothing.
    rhi::GraphicsPipelineDescription shadow;
    shadow.name = "first-light shadow";
    shadow.layout = pipeline_layout_;
    shadow.vertex_shader = shadow_vertex_;
    shadow.vertex_bindings = Span<const rhi::VertexBinding>(&binding, 1);
    // POSITION ONLY. The stride is the same — the shadow pass reads the same vertex buffer — but
    // declaring the normal and the texture coordinate to a shader that does not read them is a
    // validation performance warning on every pipeline creation, and a warning nobody can fix is a
    // warning everybody learns to scroll past.
    shadow.vertex_attributes = Span<const rhi::VertexAttribute>(attributes, 1);
    // FRONT faces culled, which is the standard trick for removing shadow acne on convex casters:
    // the back faces are the ones the light does not see, so their depth is behind the surface
    // being shaded and the self-shadowing term never fires. The ground is a single-sided quad and
    // would vanish from the shadow map under this rule, which costs nothing — a plane cannot cast a
    // shadow onto itself.
    shadow.rasterisation.cull_mode = rhi::CullMode::Front;
    shadow.depth_stencil.format = rhi::Format::D32Sfloat;
    shadow.depth_stencil.depth_test_enable = true;
    shadow.depth_stencil.depth_write_enable = true;
    Expected<rhi::GraphicsPipelineHandle, Error> shadow_pipeline =
        device_->create_graphics_pipeline(shadow);
    if (!shadow_pipeline.has_value()) {
        return make_unexpected(shadow_pipeline.error());
    }
    shadow_pipeline_ = *shadow_pipeline;

    rhi::ColorAttachmentState color;
    color.format = rhi::Format::Rgba8Unorm;

    rhi::GraphicsPipelineDescription forward;
    forward.name = "first-light forward";
    forward.layout = pipeline_layout_;
    forward.vertex_shader = forward_vertex_;
    forward.fragment_shader = forward_fragment_;
    forward.vertex_bindings = Span<const rhi::VertexBinding>(&binding, 1);
    forward.vertex_attributes = Span<const rhi::VertexAttribute>(attributes, 3);
    forward.color_attachments = Span<const rhi::ColorAttachmentState>(&color, 1);
    forward.rasterisation.cull_mode = rhi::CullMode::Back;
    forward.depth_stencil.format = rhi::Format::D32Sfloat;
    forward.depth_stencil.depth_test_enable = true;
    forward.depth_stencil.depth_write_enable = true;
    // The comparison is left at the default, which is GreaterOrEqual. design.md §3: reversed-Z is a
    // number rather than a convention, and the number is checked by `render.conventions`.
    Expected<rhi::GraphicsPipelineHandle, Error> forward_pipeline =
        device_->create_graphics_pipeline(forward);
    if (!forward_pipeline.has_value()) {
        return make_unexpected(forward_pipeline.error());
    }
    forward_pipeline_ = *forward_pipeline;
    return ok();
}

Status Renderer::create_resources(const Scene& scene) noexcept {
    rhi::BufferDescription vertices;
    vertices.name = "first-light vertices";
    vertices.size = scene.vertices().size() * sizeof(Vertex);
    vertices.usage = rhi::BufferUsage::Vertex;
    // Host-visible rather than device-local with a staging copy. The geometry is written once,
    // before the first submit, and a host write is visible to the queue the submit runs on without
    // anything having to say so — which is why these three buffers are NOT declared to the graph:
    // there is no hazard between the host and the device for it to derive. Everything the DEVICE
    // writes is declared, and that is the line.
    vertices.memory = rhi::MemoryUse::Upload;
    Expected<rhi::BufferHandle, Error> vertex_buffer = device_->create_buffer(vertices);
    if (!vertex_buffer.has_value()) {
        return make_unexpected(vertex_buffer.error());
    }
    vertices_ = *vertex_buffer;

    rhi::BufferDescription indices;
    indices.name = "first-light indices";
    indices.size = scene.indices().size() * sizeof(u16);
    indices.usage = rhi::BufferUsage::Index;
    indices.memory = rhi::MemoryUse::Upload;
    Expected<rhi::BufferHandle, Error> index_buffer = device_->create_buffer(indices);
    if (!index_buffer.has_value()) {
        return make_unexpected(index_buffer.error());
    }
    indices_ = *index_buffer;
    index_count_ = static_cast<u32>(scene.indices().size());

    rhi::BufferDescription constants;
    constants.name = "first-light frame constants";
    constants.size = sizeof(FrameConstants);
    constants.usage = rhi::BufferUsage::Uniform;
    constants.memory = rhi::MemoryUse::Upload;
    Expected<rhi::BufferHandle, Error> constant_buffer = device_->create_buffer(constants);
    if (!constant_buffer.has_value()) {
        return make_unexpected(constant_buffer.error());
    }
    constants_ = *constant_buffer;

    rhi::BufferDescription staging;
    staging.name = "first-light checker staging";
    staging.size = scene.checker_texels().size() * sizeof(u32);
    staging.usage = rhi::BufferUsage::TransferSource;
    staging.memory = rhi::MemoryUse::Upload;
    Expected<rhi::BufferHandle, Error> staging_buffer = device_->create_buffer(staging);
    if (!staging_buffer.has_value()) {
        return make_unexpected(staging_buffer.error());
    }
    checker_staging_ = *staging_buffer;

    if (options_.readback) {
        rhi::BufferDescription readback;
        readback.name = "first-light colour readback";
        readback.size = static_cast<u64>(options_.width) * options_.height * sizeof(u32);
        readback.usage = rhi::BufferUsage::TransferDestination;
        readback.memory = rhi::MemoryUse::Readback;
        Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(readback);
        if (!buffer.has_value()) {
            return make_unexpected(buffer.error());
        }
        readback_buffer_ = *buffer;
    }

    rhi::TextureDescription albedo;
    albedo.name = "first-light albedo";
    albedo.format = rhi::Format::Rgba8Unorm;
    albedo.extent = rhi::Extent3D{kCheckerExtent, kCheckerExtent, 1};
    albedo.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDestination;
    Expected<rhi::TextureHandle, Error> albedo_texture = device_->create_texture(albedo);
    if (!albedo_texture.has_value()) {
        return make_unexpected(albedo_texture.error());
    }
    albedo_ = *albedo_texture;

    rhi::TextureDescription shadow;
    shadow.name = "first-light shadow map";
    shadow.format = rhi::Format::D32Sfloat;
    shadow.extent = rhi::Extent3D{kShadowMapExtent, kShadowMapExtent, 1};
    shadow.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::DepthStencilAttachment;
    Expected<rhi::TextureHandle, Error> shadow_texture = device_->create_texture(shadow);
    if (!shadow_texture.has_value()) {
        return make_unexpected(shadow_texture.error());
    }
    shadow_map_ = *shadow_texture;

    rhi::TextureViewDescription albedo_view;
    albedo_view.name = "first-light albedo view";
    albedo_view.texture = albedo_;
    Expected<rhi::TextureViewHandle, Error> albedo_created =
        device_->create_texture_view(albedo_view);
    if (!albedo_created.has_value()) {
        return make_unexpected(albedo_created.error());
    }
    albedo_view_ = *albedo_created;

    rhi::TextureViewDescription shadow_view;
    shadow_view.name = "first-light shadow view";
    shadow_view.texture = shadow_map_;
    Expected<rhi::TextureViewHandle, Error> shadow_created =
        device_->create_texture_view(shadow_view);
    if (!shadow_created.has_value()) {
        return make_unexpected(shadow_created.error());
    }
    shadow_view_ = *shadow_created;

    rhi::SamplerDescription albedo_sampler;
    albedo_sampler.name = "first-light albedo sampler";
    albedo_sampler.mipmap_mode = rhi::MipmapMode::Nearest;
    albedo_sampler.max_lod = 0.0F;
    Expected<rhi::SamplerHandle, Error> albedo_sampler_created =
        device_->create_sampler(albedo_sampler);
    if (!albedo_sampler_created.has_value()) {
        return make_unexpected(albedo_sampler_created.error());
    }
    albedo_sampler_ = *albedo_sampler_created;
    checker_bindless_ = device_->bind_texture_globally(albedo_view_, albedo_sampler_);

    rhi::SamplerDescription shadow_sampler;
    shadow_sampler.name = "first-light shadow sampler";
    shadow_sampler.address_u = rhi::AddressMode::ClampToEdge;
    shadow_sampler.address_v = rhi::AddressMode::ClampToEdge;
    shadow_sampler.address_w = rhi::AddressMode::ClampToEdge;
    shadow_sampler.mipmap_mode = rhi::MipmapMode::Nearest;
    shadow_sampler.max_lod = 0.0F;
    // A COMPARISON sampler, comparing GreaterOrEqual — which is reversed-Z again, and the same
    // comparison the depth test uses. `render::SamplerRecord` records the same fact for the render
    // server's own samplers, for the same reason: which samplers compare is not derivable.
    shadow_sampler.compare_enable = true;
    shadow_sampler.compare_op = rhi::CompareOp::GreaterOrEqual;
    Expected<rhi::SamplerHandle, Error> shadow_sampler_created =
        device_->create_sampler(shadow_sampler);
    if (!shadow_sampler_created.has_value()) {
        return make_unexpected(shadow_sampler_created.error());
    }
    shadow_sampler_ = *shadow_sampler_created;

    // Persistent, not per-frame: every object this set names outlives the frame, which is exactly
    // the distinction `Device::allocate_descriptor_set`'s second argument exists to make.
    Expected<rhi::DescriptorSetHandle, Error> set =
        device_->allocate_descriptor_set(set_layout_, false);
    if (!set.has_value()) {
        return make_unexpected(set.error());
    }
    descriptor_set_ = *set;

    rhi::DescriptorWrite writes[5] = {};
    writes[0].binding = 0;
    writes[0].kind = rhi::DescriptorKind::UniformBuffer;
    writes[0].buffer = constants_;
    writes[0].buffer_range = sizeof(FrameConstants);
    writes[1].binding = 1;
    writes[1].kind = rhi::DescriptorKind::SampledTexture;
    writes[1].texture_view = albedo_view_;
    writes[2].binding = 2;
    writes[2].kind = rhi::DescriptorKind::Sampler;
    writes[2].sampler = albedo_sampler_;
    writes[3].binding = 3;
    writes[3].kind = rhi::DescriptorKind::SampledTexture;
    writes[3].texture_view = shadow_view_;
    // The shadow map is sampled in the layout the graph will have transitioned it into by the time
    // the forward pass runs. Naming it here is the one place the two sides have to agree, and it is
    // why the shadow map is a persistent texture rather than a graph transient: a transient's view
    // is created per frame, and a descriptor written once cannot name it.
    // ShaderReadOnly, which is what the access table gives `Access::FragmentSampledRead` — for a
    // depth image as much as for a colour one. Writing DepthStencilReadOnly here instead would name
    // a layout the graph never transitions to and produce a descriptor that disagrees with the
    // image at draw time; `src/backends/rhi/src/access.cpp` is the one place that decides, and this
    // is a reader of it rather than a second opinion.
    writes[3].use = rhi::ImageUse::SampledRead;
    writes[4].binding = 4;
    writes[4].kind = rhi::DescriptorKind::Sampler;
    writes[4].sampler = shadow_sampler_;
    return device_->update_descriptor_set(descriptor_set_,
                                          Span<const rhi::DescriptorWrite>(writes, 5));
}

Status Renderer::upload_geometry(const Scene& scene) noexcept {
    auto* vertex_bytes = static_cast<Vertex*>(device_->buffer_mapped_pointer(vertices_));
    auto* index_bytes = static_cast<u16*>(device_->buffer_mapped_pointer(indices_));
    auto* checker_bytes = static_cast<u32*>(device_->buffer_mapped_pointer(checker_staging_));
    if (vertex_bytes == nullptr || index_bytes == nullptr || checker_bytes == nullptr) {
        return fail(ErrorCode::Internal, "first-light: an upload buffer is not mapped");
    }
    for (usize index = 0; index < scene.vertices().size(); ++index) {
        vertex_bytes[index] = scene.vertices()[index];
    }
    for (usize index = 0; index < scene.indices().size(); ++index) {
        index_bytes[index] = scene.indices()[index];
    }
    for (usize index = 0; index < scene.checker_texels().size(); ++index) {
        checker_bytes[index] = scene.checker_texels()[index];
    }
    return ok();
}

void Renderer::write_frame_constants(const Scene& scene, const Camera& camera) noexcept {
    auto* block = static_cast<FrameConstants*>(device_->buffer_mapped_pointer(constants_));
    if (block == nullptr) {
        return;
    }
    FrameConstants constants;
    write_rows(constants.view_projection, view_projection(camera, options_.width, options_.height));
    write_rows(constants.light_view_projection, sun_view_projection(scene, camera));

    const Sun& sun = scene.sun();
    constants.sun_direction_and_bias[0] = sun.direction.x;
    constants.sun_direction_and_bias[1] = sun.direction.y;
    constants.sun_direction_and_bias[2] = sun.direction.z;
    constants.sun_direction_and_bias[3] = sun.shadow_depth_bias;
    constants.sun_color_and_ambient[0] = sun.color.x;
    constants.sun_color_and_ambient[1] = sun.color.y;
    constants.sun_color_and_ambient[2] = sun.color.z;
    constants.sun_color_and_ambient[3] = sun.ambient;
    constants.shadow_control[0] = scene.description().sun_shadows ? 1.0F : 0.0F;
    constants.shadow_control[1] = sun.shadow_normal_offset;
    constants.shadow_control[2] = time_seconds_;
    *block = constants;
}

Status Renderer::retain_material(u64 artefact, Span<const u8> vertex_msl, const char* vertex_entry,
                                 Span<const u8> fragment_msl, const char* fragment_entry,
                                 Span<const u8> shadow_msl, const char* shadow_entry,
                                 Span<const u8> parameters) noexcept {
    if (artefact == 0 || vertex_msl.empty() || fragment_msl.empty() || shadow_msl.empty() ||
        vertex_entry == nullptr || fragment_entry == nullptr || shadow_entry == nullptr ||
        parameters.size() > rendering::kMaterialBlockBytes) {
        return fail(ErrorCode::InvalidArgument,
                    "first-light: a compiled material description is incomplete");
    }
    if (material_pipeline_layout_.is_null() ||
        device_->capabilities().native_shader_format() != rhi::ShaderFormat::Msl) {
        return fail(ErrorCode::Unsupported,
                    "first-light: live compiled materials require Metal argument buffers");
    }
    if (materials_->find(artefact) != nullptr) {
        return ok();
    }
    if (Status reserved = materials_->programs.reserve(materials_->programs.size() + 1);
        !reserved) {
        return reserved;
    }

    MaterialState::Program program;
    program.artefact = artefact;
    const auto discard = [&] {
        if (!program.parameters.is_null()) {
            device_->destroy_buffer(program.parameters);
        }
        if (!program.shadow_pipeline.is_null()) {
            device_->destroy_graphics_pipeline(program.shadow_pipeline);
        }
        if (!program.pipeline.is_null()) {
            device_->destroy_graphics_pipeline(program.pipeline);
        }
        for (const rhi::ShaderModuleHandle shader :
             {program.shadow_vertex, program.fragment, program.vertex}) {
            if (!shader.is_null()) {
                device_->destroy_shader_module(shader);
            }
        }
    };
    const auto create_shader = [&](const char* name, rhi::ShaderStage stage, Span<const u8> source,
                                   const char* entry, rhi::ShaderModuleHandle& out) -> Status {
        rhi::ShaderModuleDescription description;
        description.name = name;
        description.stage = stage;
        description.entry_point = entry;
        description.native = source;
        description.native_format = rhi::ShaderFormat::Msl;
        auto created = device_->create_shader_module(description);
        if (!created.has_value()) {
            return make_unexpected(created.error());
        }
        out = *created;
        return ok();
    };
    if (Status created = create_shader("editor material vertex", rhi::ShaderStage::Vertex,
                                       vertex_msl, vertex_entry, program.vertex);
        !created) {
        return created;
    }
    if (Status created = create_shader("editor material fragment", rhi::ShaderStage::Fragment,
                                       fragment_msl, fragment_entry, program.fragment);
        !created) {
        discard();
        return created;
    }
    if (Status created = create_shader("editor material shadow vertex", rhi::ShaderStage::Vertex,
                                       shadow_msl, shadow_entry, program.shadow_vertex);
        !created) {
        discard();
        return created;
    }

    const rhi::VertexBinding vertex_binding{0, sizeof(Vertex), rhi::VertexInputRate::PerVertex};
    const rhi::VertexAttribute attributes[4] = {
        {0, 0, rhi::Format::Rgb32Sfloat, 0},
        {1, 0, rhi::Format::Rgb32Sfloat, 12},
        {2, 0, rhi::Format::Rg32Sfloat, 24},
        {3, 0, rhi::Format::Rgba32Sfloat, 32},
    };
    const rhi::ColorAttachmentState color{rhi::Format::Rgba8Unorm};
    rhi::GraphicsPipelineDescription pipeline;
    pipeline.name = "editor compiled material";
    pipeline.layout = material_pipeline_layout_;
    pipeline.vertex_shader = program.vertex;
    pipeline.fragment_shader = program.fragment;
    pipeline.vertex_bindings = {&vertex_binding, 1};
    pipeline.vertex_attributes = attributes;
    pipeline.color_attachments = {&color, 1};
    pipeline.rasterisation.cull_mode = rhi::CullMode::Back;
    pipeline.depth_stencil.format = rhi::Format::D32Sfloat;
    pipeline.depth_stencil.depth_test_enable = true;
    pipeline.depth_stencil.depth_write_enable = true;
    auto created_pipeline = device_->create_graphics_pipeline(pipeline);
    if (!created_pipeline.has_value()) {
        discard();
        return make_unexpected(created_pipeline.error());
    }
    program.pipeline = *created_pipeline;

    rhi::GraphicsPipelineDescription shadow = pipeline;
    shadow.name = "editor compiled material shadow";
    shadow.vertex_shader = program.shadow_vertex;
    shadow.fragment_shader = {};
    shadow.color_attachments = {};
    shadow.rasterisation.cull_mode = rhi::CullMode::Front;
    auto created_shadow = device_->create_graphics_pipeline(shadow);
    if (!created_shadow.has_value()) {
        discard();
        return make_unexpected(created_shadow.error());
    }
    program.shadow_pipeline = *created_shadow;

    rhi::BufferDescription block;
    block.name = "editor material parameter block";
    block.size = rendering::kMaterialBlockBytes;
    block.usage = rhi::BufferUsage::Uniform;
    block.memory = rhi::MemoryUse::Upload;
    auto created_buffer = device_->create_buffer(block);
    if (!created_buffer.has_value()) {
        discard();
        return make_unexpected(created_buffer.error());
    }
    program.parameters = *created_buffer;
    auto created_set = device_->allocate_descriptor_set(material_set_layout_, false);
    if (!created_set.has_value()) {
        discard();
        return make_unexpected(created_set.error());
    }
    program.descriptor_set = *created_set;
    rhi::DescriptorWrite write{};
    write.binding = 0;
    write.kind = rhi::DescriptorKind::UniformBuffer;
    write.buffer = program.parameters;
    write.buffer_range = rendering::kMaterialBlockBytes;
    if (Status updated = device_->update_descriptor_set(program.descriptor_set, {&write, 1});
        !updated) {
        discard();
        return updated;
    }
    auto* mapped = static_cast<u8*>(device_->buffer_mapped_pointer(program.parameters));
    if (mapped == nullptr) {
        discard();
        return fail(ErrorCode::Internal,
                    "first-light: the material parameter buffer is not mapped");
    }
    std::memset(mapped, 0, rendering::kMaterialBlockBytes);
    std::memcpy(mapped, parameters.data(), parameters.size());
    if (Status retained = materials_->programs.push_back(program); !retained) {
        discard();
        return retained;
    }
    return ok();
}

Status Renderer::update_material(u64 artefact, Span<const u8> parameters) noexcept {
    MaterialState::Program* program = materials_->find(artefact);
    if (program == nullptr) {
        return fail(ErrorCode::NotFound,
                    "first-light: the compiled material artefact is not retained");
    }
    if (parameters.size() > rendering::kMaterialBlockBytes) {
        return fail(ErrorCode::OutOfRange,
                    "first-light: the material parameter block exceeds its GPU layout");
    }
    auto* mapped = static_cast<u8*>(device_->buffer_mapped_pointer(program->parameters));
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal,
                    "first-light: the material parameter buffer is not mapped");
    }
    std::memset(mapped, 0, rendering::kMaterialBlockBytes);
    std::memcpy(mapped, parameters.data(), parameters.size());
    return ok();
}

Status Renderer::bind_material(u32 object, u32 material_slot, u64 artefact) noexcept {
    if (material_slot != 0) {
        return fail(ErrorCode::Unsupported,
                    "first-light: this mesh has one material section, at slot zero");
    }
    if (object >= materials_->object_artefacts.size()) {
        return fail(ErrorCode::OutOfRange,
                    "first-light: the material target is not a scene object");
    }
    if (materials_->find(artefact) == nullptr) {
        return fail(ErrorCode::NotFound,
                    "first-light: the compiled material artefact is not retained");
    }
    materials_->object_artefacts[object] = artefact;
    return ok();
}

void Renderer::unbind_material(u32 object, u32 material_slot, u64 artefact) noexcept {
    if (material_slot == 0 && object < materials_->object_artefacts.size() &&
        materials_->object_artefacts[object] == artefact) {
        materials_->object_artefacts[object] = 0;
    }
}

Expected<FrameReport, Error> Renderer::render(const Scene& scene, const Camera& camera) noexcept {
    if (forward_pipeline_.is_null()) {
        return fail(ErrorCode::Unavailable, "first-light: prepare() was not called");
    }
    rhi::Device& device = *device_;
    if (Expected<u32, Error> began = device.begin_frame(); !began.has_value()) {
        return make_unexpected(began.error());
    }
    write_frame_constants(scene, camera);

    // The per-object push blocks, computed here rather than in the record callback: the camera
    // relative subtraction is the interesting part of this sample and it belongs in one place, on
    // the CPU, in f64 — not inside a function that runs while a command buffer is open.
    Array<ObjectPush> pushes(*allocator_);
    if (Status sized = pushes.resize(scene.objects().size()); !sized) {
        return make_unexpected(sized.error());
    }
    for (usize index = 0; index < scene.objects().size(); ++index) {
        const Object& object = scene.objects()[index];
        const f32 cosine = std::cos(object.yaw_radians) * object.scale;
        const f32 sine = std::sin(object.yaw_radians) * object.scale;
        // THE SUBTRACTION. Both operands are f64 world coordinates and the difference is small, so
        // narrowing it to f32 afterwards keeps every bit that matters. Doing it the other way round
        // — narrowing first — is the bug design.md §3 exists to prevent, and `render.golden`'s far
        // case is what would catch it.
        ObjectPush& push = pushes[index];
        push.model[0][0] = cosine;
        push.model[0][2] = sine;
        push.model[0][3] = static_cast<f32>(object.world_position[0] - camera.position[0]);
        push.model[1][1] = object.scale;
        push.model[1][3] = static_cast<f32>(object.world_position[1] - camera.position[1]);
        push.model[2][0] = -sine;
        push.model[2][2] = cosine;
        push.model[2][3] = static_cast<f32>(object.world_position[2] - camera.position[2]);
        push.base_color[0] = object.base_color[0];
        push.base_color[1] = object.base_color[1];
        push.base_color[2] = object.base_color[2];
        push.base_color[3] = 1.0F;
    }

    rendering::RenderGraph graph(*allocator_);
    rendering::GraphExecutor executor(*allocator_, device);

    rendering::TextureRequest albedo_request;
    albedo_request.name = "albedo";
    albedo_request.format = rhi::Format::Rgba8Unorm;
    albedo_request.width = kCheckerExtent;
    albedo_request.height = kCheckerExtent;
    const ResourceId albedo = graph.import_texture(albedo_request, albedo_, albedo_use_);

    rendering::TextureRequest shadow_request;
    shadow_request.name = "sun shadow map";
    shadow_request.format = rhi::Format::D32Sfloat;
    shadow_request.width = kShadowMapExtent;
    shadow_request.height = kShadowMapExtent;
    const ResourceId shadow = graph.import_texture(shadow_request, shadow_map_, shadow_use_);

    rendering::TextureRequest color_request;
    color_request.name = "scene colour";
    color_request.format = rhi::Format::Rgba8Unorm;
    color_request.width = options_.width;
    color_request.height = options_.height;
    const ResourceId color = graph.create_texture(color_request);

    rendering::TextureRequest depth_request;
    depth_request.name = "scene depth";
    depth_request.format = rhi::Format::D32Sfloat;
    depth_request.width = options_.width;
    depth_request.height = options_.height;
    const ResourceId depth = graph.create_texture(depth_request);

    PassState state;
    state.executor = &executor;
    state.layout = pipeline_layout_;
    state.material_layout = material_pipeline_layout_;
    state.shadow_pipeline = shadow_pipeline_;
    state.forward_pipeline = forward_pipeline_;
    state.descriptor_set = descriptor_set_;
    state.global_textures = device_->global_texture_table();
    state.materials = materials_;
    state.vertices = vertices_;
    state.indices = indices_;
    state.checker_staging = checker_staging_;
    state.readback = readback_buffer_;
    state.albedo = albedo;
    state.shadow = shadow;
    state.color = color;
    state.depth = depth;
    state.objects = scene.objects().data();
    state.pushes = pushes.data();
    state.object_count = static_cast<u32>(scene.objects().size());
    state.width = options_.width;
    state.height = options_.height;

    // ---------------------------------------------------------------------------------------
    // THE FRAME. Four declarations, and not one barrier.
    // ---------------------------------------------------------------------------------------
    if (!albedo_uploaded_) {
        graph.add_pass("albedo upload", QueueKind::Graphics)
            .write(albedo, Access::TransferWrite)
            .record(&record_upload, &state);
    }
    graph.add_pass("sun shadow", QueueKind::Graphics)
        .write(shadow, Access::DepthStencilAttachmentWrite)
        .record(&record_shadow, &state);
    graph.add_pass("forward opaque", QueueKind::Graphics)
        .read(shadow, Access::FragmentSampledRead)
        .read(albedo, Access::FragmentSampledRead)
        .write(color, Access::ColorAttachmentWrite)
        .write(depth, Access::DepthStencilAttachmentWrite)
        .record(&record_forward, &state);

    ResourceId readback = rendering::kInvalidResource;
    if (options_.readback) {
        rendering::BufferRequest readback_request;
        readback_request.name = "colour readback";
        readback_request.size = static_cast<u64>(options_.width) * options_.height * sizeof(u32);
        readback_request.extra_usage = rhi::BufferUsage::TransferDestination;
        readback = graph.import_buffer(readback_request, readback_buffer_);
        graph.add_pass("colour readback", QueueKind::Graphics)
            .read(color, Access::TransferRead)
            .write(readback, Access::TransferWrite)
            .record(&record_readback, &state);
        // The host boundary is a dependency like any other: declaring it is what makes the graph
        // emit the transfer-to-host barrier, rather than the sample relying on coherent memory and
        // a fence. (M3's spike, gotcha 6f.)
        graph.add_pass("host read", QueueKind::Graphics)
            .read(readback, Access::HostRead)
            .side_effect();
    }
    if (Status declared = graph.status(); !declared) {
        return make_unexpected(declared.error());
    }

    rendering::CompileOptions compile_options;
    compile_options.enable_aliasing = options_.aliasing;
    Expected<rendering::ExecutionResult, Error> result =
        executor.execute(graph, compile_options, rendering::ExecuteOptions{});
    if (!result.has_value()) {
        return make_unexpected(result.error());
    }
    if (Status idle = device.wait_idle(); !idle) {
        return make_unexpected(idle.error());
    }

    // What the frame left the two persistent textures in, so the next frame's import tells the
    // truth. The graph transitioned them; the renderer only has to remember where they ended up.
    albedo_use_ = rhi::ImageUse::SampledRead;
    shadow_use_ = rhi::ImageUse::SampledRead;
    albedo_uploaded_ = true;

    if (options_.readback) {
        if (Status read = read_back_color(); !read) {
            return make_unexpected(read.error());
        }
    }
    if (Status ended = device.end_frame(); !ended) {
        return make_unexpected(ended.error());
    }
    executor.release();

    FrameReport report;
    report.submits = result->submits;
    report.passes_recorded = result->passes_recorded;
    report.passes_culled = result->passes_culled;
    report.barriers = result->barriers;
    report.barrier_batches = result->barrier_batches;
    report.queue_ownership_transfers = result->queue_ownership_transfers;
    report.transient_bytes = result->transient_bytes;
    report.transient_bytes_without_aliasing = result->transient_bytes_without_aliasing;
    report.plan_hash = result->plan_hash;
    report.draws = state.draws;
    report.triangles = state.triangles;
    return report;
}

Status Renderer::read_back_color() noexcept {
    const usize texels = static_cast<usize>(options_.width) * options_.height;
    if (Status sized = readback_.resize(texels); !sized) {
        return sized;
    }
    const auto* bytes = static_cast<const u32*>(device_->buffer_mapped_pointer(readback_buffer_));
    if (bytes == nullptr) {
        return fail(ErrorCode::Internal, "first-light: the readback buffer is not mapped");
    }
    for (usize index = 0; index < texels; ++index) {
        readback_[index] = bytes[index];
    }
    return ok();
}

}  // namespace cy::sample::first_light
