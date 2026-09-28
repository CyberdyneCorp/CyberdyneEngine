// SPDX-License-Identifier: MIT
#include <cy/rendering/fog/fog_pass.h>

#include <cy/backends/rhi/validation.h>
#include <cy/rendering/graph/executor.h>

#include <cstring>

#include "fog_msl.h"
#include "fog_spirv.h"

namespace cy::rendering::fog {
namespace {

using rhi::Access;
using rhi::QueueKind;

constexpr u32 kGroupSize = 8;
/// fog_march.slang's set, in the `ParameterBlock`'s field order — which is also the
/// argument-buffer id Metal assigns: the shadow map and the constants, then the texture, or the
/// table and the atmosphere's table.
constexpr u32 kTextureBindings = 3;
constexpr u32 kTableBindings = 4;
constexpr u64 kConstantBytes = sizeof(FogConstants);

[[nodiscard]] u32 groups(u32 extent) noexcept {
    return (extent + kGroupSize - 1U) / kGroupSize;
}

[[nodiscard]] rhi::DescriptorWrite buffer_write(u32 binding, rhi::BufferHandle buffer) noexcept {
    rhi::DescriptorWrite write;
    write.binding = binding;
    write.kind = rhi::DescriptorKind::StorageBuffer;
    write.buffer = buffer;
    return write;
}

}  // namespace

FogPass::~FogPass() {
    destroy();
}

bool FogPass::supported(const rhi::Device& device) noexcept {
    const rhi::ShaderFormat format = device.capabilities().native_shader_format();
    return device.capabilities().has(rhi::Capability::ComputeShaders) &&
           (format == rhi::ShaderFormat::Spirv || format == rhi::ShaderFormat::Msl);
}

Status FogPass::create(Allocator& allocator, rhi::Device& device,
                       const FogPassDescription& desc) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the volumetric fog pass is already created");
    }
    if (!supported(device)) {
        return fail(ErrorCode::Unsupported,
                    "volumetric fog is a compute dispatch and this device has no compute stage or "
                    "no shader format this module ships (SPIR-V or MSL)");
    }
    // The same refusals the constants make, made once here so a pass that exists can be framed.
    if (Expected<FogConstants, Error> probe =
            pack_fog_constants(desc.settings, FogView{}, FogLight{}, FogShadow{}, FogMedium{});
        !probe.has_value()) {
        return make_unexpected(probe.error());
    }
    allocator_ = &allocator;
    device_ = &device;
    desc_ = desc;
    if (Status created = create_pipeline(); !created) {
        destroy();
        return created;
    }
    if (Status created = create_resources(); !created) {
        destroy();
        return created;
    }
    return ok();
}

