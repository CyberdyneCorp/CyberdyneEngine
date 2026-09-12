#include "stage.h"

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/pipeline.h>
#include <cy/backends/rhi/validation.h>
#include <cy/core/math/projection.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>

#if defined(CY_SAMPLE_CHARACTER_VULKAN)
#    include <cy/backends/rhi/vulkan/vulkan_backend.h>
#endif

#include "golden.h"
#include "shaders/character_spirv.h"

#include <cstdio>
#include <new>

namespace cy::sample::character {
namespace {

using cy::rendering::PassContext;
using cy::rendering::ResourceId;
using rhi::Access;
using rhi::QueueKind;

/// The ground: a 64-metre square of one-metre tiles, centred on the origin.
///
/// Large enough that the run — the fastest of the four clips, and the one that travels — never
/// reaches its edge in the ten seconds this artefact records. A ground the character could run off
/// would make the picture ambiguous exactly when the claim is at its strongest.
constexpr i32 kGroundHalfTiles = 32;
constexpr f32 kGroundTile = 1.0F;

/// The shading push block, which is `CharacterPush` in shaders/character.slang. Four explicit rows
/// and three float4s; see that file for why the matrix is not a `float4x4`.
struct CharacterPush {
    f32 row0[4] = {1.0F, 0.0F, 0.0F, 0.0F};
    f32 row1[4] = {0.0F, 1.0F, 0.0F, 0.0F};
    f32 row2[4] = {0.0F, 0.0F, 1.0F, 0.0F};
    f32 row3[4] = {0.0F, 0.0F, 0.0F, 1.0F};
    f32 color[4] = {1.0F, 1.0F, 1.0F, 1.0F};
    f32 light[4] = {0.0F, -1.0F, 0.0F, 0.2F};
    f32 eye[4] = {0.0F, 0.0F, 0.0F, 0.0F};
};

static_assert(sizeof(CharacterPush) == 112,
              "the push block must fit the 128-byte portability limit");

void write_row(f32 (&out)[4], Vec4 row) noexcept {
    out[0] = row.x;
    out[1] = row.y;
    out[2] = row.z;
    out[3] = row.w;
}

/// What the draw callback needs. A struct rather than captures, because a record callback is a
/// plain function pointer — the engine has no exceptions and no per-frame allocation on this path.
struct DrawState {
    const cy::rendering::GraphExecutor* executor = nullptr;
    rhi::GraphicsPipelineHandle pipeline;
    rhi::PipelineLayoutHandle layout;
    rhi::BufferHandle skinned;
    rhi::BufferHandle ground;
    rhi::BufferHandle indices;
    ResourceId color = cy::rendering::kInvalidResource;
    ResourceId depth = cy::rendering::kInvalidResource;
    CharacterPush view{};
    u32 width = 0;
    u32 height = 0;
    u32 index_count = 0;
    i32 vertex_offset = 0;
    u32 ground_light_first = 0;
    u32 ground_light_count = 0;
    u32 ground_dark_first = 0;
    u32 ground_dark_count = 0;
};

struct ReadbackState {
    const cy::rendering::GraphExecutor* executor = nullptr;
    ResourceId color = cy::rendering::kInvalidResource;
    rhi::BufferHandle buffer;
    u32 width = 0;
    u32 height = 0;
};

void set_color(CharacterPush& push, f32 r, f32 g, f32 b) noexcept {
    push.color[0] = r;
    push.color[1] = g;
    push.color[2] = b;
}

void record_draw(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<DrawState*>(user);

    rhi::RenderAttachment color;
    color.view = state->executor->view(state->color);
    color.load = rhi::LoadOp::Clear;
    color.store = rhi::StoreOp::Store;
    // A cool, dark sky rather than black, so the character's rim light reads and a viewer can tell
    // a frame that rendered nothing from a frame that was never written.
    color.clear.color[0] = 0.055F;
    color.clear.color[1] = 0.063F;
    color.clear.color[2] = 0.082F;
    color.clear.color[3] = 1.0F;

    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, state->width, state->height};
    info.color_attachments = Span<const rhi::RenderAttachment>(&color, 1);
    info.depth_attachment.view = state->executor->view(state->depth);
    info.depth_attachment.load = rhi::LoadOp::Clear;
    info.depth_attachment.store = rhi::StoreOp::Store;
    // Reversed Z, so the clear is zero and a nearer fragment has a GREATER depth.
    info.depth_attachment.clear = rhi::reversed_z_depth_clear();

