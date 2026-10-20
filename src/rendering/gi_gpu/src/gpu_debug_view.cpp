// SPDX-License-Identifier: MIT
#include <cy/rendering/gi_gpu/gpu_debug_view.h>

#include <cy/backends/rhi/validation.h>
#include <cy/rendering/gi_gpu/gpu_radiance_cache.h>
#include <cy/rendering/gi_gpu/gpu_scene.h>
#include <cy/rendering/graph/executor.h>

#include <algorithm>
#include <cstring>

#include "gi_gpu_msl.h"
#include "gi_gpu_spirv.h"

namespace cy::rendering::gi_gpu {
namespace {

using rhi::Access;
using rhi::QueueKind;

/// gi_debug_views_common.slang's words, a uint4 each.
constexpr u32 kWordView = 0 * 4;
constexpr u32 kWordFrame = 1 * 4;
constexpr u32 kWordCamera = 2 * 4;
constexpr u32 kWordField = 3 * 4;
constexpr u32 kWordCache = 4 * 4;
constexpr u32 kWordLayout = 5 * 4;
/// The words' buffer: eight uint4.
constexpr u64 kWordBytes = u64{8} * 4U * 4U;
constexpr u32 kWordCount = 8 * 4;

/// The set's bindings, in `GiDebugSet`'s order.
constexpr u32 kBindings = 10;
constexpr u32 kTextureBindings = (1U << 3U) | (1U << 4U);

/// Four float4 a light, as `GpuGiScene::upload_lights` writes the scene's.
constexpr u32 kLightFloats = 16;
constexpr u32 kTileGroup = 8;
constexpr u32 kExposureLanes = 256;

[[nodiscard]] u32 bits(f32 value) noexcept {
    u32 word = 0;
    std::memcpy(&word, &value, sizeof(word));
    return word;
}

[[nodiscard]] u32 tile_groups(u32 extent) noexcept {
    return std::max((extent + kTileGroup - 1U) / kTileGroup, 1U);
}

[[nodiscard]] rhi::DescriptorWrite buffer_write(u32 binding, rhi::BufferHandle buffer) noexcept {
    rhi::DescriptorWrite write;
    write.binding = binding;
    write.kind = rhi::DescriptorKind::StorageBuffer;
    write.buffer = buffer;
    return write;
}

[[nodiscard]] rhi::DescriptorWrite sampled_write(u32 binding,
                                                 rhi::TextureViewHandle view) noexcept {
    rhi::DescriptorWrite write;
    write.binding = binding;
    write.kind = rhi::DescriptorKind::SampledTexture;
    write.texture_view = view;
    write.use = rhi::ImageUse::SampledRead;
    return write;
}

/// One light as the scene's lights buffer holds it: [position, intensity], [direction, range],
/// [colour, directional], [emitter normal, emitter area].
void put_light(const gi::GiLight& light, f32* out) noexcept {
    const f32 words[kLightFloats] = {light.position.x,       light.position.y,
                                     light.position.z,       light.intensity,
                                     light.direction.x,      light.direction.y,
                                     light.direction.z,      light.range,
                                     light.colour.x,         light.colour.y,
                                     light.colour.z,         light.directional ? 1.0F : 0.0F,
                                     light.emitter_normal.x, light.emitter_normal.y,
                                     light.emitter_normal.z, light.emitter_area};
    std::memcpy(out, words, sizeof(words));
}

}  // namespace

GpuGiDebugView::~GpuGiDebugView() {
    destroy();
}

bool GpuGiDebugView::draws(gi::GiDebugView view, const gi::IlluminationSystem& system) noexcept {
    if (view >= gi::GiDebugView::Count) {
        return false;
    }
    return view != gi::GiDebugView::ReflectionProbes ||
           system.reflection_probes().probe_count() == 0;
}

Status GpuGiDebugView::create(Allocator& allocator, rhi::Device& device, GpuScreenResolve& resolve,
                              const GpuGiDebugViewDescription& desc) noexcept {
    if (device_ != nullptr) {
        return fail(ErrorCode::InvalidArgument, "the device GI views are already created");
    }
    if (resolve.device_ != &device || resolve.cache_ == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "the device GI views read a resolve created on the same device");
    }
    allocator_ = &allocator;
    device_ = &device;
    resolve_ = &resolve;
    desc_ = desc;
    desc_.max_emitters = std::max(desc.max_emitters, 1U);
    width_ = resolve.desc_.width;
    height_ = resolve.desc_.height;
    probe_slots_ = resolve.cache_->desc_.max_probes;
    hardware_pipeline_ = resolve.cache_->hardware_pipelines_;
    Status made = create_layouts();
    if (made) {
        made = create_pipelines();
    }
    if (made) {
        made = create_resources();
    }
    if (!made) {
        destroy();
    }
    return made;
}