Status FogPass::create_pipeline() noexcept {
    const bool table = desc_.target == FogTarget::AerialTable;
    const char* name = table ? "volumetric fog table" : "volumetric fog";
    rhi::ShaderModuleBundle bundle;
    if (table) {
        bundle.spirv = {kVolumetricFogTableSpirv, sizeof(kVolumetricFogTableSpirv) / sizeof(u32)};
        bundle.msl = {reinterpret_cast<const u8*>(kVolumetricFogTableMsl),
                      sizeof(kVolumetricFogTableMsl) - 1};
    } else {
        bundle.spirv = {kVolumetricFogSpirv, sizeof(kVolumetricFogSpirv) / sizeof(u32)};
        bundle.msl = {reinterpret_cast<const u8*>(kVolumetricFogMsl),
                      sizeof(kVolumetricFogMsl) - 1};
    }
    bundle.msl_entry_point = "cyVolumetricFog";
    bundle.spirv_entry_point = "main";
    rhi::ValidationMessage message;
    auto module = rhi::select_shader_module(bundle, device_->capabilities().native_shader_format(),
                                            name, rhi::ShaderStage::Compute, message);
    if (!module.has_value()) {
        return make_unexpected(module.error());
    }
    Expected<rhi::ShaderModuleHandle, Error> shader = device_->create_shader_module(*module);
    if (!shader.has_value()) {
        return make_unexpected(shader.error());
    }
    shader_ = *shader;

    const rhi::DescriptorKind texture_kinds[kTextureBindings] = {
        rhi::DescriptorKind::SampledTexture, rhi::DescriptorKind::StorageBuffer,
        rhi::DescriptorKind::StorageTexture};
    const rhi::DescriptorKind table_kinds[kTableBindings] = {
        rhi::DescriptorKind::SampledTexture, rhi::DescriptorKind::StorageBuffer,
        rhi::DescriptorKind::StorageBuffer, rhi::DescriptorKind::StorageBuffer};
    const u32 count = table ? kTableBindings : kTextureBindings;
    rhi::DescriptorBinding bindings[kTableBindings] = {};
    for (u32 index = 0; index < count; ++index) {
        bindings[index].binding = index;
        bindings[index].kind = table ? table_kinds[index] : texture_kinds[index];
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Compute;
    }
    rhi::DescriptorSetLayoutDescription set_layout;
    set_layout.name = name;
    set_layout.bindings = Span<const rhi::DescriptorBinding>(bindings, count);
    Expected<rhi::DescriptorSetLayoutHandle, Error> layout =
        device_->create_descriptor_set_layout(set_layout);
    if (!layout.has_value()) {
        return make_unexpected(layout.error());
    }
    set_layout_ = *layout;

    rhi::PipelineLayoutDescription pipeline_layout;
    pipeline_layout.name = name;
    pipeline_layout.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(&set_layout_, 1);
    Expected<rhi::PipelineLayoutHandle, Error> made =
        device_->create_pipeline_layout(pipeline_layout);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    pipeline_layout_ = *made;

    rhi::ComputePipelineDescription pipeline;
    pipeline.name = name;
    pipeline.layout = pipeline_layout_;
    pipeline.shader = shader_;
    pipeline.workgroup_size[0] = kGroupSize;
    pipeline.workgroup_size[1] = kGroupSize;
    Expected<rhi::ComputePipelineHandle, Error> created =
        device_->create_compute_pipeline(pipeline);
    if (!created.has_value()) {
        return make_unexpected(created.error());
    }
    pipeline_ = *created;
    return ok();
}

