// SPDX-License-Identifier: MIT
// An irradiance volume on the device. See probe_volume_texture.h.

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/command_buffer.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/light_probes/probe_volume_texture.h>

#include <cstring>

namespace cy::rendering::light_probes {
namespace {

constexpr rhi::Format kFormat = rhi::Format::Rgba16Sfloat;
constexpr u64 kBytesPerTexel = 8;

struct UploadRecording {
    rhi::BufferHandle staging;
    rhi::TextureHandle texture;
    u32 width = 0;
    u32 height = 0;
};

void record_upload(const PassContext& context, void* user) noexcept {
    const auto* recording = static_cast<const UploadRecording*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{recording->width, recording->height, 1};
    context.commands->copy_buffer_to_texture(recording->staging, recording->texture,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

/// Stage the half texels. Returns the staging buffer, which the caller destroys.
[[nodiscard]] Expected<rhi::BufferHandle, Error> stage(
    rhi::Device& device, Allocator& allocator, const gi::IrradianceVolume& volume,
    const gi::VolumeTextureLayout& layout) noexcept {
    const usize floats = static_cast<usize>(layout.width) * layout.height * 4U;
    Array<f32> texels(allocator);
    if (Status sized = texels.resize(floats); !sized) {
        return make_unexpected(sized.error());
    }
    if (Status packed = volume.pack_texels(texels.span()); !packed) {
        return make_unexpected(packed.error());
    }
    rhi::BufferDescription description;
    description.name = "irradiance volume staging";
    description.size = static_cast<u64>(floats) * sizeof(u16);
    description.usage = rhi::BufferUsage::TransferSource;
    description.memory = rhi::MemoryUse::Upload;
    Expected<rhi::BufferHandle, Error> buffer = device.create_buffer(description);
    if (!buffer.has_value()) {
        return buffer;
    }
    auto* mapped = static_cast<u16*>(device.buffer_mapped_pointer(*buffer));
    if (mapped == nullptr) {
        device.destroy_buffer(*buffer);
        return fail(ErrorCode::Internal, "the irradiance volume staging buffer is not mapped");
    }
    for (usize index = 0; index < floats; ++index) {
        mapped[index] = half_from_float(texels[index]);
    }
    return buffer;
}

/// One submission: the copy, then the sampled read that leaves the image where a fragment stage
/// can read it. `material_textures.h` says why the second pass is not optional.
[[nodiscard]] Status run(rhi::Device& device, Allocator& allocator,
                         UploadRecording& recording) noexcept {
    RenderGraph graph(allocator);
    TextureRequest request;
    request.name = "irradiance volume";
    request.format = kFormat;
    request.width = recording.width;
    request.height = recording.height;
    const ResourceId image =
        graph.import_texture(request, recording.texture, rhi::ImageUse::Undefined);
    graph.add_pass("irradiance volume upload", rhi::QueueKind::Graphics)
        .write(image, rhi::Access::TransferWrite)
        .record(&record_upload, &recording);
    graph.add_pass("irradiance volume residency", rhi::QueueKind::Graphics)
        .read(image, rhi::Access::FragmentSampledRead)
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

}  // namespace

ProbeVolumeTexture::~ProbeVolumeTexture() {
    shutdown();
}

void ProbeVolumeTexture::initialize(rhi::Device& device, Allocator& allocator) noexcept {
    device_ = &device;
    allocator_ = &allocator;
}

void ProbeVolumeTexture::release_texture() noexcept {
    if (device_ == nullptr) {
        return;
    }
    if (!view_.is_null()) {
        device_->destroy_texture_view(view_);
        view_ = rhi::TextureViewHandle{};
    }
    if (!texture_.is_null()) {
        device_->destroy_texture(texture_);
        texture_ = rhi::TextureHandle{};
    }
    uploaded_ = false;
}

void ProbeVolumeTexture::shutdown() noexcept {
    if (device_ != nullptr) {
        (void)device_->wait_idle();
    }
    release_texture();
    device_ = nullptr;
    allocator_ = nullptr;
}

Status ProbeVolumeTexture::recreate(const gi::VolumeTextureLayout& layout) noexcept {
    release_texture();
    rhi::TextureDescription description;
    description.name = "irradiance volume";
    description.format = kFormat;
    description.extent = rhi::Extent3D{layout.width, layout.height, 1};
    description.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDestination;
    Expected<rhi::TextureHandle, Error> texture = device_->create_texture(description);
    if (!texture.has_value()) {
        return make_unexpected(texture.error());
    }
    texture_ = *texture;
    rhi::TextureViewDescription view;
    view.name = "irradiance volume";
    view.texture = texture_;
    Expected<rhi::TextureViewHandle, Error> made = device_->create_texture_view(view);
    if (!made.has_value()) {
        release_texture();
        return make_unexpected(made.error());
    }
    view_ = *made;
    return ok();
}

Status ProbeVolumeTexture::upload(const gi::IrradianceVolume& volume) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::Unavailable, "ProbeVolumeTexture: initialize() was not called");
    }
    if (uploaded_ && volume.generation() == generation_) {
        return ok();
    }
    const gi::VolumeTextureLayout layout = volume.texture_layout();
    if (layout.width == 0 || layout.height == 0) {
        return fail(ErrorCode::InvalidArgument, "ProbeVolumeTexture: the volume has no probes");
    }
    if (texture_.is_null() || layout.width != layout_.width || layout.height != layout_.height) {
        if (Status made = recreate(layout); !made) {
            return made;
        }
    }
    Expected<rhi::BufferHandle, Error> staging = stage(*device_, *allocator_, volume, layout);
    if (!staging.has_value()) {
        return make_unexpected(staging.error());
    }
    UploadRecording recording;
    recording.staging = *staging;
    recording.texture = texture_;
    recording.width = layout.width;
    recording.height = layout.height;
    Status ran = run(*device_, *allocator_, recording);
    device_->destroy_buffer(*staging);
    if (!ran) {
        return ran;
    }
    layout_ = layout;
    generation_ = volume.generation();
    uploaded_ = true;
    uploads_ += 1;
    return ok();
}

void write_probe_volume(rhi::BindlessIndex slot, const gi::IrradianceVolume& volume,
                        const gi::VolumeTextureLayout& layout, Vec3 camera,
                        pipeline::FrameViewData& view) noexcept {
    const gi::IrradianceVolumeSettings& settings = volume.settings();
    view.probe_volume_control[0] = slot;
    view.probe_volume_control[1] = settings.count_x;
    view.probe_volume_control[2] = settings.count_y;
    view.probe_volume_control[3] = settings.count_z;
    const Vec3 origin = settings.origin - camera;
    view.probe_volume_origin[0] = origin.x;
    view.probe_volume_origin[1] = origin.y;
    view.probe_volume_origin[2] = origin.z;
    view.probe_volume_origin[3] = settings.spacing_metres;
    view.probe_volume_params[0] = layout.coefficient_scale;
    view.probe_volume_params[1] = settings.normal_offset_metres;
    view.probe_volume_params[2] = settings.visibility_slack_metres;
    view.probe_volume_params[3] = 0.0F;
}

u16 half_from_float(f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const auto sign = static_cast<u16>((bits >> 16U) & 0x8000U);
    const u32 magnitude = bits & 0x7FFFFFFFU;
    if (magnitude >= 0x7F800000U) {
        // Infinity stays infinity; a NaN stays a quiet NaN.
        return static_cast<u16>(sign | 0x7C00U | (magnitude > 0x7F800000U ? 0x0200U : 0U));
    }
    if (magnitude >= 0x477FF000U) {
        // 65520 and above round past the largest half.
        return static_cast<u16>(sign | 0x7C00U);
    }
    if (magnitude < 0x38800000U) {
        // Below the smallest normal half: a subnormal, or zero.
        if (magnitude < 0x33000000U) {
            return sign;
        }
        const u32 exponent = magnitude >> 23U;
        const u32 mantissa = (magnitude & 0x7FFFFFU) | 0x800000U;
        const u32 shift = 126U - exponent;
        u32 half = mantissa >> shift;
        const u32 remainder = mantissa & ((1U << shift) - 1U);
        const u32 halfway = 1U << (shift - 1U);
        if (remainder > halfway || (remainder == halfway && (half & 1U) != 0U)) {
            half += 1U;
        }
        return static_cast<u16>(sign | half);
    }
    u32 half = (magnitude - 0x38000000U) >> 13U;
    const u32 remainder = magnitude & 0x1FFFU;
    if (remainder > 0x1000U || (remainder == 0x1000U && (half & 1U) != 0U)) {
        half += 1U;
    }
    return static_cast<u16>(sign | half);
}

}  // namespace cy::rendering::light_probes