    context.commands->begin_rendering(info);
    context.commands->set_viewport(rhi::Viewport{0.0F, 0.0F, static_cast<f32>(state->width),
                                                 static_cast<f32>(state->height), 0.0F, 1.0F});
    context.commands->set_scissor(rhi::Rect2D{0, 0, state->width, state->height});
    context.commands->bind_graphics_pipeline(state->pipeline);

    const u64 offset = 0;
    CharacterPush push = state->view;

    // --- The ground, in two runs so the squares alternate. Unskinned, and drawn first so the
    // character's depth test has something to be in front of.
    context.commands->bind_vertex_buffers(0, Span<const rhi::BufferHandle>(&state->ground, 1),
                                          Span<const u64>(&offset, 1));
    set_color(push, 0.20F, 0.21F, 0.24F);
    context.commands->push_constants(
        state->layout, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&push), sizeof(CharacterPush)));
    context.commands->draw(state->ground_light_count, 1, state->ground_light_first, 0);
    set_color(push, 0.13F, 0.14F, 0.17F);
    context.commands->push_constants(
        state->layout, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&push), sizeof(CharacterPush)));
    context.commands->draw(state->ground_dark_count, 1, state->ground_dark_first, 0);

    // --- THE CHARACTER, FROM THE COMPUTE PASS'S OUTPUT BUFFER. Offset zero and the half selected
    // through `vertex_offset`, because the output is double buffered inside one buffer and a
    // per-draw byte offset is not something a bound stream can carry.
    context.commands->bind_vertex_buffers(0, Span<const rhi::BufferHandle>(&state->skinned, 1),
                                          Span<const u64>(&offset, 1));
    context.commands->bind_index_buffer(state->indices, 0, true);
    set_color(push, 0.78F, 0.60F, 0.42F);
    context.commands->push_constants(
        state->layout, rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&push), sizeof(CharacterPush)));
    context.commands->draw_indexed(state->index_count, 1, 0, state->vertex_offset, 0);
    context.commands->end_rendering();
}

void record_readback(const PassContext& context, void* user) noexcept {
    auto* state = static_cast<ReadbackState*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{state->width, state->height, 1};
    context.commands->copy_texture_to_buffer(state->executor->texture(state->color), state->buffer,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
    }
    std::fprintf(stderr, "vulkan validation %s: %s\n",
                 severity == rhi::ValidationSeverity::Error ? "error" : "warning",
                 message != nullptr ? message : "");
}

[[nodiscard]] Status upload_bytes(rhi::Device& device, rhi::BufferHandle buffer, const void* source,
                                  u64 bytes) noexcept {
    void* mapped = device.buffer_mapped_pointer(buffer);
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal, "a staged buffer is not mapped");
    }
    const auto* from = static_cast<const u8*>(source);
    auto* to = static_cast<u8*>(mapped);
    for (u64 index = 0; index < bytes; ++index) {
        to[index] = from[index];
    }
    return ok();
}

}  // namespace

/// Everything that needs a device, so that the header names none of it and a build without the
/// Vulkan backend still compiles this file.
struct Stage::Device {
    explicit Device() noexcept = default;

    Expected<rhi::Device*, Error> handle = fail(ErrorCode::Unavailable, "not created");
    rhi::BackendSelection selection{};
    u32 validation_errors = 0;

    rhi::ShaderModuleHandle vertex;
    rhi::ShaderModuleHandle fragment;
    rhi::PipelineLayoutHandle layout;
    rhi::GraphicsPipelineHandle pipeline;
    rhi::BufferHandle ground;
    rhi::BufferHandle indices;
    rhi::BufferHandle readback;
    cy::rendering::skinning::SkinPass skin;
};

Stage::Stage(Allocator& allocator) noexcept : allocator_(&allocator), pixels_(allocator) {}

Stage::~Stage() {
    close();
}

