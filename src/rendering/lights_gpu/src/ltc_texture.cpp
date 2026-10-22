// SPDX-License-Identifier: MIT
// The frame's LTC table on the device. See ltc_texture.h.

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/command_buffer.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/lights_gpu/ltc_texture.h>

#include <cstring>

namespace cy::rendering::lights_gpu {
namespace {

constexpr rhi::Format kFormat = rhi::Format::Rgba32Sfloat;
constexpr u64 kTexelBytes = 4U * sizeof(f32);

struct UploadRecording {
    rhi::BufferHandle staging;
    rhi::TextureHandle texture;
};

void record_upload(const PassContext& context, void* user) noexcept {
    const auto& recording = *static_cast<const UploadRecording*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{kLtcTextureWidth, kLtcTextureHeight, 1};
    context.commands->copy_buffer_to_texture(recording.staging, recording.texture,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

/// One submission: the copy, then the sampled read that leaves the image where a fragment stage
/// reads it — `DecalTableTexture::upload`'s arrangement.
[[nodiscard]] Status run(rhi::Device& device, Allocator& allocator,
                         UploadRecording& recording) noexcept {
    RenderGraph graph(allocator);
    TextureRequest request;
    request.name = "ltc table";
    request.format = kFormat;
    request.width = kLtcTextureWidth;
    request.height = kLtcTextureHeight;
    const ResourceId image =
        graph.import_texture(request, recording.texture, rhi::ImageUse::Undefined);
    graph.add_pass("ltc table upload", rhi::QueueKind::Graphics)
        .write(image, rhi::Access::TransferWrite)
        .record(&record_upload, &recording);
    graph.add_pass("ltc table residency", rhi::QueueKind::Graphics)
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

Status pack_ltc_texels(const LtcTable& table, Array<f32>& out) noexcept {
    if (!table.built()) {
        return fail(ErrorCode::InvalidArgument,
                    "the LTC table is not built: LtcTable::build() before uploading it");
    }
    out.clear();
    if (Status sized = out.resize(static_cast<usize>(kLtcTextureWidth) * kLtcTextureHeight * 4U);
        !sized) {
        return sized;
    }
    for (u32 y = 0; y < kLtcTableSize; ++y) {
        for (u32 x = 0; x < kLtcTableSize; ++x) {
            const LtcEntry& entry = table.entry(x, y);
            // `Minv`'s columns are (a, 0, d), (0, c, 0), (b, 0, 1): area.cpp's `matrix_of`.
            const Mat3& m = entry.inverse_transform;
            f32* upper = out.data() + ((static_cast<usize>(y) * kLtcTextureWidth) + x) * 4U;
            upper[0] = m.columns[0].x;
            upper[1] = m.columns[2].x;
            upper[2] = m.columns[1].y;
            upper[3] = m.columns[0].z;
            f32* lower =
                out.data() + ((static_cast<usize>(y + kLtcTableSize) * kLtcTextureWidth) + x) * 4U;
            lower[0] = entry.magnitude;
            lower[1] = entry.fresnel;
            lower[2] = 0.0F;
            lower[3] = 0.0F;
        }
    }
    return ok();
}

LtcTableTexture::~LtcTableTexture() {
    shutdown();
}

void LtcTableTexture::initialize(rhi::Device& device, Allocator& allocator) noexcept {
    device_ = &device;
    allocator_ = &allocator;
}

void LtcTableTexture::release() noexcept {
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
}

void LtcTableTexture::shutdown() noexcept {
    if (device_ != nullptr) {
        (void)device_->wait_idle();
    }
    release();
    device_ = nullptr;
    allocator_ = nullptr;
}

Status LtcTableTexture::upload(const LtcTable& table) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::Unavailable, "LtcTableTexture: initialize() was not called");
    }
    Array<f32> texels(*allocator_);
    if (Status packed = pack_ltc_texels(table, texels); !packed) {
        return packed;
    }
    if (texture_.is_null()) {
        rhi::TextureDescription description;
        description.name = "ltc table";
        description.format = kFormat;
        description.extent = rhi::Extent3D{kLtcTextureWidth, kLtcTextureHeight, 1};
        description.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDestination;
        Expected<rhi::TextureHandle, Error> texture = device_->create_texture(description);
        if (!texture.has_value()) {
            return make_unexpected(texture.error());
        }
        texture_ = *texture;
        rhi::TextureViewDescription view;
        view.name = "ltc table";
        view.texture = texture_;
        Expected<rhi::TextureViewHandle, Error> made = device_->create_texture_view(view);
        if (!made.has_value()) {
            release();
            return make_unexpected(made.error());
        }
        view_ = *made;
    }
    const u64 size = static_cast<u64>(kLtcTextureWidth) * kLtcTextureHeight * kTexelBytes;
    rhi::BufferDescription description;
    description.name = "ltc table staging";
    description.size = size;
    description.usage = rhi::BufferUsage::TransferSource;
    description.memory = rhi::MemoryUse::Upload;
    Expected<rhi::BufferHandle, Error> staging = device_->create_buffer(description);
    if (!staging.has_value()) {
        return make_unexpected(staging.error());
    }
    auto* mapped = static_cast<u8*>(device_->buffer_mapped_pointer(*staging));
    if (mapped == nullptr) {
        device_->destroy_buffer(*staging);
        return fail(ErrorCode::Internal, "the LTC table staging buffer is not mapped");
    }
    std::memcpy(mapped, texels.data(), static_cast<usize>(size));
    UploadRecording recording;
    recording.staging = *staging;
    recording.texture = texture_;
    Status ran = run(*device_, *allocator_, recording);
    device_->destroy_buffer(*staging);
    return ran;
}

}  // namespace cy::rendering::lights_gpu
