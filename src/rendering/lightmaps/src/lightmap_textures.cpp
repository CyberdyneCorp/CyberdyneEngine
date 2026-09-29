// SPDX-License-Identifier: MIT
// A baked lightmap on the device. See lightmap_textures.h.

#include <cy/backends/rhi/access.h>
#include <cy/backends/rhi/command_buffer.h>
#include <cy/rendering/gi/resolve.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/lightmap_bake/asset.h>
#include <cy/rendering/lightmap_bake/mips.h>
#include <cy/rendering/lightmaps/lightmap_textures.h>

#include <cstring>

namespace cy::rendering::lightmaps {
namespace {

constexpr rhi::Format kFormat = rhi::Format::Rgba16Sfloat;
/// Textures one lightmap can need: its planes and its shadow mask.
constexpr u32 kMaxTextures = kMaxPlanes + 1U;

/// One level of one texture in the staging buffer.
struct StagedLevel {
    u64 offset = 0;
    u32 texture = 0;
    u32 level = 0;
    u32 width = 0;
    u32 height = 0;
};

struct UploadRecording {
    rhi::BufferHandle staging;
    const rhi::TextureHandle* textures = nullptr;
    StagedLevel levels[kMaxTextures * (lightmap_bake::kMaxLightmapMipLevels + 1U)];
    u32 level_count = 0;
    u32 textures_used = 0;
    u32 mip_levels = 0;
    u32 width = 0;
    u32 height = 0;
};

void record_upload(const PassContext& context, void* user) noexcept {
    const auto* recording = static_cast<const UploadRecording*>(user);
    for (u32 at = 0; at < recording->level_count; ++at) {
        const StagedLevel& staged = recording->levels[at];
        rhi::BufferTextureCopy region;
        region.buffer_offset = staged.offset;
        region.mip_level = static_cast<u16>(staged.level);
        region.texture_extent = rhi::Extent3D{staged.width, staged.height, 1};
        context.commands->copy_buffer_to_texture(recording->staging,
                                                 recording->textures[staged.texture],
                                                 Span<const rhi::BufferTextureCopy>(&region, 1));
    }
}

/// FNV-1a, a word at a time.
struct Fingerprint {
    u64 hash = 0xCBF29CE484222325ULL;