const char* Stage::absence() const noexcept {
    if (device_ == nullptr) {
        return "the stage was never opened";
    }
    return device_->selection.reason != nullptr ? device_->selection.reason
                                                : "no reason was reported";
}

Status Stage::open(u32 width, u32 height) noexcept {
    width_ = width;
    height_ = height;
    device_ = new (std::nothrow) Device();
    if (device_ == nullptr) {
        return fail(ErrorCode::OutOfMemory, "the stage did not allocate");
    }
#if defined(CY_SAMPLE_CHARACTER_VULKAN)
    (void)rhi::vulkan::register_vulkan_backend();
#endif
    (void)rhi::null::register_null_backend();

    rhi::DeviceDescription description;
    description.application_name = "cy_sample_animated-character";
    // VALIDATION ON, AND ITS ERRORS REPORTED AS A NUMBER rather than as a log line nobody reads.
    // Synchronisation validation with it, because the whole claim about the barrier between the
    // dispatch and the draw is a synchronisation claim, and an unchecked one is an assertion.
    description.enable_validation = true;
    description.enable_synchronisation_validation = true;
    device_->handle = rhi::create_device(*allocator_, "vulkan", description, device_->selection);
    if (!device_->handle.has_value()) {
        return ok();
    }
    if (device_->handle.value()->capabilities().backend() != rhi::BackendKind::Vulkan) {
        return ok();
    }
    device_->handle.value()->set_validation_callback(&count_validation,
                                                     &device_->validation_errors);
    available_ = true;
    if (Status created = create_pipeline(); !created) {
        return created;
    }
    return create_ground();
}

Status Stage::create_pipeline() noexcept {
    rhi::Device& device = *device_->handle.value();

    rhi::ShaderModuleDescription vertex;
    vertex.name = "animated character vertex";
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.entry_point = "main";
    vertex.spirv =
        Span<const u32>(kCharacterVertexSpirv, sizeof(kCharacterVertexSpirv) / sizeof(u32));
    Expected<rhi::ShaderModuleHandle, Error> vertex_module = device.create_shader_module(vertex);
    if (!vertex_module) {
        return make_unexpected(vertex_module.error());
    }
    device_->vertex = *vertex_module;

    rhi::ShaderModuleDescription fragment;
    fragment.name = "animated character fragment";
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.entry_point = "main";
    fragment.spirv =
        Span<const u32>(kCharacterFragmentSpirv, sizeof(kCharacterFragmentSpirv) / sizeof(u32));
    Expected<rhi::ShaderModuleHandle, Error> fragment_module =
        device.create_shader_module(fragment);
    if (!fragment_module) {
        return make_unexpected(fragment_module.error());
    }
    device_->fragment = *fragment_module;

    const rhi::PushConstantRange range{rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
                                       sizeof(CharacterPush)};
    rhi::PipelineLayoutDescription layout;
    layout.name = "animated character layout";
    layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    Expected<rhi::PipelineLayoutHandle, Error> layout_handle =
        device.create_pipeline_layout(layout);
    if (!layout_handle) {
        return make_unexpected(layout_handle.error());
    }
    device_->layout = *layout_handle;

    // STRIDE 12, `Rgb32Sfloat`. That is `render::VertexStream::Position` unquantised, and it is
    // exactly what the skinning dispatch writes — no repack stands between the compute pass and
    // this vertex fetch, which is the requirement's own "reusable across passes without
    // re-skinning".
    const rhi::VertexBinding binding{0, sizeof(f32) * 3, rhi::VertexInputRate::PerVertex};
    const rhi::VertexAttribute attribute{0, 0, rhi::Format::Rgb32Sfloat, 0};
    rhi::ColorAttachmentState color;
    color.format = rhi::Format::Rgba8Unorm;

    rhi::GraphicsPipelineDescription pipeline;
    pipeline.name = "animated character";
    pipeline.layout = device_->layout;
    pipeline.vertex_shader = device_->vertex;
    pipeline.fragment_shader = device_->fragment;
    pipeline.vertex_bindings = Span<const rhi::VertexBinding>(&binding, 1);
    pipeline.vertex_attributes = Span<const rhi::VertexAttribute>(&attribute, 1);
    pipeline.color_attachments = Span<const rhi::ColorAttachmentState>(&color, 1);
    // NO CULLING. A derived skin turns a few vertices inside out where two limbs are close in the
    // bind pose, and a picture whose claim is "this geometry deforms" must not answer a skinning
    // artefact by deleting the triangle — the fragment stage turns the facet towards the eye
    // instead, so a reversed triangle shades rather than disappears.
    pipeline.rasterisation.cull_mode = rhi::CullMode::None;
    pipeline.depth_stencil.format = rhi::Format::D32Sfloat;
    pipeline.depth_stencil.depth_test_enable = true;
    pipeline.depth_stencil.depth_write_enable = true;
    Expected<rhi::GraphicsPipelineHandle, Error> created =
        device.create_graphics_pipeline(pipeline);
    if (!created) {
        return make_unexpected(created.error());
    }
    device_->pipeline = *created;

    rhi::BufferDescription readback;
    readback.name = "animated character colour readback";
    readback.size = static_cast<u64>(width_) * height_ * sizeof(u32);
    readback.usage = rhi::BufferUsage::TransferDestination;
    readback.memory = rhi::MemoryUse::Readback;
    Expected<rhi::BufferHandle, Error> buffer = device.create_buffer(readback);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    device_->readback = *buffer;
    return pixels_.resize(static_cast<usize>(width_) * height_);
}

