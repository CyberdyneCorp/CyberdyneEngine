// SPDX-License-Identifier: MIT
// A baked lightmap on the device. See lightmap_textures.h.

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/command_buffer.h>
#include <cy/rendering/gi/resolve.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/lightmap_bake/asset.h>
#include <cy/rendering/lightmaps/lightmap_textures.h>

#include <cstring>

namespace cy::rendering::lightmaps {
namespace {

constexpr rhi::Format kFormat = rhi::Format::Rgba16Sfloat;

struct UploadRecording {
    rhi::BufferHandle staging;
    const rhi::TextureHandle* textures = nullptr;
    u32 planes = 0;
    u32 width = 0;
    u32 height = 0;
};

void record_upload(const PassContext& context, void* user) noexcept {
    const auto* recording = static_cast<const UploadRecording*>(user);
    for (u32 plane = 0; plane < recording->planes; ++plane) {
        rhi::BufferTextureCopy region;
        region.buffer_offset =
            u64{plane} * recording->width * recording->height * 4U * sizeof(u16);
        region.texture_extent = rhi::Extent3D{recording->width, recording->height, 1};
        context.commands->copy_buffer_to_texture(recording->staging, recording->textures[plane],
                                                 Span<const rhi::BufferTextureCopy>(&region, 1));
    }
}

/// FNV-1a over the layout and every texel's bits: what makes a second upload of the same lightmap
/// free.
[[nodiscard]] u64 fingerprint_of(const lightmap_bake::BakedLightmap& lightmap) noexcept {
    u64 hash = 0xCBF29CE484222325ULL;
    const auto mix = [&hash](u32 word) {
        for (u32 shift = 0; shift < 32U; shift += 8U) {
            hash ^= (word >> shift) & 0xFFU;
            hash *= 0x100000001B3ULL;
        }
    };
    mix(static_cast<u32>(lightmap.mode));
    mix(lightmap.texels.width);
    mix(lightmap.texels.height);
    mix(lightmap.texels.planes);
    for (const Vec4& texel : lightmap.texels.texels) {
        const f32 channels[4] = {texel.x, texel.y, texel.z, texel.w};
        for (const f32 channel : channels) {
            u32 bits = 0;
            std::memcpy(&bits, &channel, sizeof(bits));
            mix(bits);
        }
    }
    return hash;
}

[[nodiscard]] Expected<rhi::BufferHandle, Error> stage(
    rhi::Device& device, const lightmap_bake::BakedLightmap& lightmap) noexcept {
    const usize values = lightmap.texels.texels.size() * 4U;
    rhi::BufferDescription description;
    description.name = "lightmap staging";
    description.size = static_cast<u64>(values) * sizeof(u16);
    description.usage = rhi::BufferUsage::TransferSource;
    description.memory = rhi::MemoryUse::Upload;
    Expected<rhi::BufferHandle, Error> buffer = device.create_buffer(description);
    if (!buffer.has_value()) {
        return buffer;
    }
    auto* mapped = static_cast<u16*>(device.buffer_mapped_pointer(*buffer));
    if (mapped == nullptr) {
        device.destroy_buffer(*buffer);
        return fail(ErrorCode::Internal, "the lightmap staging buffer is not mapped");
    }
    // The planes are stored plane after plane, which is the order the copies read them in.
    usize at = 0;
    for (const Vec4& texel : lightmap.texels.texels) {
        mapped[at++] = lightmap_bake::half_from_float(texel.x);
        mapped[at++] = lightmap_bake::half_from_float(texel.y);
        mapped[at++] = lightmap_bake::half_from_float(texel.z);
        mapped[at++] = lightmap_bake::half_from_float(texel.w);
    }
    return buffer;
}

/// One submission: the copies, then the sampled read that leaves each plane where a fragment
/// stage can read it.
[[nodiscard]] Status run(rhi::Device& device, Allocator& allocator,
                         UploadRecording& recording) noexcept {
    RenderGraph graph(allocator);
    ResourceId images[kMaxPlanes] = {};
    for (u32 plane = 0; plane < recording.planes; ++plane) {
        TextureRequest request;
        request.name = "lightmap plane";
        request.format = kFormat;
        request.width = recording.width;
        request.height = recording.height;
        images[plane] =
            graph.import_texture(request, recording.textures[plane], rhi::ImageUse::Undefined);
    }
    auto upload = graph.add_pass("lightmap upload", rhi::QueueKind::Graphics);
    for (u32 plane = 0; plane < recording.planes; ++plane) {
        upload.write(images[plane], rhi::Access::TransferWrite);
    }
    upload.record(&record_upload, &recording);
    auto residency = graph.add_pass("lightmap residency", rhi::QueueKind::Graphics);
    for (u32 plane = 0; plane < recording.planes; ++plane) {
        residency.read(images[plane], rhi::Access::FragmentSampledRead);
    }
    residency.side_effect();
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

LightmapTextures::~LightmapTextures() {
    shutdown();
}

void LightmapTextures::initialize(rhi::Device& device, Allocator& allocator) noexcept {
    device_ = &device;
    allocator_ = &allocator;
}

void LightmapTextures::release_textures() noexcept {
    if (device_ == nullptr) {
        return;
    }
    for (u32 plane = 0; plane < kMaxPlanes; ++plane) {
        if (!views_[plane].is_null()) {
            device_->destroy_texture_view(views_[plane]);
            views_[plane] = rhi::TextureViewHandle{};
        }
        if (!textures_[plane].is_null()) {
            device_->destroy_texture(textures_[plane]);
            textures_[plane] = rhi::TextureHandle{};
        }
    }
    planes_ = 0;
    fingerprint_ = 0;
}

void LightmapTextures::shutdown() noexcept {
    if (device_ != nullptr) {
        (void)device_->wait_idle();
    }
    release_textures();
    device_ = nullptr;
    allocator_ = nullptr;
}

Status LightmapTextures::recreate(u32 width, u32 height, u32 planes) noexcept {
    release_textures();
    for (u32 plane = 0; plane < planes; ++plane) {
        rhi::TextureDescription description;
        description.name = "lightmap plane";
        description.format = kFormat;
        description.extent = rhi::Extent3D{width, height, 1};
        description.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDestination;
        Expected<rhi::TextureHandle, Error> texture = device_->create_texture(description);
        if (!texture.has_value()) {
            release_textures();
            return make_unexpected(texture.error());
        }
        textures_[plane] = *texture;
        rhi::TextureViewDescription view;
        view.name = "lightmap plane";
        view.texture = textures_[plane];
        Expected<rhi::TextureViewHandle, Error> made = device_->create_texture_view(view);
        if (!made.has_value()) {
            release_textures();
            return make_unexpected(made.error());
        }
        views_[plane] = *made;
    }
    width_ = width;
    height_ = height;
    planes_ = planes;
    return ok();
}

Status LightmapTextures::upload(const lightmap_bake::BakedLightmap& lightmap) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::Unavailable, "LightmapTextures: initialize() was not called");
    }
    const lightmap_bake::LightmapTexels& texels = lightmap.texels;
    if (texels.width == 0 || texels.height == 0 || texels.planes == 0 ||
        texels.planes > kMaxPlanes ||
        texels.texels.size() != usize{texels.width} * texels.height * texels.planes) {
        return fail(ErrorCode::InvalidArgument, "LightmapTextures: the lightmap has no texels");
    }
    const u64 fingerprint = fingerprint_of(lightmap);
    if (planes_ != 0 && fingerprint == fingerprint_) {
        return ok();
    }
    if (planes_ != texels.planes || width_ != texels.width || height_ != texels.height) {
        if (Status made = recreate(texels.width, texels.height, texels.planes); !made) {
            return made;
        }
    }
    Expected<rhi::BufferHandle, Error> staging = stage(*device_, lightmap);
    if (!staging.has_value()) {
        return make_unexpected(staging.error());
    }
    UploadRecording recording;
    recording.staging = *staging;
    recording.textures = textures_;
    recording.planes = planes_;
    recording.width = width_;
    recording.height = height_;
    Status ran = run(*device_, *allocator_, recording);
    device_->destroy_buffer(*staging);
    if (!ran) {
        return ran;
    }
    fingerprint_ = fingerprint;
    uploads_ += 1;
    return ok();
}

