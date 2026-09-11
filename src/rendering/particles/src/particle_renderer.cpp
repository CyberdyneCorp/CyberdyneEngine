// The first consumer of the pipeline layer, and the proof of it. M8.c task 1b.4.

#include <cy/rendering/particles/particle_renderer.h>

#include "particle_spirv.h"

#include <cstring>

namespace cy::rendering::particles {
namespace {

inline constexpr u32 kVerticesPerParticle = 6;
inline constexpr u32 kParticleSet = pipeline::kPassSet;

/// The extension's callback. Everything it binds is either the frame's (sets 0 and 1, already
/// bound and still valid because the layouts are identical) or its own (set 2 and the pipeline).
void record_particles(const ExtensionContext& context, void* user) noexcept {
    auto* renderer = static_cast<ParticleRenderer*>(user);
    if (renderer == nullptr || !renderer->ready() || renderer->live() == 0) {
        return;
    }
    if (!context.inside_rendering || context.commands == nullptr) {
        return;
    }
    rhi::CommandBuffer& commands = *context.commands;
    commands.bind_graphics_pipeline(renderer->pipeline());
    const rhi::DescriptorSetHandle set = renderer->set();
    commands.bind_descriptor_sets(renderer->layout(), kParticleSet,
                                  Span<const rhi::DescriptorSetHandle>(&set, 1));
    // ONE DRAW for the whole effect. Six vertices a particle, expanded from `SV_VertexID`; there is
    // no vertex buffer, no index buffer and no per-particle call.
    commands.draw(renderer->live() * kVerticesPerParticle, 1, 0, 0);
    ++renderer->mutable_report().draws;
}

}  // namespace

ParticleRenderer::~ParticleRenderer() {
    shutdown();
}

Status ParticleRenderer::create_pipeline(rhi::Device& device,
                                         const FramePipelines& pipelines) noexcept {
    rhi::ShaderModuleDescription vertex;
    vertex.name = "cy particle vertex";
    vertex.stage = rhi::ShaderStage::Vertex;
    vertex.spirv =
        Span<const u32>(kParticleVertexSpirv, sizeof(kParticleVertexSpirv) / sizeof(u32));
    Expected<rhi::ShaderModuleHandle, Error> made = device.create_shader_module(vertex);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    vertex_ = *made;

    rhi::ShaderModuleDescription fragment;
    fragment.name = "cy particle fragment";
    fragment.stage = rhi::ShaderStage::Fragment;
    fragment.spirv =
        Span<const u32>(kParticleFragmentSpirv, sizeof(kParticleFragmentSpirv) / sizeof(u32));
    made = device.create_shader_module(fragment);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    fragment_ = *made;

    rhi::DescriptorBinding binding;
    binding.binding = 0;
    binding.kind = rhi::DescriptorKind::StorageBuffer;
    binding.count = 1;
    binding.stages = rhi::ShaderStage::Vertex;
    rhi::DescriptorSetLayoutDescription set_layout;
    set_layout.name = "cy particles";
    set_layout.bindings = Span<const rhi::DescriptorBinding>(&binding, 1);
    Expected<rhi::DescriptorSetLayoutHandle, Error> layout =
        device.create_descriptor_set_layout(set_layout);
    if (!layout.has_value()) {
        return make_unexpected(layout.error());
    }
    set_layout_ = *layout;

    // SETS 0 AND 1 ARE THE FRAME'S OWN HANDLES, not copies of its declarations. Two set layouts
    // built from identical descriptions would also be compatible, and would also be a second
    // description to keep in step; reusing the handle removes the question.
    const rhi::DescriptorSetLayoutHandle sets[pipeline::kSetCount] = {
        pipelines.set_layout(pipeline::kGlobalSet),
        pipelines.set_layout(pipeline::kViewSet),
        set_layout_,
    };
    // THE SAME PUSH-CONSTANT RANGE AS THE FRAME'S, AND IT IS NOT OPTIONAL. Vulkan's pipeline-layout
    // compatibility rule (14.2.2) requires identical set layouts for sets 0..N AND IDENTICAL PUSH
    // CONSTANT RANGES before a previously bound set survives a bind with another layout. This
    // module's shader uses no push constant; omitting the range made the two layouts incompatible
    // for every set, and the symptom was seventeen "uses set #1 but that set is not bound" on a
    // draw whose set 1 had been bound moments earlier by the recorder.
    const rhi::PushConstantRange range{rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
                                       sizeof(pipeline::DrawPush)};
    rhi::PipelineLayoutDescription pipeline_layout;
    pipeline_layout.name = "cy particle layout";
    pipeline_layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    pipeline_layout.set_layouts =
        Span<const rhi::DescriptorSetLayoutHandle>(sets, pipeline::kSetCount);
    Expected<rhi::PipelineLayoutHandle, Error> created =
        device.create_pipeline_layout(pipeline_layout);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    layout_ = *created;

    rhi::ColorAttachmentState color;
    color.format = pipelines.setup().color_format;
    color.blend_enable = true;
    // PREMULTIPLIED: the fragment shader multiplies its colour by the alpha it computed, so one
    // pipeline composites an additive spark and an opaque puff. See particle.slang.
    color.source_color = rhi::BlendFactor::One;
    color.destination_color = rhi::BlendFactor::OneMinusSourceAlpha;
    color.source_alpha = rhi::BlendFactor::One;
    color.destination_alpha = rhi::BlendFactor::OneMinusSourceAlpha;

    rhi::GraphicsPipelineDescription description;
    description.name = "particles";
    description.layout = layout_;
    description.vertex_shader = vertex_;
    description.fragment_shader = fragment_;
    description.color_attachments = Span<const rhi::ColorAttachmentState>(&color, 1);
    description.sample_count = pipelines.setup().sample_count;
    description.rasterisation.cull_mode = rhi::CullMode::None;
    description.depth_stencil.format = pipelines.setup().depth_format;
    // Tested against the opaque depth so a particle behind a wall is hidden, and NOT written so two
    // particles composite instead of the nearer one erasing the further.
    description.depth_stencil.depth_test_enable = true;
    description.depth_stencil.depth_write_enable = false;
    description.depth_stencil.depth_compare = rhi::CompareOp::GreaterOrEqual;

    Expected<rhi::GraphicsPipelineHandle, Error> pipeline =
        device.create_graphics_pipeline(description);
    if (!pipeline.has_value()) {
        return make_unexpected(pipeline.error());
    }
    pipeline_ = *pipeline;
    return ok();
}

Status ParticleRenderer::initialize(rhi::Device& device, const FramePipelines& pipelines,
                                    u32 capacity) noexcept {
    if (ready_) {
        return fail(ErrorCode::InvalidArgument, "particle renderer: already initialized");
    }
    if (!pipelines.ready()) {
        return fail(ErrorCode::InvalidArgument,
                    "particle renderer: the frame's pipelines must be initialized first — this "
                    "module reuses their set layouts rather than declaring its own");
    }
    if (capacity == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "particle renderer: a capacity of zero draws "
                    "nothing and hides the reason");
    }
    device_ = &device;
    pipelines_ = &pipelines;
    capacity_ = capacity;
    slot_count_ = device.frames_in_flight();
    if (slot_count_ == 0 || slot_count_ > rhi::kMaxFramesInFlight) {
        device_ = nullptr;
        return fail(ErrorCode::InvalidArgument,
                    "particle renderer: the device reported an impossible frames-in-flight");
    }
    if (Status made = create_pipeline(device, pipelines); !made) {
        shutdown();
        return made;
    }
    for (u32 index = 0; index < slot_count_; ++index) {
        rhi::BufferDescription buffer;
        buffer.name = "cy particles";
        buffer.size = u64{capacity_} * sizeof(ParticleInstance);
        buffer.usage = rhi::BufferUsage::Storage;
        buffer.memory = rhi::MemoryUse::Upload;
        Expected<rhi::BufferHandle, Error> created = device.create_buffer(buffer);
        if (!created.has_value()) {
            shutdown();
            return make_unexpected(created.error());
        }
        rings_[index] = *created;
    }
    ready_ = true;
    return ok();
}