Status Stage::create_ground() noexcept {
    // Two runs of triangles, light squares then dark, so that one buffer and two push colours give
    // a chequerboard without a texture, a sampler or a descriptor set.
    Array<Vec3> light(*allocator_);
    Array<Vec3> dark(*allocator_);
    for (i32 z = -kGroundHalfTiles; z < kGroundHalfTiles; ++z) {
        for (i32 x = -kGroundHalfTiles; x < kGroundHalfTiles; ++x) {
            const f32 x0 = static_cast<f32>(x) * kGroundTile;
            const f32 z0 = static_cast<f32>(z) * kGroundTile;
            const f32 x1 = x0 + kGroundTile;
            const f32 z1 = z0 + kGroundTile;
            const Vec3 corners[4] = {Vec3{x0, 0.0F, z0}, Vec3{x1, 0.0F, z0}, Vec3{x1, 0.0F, z1},
                                     Vec3{x0, 0.0F, z1}};
            Array<Vec3>& into = ((x + z) & 1) == 0 ? light : dark;
            const u32 order[6] = {0, 1, 2, 0, 2, 3};
            for (const u32 corner : order) {
                if (Status added = into.push_back(corners[corner]); !added) {
                    return added;
                }
            }
        }
    }

    ground_light_first_ = 0;
    ground_light_count_ = static_cast<u32>(light.size());
    ground_dark_first_ = ground_light_count_;
    ground_dark_count_ = static_cast<u32>(dark.size());

    Array<Vec3> tiles(*allocator_);
    if (Status added = tiles.append(light.span()); !added) {
        return added;
    }
    if (Status added = tiles.append(dark.span()); !added) {
        return added;
    }

    rhi::Device& device = *device_->handle.value();
    rhi::BufferDescription description;
    description.name = "animated character ground";
    description.size = tiles.size() * sizeof(Vec3);
    description.usage = rhi::BufferUsage::Vertex;
    description.memory = rhi::MemoryUse::Upload;
    Expected<rhi::BufferHandle, Error> buffer = device.create_buffer(description);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    device_->ground = *buffer;
    return upload_bytes(device, device_->ground, tiles.data(), description.size);
}