    void mix(u32 word) noexcept {
        for (u32 shift = 0; shift < 32U; shift += 8U) {
            hash ^= (word >> shift) & 0xFFU;
            hash *= 0x100000001B3ULL;
        }
    }
    void mix(const lightmap_bake::LightmapTexels& texels) noexcept {
        mix(texels.width);
        mix(texels.height);
        mix(texels.planes);
        for (const Vec4& texel : texels.texels) {
            const f32 channels[4] = {texel.x, texel.y, texel.z, texel.w};
            for (const f32 channel : channels) {
                u32 bits = 0;
                std::memcpy(&bits, &channel, sizeof(bits));
                mix(bits);
            }
        }
    }
};

/// The layout and every texel's bits, every level and the mask: what makes a second upload of the
/// same lightmap free.
[[nodiscard]] u64 fingerprint_of(const lightmap_bake::BakedLightmap& lightmap) noexcept {
    Fingerprint print;
    print.mix(static_cast<u32>(lightmap.mode));
    print.mix(lightmap.mip_levels);
    for (u32 level = 0; level <= lightmap.mip_levels; ++level) {
        print.mix(lightmap_bake::lightmap_level(lightmap, level));
        print.mix(lightmap_bake::shadow_mask_level(lightmap, level));
    }
    return print.hash;
}

/// The texels of one level of one texture: a plane of the planes, or the mask.
[[nodiscard]] Span<const Vec4> level_texels(const lightmap_bake::BakedLightmap& lightmap,
                                            u32 texture, u32 level) noexcept {
    if (texture == kMaxPlanes) {
        return lightmap_bake::shadow_mask_level(lightmap, level).texels.span();
    }
    const lightmap_bake::LightmapTexels& texels = lightmap_bake::lightmap_level(lightmap, level);
    const usize plane = usize{texels.width} * texels.height;
    return Span<const Vec4>(texels.texels.data() + (plane * texture), plane);
}

/// Every level of every texture the lightmap needs, texture after texture, in `recording`'s order.
void plan_levels(const lightmap_bake::BakedLightmap& lightmap, bool masked,
                 UploadRecording& recording) noexcept {
    recording.level_count = 0;
    u64 offset = 0;
    for (u32 texture = 0; texture < kMaxTextures; ++texture) {
        const bool used = texture < lightmap.texels.planes || (texture == kMaxPlanes && masked);
        for (u32 level = 0; used && level < recording.mip_levels; ++level) {
            StagedLevel& staged = recording.levels[recording.level_count++];
            staged.offset = offset;
            staged.texture = texture;
            staged.level = level;
            staged.width = recording.width >> level;
            staged.height = recording.height >> level;
            offset += u64{staged.width} * staged.height * 4U * sizeof(u16);
        }
    }
}

[[nodiscard]] Expected<rhi::BufferHandle, Error> stage(rhi::Device& device,
                                                       const lightmap_bake::BakedLightmap& lightmap,
                                                       const UploadRecording& recording) noexcept {
    const StagedLevel& last = recording.levels[recording.level_count - 1U];
    rhi::BufferDescription description;
    description.name = "lightmap staging";
    description.size = last.offset + (u64{last.width} * last.height * 4U * sizeof(u16));
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
    for (u32 at = 0; at < recording.level_count; ++at) {
        const StagedLevel& staged = recording.levels[at];
        u16* out = mapped + (staged.offset / sizeof(u16));
        for (const Vec4& texel : level_texels(lightmap, staged.texture, staged.level)) {
            *out++ = lightmap_bake::half_from_float(texel.x);
            *out++ = lightmap_bake::half_from_float(texel.y);
            *out++ = lightmap_bake::half_from_float(texel.z);
            *out++ = lightmap_bake::half_from_float(texel.w);
        }
    }
    return buffer;
}

/// One submission: the copies, then the sampled read that leaves each texture where a fragment
/// stage can read it.
[[nodiscard]] Status run(rhi::Device& device, Allocator& allocator,
                         UploadRecording& recording) noexcept {
    RenderGraph graph(allocator);
    ResourceId images[kMaxTextures] = {};
    for (u32 texture = 0; texture < kMaxTextures; ++texture) {
        if (recording.textures[texture].is_null()) {
            continue;
        }
        TextureRequest request;
        request.name = "lightmap plane";
        request.format = kFormat;
        request.width = recording.width;
        request.height = recording.height;
        request.mip_levels = static_cast<u16>(recording.mip_levels);
        images[texture] =
            graph.import_texture(request, recording.textures[texture], rhi::ImageUse::Undefined);
    }
    auto upload = graph.add_pass("lightmap upload", rhi::QueueKind::Graphics);
    for (u32 texture = 0; texture < kMaxTextures; ++texture) {
        if (!recording.textures[texture].is_null()) {
            upload.write(images[texture], rhi::Access::TransferWrite);
        }
    }
    upload.record(&record_upload, &recording);
    auto residency = graph.add_pass("lightmap residency", rhi::QueueKind::Graphics);
    for (u32 texture = 0; texture < kMaxTextures; ++texture) {
        if (!recording.textures[texture].is_null()) {
            residency.read(images[texture], rhi::Access::FragmentSampledRead);
        }
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

[[nodiscard]] bool well_formed(const lightmap_bake::BakedLightmap& lightmap) noexcept {
    const lightmap_bake::LightmapTexels& texels = lightmap.texels;
    const bool planes_ok =
        texels.width != 0 && texels.height != 0 && texels.planes != 0 &&
        texels.planes <= kMaxPlanes &&
        texels.texels.size() == usize{texels.width} * texels.height * texels.planes;
    if (!planes_ok || lightmap.mip_levels > lightmap_bake::kMaxLightmapMipLevels) {
        return false;
    }
    const bool masked = !lightmap.shadow_mask.texels.empty();
    for (u32 level = 0; level <= lightmap.mip_levels; ++level) {
        const usize count = usize{texels.width >> level} * (texels.height >> level);
        if (lightmap_bake::lightmap_level(lightmap, level).texels.size() != count * texels.planes ||
            (masked && lightmap_bake::shadow_mask_level(lightmap, level).texels.size() != count)) {
            return false;
        }
    }
    return true;
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
    for (u32 texture = 0; texture < kMaxTextures; ++texture) {
        if (!views_[texture].is_null()) {
            device_->destroy_texture_view(views_[texture]);
            views_[texture] = rhi::TextureViewHandle{};
        }
        if (!textures_[texture].is_null()) {
            device_->destroy_texture(textures_[texture]);
            textures_[texture] = rhi::TextureHandle{};
        }
    }
    planes_ = 0;
    masked_ = false;
    mip_levels_ = 0;
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

Status LightmapTextures::make_texture(u32 index) noexcept {
    rhi::TextureDescription description;
    description.name = index == kMaxPlanes ? "lightmap shadow mask" : "lightmap plane";
    description.format = kFormat;
    description.extent = rhi::Extent3D{width_, height_, 1};
    description.mip_levels = static_cast<u16>(mip_levels_);
    description.usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::TransferDestination |
                        rhi::TextureUsage::TransferSource;
    Expected<rhi::TextureHandle, Error> texture = device_->create_texture(description);
    if (!texture.has_value()) {
        return make_unexpected(texture.error());
    }
    textures_[index] = *texture;
    rhi::TextureViewDescription view;
    view.name = description.name;
    view.texture = textures_[index];
    Expected<rhi::TextureViewHandle, Error> made = device_->create_texture_view(view);
    if (!made.has_value()) {
        return make_unexpected(made.error());
    }
    views_[index] = *made;
    return ok();
}

Status LightmapTextures::recreate(u32 width, u32 height, u32 planes, bool masked,
                                  u32 levels) noexcept {
    release_textures();
    width_ = width;
    height_ = height;
    mip_levels_ = levels;
    for (u32 texture = 0; texture < kMaxTextures; ++texture) {
        if (texture >= planes && !(texture == kMaxPlanes && masked)) {
            continue;
        }
        if (Status made = make_texture(texture); !made) {
            release_textures();
            return made;
        }
    }
    planes_ = planes;
    masked_ = masked;
    return ok();
}

Status LightmapTextures::upload(const lightmap_bake::BakedLightmap& lightmap) noexcept {
    if (device_ == nullptr) {
        return fail(ErrorCode::Unavailable, "LightmapTextures: initialize() was not called");
    }
    if (!well_formed(lightmap)) {
        return fail(ErrorCode::InvalidArgument,
                    "LightmapTextures: the lightmap has no texels, or a level of the wrong size");
    }
    const u64 fingerprint = fingerprint_of(lightmap);
    if (planes_ != 0 && fingerprint == fingerprint_) {
        return ok();
    }
    const lightmap_bake::LightmapTexels& texels = lightmap.texels;
    const bool masked = !lightmap.shadow_mask.texels.empty();
    const u32 levels = lightmap.mip_levels + 1U;
    if (planes_ != texels.planes || width_ != texels.width || height_ != texels.height ||
        masked_ != masked || mip_levels_ != levels) {
        if (Status made = recreate(texels.width, texels.height, texels.planes, masked, levels);
            !made) {
            return made;
        }
    }
    UploadRecording recording;
    recording.textures = textures_;
    recording.mip_levels = mip_levels_;
    recording.width = width_;
    recording.height = height_;
    plan_levels(lightmap, masked, recording);
    Expected<rhi::BufferHandle, Error> staging = stage(*device_, lightmap, recording);
    if (!staging.has_value()) {
        return make_unexpected(staging.error());
    }
    recording.staging = *staging;
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

pipeline::MaterialTextureSlot LightmapTextures::shadow_mask_slot(
    rhi::BindlessIndex index) const noexcept {
    if (!masked_) {
        return pipeline::MaterialTextureSlot{};
    }
    return pipeline::MaterialTextureSlot{index, views_[kMaxPlanes]};
}

rhi::TextureHandle LightmapTextures::texture(u32 plane) const noexcept {
    return plane < kMaxTextures ? textures_[plane] : rhi::TextureHandle{};
}

u64 LightmapTextures::device_bytes() const noexcept {
    u64 texels = 0;
    for (u32 level = 0; level < mip_levels_; ++level) {
        texels += u64{width_ >> level} * (height_ >> level);
    }
    return texels * (planes_ + (masked_ ? 1U : 0U)) * 8U;
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

namespace {

/// The frame index of the light a bake calls `id`, or `kNoLightmapLight` when the frame does not
/// shade it.
[[nodiscard]] u32 frame_index_of(Span<const u64> frame_light_ids, u64 id) noexcept {
    for (usize index = 0; index < frame_light_ids.size(); ++index) {
        if (frame_light_ids[index] == id) {
            return static_cast<u32>(index);
        }
    }
    return pipeline::kNoLightmapLight;
}

[[nodiscard]] Status write_lights(const lightmap_bake::BakedLightmap& lightmap,
                                  Span<const u64> frame_light_ids,
                                  pipeline::FrameViewData& view) noexcept {
    for (u32 word = 0; word < 4U; ++word) {
        view.lightmap_shadow_lights[word] = pipeline::kNoLightmapLight;
        view.lightmap_direct_lights[word] = 0;
    }
    for (usize channel = 0; channel < lightmap.shadow_lights.size() && channel < 4U; ++channel) {
        view.lightmap_shadow_lights[channel] =
            frame_index_of(frame_light_ids, lightmap.shadow_lights[channel]);
    }
    for (const u64 id : lightmap.direct_lights) {
        const u32 index = frame_index_of(frame_light_ids, id);
        if (index == pipeline::kNoLightmapLight) {
            continue;
        }
        if (index >= pipeline::kMaxLightmapDirectLights) {
            return fail(ErrorCode::InvalidArgument,
                        "write_lightmaps: a light whose direct term is baked is past the first 128 "
                        "of the frame's lights, and would be shaded twice");
        }
        view.lightmap_direct_lights[index / 32U] |= 1U << (index % 32U);
    }
    return ok();
}

}  // namespace

Status write_lightmaps(const LightmapSlots& slots, const lightmap_bake::BakedLightmap& lightmap,
                       gi::GiMode mode, Span<const u64> frame_light_ids,
                       pipeline::FrameViewData& view) noexcept {
    const u32 planes = lightmap.texels.planes;
    if (planes == 0 || planes > kMaxPlanes) {
        return fail(ErrorCode::InvalidArgument, "write_lightmaps: a lightmap has 1 to 3 planes");
    }
    for (u32 plane = 0; plane < planes; ++plane) {
        if (slots.planes[plane] == pipeline::kNoMaterialTexture) {
            return fail(ErrorCode::InvalidArgument,
                        "write_lightmaps: one texture slot per lightmap plane");
        }
    }
    if (!lightmap.shadow_lights.empty() && slots.shadow_mask == pipeline::kNoMaterialTexture) {
        return fail(ErrorCode::InvalidArgument,
                    "write_lightmaps: a lightmap with a shadow mask needs a slot for it, or its "
                    "stationary lights would shade unshadowed");
    }
    // A lightmapped surface that ALSO sits in a volume is the row that decides: the frame may only
    // switch the lightmap on for a mode that both admits it and excludes the volume for it.
    if (frame_ambient_source(mode, true, true) != AmbientSource::Lightmap) {
        return ok();
    }
    for (u32 plane = 0; plane < kMaxPlanes; ++plane) {
        view.lightmap_control[plane] =
            plane < planes ? slots.planes[plane] : pipeline::kNoMaterialTexture;
    }
    view.lightmap_control[3] = static_cast<u32>(lightmap.mode) + 1U;
    view.lightmap_layout[0] = lightmap.page_size;
    view.lightmap_layout[1] = lightmap.pages;
    view.lightmap_layout[2] = lightmap.gutter_texels;
    view.lightmap_layout[3] =
        lightmap.shadow_lights.empty() ? pipeline::kNoMaterialTexture : slots.shadow_mask;
    return write_lights(lightmap, frame_light_ids, view);
}

void write_lightmap_density_view(f32 target_texels_per_metre,
                                 pipeline::FrameViewData& view) noexcept {
    view.lightmap_debug[0] = pipeline::kLightmapDensityView;
    std::memcpy(&view.lightmap_debug[1], &target_texels_per_metre, sizeof(f32));
}

bool write_lightmap_debug_view(render::DebugViewMode mode, f32 target_texels_per_metre,
                               pipeline::FrameViewData& view) noexcept {
    if (mode != render::DebugViewMode::LightmapDensity) {
        return false;
    }
    write_lightmap_density_view(target_texels_per_metre, view);
    return true;
}

}  // namespace cy::rendering::lightmaps