Status FogPass::create_resources() noexcept {
    if (desc_.target == FogTarget::AerialTable) {
        rhi::BufferDescription table;
        table.name = "volumetric fog table";
        table.size = fog_table_words(desc_.settings.volume) * sizeof(Vec4);
        table.usage = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSource;
        table.memory = rhi::MemoryUse::DeviceLocal;
        Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(table);
        if (!buffer.has_value()) {
            return make_unexpected(buffer.error());
        }
        table_ = *buffer;
        // The atmosphere a frame without one binds: a header with `enabled` zero, written once.
        rhi::BufferDescription no_air;
        no_air.name = "volumetric fog no atmosphere";
        no_air.size = sky::kAerialPerspectiveHeaderWords * sizeof(Vec4);
        no_air.usage = rhi::BufferUsage::Storage;
        no_air.memory = rhi::MemoryUse::Upload;
        Expected<rhi::BufferHandle, Error> header = device_->create_buffer(no_air);
        if (!header.has_value()) {
            return make_unexpected(header.error());
        }
        no_air_ = *header;
        void* mapped = device_->buffer_mapped_pointer(no_air_);
        if (mapped == nullptr) {
            return fail(ErrorCode::Internal, "volumetric fog: an upload buffer is not mapped");
        }
        std::memset(mapped, 0, static_cast<usize>(no_air.size));
    } else {
        const FogTextureExtent size = extent();
        rhi::TextureDescription target;
        target.name = "volumetric fog";
        target.format = target_format();
        target.extent = rhi::Extent3D{size.width, size.height, 1};
        target.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::Storage |
                       rhi::TextureUsage::TransferSource;
        Expected<rhi::TextureHandle, Error> texture = device_->create_texture(target);
        if (!texture.has_value()) {
            return make_unexpected(texture.error());
        }
        target_ = *texture;
        rhi::TextureViewDescription view;
        view.name = "volumetric fog";
        view.texture = target_;
        Expected<rhi::TextureViewHandle, Error> made = device_->create_texture_view(view);
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        target_view_ = *made;
    }

    rhi::TextureDescription placeholder;
    placeholder.name = "volumetric fog no shadow";
    placeholder.format = rhi::Format::R32Sfloat;
    placeholder.extent = rhi::Extent3D{1, 1, 1};
    placeholder.usage = rhi::TextureUsage::Sampled;
    Expected<rhi::TextureHandle, Error> stand_in = device_->create_texture(placeholder);
    if (!stand_in.has_value()) {
        return make_unexpected(stand_in.error());
    }
    placeholder_ = *stand_in;

    const u32 slots = device_->frames_in_flight() < rhi::kMaxFramesInFlight
                          ? device_->frames_in_flight()
                          : rhi::kMaxFramesInFlight;
    for (u32 slot = 0; slot < slots; ++slot) {
        rhi::BufferDescription words;
        words.name = "volumetric fog constants";
        words.size = kConstantBytes;
        words.usage = rhi::BufferUsage::Storage;
        words.memory = rhi::MemoryUse::Upload;
        Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(words);
        if (!buffer.has_value()) {
            return make_unexpected(buffer.error());
        }
        words_[slot] = *buffer;
    }

    if (!desc_.readback) {
        return ok();
    }
    rhi::BufferDescription host;
    host.size = static_cast<u64>(readback_count()) * sizeof(Vec4);
    host.usage = rhi::BufferUsage::TransferDestination;
    host.memory = rhi::MemoryUse::Readback;
    host.name = "volumetric fog readback";
    Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(host);
    if (!buffer.has_value()) {
        return make_unexpected(buffer.error());
    }
    readback_ = *buffer;
    return ok();
}

void FogPass::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    if (!target_view_.is_null()) {
        device_->destroy_texture_view(target_view_);
    }
    for (rhi::TextureHandle* texture : {&target_, &placeholder_}) {
        if (!texture->is_null()) {
            device_->destroy_texture(*texture);
        }
        *texture = {};
    }
    for (rhi::BufferHandle& buffer : words_) {
        if (!buffer.is_null()) {
            device_->destroy_buffer(buffer);
        }
        buffer = {};
    }
    for (rhi::BufferHandle* buffer : {&table_, &no_air_, &readback_}) {
        if (!buffer->is_null()) {
            device_->destroy_buffer(*buffer);
        }
        *buffer = {};
    }
    if (!pipeline_.is_null()) {
        device_->destroy_compute_pipeline(pipeline_);
    }
    if (!pipeline_layout_.is_null()) {
        device_->destroy_pipeline_layout(pipeline_layout_);
    }
    if (!set_layout_.is_null()) {
        device_->destroy_descriptor_set_layout(set_layout_);
    }
    if (!shader_.is_null()) {
        device_->destroy_shader_module(shader_);
    }
    pipeline_ = {};
    pipeline_layout_ = {};
    set_layout_ = {};
    shader_ = {};
    target_view_ = {};
    imported_ = kInvalidResource;
    framed_ = false;
    device_ = nullptr;
    allocator_ = nullptr;
}

Status FogPass::set_frame(const FogFrame& frame) noexcept {
    if (frame.shadow.enabled && frame.shadow_resource == kInvalidResource) {
        return fail(ErrorCode::InvalidArgument,
                    "volumetric fog: the frame asks for shadowed light and names no shadow map");
    }
    if (frame.air != kInvalidResource && desc_.target != FogTarget::AerialTable) {
        return fail(ErrorCode::InvalidArgument,
                    "volumetric fog: only the atmosphere-table target composites the atmosphere");
    }
    Expected<FogConstants, Error> constants =
        pack_fog_constants(desc_.settings, frame.view, frame.light, frame.shadow, medium_);
    if (!constants.has_value()) {
        return make_unexpected(constants.error());
    }
    frame_ = frame;
    constants_ = *constants;
    // `words[4].w`: whether the table variant reads the atmosphere's table.
    constants_.words[4].w = frame.air != kInvalidResource ? 1.0F : 0.0F;
    framed_ = true;
    return ok();
}