Status GpuGiDebugView::create_layouts() noexcept {
    rhi::DescriptorBinding bindings[kBindings] = {};
    for (u32 index = 0; index < kBindings; ++index) {
        bindings[index].binding = index;
        bindings[index].kind = (kTextureBindings & (1U << index)) != 0U
                                   ? rhi::DescriptorKind::SampledTexture
                                   : rhi::DescriptorKind::StorageBuffer;
        bindings[index].count = 1;
        bindings[index].stages = rhi::ShaderStage::Compute;
    }
    rhi::DescriptorSetLayoutDescription description;
    description.name = "gi debug view set";
    description.bindings = Span<const rhi::DescriptorBinding>(bindings, kBindings);
    Expected<rhi::DescriptorSetLayoutHandle, Error> layout =
        device_->create_descriptor_set_layout(description);
    if (!layout) {
        return make_unexpected(layout.error());
    }
    layout_ = *layout;
    // The push block every GI dispatch's shaders declare (`GiDispatch`), though no view reads it.
    const rhi::PushConstantRange range{rhi::ShaderStage::Compute, 0,
                                       static_cast<u32>(sizeof(GpuGiScene::Dispatch))};
    const GpuRadianceCache& cache = *resolve_->cache_;
    const rhi::DescriptorSetLayoutHandle sets[4] = {cache.scene_->set_layout_, cache.probe_layout_,
                                                    layout_, cache.hardware_layout_};
    rhi::PipelineLayoutDescription pipeline;
    pipeline.name = "gi debug view layout";
    pipeline.set_layouts = {sets, 3};
    pipeline.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
    Expected<rhi::PipelineLayoutHandle, Error> software = device_->create_pipeline_layout(pipeline);
    if (!software) {
        return make_unexpected(software.error());
    }
    pipeline_layout_ = *software;
    if (!hardware_pipeline_) {
        return ok();
    }
    pipeline.name = "gi debug view hardware layout";
    pipeline.set_layouts = {sets, 4};
    Expected<rhi::PipelineLayoutHandle, Error> hardware = device_->create_pipeline_layout(pipeline);
    if (!hardware) {
        return make_unexpected(hardware.error());
    }
    hardware_pipeline_layout_ = *hardware;
    return ok();
}