Status ParticleRenderer::upload(u32 frame_slot, Span<const ParticleInstance> particles) noexcept {
    if (!ready_) {
        return fail(ErrorCode::Unavailable, "particle renderer: not initialized");
    }
    if (frame_slot >= slot_count_) {
        return fail(ErrorCode::OutOfRange, "particle renderer: the frame slot is past the ring");
    }
    live_ = static_cast<u32>(particles.size() > capacity_ ? capacity_ : particles.size());
    report_.particles = live_;
    report_.dropped = static_cast<u32>(particles.size()) - live_;

    if (live_ > 0) {
        void* mapped = device_->buffer_mapped_pointer(rings_[frame_slot]);
        if (mapped == nullptr) {
            return fail(ErrorCode::Internal, "particle renderer: the ring is not mapped");
        }
        std::memcpy(mapped, particles.data(), u64{live_} * sizeof(ParticleInstance));
    }

    Expected<rhi::DescriptorSetHandle, Error> allocated =
        device_->allocate_descriptor_set(set_layout_, true);
    if (!allocated.has_value()) {
        return make_unexpected(allocated.error());
    }
    set_ = *allocated;

    rhi::DescriptorWrite write;
    write.binding = 0;
    write.kind = rhi::DescriptorKind::StorageBuffer;
    write.buffer = rings_[frame_slot];
    return device_->update_descriptor_set(set_, Span<const rhi::DescriptorWrite>(&write, 1));
}

PassExtension ParticleRenderer::extension() noexcept {
    PassExtension extension;
    extension.kind = FramePassKind::Transparent;
    extension.record = &record_particles;
    extension.user = this;
    return extension;
}

void ParticleRenderer::shutdown() noexcept {
    if (device_ == nullptr) {
        return;
    }
    rhi::Device& device = *device_;
    for (rhi::BufferHandle& handle : rings_) {
        if (!handle.is_null()) {
            device.destroy_buffer(handle);
            handle = rhi::BufferHandle{};
        }
    }
    if (!pipeline_.is_null()) {
        device.destroy_graphics_pipeline(pipeline_);
        pipeline_ = rhi::GraphicsPipelineHandle{};
    }
    if (!layout_.is_null()) {
        device.destroy_pipeline_layout(layout_);
        layout_ = rhi::PipelineLayoutHandle{};
    }
    if (!set_layout_.is_null()) {
        device.destroy_descriptor_set_layout(set_layout_);
        set_layout_ = rhi::DescriptorSetLayoutHandle{};
    }
    if (!vertex_.is_null()) {
        device.destroy_shader_module(vertex_);
        vertex_ = rhi::ShaderModuleHandle{};
    }
    if (!fragment_.is_null()) {
        device.destroy_shader_module(fragment_);
        fragment_ = rhi::ShaderModuleHandle{};
    }
    set_ = rhi::DescriptorSetHandle{};
    device_ = nullptr;
    pipelines_ = nullptr;
    slot_count_ = 0;
    capacity_ = 0;
    live_ = 0;
    ready_ = false;
}

}  // namespace cy::rendering::particles