ResourceId FogPass::import_target(RenderGraph& graph) noexcept {
    if (desc_.target == FogTarget::AerialTable) {
        BufferRequest request;
        request.name = "volumetric fog table";
        request.size = fog_table_words(desc_.settings.volume) * sizeof(Vec4);
        request.extra_usage = rhi::BufferUsage::TransferSource;
        imported_ = graph.import_buffer(request, table_);
        return imported_;
    }
    const FogTextureExtent size = extent();
    TextureRequest request;
    request.name = "volumetric fog";
    request.format = target_format();
    request.width = size.width;
    request.height = size.height;
    request.extra_usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferSource;
    // UNDEFINED EVERY FRAME, and it is the truth: the march writes every texel a consumer reads.
    imported_ = graph.import_texture(request, target_, rhi::ImageUse::Undefined);
    return imported_;
}

FrameStageDeclaration FogPass::stage() noexcept {
    return FrameStageDeclaration{&declare_stage, this};
}

PassId FogPass::declare_stage(RenderGraph& graph, const ScreenSpaceStageInputs& inputs,
                              void* user) noexcept {
    return static_cast<FogPass*>(user)->declare(graph, inputs);
}

PassId FogPass::declare(RenderGraph& graph, const ScreenSpaceStageInputs& inputs) noexcept {
    // Each refusal is a frame that would otherwise sample a volume nothing wrote, or march with the
    // constants of a frame that is not this one.
    if (device_ == nullptr || !framed_ || inputs.target == kInvalidResource ||
        inputs.target != imported_) {
        return kInvalidPass;
    }
    ResourceId shadow = frame_.shadow_resource;
    if (!frame_.shadow.enabled) {
        TextureRequest request;
        request.name = "volumetric fog no shadow";
        request.format = rhi::Format::R32Sfloat;
        request.width = 1;
        request.height = 1;
        shadow = graph.import_texture(request, placeholder_, rhi::ImageUse::Undefined);
    }
    const bool table = desc_.target == FogTarget::AerialTable;
    march_ = March{this, shadow, inputs.target, table ? frame_.air : kInvalidResource};
    PassBuilder builder =
        graph.add_pass(table ? "volumetric fog table" : "volumetric fog", QueueKind::Graphics);
    builder.read(shadow, Access::ComputeSampledRead);
    if (march_.air != kInvalidResource) {
        builder.read(march_.air, Access::ComputeStorageRead);
    }
    builder.write(inputs.target, Access::ComputeStorageWrite).record(&record_march, &march_);
    const PassId first = builder.id();
    if (desc_.readback) {
        copy_ = Readback{this, inputs.target};
        BufferRequest request;
        request.name = "volumetric fog readback";
        request.size = static_cast<u64>(readback_count()) * sizeof(Vec4);
        request.extra_usage = rhi::BufferUsage::TransferDestination;
        const ResourceId destination = graph.import_buffer(request, readback_);
        graph.add_pass("volumetric fog readback", QueueKind::Graphics)
            .read(inputs.target, Access::TransferRead)
            .write(destination, Access::TransferWrite)
            .record(&record_readback, &copy_);
        // The host boundary as a declared read, so the graph derives the transfer-to-host barrier.
        graph.add_pass("volumetric fog host", QueueKind::Graphics)
            .read(destination, Access::HostRead)
            .side_effect();
    }
    return first;
}

