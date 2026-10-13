// SPDX-License-Identifier: MIT
// A GI debug view's image on the device. See gi_debug_overlay.h.

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/command_buffer.h>
#include <cy/rendering/gi/debug_views.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/lightmaps/gi_debug_overlay.h>

#include <cstring>

namespace cy::rendering::lightmaps {
namespace {

constexpr rhi::Format kFormat = rhi::Format::Rgba8Unorm;

struct OverlayRecording {
    rhi::BufferHandle staging;
    rhi::TextureHandle texture;
    u32 width = 0;
    u32 height = 0;
};

void record_upload(const PassContext& context, void* user) noexcept {
    const auto* recording = static_cast<const OverlayRecording*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{recording->width, recording->height, 1};
    context.commands->copy_buffer_to_texture(recording->staging, recording->texture,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

/// One submission: the copy, then the sampled read that leaves the image where a fragment stage
/// can read it — `light_probes::ProbeVolumeTexture`'s upload, for eight-bit texels.
[[nodiscard]] Status run(rhi::Device& device, Allocator& allocator,
                         OverlayRecording& recording) noexcept {
    RenderGraph graph(allocator);
    TextureRequest request;
    request.name = "gi debug overlay";
    request.format = kFormat;
    request.width = recording.width;
    request.height = recording.height;
    const ResourceId image =
        graph.import_texture(request, recording.texture, rhi::ImageUse::Undefined);
    graph.add_pass("gi debug overlay upload", rhi::QueueKind::Graphics)
        .write(image, rhi::Access::TransferWrite)
        .record(&record_upload, &recording);
    graph.add_pass("gi debug overlay residency", rhi::QueueKind::Graphics)
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

GiDebugOverlayTexture::~GiDebugOverlayTexture() {
    shutdown();
}

void GiDebugOverlayTexture::initialize(rhi::Device& device, Allocator& allocator) noexcept {
    device_ = &device;
    allocator_ = &allocator;
}

void GiDebugOverlayTexture::release_texture() noexcept {
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
    width_ = 0;
    height_ = 0;
}

void GiDebugOverlayTexture::shutdown() noexcept {
    if (device_ != nullptr) {
        (void)device_->wait_idle();
    }
    release_texture();
    device_ = nullptr;
    allocator_ = nullptr;
}

Status GiDebugOverlayTexture::recreate(u32 width, u32 height) noexcept {
    release_texture();
    rhi::TextureDescription description;
    description.name = "gi debug overlay";
    description.format = kFormat;
    description.extent = rhi::Extent3D{width, height, 1};
    description.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDestination;
    Expected<rhi::TextureHandle, Error> texture = device_->create_texture(description);
    if (!texture.has_value()) {
        return make_unexpected(texture.error());
    }
    texture_ = *texture;
    rhi::TextureViewDescription view;
    view.name = "gi debug overlay";
    view.texture = texture_;
    Expected<rhi::TextureViewHandle, Error> made = device_->create_texture_view(view);
    if (!made.has_value()) {
        release_texture();
        return make_unexpected(made.error());
    }
    view_ = *made;
    width_ = width;
    height_ = height;
    return ok();
}

Status GiDebugOverlayTexture::upload(Span<const u32> texels, u32 width, u32 height) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::Unavailable, "GiDebugOverlayTexture: initialize() was not called");
    }
    if (width == 0 || height == 0 || texels.size() != static_cast<usize>(width) * height) {
        return fail(ErrorCode::InvalidArgument,
                    "GiDebugOverlayTexture: the image is empty or not width * height texels");
    }
    if (texture_.is_null() || width != width_ || height != height_) {
        if (Status made = recreate(width, height); !made) {
            return made;
        }
    }
    rhi::BufferDescription description;
    description.name = "gi debug overlay staging";
    description.size = static_cast<u64>(texels.size()) * sizeof(u32);
    description.usage = rhi::BufferUsage::TransferSource;
    description.memory = rhi::MemoryUse::Upload;
    Expected<rhi::BufferHandle, Error> staging = device_->create_buffer(description);
    if (!staging.has_value()) {
        return make_unexpected(staging.error());
    }
    void* mapped = device_->buffer_mapped_pointer(*staging);
    if (mapped == nullptr) {
        device_->destroy_buffer(*staging);
        return fail(ErrorCode::Internal, "the gi debug overlay staging buffer is not mapped");
    }
    std::memcpy(mapped, texels.data(), texels.size() * sizeof(u32));
    OverlayRecording recording;
    recording.staging = *staging;
    recording.texture = texture_;
    recording.width = width;
    recording.height = height;
    const Status ran = run(*device_, *allocator_, recording);
    device_->destroy_buffer(*staging);
    return ran;
}

bool write_gi_debug_view(render::DebugViewMode mode, rhi::BindlessIndex texture_slot,
                         pipeline::FrameViewData& view) noexcept {
    if (gi::gi_debug_view_from_name(render::debug_view_mode_name(mode)) == gi::GiDebugView::Count) {
        return false;
    }
    pipeline::write_gi_debug_overlay(texture_slot, view);
    return true;
}

}  // namespace cy::rendering::lightmaps
