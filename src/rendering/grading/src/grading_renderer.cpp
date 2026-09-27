// SPDX-License-Identifier: MIT
// Exposure and colour grading on the device. See the header for how it reaches a frame.

#include <cy/rendering/grading/grading_renderer.h>

#include <cy/backends/rhi/command_buffer.h>
#include <cy/backends/rhi/validation.h>
#include <cy/core/math/scalar.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/post/exposure.h>
#include <cy/rendering/post/lut.h>

#include <cmath>
#include <cstring>

#include "grading_msl.h"
#include "grading_spirv.h"

namespace cy::rendering::grading {
namespace {

using rhi::Access;
using rhi::QueueKind;

constexpr u32 kClear = 0;
constexpr u32 kHistogram = 1;
constexpr u32 kAdapt = 2;
constexpr u32 kComputeCount = 3;

/// graded_resolve.slang's set: scene colour, sampler, table, exposure state.
constexpr u32 kResolveBindings = 4;
constexpr u32 kHistogramGroup = 16;
constexpr u64 kHistogramBytes = kExposureBins * sizeof(u32);
constexpr u64 kStateBytes = 4 * sizeof(f32);
constexpr rhi::Format kTableFormat = rhi::Format::Rgba16Sfloat;

template <typename T, usize N>
[[nodiscard]] Span<const u32> words(const T (&module)[N]) noexcept {
    return Span<const u32>(module, N);
}

template <usize N>
[[nodiscard]] Span<const u8> text(const char (&source)[N]) noexcept {
    return {reinterpret_cast<const u8*>(source), N - 1};
}

/// IEEE 754 binary32 to binary16, rounding to nearest even. The table is `Rgba16Sfloat` and its
/// values are encodings in [0, 1], so neither overflow nor a subnormal result needs more than the
/// plain path; both are handled anyway rather than assumed away.
[[nodiscard]] u16 to_half(f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const u32 sign = (bits >> 16U) & 0x8000U;
    const i32 exponent = static_cast<i32>((bits >> 23U) & 0xFFU) - 127 + 15;
    u32 mantissa = bits & 0x7FFFFFU;
    if (exponent >= 31) {
        return static_cast<u16>(sign | 0x7C00U);
    }
    if (exponent <= 0) {
        if (exponent < -10) {
            return static_cast<u16>(sign);
        }
        mantissa |= 0x800000U;
        const u32 shift = static_cast<u32>(14 - exponent);
        u32 half = mantissa >> shift;
        const u32 remainder = mantissa & ((1U << shift) - 1U);
        const u32 midpoint = 1U << (shift - 1U);
        if (remainder > midpoint || (remainder == midpoint && (half & 1U) != 0U)) {
            ++half;
        }
        return static_cast<u16>(sign | half);
    }
    u32 half = (static_cast<u32>(exponent) << 10U) | (mantissa >> 13U);
    const u32 remainder = mantissa & 0x1FFFU;
    if (remainder > 0x1000U || (remainder == 0x1000U && (half & 1U) != 0U)) {
        ++half;
    }
    return static_cast<u16>(sign | half);
}

[[nodiscard]] rhi::DescriptorBinding binding(u32 index, rhi::DescriptorKind kind,
                                             rhi::ShaderStage stages) noexcept {
    rhi::DescriptorBinding entry;
    entry.binding = index;
    entry.kind = kind;
    entry.count = 1;
    entry.stages = stages;
    return entry;
}

[[nodiscard]] rhi::DescriptorWrite sampled(u32 index, rhi::TextureViewHandle view) noexcept {
    rhi::DescriptorWrite write;
    write.binding = index;
    write.kind = rhi::DescriptorKind::SampledTexture;
    write.texture_view = view;
    write.use = rhi::ImageUse::SampledRead;
    return write;
}

[[nodiscard]] rhi::DescriptorWrite storage(u32 index, rhi::BufferHandle buffer,
                                           u64 bytes) noexcept {
    rhi::DescriptorWrite write;
    write.binding = index;
    write.kind = rhi::DescriptorKind::StorageBuffer;
    write.buffer = buffer;
    write.buffer_range = bytes;
    return write;
}

[[nodiscard]] u32 groups(u32 extent) noexcept {
    return (extent + kHistogramGroup - 1U) / kHistogramGroup;
}

/// The identity at the corners of a 2³ table: every lattice point is its own encoding.
void identity_table(Vec3 (&out)[8]) noexcept {
    for (u32 index = 0; index < 8; ++index) {
        out[index] = Vec3{static_cast<f32>(index & 1U), static_cast<f32>((index >> 1U) & 1U),
                          static_cast<f32>((index >> 2U) & 1U)};
    }
}

struct TableUpload {
    rhi::BufferHandle staging;
    rhi::TextureHandle texture;
    u32 size = 0;
};

void record_table_upload(const PassContext& context, void* user) noexcept {
    const auto* upload = static_cast<const TableUpload*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{upload->size, upload->size, upload->size};
    context.commands->copy_buffer_to_texture(upload->staging, upload->texture,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

/// Copy the texture's contents in through a graph of its own, and leave it in the sampled layout
/// the resolve reads it in. The graph derives both transitions; nothing here names a layout.
[[nodiscard]] Status run_table_upload(rhi::Device& device, Allocator& allocator,
                                      TableUpload& upload) noexcept {
    RenderGraph graph(allocator);
    TextureRequest request;
    request.name = "grading table";
    request.format = kTableFormat;
    request.width = upload.size;
    request.height = upload.size;
    request.depth = upload.size;
    request.dimension = rhi::TextureDimension::Texture3D;
    const ResourceId table =
        graph.import_texture(request, upload.texture, rhi::ImageUse::Undefined);
    graph.add_pass("grading table upload", QueueKind::Graphics)
        .write(table, Access::TransferWrite)
        .record(&record_table_upload, &upload);
    graph.add_pass("grading table residency", QueueKind::Graphics)
        .read(table, Access::FragmentSampledRead)
        .side_effect();
    if (Status declared = graph.status(); !declared) {
        return declared;
    }
    const Expected<u32, Error> began = device.begin_frame();
    if (!began.has_value()) {
        return make_unexpected(began.error());
    }
    Status executed = ok();
    {
        GraphExecutor executor(allocator, device);
        if (auto result = executor.execute(graph, CompileOptions{}, ExecuteOptions{}); !result) {
            executed = make_unexpected(result.error());
        } else {
            executed = device.wait_idle();
        }
        executor.release();
    }
    if (Status ended = device.end_frame(); !ended && executed) {
        executed = ended;
    }
    return executed;
}

[[nodiscard]] GradingRenderer& self_of(void* user) noexcept {
    return *static_cast<GradingRenderer*>(user);
}

void record_resolve_step(const PassContext& context, void* user) noexcept {
    self_of(user).record_resolve(context);
}
void record_clear_step(const PassContext& context, void* user) noexcept {
    self_of(user).record_clear(context);
}
void record_histogram_step(const PassContext& context, void* user) noexcept {
    self_of(user).record_histogram(context);
}
void record_adapt_step(const PassContext& context, void* user) noexcept {
    self_of(user).record_adapt(context);
}
void record_readback_step(const PassContext& context, void* user) noexcept {
    self_of(user).record_readback(context);
}

}  // namespace

const char* resolve_curve_name(ResolveCurve curve) noexcept {
    switch (curve) {
        case ResolveCurve::None:
            return "none";
        case ResolveCurve::Reinhard:
            return "reinhard";
        case ResolveCurve::AcesFit:
            return "aces-fit";
        case ResolveCurve::Count:
            break;
    }
    return "unknown";
}

ExposureConstants exposure_constants(const AutoExposureSettings& settings, f32 delta_seconds,
                                     bool restart, f32 restart_ev100, u32 width,
                                     u32 height) noexcept {
    ExposureConstants constants;
    constants.histogram[0] = settings.min_ev;
    constants.histogram[1] = settings.max_ev;
    constants.histogram[2] = settings.low_percentile;
    constants.histogram[3] = settings.high_percentile;
    constants.limits[0] = settings.min_ev;
    constants.limits[1] = settings.max_ev;
    constants.limits[2] = settings.speed_brightening;
    constants.limits[3] = settings.speed_darkening;
    for (u32 index = 0; index < ExposureCompensationCurve::kPoints; ++index) {
        constants.curve_ev[index] = settings.compensation.metered_ev[index];
        constants.curve_compensation[index] = settings.compensation.compensation[index];
    }
    constants.frame[0] = delta_seconds;
    constants.frame[1] = restart ? 1.0F : 0.0F;
    constants.frame[2] = restart_ev100;
    constants.extent[0] = width;
    constants.extent[1] = height;
    return constants;
}

GradingRenderer::~GradingRenderer() {
    shutdown();
}

Status GradingRenderer::create_modules() noexcept {
    struct Request {
        const char* name;
        rhi::ShaderStage stage;
        const char* entry;
        Span<const u32> spirv;
        Span<const u8> msl;
        rhi::ShaderModuleHandle* out;
    };
    // The vertex stage is the library's `fullscreenVertex`, compiled from cy/fullscreen.slang by
    // shaders/regenerate.py: the frame resolve's triangle, not a second one.
    const Request requests[] = {
        {"cy grading vertex", rhi::ShaderStage::Vertex, "fullscreenVertex",
         words(kGradingVertexSpirv), text(kGradingVertexMsl), &vertex_},
        {"cy graded resolve", rhi::ShaderStage::Fragment, "cyGradedResolve",
         words(kGradedResolveSpirv), text(kGradedResolveMsl), &fragment_},
        {"cy exposure clear", rhi::ShaderStage::Compute, "cyExposureClear",
         words(kExposureClearSpirv), text(kExposureClearMsl), &compute_[kClear]},
        {"cy exposure histogram", rhi::ShaderStage::Compute, "cyExposureHistogram",
         words(kExposureHistogramSpirv), text(kExposureHistogramMsl), &compute_[kHistogram]},
        {"cy exposure adapt", rhi::ShaderStage::Compute, "cyExposureAdapt",
         words(kExposureAdaptSpirv), text(kExposureAdaptMsl), &compute_[kAdapt]},
    };
    for (const Request& request : requests) {
        rhi::ShaderModuleBundle bundle;
        bundle.spirv = request.spirv;
        bundle.msl = request.msl;
        bundle.spirv_entry_point = "main";
        bundle.msl_entry_point = request.entry;
        rhi::ValidationMessage message;
        auto module =
            rhi::select_shader_module(bundle, device_->capabilities().native_shader_format(),
                                      request.name, request.stage, message);
        if (!module.has_value()) {
            return make_unexpected(module.error());
        }
        Expected<rhi::ShaderModuleHandle, Error> made = device_->create_shader_module(*module);
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        *request.out = *made;
    }
    return ok();
}

Status GradingRenderer::create_layouts() noexcept {
    struct SetRequest {
        const char* name;
        Span<const rhi::DescriptorBinding> bindings;
        rhi::ShaderStage stages;
        u32 push_bytes;
        rhi::DescriptorSetLayoutHandle* set;
        rhi::PipelineLayoutHandle* layout;
    };
    constexpr rhi::ShaderStage kFragment = rhi::ShaderStage::Fragment;
    constexpr rhi::ShaderStage kCompute = rhi::ShaderStage::Compute;
    const rhi::DescriptorBinding resolve[] = {
        binding(0, rhi::DescriptorKind::SampledTexture, kFragment),
        binding(1, rhi::DescriptorKind::Sampler, kFragment),
        binding(2, rhi::DescriptorKind::SampledTexture, kFragment),
        binding(3, rhi::DescriptorKind::StorageBuffer, kFragment),
    };
    const rhi::DescriptorBinding clear[] = {
        binding(0, rhi::DescriptorKind::StorageBuffer, kCompute)};
    const rhi::DescriptorBinding histogram[] = {
        binding(0, rhi::DescriptorKind::SampledTexture, kCompute),
        binding(1, rhi::DescriptorKind::StorageBuffer, kCompute),
    };
    const rhi::DescriptorBinding adapt[] = {
        binding(0, rhi::DescriptorKind::StorageBuffer, kCompute),
        binding(1, rhi::DescriptorKind::StorageBuffer, kCompute),
    };
    const SetRequest requests[] = {
        {"cy graded resolve",
         {resolve, kResolveBindings},
         kFragment,
         static_cast<u32>(sizeof(GradedResolvePush)),
         &resolve_set_,
         &resolve_layout_},
        {"cy exposure clear",
         {clear, 1},
         kCompute,
         static_cast<u32>(sizeof(ExposureConstants)),
         &compute_sets_[kClear],
         &compute_layouts_[kClear]},
        {"cy exposure histogram",
         {histogram, 2},
         kCompute,
         static_cast<u32>(sizeof(ExposureConstants)),
         &compute_sets_[kHistogram],
         &compute_layouts_[kHistogram]},
        {"cy exposure adapt",
         {adapt, 2},
         kCompute,
         static_cast<u32>(sizeof(ExposureConstants)),
         &compute_sets_[kAdapt],
         &compute_layouts_[kAdapt]},
    };
    for (const SetRequest& request : requests) {
        rhi::DescriptorSetLayoutDescription set;
        set.name = request.name;
        set.bindings = request.bindings;
        Expected<rhi::DescriptorSetLayoutHandle, Error> made =
            device_->create_descriptor_set_layout(set);
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        *request.set = *made;
        const rhi::PushConstantRange range{request.stages, 0, request.push_bytes};
        rhi::PipelineLayoutDescription layout;
        layout.name = request.name;
        layout.set_layouts = Span<const rhi::DescriptorSetLayoutHandle>(request.set, 1);
        layout.push_constants = Span<const rhi::PushConstantRange>(&range, 1);
        Expected<rhi::PipelineLayoutHandle, Error> created =
            device_->create_pipeline_layout(layout);
        if (!created.has_value()) {
            return make_unexpected(created.error());
        }
        *request.layout = *created;
    }

    // The frame resolve's own sampler state: clamped, linear, and at a pixel centre an exact texel.
    rhi::SamplerDescription sampler;
    sampler.name = "cy grading linear clamp";
    sampler.address_u = rhi::AddressMode::ClampToEdge;
    sampler.address_v = rhi::AddressMode::ClampToEdge;
    sampler.address_w = rhi::AddressMode::ClampToEdge;
    Expected<rhi::SamplerHandle, Error> made = device_->create_sampler(sampler);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    sampler_ = *made;
    return ok();
}

Status GradingRenderer::create_pipelines() noexcept {
    rhi::ColorAttachmentState color;
    color.format = description_.output_format;
    const rhi::SpecializationConstant curve{0, static_cast<u32>(description_.curve)};
    rhi::GraphicsPipelineDescription resolve;
    resolve.name = "graded resolve";
    resolve.layout = resolve_layout_;
    resolve.vertex_shader = vertex_;
    resolve.fragment_shader = fragment_;
    resolve.color_attachments = Span<const rhi::ColorAttachmentState>(&color, 1);
    resolve.specialization = Span<const rhi::SpecializationConstant>(&curve, 1);
    resolve.rasterisation.cull_mode = rhi::CullMode::None;
    resolve.depth_stencil.format = rhi::Format::Undefined;
    resolve.depth_stencil.depth_test_enable = false;
    resolve.depth_stencil.depth_write_enable = false;
    Expected<rhi::GraphicsPipelineHandle, Error> graphics =
        device_->create_graphics_pipeline(resolve);
    if (!graphics.has_value()) {
        return make_unexpected(graphics.error());
    }
    resolve_ = *graphics;

    const char* names[kComputeCount] = {"exposure clear", "exposure histogram", "exposure adapt"};
    const u32 sizes[kComputeCount][2] = {
        {kExposureBins, 1}, {kHistogramGroup, kHistogramGroup}, {1, 1}};
    for (u32 which = 0; which < kComputeCount; ++which) {
        rhi::ComputePipelineDescription pipeline;
        pipeline.name = names[which];
        pipeline.layout = compute_layouts_[which];
        pipeline.shader = compute_[which];
        pipeline.workgroup_size[0] = sizes[which][0];
        pipeline.workgroup_size[1] = sizes[which][1];
        Expected<rhi::ComputePipelineHandle, Error> made =
            device_->create_compute_pipeline(pipeline);
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        compute_pipelines_[which] = *made;
    }
    return ok();
}

Status GradingRenderer::create_buffers() noexcept {
    struct Request {
        const char* name;
        u64 bytes;
        rhi::BufferUsage usage;
        rhi::MemoryUse memory;
        rhi::BufferHandle* out;
    };
    const rhi::BufferUsage device_usage =
        rhi::BufferUsage::Storage | rhi::BufferUsage::TransferSource;
    const Request requests[] = {
        {"exposure histogram", kHistogramBytes, device_usage, rhi::MemoryUse::DeviceLocal,
         &histogram_},
        {"exposure state", kStateBytes, device_usage, rhi::MemoryUse::DeviceLocal, &state_},
        {"exposure histogram readback", kHistogramBytes, rhi::BufferUsage::TransferDestination,
         rhi::MemoryUse::Readback, &histogram_readback_},
        {"exposure state readback", kStateBytes, rhi::BufferUsage::TransferDestination,
         rhi::MemoryUse::Readback, &state_readback_},
    };
    const usize count = description_.readback ? 4U : 2U;
    for (usize index = 0; index < count; ++index) {
        const Request& request = requests[index];
        rhi::BufferDescription buffer;
        buffer.name = request.name;
        buffer.size = request.bytes;
        buffer.usage = request.usage;
        buffer.memory = request.memory;
        Expected<rhi::BufferHandle, Error> made = device_->create_buffer(buffer);
        if (!made.has_value()) {
            return make_unexpected(made.error());
        }
        *request.out = *made;
    }
    return ok();
}

Status GradingRenderer::initialize(rhi::Device& device, Allocator& allocator,
                                   const GradingRendererDescription& description) noexcept {
    if (ready_) {
        return fail(ErrorCode::InvalidArgument, "grading: already initialized");
    }
    if (!device.capabilities().has(rhi::Capability::ComputeShaders)) {
        return fail(ErrorCode::Unsupported,
                    "grading: auto-exposure meters with compute dispatches and this device has "
                    "no compute stage");
    }
    if (description.curve == ResolveCurve::Count) {
        return fail(ErrorCode::InvalidArgument, "grading: the resolve curve is not one of three");
    }
    device_ = &device;
    allocator_ = &allocator;
    description_ = description;
    Status made = create_modules();
    if (made) {
        made = create_layouts();
    }
    if (made) {
        made = create_pipelines();
    }
    if (made) {
        made = create_buffers();
    }
    if (made) {
        Vec3 identity[8];
        identity_table(identity);
        made = upload_table(Span<const Vec3>(identity, 8), 2);
    }
    if (!made) {
        shutdown();
        return made;
    }
    lut_applied_ = false;
    ready_ = true;
    return ok();
}

void GradingRenderer::destroy_table() noexcept {
    if (!table_view_.is_null()) {
        device_->destroy_texture_view(table_view_);
        table_view_ = rhi::TextureViewHandle{};
    }
    if (!table_.is_null()) {
        device_->destroy_texture(table_);
        table_ = rhi::TextureHandle{};
    }
    table_size_ = 0;
}

void GradingRenderer::shutdown() noexcept {
    if (device_ == nullptr) {
        return;
    }
    rhi::Device& device = *device_;
    destroy_table();
    for (rhi::BufferHandle* buffer :
         {&histogram_, &state_, &histogram_readback_, &state_readback_}) {
        if (!buffer->is_null()) {
            device.destroy_buffer(*buffer);
            *buffer = rhi::BufferHandle{};
        }
    }
    for (rhi::ComputePipelineHandle& pipeline : compute_pipelines_) {
        if (!pipeline.is_null()) {
            device.destroy_compute_pipeline(pipeline);
            pipeline = rhi::ComputePipelineHandle{};
        }
    }
    if (!resolve_.is_null()) {
        device.destroy_graphics_pipeline(resolve_);
        resolve_ = rhi::GraphicsPipelineHandle{};
    }
    for (rhi::PipelineLayoutHandle* layout :
         {&resolve_layout_, &compute_layouts_[0], &compute_layouts_[1], &compute_layouts_[2]}) {
        if (!layout->is_null()) {
            device.destroy_pipeline_layout(*layout);
            *layout = rhi::PipelineLayoutHandle{};
        }
    }
    for (rhi::DescriptorSetLayoutHandle* set :
         {&resolve_set_, &compute_sets_[0], &compute_sets_[1], &compute_sets_[2]}) {
        if (!set->is_null()) {
            device.destroy_descriptor_set_layout(*set);
            *set = rhi::DescriptorSetLayoutHandle{};
        }
    }
    if (!sampler_.is_null()) {
        device.destroy_sampler(sampler_);
        sampler_ = rhi::SamplerHandle{};
    }
    for (rhi::ShaderModuleHandle* module :
         {&vertex_, &fragment_, &compute_[0], &compute_[1], &compute_[2]}) {
        if (!module->is_null()) {
            device.destroy_shader_module(*module);
            *module = rhi::ShaderModuleHandle{};
        }
    }
    device_ = nullptr;
    lut_applied_ = false;
    ready_ = false;
}

Status GradingRenderer::upload_table(Span<const Vec3> table, u32 size) noexcept {
    const usize count = static_cast<usize>(size) * size * size;
    if (size < 2 || table.size() != count) {
        return fail(ErrorCode::InvalidArgument, "grading: the table is not its edge cubed");
    }
    rhi::TextureDescription texture;
    texture.name = "grading table";
    texture.dimension = rhi::TextureDimension::Texture3D;
    texture.format = kTableFormat;
    texture.extent = rhi::Extent3D{size, size, size};
    texture.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDestination;
    Expected<rhi::TextureHandle, Error> image = device_->create_texture(texture);
    if (!image.has_value()) {
        return make_unexpected(image.error());
    }
    rhi::BufferDescription staging;
    staging.name = "grading table staging";
    staging.size = count * 4U * sizeof(u16);
    staging.usage = rhi::BufferUsage::TransferSource;
    staging.memory = rhi::MemoryUse::Upload;
    Expected<rhi::BufferHandle, Error> buffer = device_->create_buffer(staging);
    if (!buffer.has_value()) {
        device_->destroy_texture(*image);
        return make_unexpected(buffer.error());
    }
    auto* halves = static_cast<u16*>(device_->buffer_mapped_pointer(*buffer));
    Status uploaded = halves != nullptr
                          ? ok()
                          : fail(ErrorCode::Internal, "grading: the staging buffer is not mapped");
    if (uploaded) {
        for (usize index = 0; index < count; ++index) {
            halves[(index * 4U) + 0U] = to_half(table[index].x);
            halves[(index * 4U) + 1U] = to_half(table[index].y);
            halves[(index * 4U) + 2U] = to_half(table[index].z);
            halves[(index * 4U) + 3U] = to_half(1.0F);
        }
        TableUpload upload{*buffer, *image, size};
        uploaded = run_table_upload(*device_, *allocator_, upload);
    }
    device_->destroy_buffer(*buffer);
    if (!uploaded) {
        device_->destroy_texture(*image);
        return uploaded;
    }
    rhi::TextureViewDescription view;
    view.name = "grading table";
    view.texture = *image;
    view.dimension = rhi::TextureDimension::Texture3D;
    Expected<rhi::TextureViewHandle, Error> made = device_->create_texture_view(view);
    if (!made.has_value()) {
        device_->destroy_texture(*image);
        return make_unexpected(made.error());
    }
    // The previous table is idle: the upload above waited for the device.
    destroy_table();
    table_ = *image;
    table_view_ = *made;
    table_size_ = size;
    return ok();
}

Status GradingRenderer::set_lut(Span<const Vec3> table, u32 size, bool& applied) noexcept {
    applied = false;
    if (!ready_) {
        return fail(ErrorCode::Unavailable, "grading: not initialized");
    }
    const usize count = static_cast<usize>(size) * size * size;
    if (size < 2 || table.size() != count) {
        return fail(ErrorCode::InvalidArgument, "grading: the table is not its edge cubed");
    }
    // AN IDENTITY IS NOT APPLIED. The lookup could only round, and a frame graded neutrally is then
    // the frame without grading, byte for byte — which is what makes "neutral" mean something.
    if (display_lut_is_identity(table.data(), size)) {
        lut_applied_ = false;
        return ok();
    }
    if (Status uploaded = upload_table(table, size); !uploaded) {
        return uploaded;
    }
    lut_applied_ = true;
    applied = true;
    return ok();
}

Status GradingRenderer::force_lut(Span<const Vec3> table, u32 size) noexcept {
    if (!ready_) {
        return fail(ErrorCode::Unavailable, "grading: not initialized");
    }
    if (Status uploaded = upload_table(table, size); !uploaded) {
        return uploaded;
    }
    lut_applied_ = true;
    return ok();
}

void GradingRenderer::set_manual_stops(f32 stops) noexcept {
    automatic_ = false;
    manual_stops_ = stops;
    restart_pending_ = false;
}

void GradingRenderer::set_manual_ev100(f32 ev100) noexcept {
    set_manual_stops(exposure_stops_for_ev100(ev100));
}

void GradingRenderer::set_automatic(const AutoExposureSettings& settings,
                                    f32 starting_ev100) noexcept {
    automatic_ = true;
    settings_constants_ = exposure_constants(settings, 0.0F, false, 0.0F, 0, 0);
    restart(starting_ev100);
}

void GradingRenderer::restart(f32 ev100) noexcept {
    restart_ev100_ = ev100;
    restart_pending_ = true;
}

ResourceId GradingRenderer::import_state(RenderGraph& graph) noexcept {
    BufferRequest request;
    request.name = "exposure state";
    request.size = kStateBytes;
    request.extra_usage = rhi::BufferUsage::Storage;
    state_resource_ = graph.import_buffer(request, state_);
    return state_resource_;
}

FramePassCallback GradingRenderer::post_process() noexcept {
    FramePassCallback callback{&record_resolve_step, this};
    if (state_resource_ != kInvalidResource) {
        state_read_ = FrameResourceRead{state_resource_, Access::FragmentStorageRead};
        callback.reads = Span<const FrameResourceRead>(&state_read_, 1);
    }
    return callback;
}

Status GradingRenderer::declare_metering(RenderGraph& graph, const FrameResources& resources,
                                         u32 width, u32 height) noexcept {
    if (!ready_) {
        return fail(ErrorCode::Unavailable, "grading: not initialized");
    }
    scene_ = resources.post_source;
    output_ = resources.output;
    width_ = width;
    height_ = height;

    // THIS FRAME'S RESOLVE. Metered, it applies what the previous frame's adaptation wrote — except
    // on a restart, where there is no previous frame and the starting EV is pushed instead.
    resolve_push_ = GradedResolvePush{};
    const bool metered = automatic_ && !restart_pending_;
    resolve_push_.exposure[0] =
        automatic_ ? exposure_stops_for_ev100(restart_ev100_) : manual_stops_;
    resolve_push_.exposure[1] = metered ? 1.0F : 0.0F;
    resolve_push_.exposure[2] = -exposure_stops_for_ev100(0.0F);
    resolve_push_.lut[0] = lut_applied_ ? 1.0F : 0.0F;
    resolve_push_.lut[1] = static_cast<f32>(table_size_);
    if (!automatic_) {
        // The imported id belongs to this frame's graph; the next frame imports its own.
        state_resource_ = kInvalidResource;
        return ok();
    }
    // Step 6 meters the scene-referred colour BEFORE bloom: the temporal history when the frame
    // resolved one, and otherwise the shading target (single-sample: MSAA resolves into it).
    const ResourceId source = resources.temporal_history != kInvalidResource
                                  ? resources.temporal_history
                                  : resources.color;
    return declare_metering_of(graph, source, width, height);
}

Status GradingRenderer::declare_metering_of(RenderGraph& graph, ResourceId source, u32 width,
                                            u32 height) noexcept {
    if (!ready_) {
        return fail(ErrorCode::Unavailable, "grading: not initialized");
    }
    if (source == kInvalidResource || width == 0 || height == 0) {
        return fail(ErrorCode::InvalidArgument, "grading: metering needs an image and its extent");
    }
    if (state_resource_ == kInvalidResource) {
        (void)import_state(graph);
    }
    metered_source_ = source;
    metered_width_ = width;
    metered_height_ = height;
    constants_ = settings_constants_;
    constants_.frame[0] = delta_seconds_;
    constants_.frame[1] = restart_pending_ ? 1.0F : 0.0F;
    constants_.frame[2] = restart_ev100_;
    constants_.extent[0] = width;
    constants_.extent[1] = height;
    restart_pending_ = false;

    BufferRequest request;
    request.name = "exposure histogram";
    request.size = kHistogramBytes;
    request.extra_usage = rhi::BufferUsage::Storage;
    histogram_resource_ = graph.import_buffer(request, histogram_);
    graph.add_pass("exposure clear", QueueKind::Graphics)
        .write(histogram_resource_, Access::ComputeStorageWrite)
        .record(&record_clear_step, this);
    graph.add_pass("exposure histogram", QueueKind::Graphics)
        .read(source, Access::ComputeSampledRead)
        .use(histogram_resource_, Access::ComputeStorageReadWrite)
        .record(&record_histogram_step, this);
    graph.add_pass("exposure adapt", QueueKind::Graphics)
        .read(histogram_resource_, Access::ComputeStorageRead)
        .use(state_resource_, Access::ComputeStorageReadWrite)
        .record(&record_adapt_step, this);
    if (description_.readback) {
        request.name = "exposure histogram readback";
        request.extra_usage = rhi::BufferUsage::TransferDestination;
        request.memory = rhi::MemoryUse::Readback;
        histogram_copy_ = graph.import_buffer(request, histogram_readback_);
        request.name = "exposure state readback";
        request.size = kStateBytes;
        state_copy_ = graph.import_buffer(request, state_readback_);
        graph.add_pass("exposure readback", QueueKind::Graphics)
            .read(histogram_resource_, Access::TransferRead)
            .read(state_resource_, Access::TransferRead)
            .write(histogram_copy_, Access::TransferWrite)
            .write(state_copy_, Access::TransferWrite)
            .record(&record_readback_step, this);
        // The host boundary as a declared read, so the graph derives the transfer-to-host barrier.
        graph.add_pass("exposure host", QueueKind::Graphics)
            .read(histogram_copy_, Access::HostRead)
            .read(state_copy_, Access::HostRead)
            .side_effect();
    }
    // The imported ids belong to this frame's graph; the next frame imports its own.
    state_resource_ = kInvalidResource;
    histogram_resource_ = kInvalidResource;
    return graph.status();
}

Status GradingRenderer::write_set(rhi::DescriptorSetLayoutHandle layout,
                                  Span<const rhi::DescriptorWrite> writes,
                                  rhi::DescriptorSetHandle& out) noexcept {
    // A FRESH SET PER RECORDING: a set an earlier command buffer bound may not be written again,
    // and the scene colour's view exists only once the graph has realised the frame's transients.
    Expected<rhi::DescriptorSetHandle, Error> set = device_->allocate_descriptor_set(layout, true);
    if (!set.has_value()) {
        return make_unexpected(set.error());
    }
    if (Status written = device_->update_descriptor_set(*set, writes); !written) {
        return written;
    }
    out = *set;
    return ok();
}

void GradingRenderer::record_resolve(const PassContext& context) noexcept {
    const GraphExecutor& executor = *context.executor;
    rhi::DescriptorWrite writes[kResolveBindings];
    writes[0] = sampled(0, executor.view(scene_));
    writes[1].binding = 1;
    writes[1].kind = rhi::DescriptorKind::Sampler;
    writes[1].sampler = sampler_;
    writes[2] = sampled(2, table_view_);
    writes[3] = storage(3, state_, kStateBytes);
    rhi::DescriptorSetHandle set;
    if (Status written = write_set(resolve_set_, {writes, kResolveBindings}, set); !written) {
        return;
    }
    rhi::RenderAttachment target;
    target.view = executor.view(output_);
    target.load = rhi::LoadOp::Clear;
    target.store = rhi::StoreOp::Store;
    target.clear.color[3] = 1.0F;
    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, width_, height_};
    info.color_attachments = Span<const rhi::RenderAttachment>(&target, 1);

    rhi::CommandBuffer& commands = *context.commands;
    commands.begin_rendering(info);
    commands.set_viewport(
        rhi::Viewport{0.0F, 0.0F, static_cast<f32>(width_), static_cast<f32>(height_), 0.0F, 1.0F});
    commands.set_scissor(rhi::Rect2D{0, 0, width_, height_});
    commands.bind_graphics_pipeline(resolve_);
    commands.bind_descriptor_sets(resolve_layout_, 0,
                                  Span<const rhi::DescriptorSetHandle>(&set, 1));
    commands.push_constants(
        resolve_layout_, rhi::ShaderStage::Fragment, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&resolve_push_), sizeof(resolve_push_)));
    commands.draw(3, 1, 0, 0);
    commands.end_rendering();
    ++report_.resolves;
    report_.graded_resolves += resolve_push_.lut[0] > 0.5F ? 1U : 0U;
    report_.metered_resolves += resolve_push_.exposure[1] > 0.5F ? 1U : 0U;
}