Status GpuGiDebugView::create_pipelines() noexcept {
    struct Source {
        const char* name;
        Span<const u32> spirv;
        const char* msl;
        usize msl_size;
        const char* entry;
        u32 group_x;
        u32 group_y;
    };
#define CY_GI_VIEW_SOURCE(label, stem, entry, x, y)                                  \
    Source {                                                                         \
        label, {k##stem##Spirv, sizeof(k##stem##Spirv) / sizeof(u32)}, k##stem##Msl, \
            sizeof(k##stem##Msl) - 1, entry, x, y                                    \
    }
    const Source sources[kPipelineCount] = {
        CY_GI_VIEW_SOURCE("gi debug exposure", GiDebugExposure, "cyGiDebugExposure", kExposureLanes,
                          1),
        CY_GI_VIEW_SOURCE("gi debug view", GiDebugView, "cyGiDebugView", kTileGroup, kTileGroup),
        CY_GI_VIEW_SOURCE("gi debug suspects", GiDebugSuspects, "cyGiDebugSuspects", kTileGroup,
                          kTileGroup),
        CY_GI_VIEW_SOURCE("gi debug view hardware", GiDebugViewHardware, "cyGiDebugViewHardware",
                          kTileGroup, kTileGroup),
    };
#undef CY_GI_VIEW_SOURCE
    // The ray query pipeline exists only where the device has ray queries, as the resolve's does.
    const u32 count =
        hardware_pipeline_ ? static_cast<u32>(kPipelineCount) : static_cast<u32>(kViewHardware);
    for (u32 which = 0; which < count; ++which) {
        const Source& source = sources[which];
        rhi::ShaderModuleBundle bundle;
        bundle.spirv = source.spirv;
        bundle.msl = {reinterpret_cast<const u8*>(source.msl), source.msl_size};
        bundle.spirv_entry_point = "main";
        bundle.msl_entry_point = source.entry;
        rhi::ValidationMessage message;
        auto module =
            rhi::select_shader_module(bundle, device_->capabilities().native_shader_format(),
                                      source.name, rhi::ShaderStage::Compute, message);
        if (!module.has_value()) {
            return make_unexpected(module.error());
        }
        Expected<rhi::ShaderModuleHandle, Error> shader = device_->create_shader_module(*module);
        if (!shader.has_value()) {
            return make_unexpected(shader.error());
        }
        shaders_[which] = *shader;
        rhi::ComputePipelineDescription pipeline;
        pipeline.name = source.name;
        pipeline.layout = which == kViewHardware ? hardware_pipeline_layout_ : pipeline_layout_;
        pipeline.shader = shaders_[which];
        pipeline.workgroup_size[0] = source.group_x;
        pipeline.workgroup_size[1] = source.group_y;
        Expected<rhi::ComputePipelineHandle, Error> created =
            device_->create_compute_pipeline(pipeline);
        if (!created.has_value()) {
            return make_unexpected(created.error());
        }
        pipelines_[which] = *created;
    }
    return ok();
}

Status GpuGiDebugView::create_resources() noexcept {
    const auto make_buffer =
        [this](const char* name, u64 size, rhi::BufferUsage usage,
               rhi::MemoryUse memory) noexcept -> Expected<rhi::BufferHandle, Error> {
        rhi::BufferDescription description;
        description.name = name;
        description.size = size;
        description.usage = usage;
        description.memory = memory;
        Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(description);
        if (buffer) {
            if (void* mapped = device_->buffer_mapped_pointer(*buffer); mapped != nullptr) {
                std::memset(mapped, 0, static_cast<usize>(size));
            }
        }
        return buffer;
    };
    for (u32 slot = 0; slot < rhi::kMaxFramesInFlight; ++slot) {
        auto words = make_buffer("gi debug view words", kWordBytes, rhi::BufferUsage::Storage,
                                 rhi::MemoryUse::HostVisibleDeviceLocal);
        if (!words) {
            return make_unexpected(words.error());
        }
        words_[slot] = *words;
        auto emitters =
            make_buffer("gi debug view emitters", u64{desc_.max_emitters} * kLightFloats * 4U,
                        rhi::BufferUsage::Storage, rhi::MemoryUse::HostVisibleDeviceLocal);
        if (!emitters) {
            return make_unexpected(emitters.error());
        }
        emitters_[slot] = *emitters;
    }
    const u64 pixels = u64{width_} * height_;
    const auto device_only = rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSource;
    struct Request {
        const char* name;
        u64 size;
        rhi::BufferUsage usage;
        rhi::MemoryUse memory;
        bool host_copy;
    };
    const Request requests[kBufferCount] = {
        {"gi debug suspects", u64{std::max(probe_slots_, 1U)} * 4U, device_only,
         rhi::MemoryUse::DeviceLocal, false},
        {"gi debug exposure", 16U, device_only, rhi::MemoryUse::DeviceLocal, false},
        {"gi debug output", pixels * 4U, device_only, rhi::MemoryUse::DeviceLocal, false},
        {"gi debug output host", pixels * 4U, rhi::BufferUsage::TransferDestination,
         rhi::MemoryUse::Readback, true},
        {"gi debug exposure host", 16U, rhi::BufferUsage::TransferDestination,
         rhi::MemoryUse::Readback, true},
    };
    for (u32 index = 0; index < kBufferCount; ++index) {
        sizes_[index] = requests[index].size;
        if (requests[index].host_copy && !desc_.readback) {
            continue;
        }
        auto buffer = make_buffer(requests[index].name, requests[index].size, requests[index].usage,
                                  requests[index].memory);
        if (!buffer) {
            return make_unexpected(buffer.error());
        }
        buffers_[index] = *buffer;
    }
    rhi::TextureDescription texture;
    texture.name = "gi debug view";
    texture.format = rhi::Format::Rgba8Unorm;
    texture.extent = rhi::Extent3D{width_, height_, 1};
    texture.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDestination;
    Expected<rhi::TextureHandle, Error> made = device_->create_texture(texture);
    if (!made) {
        return make_unexpected(made.error());
    }
    texture_ = *made;
    rhi::TextureViewDescription view;
    view.name = "gi debug view";
    view.texture = texture_;
    Expected<rhi::TextureViewHandle, Error> made_view = device_->create_texture_view(view);
    if (!made_view) {
        return make_unexpected(made_view.error());
    }
    texture_view_ = *made_view;
    return ok();
}

void GpuGiDebugView::destroy() noexcept {
    if (device_ == nullptr) {
        return;
    }
    for (rhi::BufferHandle* ring : {words_, emitters_}) {
        for (u32 slot = 0; slot < rhi::kMaxFramesInFlight; ++slot) {
            if (ring[slot]) {
                device_->destroy_buffer(ring[slot]);
            }
            ring[slot] = {};
        }
    }
    for (rhi::BufferHandle& buffer : buffers_) {
        if (buffer) {
            device_->destroy_buffer(buffer);
        }
        buffer = {};
    }
    for (u32 which = 0; which < kPipelineCount; ++which) {
        if (pipelines_[which]) {
            device_->destroy_compute_pipeline(pipelines_[which]);
        }
        if (shaders_[which]) {
            device_->destroy_shader_module(shaders_[which]);
        }
        pipelines_[which] = {};
        shaders_[which] = {};
    }
    for (rhi::PipelineLayoutHandle* layout : {&pipeline_layout_, &hardware_pipeline_layout_}) {
        if (*layout) {
            device_->destroy_pipeline_layout(*layout);
        }
        *layout = {};
    }
    if (layout_) {
        device_->destroy_descriptor_set_layout(layout_);
    }
    layout_ = {};
    if (texture_view_) {
        device_->destroy_texture_view(texture_view_);
    }
    texture_view_ = {};
    if (texture_) {
        device_->destroy_texture(texture_);
    }
    texture_ = {};
    view_ = gi::GiDebugView::Count;
    began_ = false;
    resolve_ = nullptr;
    device_ = nullptr;
    allocator_ = nullptr;
}

// --- One frame -----------------------------------------------------------------------------------

Status GpuGiDebugView::write_emitters(const gi::IlluminationSystem& system) noexcept {
    const Span<const gi::GiLight> lights = system.emitters().lights();
    if (lights.size() > desc_.max_emitters) {
        return fail(ErrorCode::OutOfRange,
                    "the device GI views were created for fewer promoted emitters than the system "
                    "holds; raise max_emitters");
    }
    auto* out = static_cast<f32*>(device_->buffer_mapped_pointer(emitters_[ring_]));
    if (out == nullptr) {
        return fail(ErrorCode::Internal, "the GI views' emitter buffer is not mapped");
    }
    for (usize index = 0; index < lights.size(); ++index) {
        put_light(lights[index], out + (index * kLightFloats));
    }
    return ok();
}

Status GpuGiDebugView::prepare(const gi::IlluminationSystem& system, Vec3 camera, u64 frame,
                               const GpuGiDebugViewSettings& settings) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the device GI views are not created");
    }
    view_ = gi::GiDebugView::Count;
    texture_resource_ = kInvalidResource;
    if (settings.view == gi::GiDebugView::Count) {
        return ok();
    }
    if (!draws(settings.view, system)) {
        return fail(ErrorCode::Unsupported,
                    "the device does not hold this GI view's data; draw it on the host");
    }
    if (system.radiance().probes().size() > probe_slots_) {
        return fail(ErrorCode::OutOfRange,
                    "the radiance cache holds more probe slots than the device views address");
    }
    if (began_) {
        ring_ = (ring_ + 1U) % rhi::kMaxFramesInFlight;
    }
    began_ = true;
    if (Status written = write_emitters(system); !written) {
        return written;
    }
    const gi::RadianceCache& cache = system.radiance();
    const gi::ProbeCacheSettings& probes = cache.settings();
    u32 words[kWordCount] = {};
    words[kWordView + 0] = static_cast<u32>(settings.view);
    words[kWordView + 1] = width_;
    words[kWordView + 2] = height_;
    words[kWordView + 3] = resolve_->divisor();
    words[kWordFrame + 0] = bits(settings.exposure);
    words[kWordFrame + 1] = static_cast<u32>(frame & 0xFFFFFFFFULL);
    words[kWordFrame + 2] = static_cast<u32>(std::min<u64>(settings.max_age_frames, 0xFFFFFFFFULL));
    words[kWordFrame + 3] = static_cast<u32>(cache.probes().size());
    words[kWordCamera + 0] = bits(camera.x);
    words[kWordCamera + 1] = bits(camera.y);
    words[kWordCamera + 2] = bits(camera.z);
    words[kWordCamera + 3] = bits(settings.probe_disc_metres);
    words[kWordField + 0] = bits(settings.field_slice_height);
    words[kWordField + 1] = static_cast<u32>(system.emitters().lights().size());
    words[kWordField + 2] = 0;
    words[kWordField + 3] = 0;
    words[kWordCache + 0] = bits(cache.camera().x);
    words[kWordCache + 1] = bits(cache.camera().y);
    words[kWordCache + 2] = bits(cache.camera().z);
    words[kWordCache + 3] = probes.levels;
    words[kWordLayout + 0] = bits(probes.base_spacing_metres);
    words[kWordLayout + 1] = probes.half_extent_probes;
    void* mapped = device_->buffer_mapped_pointer(words_[ring_]);
    if (mapped == nullptr) {
        return fail(ErrorCode::Internal, "the GI views' words are not mapped");
    }
    std::memcpy(mapped, words, sizeof(words));
    // The tier and ray-count views trace the frame's own world tier.
    hardware_ =
        hardware_pipeline_ && resolve_->cache_->world_tier() == gi::RadianceSource::HardwareTrace;
    view_ = settings.view;
    return ok();
}