void FogPass::record_march(const PassContext& context, void* user) noexcept {
    const auto* march = static_cast<const March*>(user);
    FogPass& self = *march->self;
    // THE CONSTANTS ARE WRITTEN WHEN THE FRAME RECORDS, into the block of the frame slot being
    // recorded: `begin_frame` has already waited for the frame that last used it.
    const u32 slot = self.device_->frame_slot() % rhi::kMaxFramesInFlight;
    const rhi::BufferHandle words = self.words_[slot];
    void* mapped = words.is_null() ? nullptr : self.device_->buffer_mapped_pointer(words);
    if (mapped == nullptr) {
        return;
    }
    std::memcpy(mapped, self.constants_.words, kConstantBytes);

    Expected<rhi::DescriptorSetHandle, Error> set =
        self.device_->allocate_descriptor_set(self.set_layout_, true);
    if (!set.has_value()) {
        return;
    }
    const bool table = self.desc_.target == FogTarget::AerialTable;
    rhi::DescriptorWrite writes[kTableBindings] = {};
    writes[0].binding = 0;
    writes[0].kind = rhi::DescriptorKind::SampledTexture;
    writes[0].texture_view = context.executor->view(march->shadow);
    writes[0].use = rhi::ImageUse::SampledRead;
    writes[1] = buffer_write(1, words);
    writes[1].buffer_range = kConstantBytes;
    if (table) {
        writes[2] = buffer_write(2, context.executor->buffer(march->output));
        writes[3] =
            buffer_write(3, march->air != kInvalidResource ? context.executor->buffer(march->air)
                                                           : self.no_air_);
    } else {
        writes[2].binding = 2;
        writes[2].kind = rhi::DescriptorKind::StorageTexture;
        writes[2].texture_view = context.executor->view(march->output);
        writes[2].use = rhi::ImageUse::Storage;
    }
    const u32 count = table ? kTableBindings : kTextureBindings;
    if (Status written = self.device_->update_descriptor_set(
            *set, Span<const rhi::DescriptorWrite>(writes, count));
        !written) {
        return;
    }
    const FroxelVolume& volume = self.desc_.settings.volume;
    context.commands->bind_compute_pipeline(self.pipeline_);
    context.commands->bind_descriptor_sets(self.pipeline_layout_, 0,
                                           Span<const rhi::DescriptorSetHandle>(&*set, 1));
    context.commands->dispatch(groups(volume.width), groups(volume.height), 1);
}

void FogPass::record_readback(const PassContext& context, void* user) noexcept {
    const auto* copy = static_cast<const Readback*>(user);
    const FogPass& self = *copy->self;
    if (self.desc_.target == FogTarget::AerialTable) {
        rhi::BufferCopy region;
        region.size = static_cast<u64>(self.readback_count()) * sizeof(Vec4);
        context.commands->copy_buffer(context.executor->buffer(copy->source), self.readback_,
                                      Span<const rhi::BufferCopy>(&region, 1));
        return;
    }
    const FogTextureExtent size = self.extent();
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{size.width, size.height, 1};
    context.commands->copy_texture_to_buffer(context.executor->texture(copy->source),
                                             self.readback_,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

usize FogPass::readback_count() const noexcept {
    if (desc_.target == FogTarget::AerialTable) {
        return static_cast<usize>(fog_table_words(desc_.settings.volume));
    }
    const FogTextureExtent size = extent();
    return static_cast<usize>(size.width) * size.height;
}

Status FogPass::read_back(Span<Vec4> out) const noexcept {
    if (device_ == nullptr || readback_.is_null()) {
        return fail(ErrorCode::Unavailable,
                    "volumetric fog: created without `readback`, so there is nothing to read");
    }
    if (out.size() != readback_count()) {
        return fail(ErrorCode::InvalidArgument,
                    "volumetric fog: the readback span is not the volume's size");
    }
    const void* texels = device_->buffer_mapped_pointer(readback_);
    if (texels == nullptr) {
        return fail(ErrorCode::Internal, "volumetric fog: the readback buffer is not mapped");
    }
    std::memcpy(out.data(), texels, out.size() * sizeof(Vec4));
    return ok();
}

}  // namespace cy::rendering::fog