void GradingRenderer::dispatch(const PassContext& context, u32 which, rhi::DescriptorSetHandle set,
                               u32 groups_x, u32 groups_y) noexcept {
    rhi::CommandBuffer& commands = *context.commands;
    commands.bind_compute_pipeline(compute_pipelines_[which]);
    commands.bind_descriptor_sets(compute_layouts_[which], 0,
                                  Span<const rhi::DescriptorSetHandle>(&set, 1));
    commands.push_constants(
        compute_layouts_[which], rhi::ShaderStage::Compute, 0,
        Span<const u8>(reinterpret_cast<const u8*>(&constants_), sizeof(constants_)));
    commands.dispatch(groups_x, groups_y, 1);
    ++report_.dispatches;
}

void GradingRenderer::record_clear(const PassContext& context) noexcept {
    const rhi::DescriptorWrite writes[] = {storage(0, histogram_, kHistogramBytes)};
    rhi::DescriptorSetHandle set;
    if (write_set(compute_sets_[kClear], {writes, 1}, set)) {
        dispatch(context, kClear, set, 1, 1);
    }
}

void GradingRenderer::record_histogram(const PassContext& context) noexcept {
    const rhi::DescriptorWrite writes[] = {
        sampled(0, context.executor->view(metered_source_)),
        storage(1, histogram_, kHistogramBytes),
    };
    rhi::DescriptorSetHandle set;
    if (write_set(compute_sets_[kHistogram], {writes, 2}, set)) {
        dispatch(context, kHistogram, set, groups(metered_width_), groups(metered_height_));
    }
}