ResourceId GpuGiDebugView::import_buffer(RenderGraph& graph, rhi::BufferHandle buffer, u64 size,
                                         const char* name) noexcept {
    BufferRequest request;
    request.name = name;
    request.size = size;
    return graph.import_buffer_once(request, buffer);
}

ResourceId GpuGiDebugView::import_texture(RenderGraph& graph) noexcept {
    TextureRequest request;
    request.name = "gi debug view";
    request.format = rhi::Format::Rgba8Unorm;
    request.width = width_;
    request.height = height_;
    request.extra_usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDestination;
    // UNDEFINED EVERY FRAME: the copy rewrites every texel.
    texture_resource_ = graph.import_texture(request, texture_, rhi::ImageUse::Undefined);
    return texture_resource_;
}

/// Every resource a view pass reads beyond its own: the resolve's words, surfaces, history and
/// targets, and the scene and probe set the cache bound.
void GpuGiDebugView::read_frame(RenderGraph& graph, PassBuilder& pass) noexcept {
    GpuScreenResolve& resolve = *resolve_;
    GpuRadianceCache& cache = *resolve.cache_;
    pass.read(resolve.import_constants(graph), Access::ComputeStorageRead)
        .read(resolve.import(graph, GpuScreenResolve::kSurfaces), Access::ComputeStorageRead)
        .read(resolve.import(graph, GpuScreenResolve::kSources), Access::ComputeStorageRead)
        .read(diffuse_, Access::ComputeSampledRead)
        .read(specular_, Access::ComputeSampledRead)
        .read(import_buffer(graph, words_[ring_], kWordBytes, "gi debug view words"),
              Access::ComputeStorageRead)
        .read(import_buffer(graph, emitters_[ring_], u64{desc_.max_emitters} * kLightFloats * 4U,
                            "gi debug view emitters"),
              Access::ComputeStorageRead);
    for (const GpuRadianceCache::Binding binding :
         {GpuRadianceCache::kProbes, GpuRadianceCache::kPayloads, GpuRadianceCache::kLayout}) {
        pass.read(cache.import(graph, binding), Access::ComputeStorageRead);
    }
    cache.read_scene(graph, pass, hardware_);
}