Status Stage::stage_character(const Character& character) noexcept {
    if (!available_) {
        return fail(ErrorCode::Unavailable, "no graphics device answered");
    }
    rhi::Device& device = *device_->handle.value();
    vertex_count_ = static_cast<u32>(character.positions.size());
    index_count_ = static_cast<u32>(character.indices.size());
    // The whole pose world, not one skeleton's worth: `PoseWorld` reserves TWO frames of matrices
    // per instance and `matrix_offset` alternates between them, so a pass sized for one frame's
    // bones would refuse every other frame.
    bone_count_ = static_cast<u32>(character.skeleton.joint_count()) * 2U;

    cy::rendering::skinning::SkinPassDescription desc;
    desc.max_vertices = vertex_count_;
    desc.max_bones = bone_count_;
    desc.with_frames = true;
    desc.read_back = false;
    if (Status created = device_->skin.create(*allocator_, device, desc); !created) {
        return created;
    }
    if (Status uploaded = device_->skin.upload_mesh(
            character.positions.span(), character.frames.span(), character.influences.span());
        !uploaded) {
        return uploaded;
    }

    rhi::BufferDescription description;
    description.name = "animated character indices";
    description.size = character.indices.size() * sizeof(u32);
    description.usage = rhi::BufferUsage::Index;
    description.memory = rhi::MemoryUse::Upload;
    Expected<rhi::BufferHandle, Error> buffer = device.create_buffer(description);
    if (!buffer) {
        return make_unexpected(buffer.error());
    }
    device_->indices = *buffer;
    return upload_bytes(device, device_->indices, character.indices.data(), description.size);
}

Status Stage::shoot(Span<const Mat4> pose, u32 pose_offset, const Shot& shot, u64 frame_index,
                    const char* png_path, FrameReport& out) noexcept {
    if (!available_) {
        return fail(ErrorCode::Unavailable, "no graphics device answered");
    }
    rhi::Device& device = *device_->handle.value();

    render::geometry::SkinningDescriptor descriptor;
    descriptor.mesh = 0;
    descriptor.vertex_count = vertex_count_;
    descriptor.bone_count = static_cast<u32>(pose.size()) / 2U;
    descriptor.retained_bones = descriptor.bone_count;
    descriptor.pose_offset = pose_offset;
    descriptor.influences = render::geometry::InfluenceCount::Four;
    descriptor.method = render::geometry::SkinningMethod::LinearBlend;
    descriptor.source = render::geometry::PoseSource::GpuPoseWorld;
    descriptor.tier = render::geometry::AnimationTier::Full;
    if (Status uploaded = device_->skin.upload(descriptor, pose, frame_index); !uploaded) {
        return uploaded;
    }

    if (Expected<u32, Error> began = device.begin_frame(); !began) {
        return make_unexpected(began.error());
    }

    cy::rendering::RenderGraph graph(*allocator_);
    cy::rendering::GraphExecutor executor(*allocator_, device);
    if (Status declared = device_->skin.declare(graph); !declared) {
        return declared;
    }

    cy::rendering::TextureRequest color_request;
    color_request.name = "animated character colour";
    color_request.format = rhi::Format::Rgba8Unorm;
    color_request.width = width_;
    color_request.height = height_;
    const ResourceId color = graph.create_texture(color_request);

    cy::rendering::TextureRequest depth_request;
    depth_request.name = "animated character depth";
    depth_request.format = rhi::Format::D32Sfloat;
    depth_request.width = width_;
    depth_request.height = height_;
    const ResourceId depth = graph.create_texture(depth_request);

    cy::rendering::BufferRequest readback_request;
    readback_request.name = "animated character colour readback";
    readback_request.size = static_cast<u64>(width_) * height_ * sizeof(u32);
    readback_request.extra_usage = rhi::BufferUsage::TransferDestination;
    const ResourceId color_out = graph.import_buffer(readback_request, device_->readback);

    const Mat4 projection = perspective_reversed_z(
        shot.fov_y_radians, static_cast<f32>(width_) / static_cast<f32>(height_), 0.05F, 200.0F);
    const Mat4 world_to_clip = projection * look_at(shot.eye, shot.target);

    DrawState state;
    state.executor = &executor;
    state.pipeline = device_->pipeline;
    state.layout = device_->layout;
    state.skinned = device_->skin.output_positions();
    state.ground = device_->ground;
    state.indices = device_->indices;
    state.color = color;
    state.depth = depth;
    state.width = width_;
    state.height = height_;
    state.index_count = index_count_;
    state.vertex_offset = static_cast<i32>(device_->skin.vertex_offset());
    state.ground_light_first = ground_light_first_;
    state.ground_light_count = ground_light_count_;
    state.ground_dark_first = ground_dark_first_;
    state.ground_dark_count = ground_dark_count_;
    for (usize row = 0; row < 4; ++row) {
        const Vec4 values = world_to_clip.row(row);
        switch (row) {
            case 0:
                write_row(state.view.row0, values);
                break;
            case 1:
                write_row(state.view.row1, values);
                break;
            case 2:
                write_row(state.view.row2, values);
                break;
            default:
                write_row(state.view.row3, values);
                break;
        }
    }
    // A key light from high and to the camera's left, and an ambient floor of a fifth. The
    // direction is the direction light TRAVELS, which is what the shader's `-push.light.xyz`
    // undoes.
    const Vec3 key = normalize(Vec3{0.45F, -0.75F, -0.48F});
    state.view.light[0] = key.x;
    state.view.light[1] = key.y;
    state.view.light[2] = key.z;
    state.view.light[3] = 0.22F;
    state.view.eye[0] = shot.eye.x;
    state.view.eye[1] = shot.eye.y;
    state.view.eye[2] = shot.eye.z;

    ReadbackState readback;
    readback.executor = &executor;
    readback.color = color;
    readback.buffer = device_->readback;
    readback.width = width_;
    readback.height = height_;

    // THE DEPENDENCY THAT MAKES THE BARRIER THE GRAPH'S. The draw READS the buffer the dispatch
    // WROTE, as a vertex attribute. No barrier is written here.
    graph.add_pass("animated character draw", QueueKind::Graphics)
        .read(device_->skin.positions_resource(), Access::VertexAttributeRead)
        .write(color, Access::ColorAttachmentWrite)
        .write(depth, Access::DepthStencilAttachmentWrite)
        .record(&record_draw, &state);
    graph.add_pass("animated character readback", QueueKind::Graphics)
        .read(color, Access::TransferRead)
        .write(color_out, Access::TransferWrite)
        .record(&record_readback, &readback);
    graph.add_pass("animated character host", QueueKind::Graphics)
        .read(color_out, Access::HostRead)
        .side_effect();
    if (Status declared = graph.status(); !declared) {
        return declared;
    }

    Status frame = ok();
    if (Expected<cy::rendering::ExecutionResult, Error> executed = executor.execute(
            graph, cy::rendering::CompileOptions{}, cy::rendering::ExecuteOptions{});
        !executed) {
        frame = make_unexpected(executed.error());
    }
    if (frame) {
        frame = device.wait_idle();
    }
    if (frame && png_path != nullptr) {
        frame = write_png(png_path);
    }
    executor.release();
    if (Status ended = device.end_frame(); !ended && frame) {
        frame = ended;
    }

    out.validation_errors = device_->validation_errors;
    out.vertex_offset = device_->skin.vertex_offset();
    out.pose_offset = pose_offset;
    return frame;
}