pipeline::MaterialTextureSlot LightmapTextures::slot(u32 plane,
                                                     rhi::BindlessIndex index) const noexcept {
    if (plane >= planes_) {
        return pipeline::MaterialTextureSlot{};
    }
    return pipeline::MaterialTextureSlot{index, views_[plane]};
}

AmbientSource frame_ambient_source(gi::GiMode mode, bool surface_has_lightmap,
                                   bool surface_has_irradiance_volume) noexcept {
    const u32 excluded =
        gi::exclusion_for(mode, surface_has_lightmap, surface_has_irradiance_volume);
    if (surface_has_lightmap && (excluded & gi::source_bit(gi::RadianceSource::Lightmap)) == 0U) {
        return AmbientSource::Lightmap;
    }
    if (surface_has_irradiance_volume &&
        (excluded & gi::source_bit(gi::RadianceSource::IrradianceVolume)) == 0U) {
        return AmbientSource::IrradianceVolume;
    }
    return AmbientSource::Sky;
}

Status write_lightmaps(Span<const rhi::BindlessIndex> slots,
                       const lightmap_bake::BakedLightmap& lightmap, gi::GiMode mode,
                       pipeline::FrameViewData& view) noexcept {
    if (slots.size() < lightmap.texels.planes || lightmap.texels.planes > kMaxPlanes) {
        return fail(ErrorCode::InvalidArgument,
                    "write_lightmaps: one texture slot per lightmap plane");
    }
    // A lightmapped surface that ALSO sits in a volume is the row that decides: the frame may only
    // switch the lightmap on for a mode that both admits it and excludes the volume for it.
    if (frame_ambient_source(mode, true, true) != AmbientSource::Lightmap) {
        return ok();
    }
    for (u32 plane = 0; plane < 3U; ++plane) {
        view.lightmap_control[plane] =
            plane < lightmap.texels.planes ? slots[plane] : pipeline::kNoMaterialTexture;
    }
    view.lightmap_control[3] = static_cast<u32>(lightmap.mode) + 1U;
    view.lightmap_layout[0] = lightmap.page_size;
    view.lightmap_layout[1] = lightmap.pages;
    view.lightmap_layout[2] = lightmap.gutter_texels;
    view.lightmap_layout[3] = 0;
    return ok();
}

}  // namespace cy::rendering::lightmaps