Status GpuGiDebugView::declare(RenderGraph& graph) noexcept {
    if (device_ == nullptr || view_ == gi::GiDebugView::Count) {
        return ok();
    }
    diffuse_ = resolve_->imported_diffuse_;
    specular_ = resolve_->imported_specular_;
    if (diffuse_ == kInvalidResource || specular_ == kInvalidResource) {
        return fail(ErrorCode::InvalidArgument,
                    "the GI views read the resolve's targets; import them into this frame first");
    }
    const ResourceId suspects =
        import_buffer(graph, buffers_[kSuspectFlags], sizes_[kSuspectFlags], "gi debug suspects");
    const ResourceId exposure =
        import_buffer(graph, buffers_[kExposureWord], sizes_[kExposureWord], "gi debug exposure");
    const ResourceId output =
        import_buffer(graph, buffers_[kOutput], sizes_[kOutput], "gi debug output");

    steps_[0] = Step{this, kExposure, 1, 1};
    PassBuilder measured = graph.add_pass("gi debug exposure", QueueKind::Graphics);
    read_frame(graph, measured);
    measured.write(suspects, Access::ComputeStorageWrite)
        .write(exposure, Access::ComputeStorageWrite)
        .record(&record_step, &steps_[0]);

    steps_[1] =
        Step{this, hardware_ ? kViewHardware : kView, tile_groups(width_), tile_groups(height_)};
    PassBuilder drawn = graph.add_pass("gi debug view", QueueKind::Graphics);
    read_frame(graph, drawn);
    drawn.read(exposure, Access::ComputeStorageRead)
        .write(suspects, Access::ComputeStorageReadWrite)
        .write(output, Access::ComputeStorageWrite)
        .record(&record_step, &steps_[1]);

    if (view_ == gi::GiDebugView::ProbeVisibility) {
        steps_[2] = Step{this, kSuspects, tile_groups(width_), tile_groups(height_)};
        PassBuilder discs = graph.add_pass("gi debug suspects", QueueKind::Graphics);
        read_frame(graph, discs);
        discs.read(exposure, Access::ComputeStorageRead)
            .read(suspects, Access::ComputeStorageRead)
            .write(output, Access::ComputeStorageReadWrite)
            .record(&record_step, &steps_[2]);
    }

    // The texture the caller imported for the opaque pass's read, so the graph orders the copy
    // before it; imported here only when no caller asked (a test that reads the words alone).
    const ResourceId texture =
        texture_resource_ != kInvalidResource ? texture_resource_ : import_texture(graph);
    graph.add_pass("gi debug view copy", QueueKind::Graphics)
        .read(output, Access::TransferRead)
        .write(texture, Access::TransferWrite)
        .record(&record_texture_copy, this);

    if (desc_.readback) {
        copies_[0] = Copy{this, kOutput, kOutputHost, sizes_[kOutput]};
        copies_[1] = Copy{this, kExposureWord, kExposureHost, sizes_[kExposureWord]};
        const char* names[2] = {"gi debug output readback", "gi debug exposure readback"};
        for (u32 index = 0; index < 2; ++index) {
            const ResourceId host = import_buffer(graph, buffers_[copies_[index].destination],
                                                  copies_[index].bytes, names[index]);
            graph.add_pass(names[index], QueueKind::Graphics)
                .read(index == 0 ? output : exposure, Access::TransferRead)
                .write(host, Access::TransferWrite)
                .record(&record_copy, &copies_[index]);
            graph.add_pass(names[index], QueueKind::Graphics)
                .read(host, Access::HostRead)
                .side_effect();
        }
    }
    return graph.status();
}

