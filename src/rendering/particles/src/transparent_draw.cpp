// One pipeline, one ring, one draw. See transparent_draw.h.

#include <cy/rendering/particles/detail/transparent_draw.h>

#include <cstring>

namespace cy::rendering::particles::detail {
namespace {

inline constexpr u32 kRecordSet = pipeline::kPassSet;

[[nodiscard]] Expected<rhi::ShaderModuleHandle, Error> create_stage(rhi::Device& device,
                                                                    const StageSource& source,
                                                                    rhi::ShaderStage stage,
                                                                    bool metal) noexcept {
    rhi::ShaderModuleDescription module;
    module.name = source.name;
    module.stage = stage;
    if (metal) {
        module.entry_point = source.msl_entry;
        module.native = source.msl;
        module.native_format = rhi::ShaderFormat::Msl;
    } else {
        module.spirv = source.spirv;
    }
    return device.create_shader_module(module);
}

}  // namespace

TransparentDraw::~TransparentDraw() {
    shutdown();
}

Status TransparentDraw::create_pipeline(const pipeline::FramePipelines& pipelines,
                                        const TransparentDrawDescription& description) noexcept {
    rhi::Device& device = *device_;
    const bool metal = device.capabilities().native_shader_format() == rhi::ShaderFormat::Msl;
    Expected<rhi::ShaderModuleHandle, Error> made =
        create_stage(device, description.vertex, rhi::ShaderStage::Vertex, metal);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    vertex_ = *made;
    made = create_stage(device, description.fragment, rhi::ShaderStage::Fragment, metal);
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
    set_layout.name = description.name;
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
    // CONSTANT RANGES before a previously bound set survives a bind with another layout. These
    // shaders use no push constant; omitting the range made the two layouts incompatible for every
    // set, and the symptom was seventeen "uses set #1 but that set is not bound" on a draw whose
    // set 1 had been bound moments earlier by the recorder.
    const rhi::PushConstantRange range{rhi::ShaderStage::Vertex | rhi::ShaderStage::Fragment, 0,
                                       sizeof(pipeline::DrawPush)};
    rhi::PipelineLayoutDescription pipeline_layout;
    pipeline_layout.name = description.name;
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

    rhi::GraphicsPipelineDescription graphics;
    graphics.name = description.name;
    graphics.layout = layout_;
    graphics.vertex_shader = vertex_;
    graphics.fragment_shader = fragment_;
    graphics.color_attachments = Span<const rhi::ColorAttachmentState>(&color, 1);
    graphics.sample_count = pipelines.setup().sample_count;
    graphics.rasterisation.cull_mode = rhi::CullMode::None;
    graphics.depth_stencil.format = pipelines.setup().depth_format;
    // Tested against the opaque depth so an effect behind a wall is hidden, and NOT written so two
    // overlapping records composite instead of the nearer one erasing the further.
    graphics.depth_stencil.depth_test_enable = true;
    graphics.depth_stencil.depth_write_enable = false;
    graphics.depth_stencil.depth_compare = rhi::CompareOp::GreaterOrEqual;

    Expected<rhi::GraphicsPipelineHandle, Error> made_pipeline =
        device.create_graphics_pipeline(graphics);
    if (!made_pipeline.has_value()) {
        return make_unexpected(made_pipeline.error());
    }
    pipeline_ = *made_pipeline;
    return ok();
}

Status TransparentDraw::initialize(rhi::Device& device, const pipeline::FramePipelines& pipelines,
                                   const TransparentDrawDescription& description) noexcept {
    if (ready_) {
        return fail(ErrorCode::InvalidArgument, "transparent draw: already initialized");
    }
    if (!pipelines.ready()) {
        return fail(ErrorCode::InvalidArgument,
                    "transparent draw: the frame's pipelines must be initialized first — this "
                    "module reuses their set layouts rather than declaring its own");
    }
    if (description.capacity == 0 || description.record_size == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "transparent draw: a capacity of zero draws nothing and hides the reason");
    }
    device_ = &device;
    capacity_ = description.capacity;
    record_size_ = description.record_size;
    slot_count_ = device.frames_in_flight();
    if (slot_count_ == 0 || slot_count_ > rhi::kMaxFramesInFlight) {
        device_ = nullptr;
        return fail(ErrorCode::InvalidArgument,
                    "transparent draw: the device reported an impossible frames-in-flight");
    }
    if (Status made = create_pipeline(pipelines, description); !made) {
        shutdown();
        return made;
    }
    for (u32 index = 0; index < slot_count_; ++index) {
        rhi::BufferDescription buffer;
        buffer.name = description.name;
        buffer.size = u64{capacity_} * record_size_;
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

Status TransparentDraw::upload(u32 frame_slot, const void* records, u32 count) noexcept {
    if (!ready_) {
        return fail(ErrorCode::Unavailable, "transparent draw: not initialized");
    }
    if (frame_slot >= slot_count_) {
        return fail(ErrorCode::OutOfRange, "transparent draw: the frame slot is past the ring");
    }
    if (count > capacity_) {
        return fail(ErrorCode::OutOfRange, "transparent draw: more records than the ring holds");
    }
    if (count > 0) {
        void* mapped = device_->buffer_mapped_pointer(rings_[frame_slot]);
        if (mapped == nullptr) {
            return fail(ErrorCode::Internal, "transparent draw: the ring is not mapped");
        }
        std::memcpy(mapped, records, u64{count} * record_size_);
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

void TransparentDraw::bind(rhi::CommandBuffer& commands) const noexcept {
    commands.bind_graphics_pipeline(pipeline_);
    const rhi::DescriptorSetHandle set = set_;
    commands.bind_descriptor_sets(layout_, kRecordSet,
                                  Span<const rhi::DescriptorSetHandle>(&set, 1));
}

void TransparentDraw::shutdown() noexcept {
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
    slot_count_ = 0;
    capacity_ = 0;
    record_size_ = 0;
    ready_ = false;
}

}  // namespace cy::rendering::particles::detail