Status Stage::write_png(const char* path) noexcept {
    rhi::Device& device = *device_->handle.value();
    const auto* mapped = static_cast<const u32*>(device.buffer_mapped_pointer(device_->readback));
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal, "the colour readback buffer is not mapped");
    }
    for (usize texel = 0; texel < pixels_.size(); ++texel) {
        pixels_[texel] = mapped[texel];
    }
    render_test::Image image(*allocator_);
    if (Status adopted = render_test::adopt(image, pixels_.span(), width_, height_); !adopted) {
        return adopted;
    }
    return render_test::write_png(path, image);
}

void Stage::close() noexcept {
    if (device_ == nullptr) {
        return;
    }
    if (device_->handle.has_value()) {
        rhi::Device& device = *device_->handle.value();
        (void)device.wait_idle();
        device_->skin.destroy();
        device.destroy_graphics_pipeline(device_->pipeline);
        device.destroy_pipeline_layout(device_->layout);
        device.destroy_shader_module(device_->vertex);
        device.destroy_shader_module(device_->fragment);
        device.destroy_buffer(device_->ground);
        device.destroy_buffer(device_->indices);
        device.destroy_buffer(device_->readback);
        rhi::destroy_device(*allocator_, device_->handle.value());
    }
    delete device_;
    device_ = nullptr;
    available_ = false;
}

}  // namespace cy::sample::character