// --- Recording -----------------------------------------------------------------------------------

Expected<rhi::DescriptorSetHandle, Error> GpuGiDebugView::transient_set(
    const PassContext& context) noexcept {
    GpuScreenResolve& resolve = *resolve_;
    const rhi::DescriptorWrite writes[kBindings] = {
        buffer_write(0, resolve.constants()),
        buffer_write(1, resolve.buffers_[GpuScreenResolve::kSurfaces]),
        buffer_write(2, resolve.buffers_[GpuScreenResolve::kSources]),
        sampled_write(3, context.executor->view(diffuse_)),
        sampled_write(4, context.executor->view(specular_)),
        buffer_write(5, words_[ring_]),
        buffer_write(6, emitters_[ring_]),
        buffer_write(7, buffers_[kSuspectFlags]),
        buffer_write(8, buffers_[kExposureWord]),
        buffer_write(9, buffers_[kOutput]),
    };
    Expected<rhi::DescriptorSetHandle, Error> set = device_->allocate_descriptor_set(layout_, true);
    if (!set) {
        return set;
    }
    if (Status written = device_->update_descriptor_set(
            *set, Span<const rhi::DescriptorWrite>(writes, kBindings));
        !written) {
        return make_unexpected(written.error());
    }
    return set;
}

void GpuGiDebugView::record_step(const PassContext& context, void* user) noexcept {
    const auto* step = static_cast<const Step*>(user);
    GpuGiDebugView& self = *step->self;
    Expected<rhi::DescriptorSetHandle, Error> set = self.transient_set(context);
    if (!set) {
        return;
    }
    const GpuRadianceCache& cache = *self.resolve_->cache_;
    const bool hardware = step->pipeline == kViewHardware;
    const rhi::PipelineLayoutHandle layout =
        hardware ? self.hardware_pipeline_layout_ : self.pipeline_layout_;
    const rhi::DescriptorSetHandle sets[4] = {cache.scene_->set_, cache.probe_set_, *set,
                                              cache.hardware_set_};
    rhi::CommandBuffer& commands = *context.commands;
    commands.bind_compute_pipeline(self.pipelines_[step->pipeline]);
    commands.bind_descriptor_sets(layout, 0,
                                  Span<const rhi::DescriptorSetHandle>(sets, hardware ? 4U : 3U));
    const GpuGiScene::Dispatch dispatch{};
    commands.push_constants(
        layout, rhi::ShaderStage::Compute, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&dispatch), sizeof(dispatch)));
    commands.dispatch(step->groups_x, step->groups_y, 1);
}