void GradingRenderer::record_adapt(const PassContext& context) noexcept {
    const rhi::DescriptorWrite writes[] = {
        storage(0, histogram_, kHistogramBytes),
        storage(1, state_, kStateBytes),
    };
    rhi::DescriptorSetHandle set;
    if (write_set(compute_sets_[kAdapt], {writes, 2}, set)) {
        dispatch(context, kAdapt, set, 1, 1);
    }
}

void GradingRenderer::record_readback(const PassContext& context) noexcept {
    rhi::BufferCopy histogram;
    histogram.size = kHistogramBytes;
    context.commands->copy_buffer(histogram_, histogram_readback_,
                                  Span<const rhi::BufferCopy>(&histogram, 1));
    rhi::BufferCopy state;
    state.size = kStateBytes;
    context.commands->copy_buffer(state_, state_readback_, Span<const rhi::BufferCopy>(&state, 1));
}

Status GradingRenderer::read_exposure(ExposureReadback& out) const noexcept {
    if (!ready_ || histogram_readback_.is_null() || state_readback_.is_null()) {
        return fail(ErrorCode::Unavailable,
                    "grading: created without `readback`, so there is nothing to read");
    }
    const auto* bins = static_cast<const u32*>(device_->buffer_mapped_pointer(histogram_readback_));
    const auto* state = static_cast<const f32*>(device_->buffer_mapped_pointer(state_readback_));
    if (bins == nullptr || state == nullptr) {
        return fail(ErrorCode::Internal, "grading: a readback buffer is not mapped");
    }
    std::memcpy(out.histogram, bins, kHistogramBytes);
    out.current_ev100 = state[0];
    out.target_ev100 = state[1];
    out.metered_ev100 = state[2];
    out.frames = state[3];
    return ok();
}

}  // namespace cy::rendering::grading