void GpuGiDebugView::record_texture_copy(const PassContext& context, void* user) noexcept {
    const auto* self = static_cast<const GpuGiDebugView*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{self->width_, self->height_, 1};
    context.commands->copy_buffer_to_texture(self->buffers_[kOutput], self->texture_,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

void GpuGiDebugView::record_copy(const PassContext& context, void* user) noexcept {
    const auto* copy = static_cast<const Copy*>(user);
    const rhi::BufferCopy region{0, 0, copy->bytes};
    context.commands->copy_buffer(copy->self->buffers_[copy->source],
                                  copy->self->buffers_[copy->destination],
                                  Span<const rhi::BufferCopy>(&region, 1));
}

Status GpuGiDebugView::read_back(Span<u32> out) const noexcept {
    const usize pixels = static_cast<usize>(width_) * height_;
    const void* data = desc_.readback && buffers_[kOutputHost]
                           ? device_->buffer_mapped_pointer(buffers_[kOutputHost])
                           : nullptr;
    if (data == nullptr || out.size() != pixels) {
        return fail(ErrorCode::InvalidArgument,
                    "read_back needs `readback` and one word a frame pixel");
    }
    std::memcpy(out.data(), data, pixels * sizeof(u32));
    return ok();
}

Expected<f32, Error> GpuGiDebugView::read_back_exposure() const noexcept {
    const void* data = desc_.readback && buffers_[kExposureHost]
                           ? device_->buffer_mapped_pointer(buffers_[kExposureHost])
                           : nullptr;
    if (data == nullptr) {
        return fail(ErrorCode::InvalidArgument, "read_back_exposure needs `readback`");
    }
    f32 exposure = 0.0F;
    std::memcpy(&exposure, data, sizeof(exposure));
    return exposure;
}

}  // namespace cy::rendering::gi_gpu
