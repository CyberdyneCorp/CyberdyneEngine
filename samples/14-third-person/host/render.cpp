// SPDX-License-Identifier: MIT
// render.cpp — the third-person run drawn by the engine's frame. See render.h.

#include "render.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/assembly/frame_assembly.h>
#include <cy/rendering/gi_gpu/gpu_frame.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/lighting/soft_shadows.h>
#include <cy/rendering/material/standard.h>
#include <cy/rendering/occlusion/occlusion_pass.h>
#include <cy/rendering/pipeline/frame_recorder.h>
#include <cy/rendering/pipeline/material_textures.h>
#include <cy/rendering/skinning/frame_skinning.h>
#include <cy/rendering/skinning/skinned_scene.h>
#include <cy/rendering/upscale/upscale_pass.h>
#include <cy/servers/render/geometry/skin_dispatch.h>
#include <cy/servers/render/mesh.h>
#include <cy/servers/render/server.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "golden.h"
#include "hero_mesh.h"
#include "hero_rig.h"
#include "level.h"
#include "map_format.h"
#include "tps_host.h"

namespace sample::tps {
namespace {

namespace rendering = cy::rendering;
namespace pipeline = cy::rendering::pipeline;
namespace gi_gpu = cy::rendering::gi_gpu;
namespace upscale = cy::rendering::upscale;
namespace rhi = cy::rhi;
namespace map = sample::rts::map;
using cy::Aabb;
using cy::f32;
using cy::f64;
using cy::i32;
using cy::Mat4;
using cy::Status;
using cy::u16;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::usize;
using cy::Vec2;
using cy::Vec3;
using cy::Vec4;

/// The frame-texture slots the frame's own targets are bound at; the material textures take the
/// low slots the device's table hands out.
constexpr u32 kGiDiffuseSlot = 118;
constexpr u32 kGiSpecularSlot = 119;
constexpr u32 kOcclusionSlot = 120;
constexpr u32 kShadowSlot = 121;
constexpr u32 kShadowExtent = 2048;
/// The sun's map: a square this many metres round the hero, so the shadows near the camera are
/// sharp; what is further off is lit by the GI.
constexpr f32 kShadowRadius = 26.0F;
constexpr f32 kNear = 0.1F;
constexpr f32 kFar = 400.0F;
constexpr rhi::Format kOutputFormat = rhi::Format::Rgba8Unorm;
/// Exposure for a frame in physical units under the map's 26 000 lux sun.
constexpr f32 kExposureStops = -11.2F;
/// The stills, at the frames the script makes worth a picture: the courtyard ahead, on the stairs,
/// the jump on the platform, the camera pulled in by the north wall, running down the ramp, the
/// jump onto the hill, and looking back down from it.
constexpr u64 kStills[] = {120, 270, 410, 630, 880, 990, 1190};
constexpr u64 kWarmFrames = 10;
/// THE SKY: a dome round the eye, far inside the far plane and outside everything else, in bands
/// of elevation each glowing with its own colour — the frame's materials carry one emission each —
/// from the haze below the horizon to the zenith's blue, and the sun's disc.
constexpr u32 kSkyBands = 64;
constexpr u32 kSkySegments = 48;
constexpr f32 kSkyRadius = 320.0F;
constexpr f32 kSkyLowest = -0.3F;
constexpr f32 kSunDisc = 0.022F;
/// Tints a plant's variation byte is bucketed into, so a batch shares one.
constexpr u32 kTintBuckets = 3;
/// The square the foliage is batched in, metres.
constexpr f32 kBatchMetres = 32.0F;

[[nodiscard]] f64 since_ms(std::chrono::steady_clock::time_point start) noexcept {
    return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
        std::fprintf(stderr, "graphics validation error: %s\n", message != nullptr ? message : "");
    }
}

/// A rotation about +Y by `yaw`: +z goes to (sin, cos), as the map's kit is turned.
[[nodiscard]] Mat4 yaw_matrix(f32 yaw) noexcept {
    const f32 c = std::cos(yaw);
    const f32 s = std::sin(yaw);
    return Mat4::from_columns(Vec4{c, 0.0F, -s, 0.0F}, Vec4{0.0F, 1.0F, 0.0F, 0.0F},
                              Vec4{s, 0.0F, c, 0.0F}, Vec4{0.0F, 0.0F, 0.0F, 1.0F});
}

/// Tilts about the x and z axes: how a plant leans with the ground it stands on.
[[nodiscard]] Mat4 tilt_matrix(f32 about_x, f32 about_z) noexcept {
    const f32 cx = std::cos(about_x);
    const f32 sx = std::sin(about_x);
    const f32 cz = std::cos(about_z);
    const f32 sz = std::sin(about_z);
    const Mat4 x = Mat4::from_columns(Vec4{1.0F, 0.0F, 0.0F, 0.0F}, Vec4{0.0F, cx, sx, 0.0F},
                                      Vec4{0.0F, -sx, cx, 0.0F}, Vec4{0.0F, 0.0F, 0.0F, 1.0F});
    const Mat4 z = Mat4::from_columns(Vec4{cz, sz, 0.0F, 0.0F}, Vec4{-sz, cz, 0.0F, 0.0F},
                                      Vec4{0.0F, 0.0F, 1.0F, 0.0F}, Vec4{0.0F, 0.0F, 0.0F, 1.0F});
    return x * z;
}

[[nodiscard]] Mat4 placement_matrix(Vec3 position, f32 yaw, Vec3 scale) noexcept {
    return Mat4::from_translation(position) * yaw_matrix(yaw) * Mat4::from_scale(scale);
}

[[nodiscard]] Vec3 apply_point(const Mat4& m, Vec3 p) noexcept {
    const Vec4 r = m * Vec4{p.x, p.y, p.z, 1.0F};
    return Vec3{r.x, r.y, r.z};
}

[[nodiscard]] Vec3 apply_direction(const Mat4& m, Vec3 d) noexcept {
    const Vec4 r = m * Vec4{d.x, d.y, d.z, 0.0F};
    return cy::normalize(Vec3{r.x, r.y, r.z});
}

[[nodiscard]] Aabb transform_box(const Mat4& m, Vec3 low, Vec3 high) noexcept {
    Aabb out = Aabb::empty();
    for (u32 corner = 0; corner < 8U; ++corner) {
        const Vec3 p{(corner & 1U) != 0U ? high.x : low.x, (corner & 2U) != 0U ? high.y : low.y,
                     (corner & 4U) != 0U ? high.z : low.z};
        const Vec3 w = apply_point(m, p);
        out = Aabb::from_min_max(cy::cwise_min(out.min, w), cy::cwise_max(out.max, w));
    }
    return out;
}

/// Any unit vector perpendicular to `normal`: these materials sample no normal map.
[[nodiscard]] Vec3 any_tangent(Vec3 normal) noexcept {
    const Vec3 axis = std::fabs(normal.y) < 0.9F ? Vec3{0.0F, 1.0F, 0.0F} : Vec3{1.0F, 0.0F, 0.0F};
    return cy::normalize(cy::cross(axis, normal));
}

/// One drawable mesh in the frame's streams: where its vertices and its parts' indices are. A
/// part's material is a slot in the frame's material table.
struct DrawPart {
    u32 first_index = 0;
    u32 index_count = 0;
    u32 material = 0;
    bool translucent = false;
};

struct DrawMesh {
    i32 vertex_offset = 0;
    std::vector<DrawPart> parts;
    Vec3 low{0.0F, 0.0F, 0.0F};
    Vec3 high{0.0F, 0.0F, 0.0F};
};

/// The frame's three streams and its indices, built on the host and uploaded once.
struct Streams {
    std::vector<Vec3> positions;
    std::vector<u16> normals;
    std::vector<Vec2> uvs;
    std::vector<u32> indices;

    void vertex(Vec3 position, Vec3 normal, Vec2 uv) {
        positions.push_back(position);
        u16 packed[4];
        pipeline::pack_normal_stream(normal, any_tangent(normal), packed);
        normals.insert(normals.end(), packed, packed + 4);
        uvs.push_back(uv);
    }
};

}  // namespace

// --- The state -----------------------------------------------------------------------------------

struct Renderer::State {
    explicit State(cy::Allocator& memory) noexcept
        : allocator(&memory),
          assembly(memory),
          index(memory),
          graph(memory),
          program(memory),
          server(cy::system_allocator(cy::MemoryDomain::Gpu)),
          skins(memory),
          lights(memory) {}

    struct Instance {
        u32 mesh = 0;
        Mat4 world = Mat4::identity();
        Vec3 tint{1.0F, 1.0F, 1.0F};
        u32 index_slot = 0;
        rendering::skinning::SkinnedInstance skinned;
        std::vector<rendering::DrawSurface> surfaces;
    };

    cy::Allocator* allocator;
    RenderOptions options;
    rhi::Device* device = nullptr;
    rhi::BackendSelection selection{};
    u32 validation_errors = 0;
    ThirdPersonHost* host = nullptr;
    upscale::Extent2D render{};

    rendering::assembly::FrameAssembly assembly;
    rendering::SpatialIndex index;
    rendering::RenderGraph graph;
    rendering::MaterialProgram program;
    pipeline::FramePipelines pipelines;
    pipeline::FrameBindings bindings;
    pipeline::FrameRecorder recorder;
    cy::render::RenderServer server;
    pipeline::MaterialTextureTable textures;
    rendering::skinning::SkinnedScene skins;
    rendering::occlusion::AmbientOcclusionPass occlusion;
    gi_gpu::GpuGiFrame gi;
    upscale::TemporalUpscalePass upscaler;

    std::vector<DrawMesh> meshes;
    std::vector<Instance> instances;
    std::vector<pipeline::InstanceTransform> rows;
    rhi::BufferHandle positions;
    rhi::BufferHandle normals;
    rhi::BufferHandle uvs;
    rhi::BufferHandle indices;
    rhi::BufferHandle readback;
    rhi::TextureHandle output;
    rhi::TextureHandle shadow_color;
    rhi::TextureHandle shadow_depth;
    rhi::TextureViewHandle shadow_view;

    u32 material_offsets[4] = {};
    u32 material_texture_offsets[4] = {pipeline::kNoMaterialTexture, pipeline::kNoMaterialTexture,
                                       pipeline::kNoMaterialTexture, pipeline::kNoMaterialTexture};
    /// The map's materials, by index, as slots in the frame's table.
    std::vector<u32> material_slots;
    std::vector<bool> material_translucent;
    const map::MapLayout* layout = nullptr;
    std::vector<pipeline::MaterialTextureSlot> resident;
    cy::Array<cy::render::LightDescription> lights;

    u32 hero = 0;
    u32 sky = 0;
    /// The sky's materials: one a band, then the sun's.
    u32 first_sky_material = 0;
    i32 hero_uv_base = 0;
    std::vector<Mat4> pose;

    Mat4 view = Mat4::identity();
    Mat4 relative_view = Mat4::identity();
    Mat4 projection = Mat4::identity();
    Vec3 eye{0.0F, 0.0F, 0.0F};
    Vec3 forward{0.0F, 0.0F, -1.0F};
    f32 fov = 1.08F;
    Mat4 shadow_to_clip = Mat4::identity();
    rendering::FrameResourceRead opaque_reads[1];
    Vec3 sun_travel{-0.45F, -0.68F, -0.58F};
    f32 sun_lux = 26000.0F;

    RenderReport report;
    f64 last_record_ms = 0.0;
    f64 last_wait_ms = 0.0;
    std::vector<u32> pixels;
    u64 frame = 0;

    // --- Building
    // ----------------------------------------------------------------------------------

    [[nodiscard]] Status create_device() noexcept {
        if (options.device != nullptr) {
            device = options.device;  // the play host's: its swapchain presents what is drawn
            return cy::ok();
        }
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_sample_third-person";
        description.enable_validation = options.validation;
        cy::Allocator& gpu = cy::system_allocator(cy::MemoryDomain::Gpu);
        cy::Expected<rhi::Device*, cy::Error> made =
            rhi::create_device(gpu, "vulkan", description, selection);
        if (!made || made.value()->capabilities().backend() != rhi::BackendKind::Vulkan) {
            if (made) {
                rhi::destroy_device(gpu, made.value());
            }
            return cy::fail(cy::ErrorCode::Unavailable,
                            "no requested graphics device on this machine");
        }
        device = made.value();
        device->set_validation_callback(&count_validation, &validation_errors);
        return cy::ok();
    }

    [[nodiscard]] Status create_frame(u32 material_count) noexcept {
        const upscale::Extent2D extent{options.width, options.height};
        render = options.upscale ? upscale::render_extent_for(
                                       extent, upscale::upscale_mode_scale(options.upscale_mode))
                                 : extent;
        rendering::assembly::AssemblyDescription description;
        description.width = options.width;
        description.height = options.height;
        description.near_plane = kNear;
        description.far_plane = kFar;
        description.clusters = rendering::ClusterGridConfig{32, 18, 32};
        description.material_capacity = material_count;
        description.max_draws = 8192;
        description.max_instances = 1024;
        description.gpu_culling = false;
        description.post.ambient_occlusion = options.ambient_occlusion;
        description.global_illumination = options.gi;
        if (options.upscale) {
            // THE TEMPORAL UPSCALER: every scene pass at the render extent, reconstructed to the
            // output by the pass at the frame's temporal stage.
            description.post.temporal_upscaling = true;
            description.produced_temporal_stage = true;
            description.render_width = render.width;
            description.render_height = render.height;
            description.temporal.jitter = upscale::jitter_config_for(extent, render);
        } else {
            description.post.temporal_antialiasing = true;
        }
        if (Status made = assembly.initialize(description); !made) {
            return made;
        }
        if (Status attached = assembly.attach_device(*device); !attached) {
            return attached;
        }
        pipeline::PipelineSetup setup;
        setup.color_format = description.color_format;
        setup.depth_format = description.depth_format;
        setup.output_format = kOutputFormat;
        setup.prepass_normal = true;
        setup.prepass_velocity = true;
        setup.skinned = true;
        if (Status made = pipelines.initialize(*device, setup); !made) {
            return made;
        }
        cy::Expected<rendering::ClusterGrid, cy::Error> grid =
            rendering::make_cluster_grid(description.clusters, options.width, options.height,
                                         description.near_plane, description.far_plane);
        if (!grid) {
            return cy::make_unexpected(grid.error());
        }
        const pipeline::BindingCapacity capacity = pipeline::BindingCapacity::for_grid(
            *grid, description.max_draws, description.max_instances, description.material_capacity,
            16);
        if (Status made = bindings.initialize(*device, pipelines, capacity); !made) {
            return made;
        }
        if (Status made = recorder.initialize(pipelines, bindings); !made) {
            return made;
        }
        if (!options.upscale) {
            return cy::ok();
        }
        upscale::UpscalePassDescription pass;
        pass.output = extent;
        pass.depth_format = description.depth_format;
        if (Status made = upscaler.create(*allocator, *device, pass); !made) {
            return made;
        }
        upscale::UpscaleSettings settings;
        settings.mode = options.upscale_mode;
        settings.exposure = std::exp2(kExposureStops);
        upscaler.set_settings(settings);
        return upscaler.bind_temporal(assembly.temporal());
    }

    [[nodiscard]] Status upload_buffer(const char* name, const void* data, u64 bytes,
                                       rhi::BufferUsage usage,
                                       rhi::BufferHandle& out) const noexcept {
        rhi::BufferDescription description;
        description.name = name;
        description.size = std::max<u64>(bytes, 16U);
        description.usage = usage;
        description.memory = rhi::MemoryUse::Upload;
        cy::Expected<rhi::BufferHandle, cy::Error> made = device->create_buffer(description);
        if (!made) {
            return cy::make_unexpected(made.error());
        }
        out = *made;
        void* mapped = device->buffer_mapped_pointer(out);
        if (mapped == nullptr) {
            return cy::fail(cy::ErrorCode::Internal, "a level buffer is not mapped");
        }
        std::memcpy(mapped, data, bytes);
        return cy::ok();
    }

    /// The baked albedo and the content's textures, resident in the device's table.
    [[nodiscard]] Status create_textures(const map::MapContent& content,
                                         std::vector<u32>& slot_of_texture) noexcept {
        cy::render::RenderServerConfig config;
        config.debug_primitive_capacity = 16;
        config.debug_label_capacity = 4;
        if (Status configured = server.configure(config); !configured) {
            return configured;
        }
        if (Status initialized = server.initialize(); !initialized) {
            return initialized;
        }
        rhi::SamplerDescription sampler;
        sampler.name = "level material sampler";
        sampler.max_anisotropy = 8.0F;
        if (options.upscale) {
            // A texture as sharp at the render extent as it would be at the output's.
            sampler.mip_lod_bias = upscale::texture_lod_bias(render.width, options.width);
        }
        if (Status made = textures.initialize(*device, *allocator, sampler); !made) {
            return made;
        }
        std::vector<const map::TextureData*> sources;
        sources.reserve(content.textures.size() + 1U);
        for (const map::TextureData& texture : content.textures) {
            sources.push_back(&texture);
        }
        sources.push_back(&content.albedo);
        std::vector<pipeline::TextureUpload> uploads;
        std::vector<cy::render::TextureHandle> handles;
        for (const map::TextureData* texture : sources) {
            cy::render::TextureRecord record;
            record.name = cy::Name::intern("level texture");
            record.format = texture->srgb ? cy::render::TextureFormat::Rgba8Srgb
                                          : cy::render::TextureFormat::Rgba8Unorm;
            record.usage_class = cy::render::TextureUsageClass::Color;
            record.width = texture->width;
            record.height = texture->height;
            record.mip_levels = static_cast<u8>(texture->mips);
            cy::Expected<cy::render::TextureHandle, cy::Error> handle =
                server.create_texture(record);
            if (!handle) {
                return cy::make_unexpected(handle.error());
            }
            handles.push_back(*handle);
            uploads.push_back(pipeline::TextureUpload{
                *handle, cy::Span<const u8>(texture->pixels.data(), texture->pixels.size())});
        }
        if (Status uploaded = textures.upload(
                server, cy::Span<const pipeline::TextureUpload>(uploads.data(), uploads.size()));
            !uploaded) {
            return uploaded;
        }
        for (const cy::render::TextureHandle handle : handles) {
            slot_of_texture.push_back(textures.slot_of(handle));
        }
        resident.resize(textures.resident());
        (void)textures.slots(
            cy::Span<pipeline::MaterialTextureSlot>(resident.data(), resident.size()));
        return cy::ok();
    }

    [[nodiscard]] Status set_material(u32 slot, const map::MapMaterial& material,
                                      u32 texture) noexcept {
        const rendering::StandardParameters ids;
        rendering::MaterialTable& table = assembly.materials();
        if (Status defaults = rendering::apply_standard_defaults(program, table, slot, ids);
            !defaults) {
            return defaults;
        }
        const Vec3 colour = material.base_colour;
        const Vec3 emission = material.emission;
        if (Status set = table.set_color(program, slot, ids.base_color_factor,
                                         Vec4{colour.x, colour.y, colour.z, 1.0F});
            !set) {
            return set;
        }
        if (Status set = table.set_float(program, slot, ids.roughness_factor, material.roughness);
            !set) {
            return set;
        }
        if (Status set = table.set_float(program, slot, ids.metallic_factor, material.metallic);
            !set) {
            return set;
        }
        if (Status set = table.set_color(program, slot, ids.emission_color,
                                         Vec4{emission.x, emission.y, emission.z, 1.0F});
            !set) {
            return set;
        }
        return table.set_texture(program, slot, ids.base_color_texture, texture);
    }

    [[nodiscard]] Status create_materials(const map::MapContent& content,
                                          const std::vector<u32>& slot_of_texture) noexcept {
        if (Status described = rendering::describe_standard_material(
                program, cy::Name::intern("standard"), cy::render::ShadingModel::Lit,
                cy::render::BlendMode::Opaque);
            !described) {
            return described;
        }
        const rendering::StandardParameters ids;
        const rendering::ParameterId wanted[4] = {ids.base_color_factor, ids.roughness_factor,
                                                  ids.metallic_factor, ids.emission_color};
        for (u32 word = 0; word < 4U; ++word) {
            const rendering::MaterialParameter* parameter = program.find(wanted[word]);
            if (parameter == nullptr) {
                return cy::fail(cy::ErrorCode::NotFound,
                                "the standard material lacks a parameter the frame reads");
            }
            material_offsets[word] = parameter->offset / 4U;
        }
        const rendering::MaterialParameter* texture = program.find(ids.base_color_texture);
        if (texture == nullptr) {
            return cy::fail(cy::ErrorCode::NotFound, "the standard material has no texture slot");
        }
        material_texture_offsets[0] = texture->offset / 4U;

        rendering::MaterialTable& table = assembly.materials();
        const auto texture_slot = [&](u32 which) -> u32 {
            if (which == map::kNoTexture) {
                return pipeline::kNoMaterialTexture;
            }
            if (which == map::kAlbedoTexture) {
                return slot_of_texture.back();
            }
            return which < slot_of_texture.size() - 1U ? slot_of_texture[which]
                                                       : pipeline::kNoMaterialTexture;
        };
        for (const map::MapMaterial& material : content.layout.materials) {
            cy::Expected<u32, cy::Error> slot = table.allocate();
            if (!slot) {
                return cy::make_unexpected(slot.error());
            }
            material_slots.push_back(*slot);
            material_translucent.push_back(material.blend == map::Blend::Transparent);
            if (Status set = set_material(*slot, material, texture_slot(material.texture)); !set) {
                return set;
            }
        }
        return create_sky_materials();
    }

    /// The radiance of the sky `elevation` radians above the horizon, in the frame's units.
    [[nodiscard]] static Vec3 sky_radiance(f32 elevation) noexcept {
        const Vec3 haze{0.66F, 0.72F, 0.80F};
        const Vec3 horizon{0.70F, 0.82F, 0.97F};
        const Vec3 zenith{0.16F, 0.36F, 0.84F};
        if (elevation < 0.0F) {
            return haze * 2600.0F;
        }
        const f32 t = std::fmin(elevation / 1.5707963F, 1.0F);
        const f32 blend = std::pow(t, 0.45F);
        const Vec3 colour = horizon + ((zenith - horizon) * blend);
        return colour * (3400.0F - (1700.0F * t));
    }

    [[nodiscard]] Status create_sky_materials() noexcept {
        first_sky_material = static_cast<u32>(material_slots.size());
        rendering::MaterialTable& table = assembly.materials();
        for (u32 band = 0; band <= kSkyBands; ++band) {
            map::MapMaterial glow;
            glow.base_colour = Vec3{0.0F, 0.0F, 0.0F};
            glow.roughness = 1.0F;
            glow.metallic = 0.0F;
            glow.emission = band < kSkyBands
                                ? sky_radiance(kSkyLowest + ((static_cast<f32>(band) + 0.5F) *
                                                             (1.5707963F - kSkyLowest) /
                                                             static_cast<f32>(kSkyBands)))
                                : Vec3{1.6e6F, 1.5e6F, 1.3e6F};
            cy::Expected<u32, cy::Error> slot = table.allocate();
            if (!slot) {
                return cy::make_unexpected(slot.error());
            }
            material_slots.push_back(*slot);
            material_translucent.push_back(false);
            if (Status set = set_material(*slot, glow, pipeline::kNoMaterialTexture); !set) {
                return set;
            }
        }
        return cy::ok();
    }

    /// The dome, unit-sized round the origin, seen from inside: one part a band, and the sun's
    /// disc a cap round the direction its light comes from.
    [[nodiscard]] Status add_sky(Streams& streams) noexcept {
        DrawMesh dome;
        dome.vertex_offset = static_cast<i32>(streams.positions.size());
        dome.low = Vec3{-1.0F, kSkyLowest, -1.0F};
        dome.high = Vec3{1.0F, 1.0F, 1.0F};
        const f32 step = (1.5707963F - kSkyLowest) / static_cast<f32>(kSkyBands);
        const auto point = [](f32 elevation, f32 azimuth) noexcept {
            return Vec3{std::cos(elevation) * std::cos(azimuth), std::sin(elevation),
                        std::cos(elevation) * std::sin(azimuth)};
        };
        u32 local = 0;
        for (u32 band = 0; band < kSkyBands; ++band) {
            const f32 low = kSkyLowest + (static_cast<f32>(band) * step);
            const f32 high = low + step;
            const u32 first = static_cast<u32>(streams.indices.size());
            for (u32 segment = 0; segment < kSkySegments; ++segment) {
                const f32 a0 =
                    6.2831853F * static_cast<f32>(segment) / static_cast<f32>(kSkySegments);
                const f32 a1 =
                    6.2831853F * static_cast<f32>(segment + 1U) / static_cast<f32>(kSkySegments);
                const Vec3 corners[4] = {point(low, a0), point(low, a1), point(high, a1),
                                         point(high, a0)};
                for (const Vec3& corner : corners) {
                    streams.vertex(corner, corner * -1.0F, Vec2{0.0F, 0.0F});
                }
                // Wound to face the eye inside: clockwise seen from outside.
                streams.indices.insert(streams.indices.end(), {local, local + 1U, local + 2U, local,
                                                               local + 2U, local + 3U});
                local += 4U;
            }
            dome.parts.push_back(DrawPart{first, static_cast<u32>(streams.indices.size()) - first,
                                          first_sky_material + band, false});
        }
        // The sun's disc, a little inside the dome so it is drawn over it.
        const Vec3 toward = cy::normalize(sun_travel * -1.0F);
        const Vec3 side = any_tangent(toward);
        const Vec3 up = cy::cross(toward, side);
        const u32 first = static_cast<u32>(streams.indices.size());
        const u32 centre = local;
        streams.vertex(toward * 0.99F, toward * -1.0F, Vec2{0.0F, 0.0F});
        ++local;
        constexpr u32 kRim = 16;
        for (u32 rim = 0; rim < kRim; ++rim) {
            const f32 angle = 6.2831853F * static_cast<f32>(rim) / static_cast<f32>(kRim);
            const Vec3 at = cy::normalize(toward + (side * (std::cos(angle) * kSunDisc)) +
                                          (up * (std::sin(angle) * kSunDisc))) *
                            0.99F;
            streams.vertex(at, toward * -1.0F, Vec2{0.0F, 0.0F});
        }
        for (u32 rim = 0; rim < kRim; ++rim) {
            streams.indices.insert(streams.indices.end(),
                                   {centre, centre + 1U + ((rim + 1U) % kRim), centre + 1U + rim});
        }
        dome.parts.push_back(DrawPart{first, static_cast<u32>(streams.indices.size()) - first,
                                      first_sky_material + kSkyBands, false});
        meshes.push_back(std::move(dome));
        sky = static_cast<u32>(instances.size());
        return add_instance(static_cast<u32>(meshes.size() - 1U), Mat4::identity(),
                            Vec3{1.0F, 1.0F, 1.0F});
    }

    /// The frame's slot for the level's material `name`.
    [[nodiscard]] cy::Expected<u32, cy::Error> material_named(const char* name) const noexcept {
        const u32 found = layout->material(name);
        if (found == map::kNoMaterial) {
            return cy::fail(cy::ErrorCode::NotFound,
                            "the level declares no material the frame names");
        }
        return found;
    }

    void fill_surfaces(Instance& instance) {
        instance.surfaces.clear();
        const DrawMesh& mesh = meshes[instance.mesh];
        for (usize part = 0; part < mesh.parts.size(); ++part) {
            rendering::DrawSurface surface;
            surface.material = material_slots[mesh.parts[part].material];
            surface.mesh = instance.mesh;
            surface.surface = static_cast<u32>(part);
            surface.blend = mesh.parts[part].translucent ? cy::render::BlendMode::Translucent
                                                         : cy::render::BlendMode::Opaque;
            instance.surfaces.push_back(surface);
        }
    }

    [[nodiscard]] Status add_instance(u32 mesh, const Mat4& world, Vec3 tint) noexcept {
        Instance instance;
        instance.mesh = mesh;
        instance.world = world;
        instance.tint = tint;
        fill_surfaces(instance);
        rendering::SpatialEntry entry;
        entry.bounds = transform_box(world, meshes[mesh].low, meshes[mesh].high);
        entry.stable_id = 5000U + instances.size();
        entry.gpu_slot = static_cast<u32>(instances.size());
        entry.radius = cy::length(entry.bounds.max - entry.bounds.min) * 0.5F;
        cy::Expected<u32, cy::Error> slot = index.insert(entry);
        if (!slot) {
            return cy::make_unexpected(slot.error());
        }
        instance.index_slot = *slot;
        instances.push_back(std::move(instance));
        return cy::ok();
    }

    /// Every mesh of `library`, each a `DrawMesh`, in the library's order.
    void add_library(Streams& streams, const map::MeshLibrary& library) {
        const auto vertex_base = static_cast<i32>(streams.positions.size());
        const auto index_base = static_cast<u32>(streams.indices.size());
        for (usize vertex = 0; vertex < library.positions.size(); ++vertex) {
            streams.vertex(library.positions[vertex], library.normals[vertex], library.uvs[vertex]);
        }
        streams.indices.insert(streams.indices.end(), library.indices.begin(),
                               library.indices.end());
        for (const map::MeshRange& range : library.meshes) {
            DrawMesh mesh;
            mesh.vertex_offset = vertex_base + static_cast<i32>(range.first_vertex);
            for (u32 part = range.first_part; part < range.first_part + range.part_count; ++part) {
                const map::MeshPart& source = library.parts[part];
                mesh.parts.push_back(DrawPart{source.first_index + index_base, source.index_count,
                                              source.material,
                                              material_translucent[source.material]});
            }
            mesh.low = range.min;
            mesh.high = range.max;
            meshes.push_back(std::move(mesh));
        }
    }

    /// The foliage, merged a 32 m square, a species and a tint at a time: one draw a material.
    [[nodiscard]] Status add_foliage(Streams& streams, const map::MapContent& content,
                                     u32 kit) noexcept {
        const map::FoliageData& foliage = content.foliage;
        const map::MeshLibrary& library = content.props;
        const auto squares = static_cast<u32>(std::ceil(content.layout.size / kBatchMetres));
        for (u32 square = 0; square < squares * squares; ++square) {
            const u32 column = square % squares;
            const u32 row = square / squares;
            const f32 min_x = content.layout.origin_x + (static_cast<f32>(column) * kBatchMetres);
            const f32 min_z = content.layout.origin_z + (static_cast<f32>(row) * kBatchMetres);
            const Vec3 origin{min_x + (kBatchMetres * 0.5F), 0.0F, min_z + (kBatchMetres * 0.5F)};
            for (u32 species = 0; species < foliage.species.size(); ++species) {
                const map::MeshRange& range = library.meshes[foliage.species[species].mesh];
                for (u32 bucket = 0; bucket < kTintBuckets; ++bucket) {
                    DrawMesh batch;
                    batch.vertex_offset = static_cast<i32>(streams.positions.size());
                    batch.low = Vec3{1e9F, 1e9F, 1e9F};
                    batch.high = Vec3{-1e9F, -1e9F, -1e9F};
                    std::vector<std::vector<u32>> part_indices(range.part_count);
                    u32 local = 0;
                    for (const map::FoliagePlant& plant : foliage.plants) {
                        if (plant.species != species ||
                            (static_cast<u32>(plant.variation) % kTintBuckets) != bucket ||
                            plant.position.x < min_x || plant.position.x >= min_x + kBatchMetres ||
                            plant.position.z < min_z || plant.position.z >= min_z + kBatchMetres) {
                            continue;
                        }
                        const Vec3 at{plant.position.x - origin.x, plant.position.y,
                                      plant.position.z - origin.z};
                        const Mat4 place =
                            Mat4::from_translation(at) * tilt_matrix(plant.tilt_x, plant.tilt_z) *
                            yaw_matrix(plant.yaw) *
                            Mat4::from_scale(Vec3{plant.scale, plant.scale, plant.scale});
                        for (u32 vertex = 0; vertex < range.vertex_count; ++vertex) {
                            const u32 from = range.first_vertex + vertex;
                            const Vec3 p = apply_point(place, library.positions[from]);
                            streams.vertex(p, apply_direction(place, library.normals[from]),
                                           library.uvs[from]);
                            batch.low = cy::cwise_min(batch.low, p);
                            batch.high = cy::cwise_max(batch.high, p);
                        }
                        for (u32 part = 0; part < range.part_count; ++part) {
                            const map::MeshPart& source = library.parts[range.first_part + part];
                            for (u32 corner = 0; corner < source.index_count; ++corner) {
                                part_indices[part].push_back(
                                    local + library.indices[source.first_index + corner]);
                            }
                        }
                        local += range.vertex_count;
                    }
                    if (local == 0) {
                        continue;
                    }
                    for (u32 part = 0; part < range.part_count; ++part) {
                        const map::MeshPart& source = library.parts[range.first_part + part];
                        batch.parts.push_back(DrawPart{static_cast<u32>(streams.indices.size()),
                                                       static_cast<u32>(part_indices[part].size()),
                                                       source.material,
                                                       material_translucent[source.material]});
                        streams.indices.insert(streams.indices.end(), part_indices[part].begin(),
                                               part_indices[part].end());
                    }
                    const map::FoliageSpecies& kind = foliage.species[species];
                    const f32 t =
                        (static_cast<f32>(bucket) + 0.5F) / static_cast<f32>(kTintBuckets);
                    meshes.push_back(std::move(batch));
                    if (Status placed = add_instance(
                            static_cast<u32>(meshes.size() - 1U), Mat4::from_translation(origin),
                            kind.tint_a + ((kind.tint_b - kind.tint_a) * t));
                        !placed) {
                        return placed;
                    }
                }
            }
        }
        (void)kit;
        return cy::ok();
    }

    /// The courtyard's pieces: a box each, its UVs a metre a repeat on every face, turned by its
    /// slope, in the world already.
    [[nodiscard]] Status add_pieces(Streams& streams) noexcept {
        for (const LevelPiece& piece : level_pieces()) {
            if (piece.material[0] == '\0') {
                continue;
            }
            const cy::Expected<u32, cy::Error> material = material_named(piece.material);
            if (!material) {
                return cy::make_unexpected(material.error());
            }
            const Mat4 place =
                Mat4::from_translation(piece.centre) *
                Mat4::from_quat(cy::Quat::from_axis_angle(Vec3{0.0F, 0.0F, 1.0F}, piece.roll));
            DrawMesh mesh;
            mesh.vertex_offset = static_cast<i32>(streams.positions.size());
            mesh.low = Vec3{1e9F, 1e9F, 1e9F};
            mesh.high = Vec3{-1e9F, -1e9F, -1e9F};
            const u32 first_index = static_cast<u32>(streams.indices.size());
            const Vec3 h = piece.half;
            // Each face: its normal, and its two in-plane axes with their half lengths.
            const struct {
                Vec3 normal;
                Vec3 u;
                Vec3 v;
                f32 hu{};
                f32 hv{};
                f32 hn{};
            } faces[] = {
                {Vec3{1, 0, 0}, Vec3{0, 0, -1}, Vec3{0, 1, 0}, h.z, h.y, h.x},
                {Vec3{-1, 0, 0}, Vec3{0, 0, 1}, Vec3{0, 1, 0}, h.z, h.y, h.x},
                {Vec3{0, 1, 0}, Vec3{1, 0, 0}, Vec3{0, 0, -1}, h.x, h.z, h.y},
                {Vec3{0, -1, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1}, h.x, h.z, h.y},
                {Vec3{0, 0, 1}, Vec3{1, 0, 0}, Vec3{0, 1, 0}, h.x, h.y, h.z},
                {Vec3{0, 0, -1}, Vec3{-1, 0, 0}, Vec3{0, 1, 0}, h.x, h.y, h.z},
            };
            u32 local = 0;
            for (const auto& face : faces) {
                const Vec3 normal = apply_direction(place, face.normal);
                for (u32 corner = 0; corner < 4U; ++corner) {
                    const f32 su = (corner == 1U || corner == 2U) ? 1.0F : -1.0F;
                    const f32 sv = corner >= 2U ? 1.0F : -1.0F;
                    const Vec3 model = (face.normal * face.hn) + (face.u * (su * face.hu)) +
                                       (face.v * (sv * face.hv));
                    const Vec3 world = apply_point(place, model);
                    // A metre of stone a repeat, measured on the face itself.
                    const Vec2 uv{(su * face.hu) * 0.5F, (sv * face.hv) * 0.5F};
                    streams.vertex(world, normal, uv);
                    mesh.low = cy::cwise_min(mesh.low, world);
                    mesh.high = cy::cwise_max(mesh.high, world);
                }
                streams.indices.insert(streams.indices.end(), {local, local + 1U, local + 2U, local,
                                                               local + 2U, local + 3U});
                local += 4U;
            }
            mesh.parts.push_back(DrawPart{first_index,
                                          static_cast<u32>(streams.indices.size()) - first_index,
                                          *material, false});
            meshes.push_back(std::move(mesh));
            if (Status placed = add_instance(static_cast<u32>(meshes.size() - 1U), Mat4::identity(),
                                             Vec3{1.0F, 1.0F, 1.0F});
                !placed) {
                return placed;
            }
        }
        return cy::ok();
    }

    /// The hero: its UVs and indices in the frame's streams, its positions and frames the skinning
    /// pass's own.
    [[nodiscard]] Status add_hero(Streams& streams) noexcept {
        const HeroMesh mesh = build_hero_mesh();
        hero_uv_base = static_cast<i32>(streams.positions.size());
        for (usize vertex = 0; vertex < mesh.positions.size(); ++vertex) {
            streams.vertex(mesh.positions[vertex], mesh.normals[vertex], mesh.uvs[vertex]);
        }
        const auto index_base = static_cast<u32>(streams.indices.size());
        streams.indices.insert(streams.indices.end(), mesh.indices.begin(), mesh.indices.end());
        DrawMesh figure;
        for (const HeroPart& part : mesh.parts) {
            const cy::Expected<u32, cy::Error> material = material_named(part.material.c_str());
            if (!material) {
                return cy::make_unexpected(material.error());
            }
            figure.parts.push_back(
                DrawPart{part.first_index + index_base, part.index_count, *material, false});
        }
        // A posed hero stays inside this box round its feet.
        figure.low = Vec3{-1.0F, -0.5F, -1.0F};
        figure.high = Vec3{1.0F, 2.3F, 1.0F};
        meshes.push_back(figure);

        rendering::skinning::SkinnedSceneDescription description;
        description.max_mesh_vertices = static_cast<u32>(mesh.positions.size());
        description.max_instance_vertices = static_cast<u32>(mesh.positions.size());
        description.max_pose_matrices = kHeroJoints * 2U;
        description.max_meshes = 1;
        description.max_instances = 1;
        if (Status made = skins.create(*device, description); !made) {
            return made;
        }
        std::vector<cy::render::PackedNormalTangent> frames;
        std::vector<cy::render::geometry::GpuSkinInfluence> influences;
        for (usize vertex = 0; vertex < mesh.positions.size(); ++vertex) {
            frames.push_back(cy::render::pack_normal_tangent(
                mesh.normals[vertex], any_tangent(mesh.normals[vertex]), 1.0F));
            const u8 bones[4] = {mesh.joints[vertex], 0, 0, 0};
            const u8 weights[4] = {255, 0, 0, 0};
            influences.push_back(cy::render::geometry::skin_influence(bones, weights));
        }
        rendering::skinning::SkinnedMeshDescription skinned;
        skinned.positions = cy::Span<const Vec3>(mesh.positions.data(), mesh.positions.size());
        skinned.frames =
            cy::Span<const cy::render::PackedNormalTangent>(frames.data(), frames.size());
        skinned.influences = cy::Span<const cy::render::geometry::GpuSkinInfluence>(
            influences.data(), influences.size());
        skinned.bone_count = kHeroJoints;
        cy::Expected<rendering::skinning::SkinnedMeshId, cy::Error> added = skins.add_mesh(skinned);
        if (!added) {
            return cy::make_unexpected(added.error());
        }
        hero = static_cast<u32>(instances.size());
        if (Status placed = add_instance(static_cast<u32>(meshes.size() - 1U), Mat4::identity(),
                                         Vec3{1.0F, 1.0F, 1.0F});
            !placed) {
            return placed;
        }
        cy::Expected<rendering::skinning::SkinnedInstance, cy::Error> instance =
            skins.add_instance(*added);
        if (!instance) {
            return cy::make_unexpected(instance.error());
        }
        instances.back().skinned = *instance;
        pose.assign(kHeroJoints, Mat4::identity());
        return cy::ok();
    }

    [[nodiscard]] Status create_geometry(const map::MapContent& content) noexcept {
        Streams streams;
        // The ground, then the kit: mesh 0.. is the terrain and the kit follows in its own order.
        add_library(streams, content.ground);
        const auto kit = static_cast<u32>(meshes.size());
        add_library(streams, content.props);
        for (u32 mesh = 0; mesh < kit; ++mesh) {
            if (Status placed = add_instance(mesh, Mat4::identity(), Vec3{1.0F, 1.0F, 1.0F});
                !placed) {
                return placed;
            }
        }
        for (const map::Placement& placement : content.layout.placements) {
            if (Status placed = add_instance(
                    kit + placement.mesh,
                    placement_matrix(placement.position, placement.yaw, placement.scale),
                    placement.tint);
                !placed) {
                return placed;
            }
        }
        if (Status planted = add_foliage(streams, content, kit); !planted) {
            return planted;
        }
        if (Status built = add_pieces(streams); !built) {
            return built;
        }
        if (Status built = add_sky(streams); !built) {
            return built;
        }
        if (Status built = add_hero(streams); !built) {
            return built;
        }
        if (Status made = upload_buffer("level positions", streams.positions.data(),
                                        streams.positions.size() * sizeof(Vec3),
                                        rhi::BufferUsage::Vertex, positions);
            !made) {
            return made;
        }
        if (Status made = upload_buffer("level normals", streams.normals.data(),
                                        streams.normals.size() * sizeof(u16),
                                        rhi::BufferUsage::Vertex, normals);
            !made) {
            return made;
        }
        if (Status made =
                upload_buffer("level uvs", streams.uvs.data(), streams.uvs.size() * sizeof(Vec2),
                              rhi::BufferUsage::Vertex, uvs);
            !made) {
            return made;
        }
        if (Status made = upload_buffer("level indices", streams.indices.data(),
                                        streams.indices.size() * sizeof(u32),
                                        rhi::BufferUsage::Index, indices);
            !made) {
            return made;
        }
        report.triangles = static_cast<u32>(streams.indices.size() / 3U);
        report.plants = static_cast<u32>(content.foliage.plants.size());
        report.instances = static_cast<u32>(instances.size());
        rows.resize(instances.size());

        pipeline::GeometrySource source;
        source.streams[pipeline::kPositionStream] = positions;
        source.streams[pipeline::kNormalStream] = normals;
        source.streams[pipeline::kUvStream] = uvs;
        source.geometry = &State::geometry;
        source.user = this;
        recorder.set_geometry(source);
        return cy::ok();
    }

    [[nodiscard]] Status create_texture(const char* name, rhi::Format format, u32 width, u32 height,
                                        rhi::TextureUsage usage,
                                        rhi::TextureHandle& out) const noexcept {
        rhi::TextureDescription description;
        description.name = name;
        description.format = format;
        description.extent = rhi::Extent3D{width, height, 1};
        description.usage = usage;
        cy::Expected<rhi::TextureHandle, cy::Error> made = device->create_texture(description);
        if (!made) {
            return cy::make_unexpected(made.error());
        }
        out = *made;
        return cy::ok();
    }

    [[nodiscard]] Status create_targets() noexcept {
        rhi::BufferDescription host_copy;
        host_copy.name = "third-person readback";
        host_copy.size = u64{options.width} * options.height * 4U;
        host_copy.usage = rhi::BufferUsage::TransferDestination;
        host_copy.memory = rhi::MemoryUse::Readback;
        cy::Expected<rhi::BufferHandle, cy::Error> buffer = device->create_buffer(host_copy);
        if (!buffer) {
            return cy::make_unexpected(buffer.error());
        }
        readback = *buffer;
        if (Status made =
                create_texture("third-person output", kOutputFormat, options.width, options.height,
                               rhi::TextureUsage::ColorAttachment |
                                   rhi::TextureUsage::TransferSource | rhi::TextureUsage::Sampled,
                               output);
            !made) {
            return made;
        }
        if (!options.shadows) {
            return cy::ok();
        }
        if (Status made = create_texture(
                "third-person shadow map", rhi::Format::R32Sfloat, kShadowExtent, kShadowExtent,
                rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled, shadow_color);
            !made) {
            return made;
        }
        if (Status made = create_texture("third-person shadow depth", rhi::Format::D32Sfloat,
                                         kShadowExtent, kShadowExtent,
                                         rhi::TextureUsage::DepthStencilAttachment, shadow_depth);
            !made) {
            return made;
        }
        rhi::TextureViewDescription view_description;
        view_description.texture = shadow_color;
        cy::Expected<rhi::TextureViewHandle, cy::Error> made_view =
            device->create_texture_view(view_description);
        if (!made_view) {
            return cy::make_unexpected(made_view.error());
        }
        shadow_view = *made_view;
        return cy::ok();
    }

    // --- The frame's lookups
    // -------------------------------------------------------------------------

    static cy::Span<const rendering::DrawSurface> surfaces(
        const rendering::VisibleInstance& visible, void* user) noexcept {
        auto* self = static_cast<State*>(user);
        if (visible.gpu_slot >= self->instances.size()) {
            return {};
        }
        const Instance& instance = self->instances[visible.gpu_slot];
        return {instance.surfaces.data(), instance.surfaces.size()};
    }

    static bool geometry(const cy::render::DrawItem& item, const rendering::GpuDrawInstance& draw,
                         void* user, pipeline::DrawGeometry& out) noexcept {
        auto* self = static_cast<State*>(user);
        if (draw.instance_slot >= self->instances.size()) {
            return false;
        }
        const Instance& instance = self->instances[draw.instance_slot];
        const DrawMesh& mesh = self->meshes[instance.mesh];
        if (item.surface >= mesh.parts.size()) {
            return false;
        }
        const DrawPart& part = mesh.parts[item.surface];
        if (instance.skinned.valid()) {
            rendering::skinning::SkinnedDrawMesh skinned;
            skinned.indices = self->indices;
            skinned.wide_indices = true;
            skinned.index_count = part.index_count;
            skinned.first_index = part.first_index;
            skinned.static_vertex_offset = self->hero_uv_base;
            return rendering::skinning::skinned_draw_geometry(self->skins, instance.skinned,
                                                              skinned, out);
        }
        out.indices = self->indices;
        out.wide_indices = true;
        out.index_count = part.index_count;
        out.first_index = part.first_index;
        out.vertex_offset = mesh.vertex_offset;
        return true;
    }

    // --- One frame
    // -----------------------------------------------------------------------------------

    void set_camera(const ThirdPersonHost& from) noexcept {
        const f32 aspect = static_cast<f32>(options.width) / static_cast<f32>(options.height);
        eye = from.camera_position();
        forward = cy::normalize(from.camera_forward());
        fov = from.camera_fov();
        view = cy::look_at(eye, eye + forward, Vec3{0.0F, 1.0F, 0.0F});
        relative_view = cy::look_at(Vec3{0.0F, 0.0F, 0.0F}, forward, Vec3{0.0F, 1.0F, 0.0F});
        projection = cy::perspective_reversed_z(fov, aspect, kNear, kFar);
        // THE SUN'S MAP follows the hero: an orthographic box round it, camera-relative as the
        // frame's rows are.
        const Vec3 travel = cy::normalize(sun_travel);
        const Vec3 centre = from.drawn_hero().translation - eye;
        const Vec3 light_eye = centre - (travel * (kShadowRadius * 2.5F));
        const Mat4 light_view = cy::look_at(light_eye, centre, Vec3{0.0F, 1.0F, 0.0F});
        const Mat4 light_projection =
            cy::orthographic_reversed_z(-kShadowRadius, kShadowRadius, -kShadowRadius,
                                        kShadowRadius, 0.5F, kShadowRadius * 5.0F);
        shadow_to_clip = light_projection * light_view;
    }

    void place_rows() noexcept {
        for (usize slot = 0; slot < instances.size(); ++slot) {
            const Instance& instance = instances[slot];
            pipeline::InstanceTransform& row = rows[slot];
            for (u32 column = 0; column < 4U; ++column) {
                row.row0[column] = instance.world.at(0, column);
                row.row1[column] = instance.world.at(1, column);
                row.row2[column] = instance.world.at(2, column);
            }
            row.row0[3] -= eye.x;
            row.row1[3] -= eye.y;
            row.row2[3] -= eye.z;
            row.tint[0] = instance.tint.x;
            row.tint[1] = instance.tint.y;
            row.tint[2] = instance.tint.z;
            row.tint[3] = 1.0F;
        }
    }

    /// The sky round the eye: it has no parallax, whatever the camera does.
    [[nodiscard]] Status place_sky() noexcept {
        Instance& dome = instances[sky];
        dome.world = Mat4::from_translation(eye) *
                     Mat4::from_scale(Vec3{kSkyRadius, kSkyRadius, kSkyRadius});
        const DrawMesh& mesh = meshes[dome.mesh];
        return index.update(dome.index_slot, transform_box(dome.world, mesh.low, mesh.high));
    }

    /// The hero where its node is, posed as the animation system published it.
    [[nodiscard]] Status place_hero(const ThirdPersonHost& from) noexcept {
        Instance& figure = instances[hero];
        figure.world = from.drawn_hero().to_matrix();
        const DrawMesh& mesh = meshes[figure.mesh];
        if (Status moved =
                index.update(figure.index_slot, transform_box(figure.world, mesh.low, mesh.high));
            !moved) {
            return moved;
        }
        const cy::Span<const Mat4> published = from.hero_pose();
        if (published.size() >= kHeroJoints) {
            std::copy(published.begin(), published.begin() + kHeroJoints, pose.begin());
        }
        if (Status uploaded =
                skins.upload_poses(cy::Span<const Mat4>(pose.data(), pose.size()), 0, kHeroJoints);
            !uploaded) {
            return uploaded;
        }
        return skins.set_pose(figure.skinned, 0);
    }

    [[nodiscard]] Status before_assemble(rendering::assembly::AssemblyView& assembly_view,
                                         rendering::assembly::FrameSinks& sinks) noexcept {
        if (Status declared = skins.declare(graph, frame); !declared) {
            return declared;
        }
        const cy::Span<const rendering::ResourceId> reads = skins.vertex_reads();
        for (const rendering::FramePassKind kind :
             {rendering::FramePassKind::DepthPrepass, rendering::FramePassKind::Shadow,
              rendering::FramePassKind::Opaque, rendering::FramePassKind::Transparent}) {
            sinks.passes[static_cast<usize>(kind)].vertex_reads = reads;
        }
        if (options.shadows) {
            rendering::TextureRequest request;
            request.name = "third-person shadow map";
            request.format = rhi::Format::R32Sfloat;
            request.width = kShadowExtent;
            request.height = kShadowExtent;
            assembly_view.shadow_color =
                graph.import_texture(request, shadow_color, rhi::ImageUse::Undefined);
            request.name = "third-person shadow depth";
            request.format = rhi::Format::D32Sfloat;
            assembly_view.shadow_depth =
                graph.import_texture(request, shadow_depth, rhi::ImageUse::Undefined);
            recorder.set_shadow_targets(assembly_view.shadow_color, assembly_view.shadow_depth,
                                        kShadowExtent);
            const auto shadow = static_cast<usize>(rendering::FramePassKind::Shadow);
            sinks.passes[shadow] = recorder.sinks().passes[shadow];
            sinks.passes[shadow].vertex_reads = reads;
            opaque_reads[0] = rendering::FrameResourceRead{assembly_view.shadow_color,
                                                           rhi::Access::FragmentSampledRead};
            sinks.passes[static_cast<usize>(rendering::FramePassKind::Opaque)].reads =
                cy::Span<const rendering::FrameResourceRead>(opaque_reads, 1);
        }
        if (options.ambient_occlusion) {
            rendering::occlusion::GtaoView gtao;
            gtao.projection = projection;
            gtao.relative_to_view = relative_view;
            gtao.width = render.width;
            gtao.height = render.height;
            if (Status set = occlusion.set_view(gtao); !set) {
                return set;
            }
            assembly_view.ambient_occlusion = occlusion.import_target(graph);
            sinks.ambient_occlusion = occlusion.stage();
        }
        if (options.upscale) {
            sinks.temporal = upscale::stage_of(upscaler);
        }
        if (!options.gi) {
            return cy::ok();
        }
        gi_gpu::GpuGiFrameView gi_view;
        gi_view.eye = eye;
        gi_view.world_to_clip = projection * view;
        gi_view.projection = projection;
        gi_view.relative_to_view = relative_view;
        gi_gpu::GpuGiFrameTargets targets;
        if (Status prepared = gi.prepare(graph, gi_view, host->illumination().frame(), targets);
            !prepared) {
            return prepared;
        }
        assembly_view.screen_space_gi = targets.diffuse;
        assembly_view.screen_space_gi_specular = targets.specular;
        sinks.screen_space_gi = gi.stage();
        return cy::ok();
    }

    [[nodiscard]] Status before_upload(pipeline::FrameUpload& upload) noexcept {
        std::vector<pipeline::MaterialTextureSlot> slots = resident;
        if (options.ambient_occlusion) {
            rendering::occlusion::GtaoView gtao;
            gtao.projection = projection;
            gtao.relative_to_view = relative_view;
            gtao.width = render.width;
            gtao.height = render.height;
            gtao.jitter = Vec2{upload.view.temporal_jitter[0], upload.view.temporal_jitter[1]};
            if (Status set = occlusion.set_view(gtao); !set) {
                return set;
            }
            slots.push_back(pipeline::MaterialTextureSlot{kOcclusionSlot, occlusion.target_view()});
            rendering::occlusion::write_occlusion_control(
                kOcclusionSlot, occlusion.settings().shared, upload.view.occlusion_control);
        }
        if (options.shadows) {
            u32 sun = static_cast<u32>(upload.lights.size());
            for (usize light = 0; light < upload.lights.size(); ++light) {
                if (upload.lights[light].kind == rendering::kGpuLightDirectional) {
                    sun = static_cast<u32>(light);
                }
            }
            for (u32 row = 0; row < 4U; ++row) {
                for (u32 column = 0; column < 4U; ++column) {
                    upload.view.shadow_to_clip[(row * 4U) + column] =
                        shadow_to_clip.at(row, column);
                }
            }
            upload.view.shadow_control[0] = kShadowSlot;
            upload.view.shadow_control[1] = sun;
            upload.view.shadow_control[2] = kShadowExtent;
            upload.view.shadow_control[3] = 1U;
            rendering::SoftShadowSettings soft;
            soft.quality = rendering::ShadowQuality::High;
            soft.max_radius_texels = 6.0F;
            rendering::DirectionalShadowFootprint footprint;
            footprint.depth_range_metres = (kShadowRadius * 5.0F) - 0.5F;
            footprint.width_metres = kShadowRadius * 2.0F;
            footprint.extent = kShadowExtent;
            rendering::write_soft_shadow_words(
                pipeline::kSoftShadowPcss, pipeline::kNoMaterialTexture,
                rendering::make_pcss_shape(soft, footprint), upload.view.soft_shadow_control,
                upload.view.soft_shadow_shape);
            slots.push_back(pipeline::MaterialTextureSlot{kShadowSlot, shadow_view});
        }
        if (options.gi) {
            gi.set_jitter(Vec2{upload.view.temporal_jitter[0], upload.view.temporal_jitter[1]});
            slots.push_back(pipeline::MaterialTextureSlot{kGiDiffuseSlot, gi.diffuse_view()});
            slots.push_back(pipeline::MaterialTextureSlot{kGiSpecularSlot, gi.specular_view()});
            if (gi.composites()) {
                gi.write_control(kGiDiffuseSlot, kGiSpecularSlot, upload.view.gi_control);
            }
        }
        return bindings.set_material_textures(
            cy::Span<const pipeline::MaterialTextureSlot>(slots.data(), slots.size()));
    }

    struct Capture {
        rendering::ResourceId output = rendering::kInvalidResource;
        rhi::BufferHandle buffer;
        u32 width = 0;
        u32 height = 0;
    };
    Capture capture;

    static void record_capture(const rendering::PassContext& context, void* user) noexcept {
        auto* copy = static_cast<Capture*>(user);
        rhi::BufferTextureCopy region;
        region.texture_extent = rhi::Extent3D{copy->width, copy->height, 1};
        context.commands->copy_texture_to_buffer(
            context.executor->texture(copy->output), copy->buffer,
            cy::Span<const rhi::BufferTextureCopy>(&region, 1));
    }

    [[nodiscard]] Status render_frame(bool read_back,
                                      rendering::assembly::AssemblyReport& out) noexcept {
        cy::Expected<u32, cy::Error> began = device->begin_frame();
        if (!began) {
            return cy::make_unexpected(began.error());
        }
        const u32 slot = *began;
        graph.reset();
        rendering::assembly::AssemblyView frame_view;
        frame_view.fov_y_radians = fov;
        frame_view.view = view;
        frame_view.projection = projection;
        // THE CULL LOOKS WIDER THAN THE PICTURE, so what stands just off screen still casts its
        // shadow into it: the shadow pass draws the opaque list the cull produced.
        const f32 aspect = static_cast<f32>(options.width) / static_cast<f32>(options.height);
        const Mat4 wide =
            cy::perspective_reversed_z(std::min(fov * 1.8F, 2.8F), aspect, kNear, kFar);
        frame_view.cull.frustum = cy::Frustum::from_view_projection(wide * view);
        frame_view.cull.camera_position = eye;
        frame_view.cull.camera_forward = forward;
        frame_view.cull.fov_y_radians = fov;
        frame_view.lights = lights.span();
        frame_view.sun_direction = cy::normalize(sun_travel * -1.0F);
        rendering::TextureRequest request;
        request.name = "third-person output";
        request.format = kOutputFormat;
        request.width = options.width;
        request.height = options.height;
        request.extra_usage = rhi::TextureUsage::TransferSource | rhi::TextureUsage::Sampled;
        frame_view.output = graph.import_texture(request, output, rhi::ImageUse::Undefined);

        if (Status bound = recorder.bind(assembly); !bound) {
            return bound;
        }
        rendering::assembly::FrameSinks sinks = recorder.sinks();
        sinks.surfaces = &State::surfaces;
        sinks.surfaces_user = this;
        if (Status hooked = before_assemble(frame_view, sinks); !hooked) {
            return hooked;
        }
        if (Status assembled = assembly.assemble(index, frame_view, sinks, graph, out);
            !assembled) {
            return assembled;
        }
        pipeline::GlobalsData globals;
        globals.exposure_stops = kExposureStops;
        pipeline::FrameUpload upload = pipeline::upload_for(
            assembly, out, projection * relative_view, relative_view,
            cy::Span<const pipeline::InstanceTransform>(rows.data(), rows.size()), globals,
            material_offsets);
        for (u32 word = 0; word < 4U; ++word) {
            upload.view.material_textures[word] = material_texture_offsets[word];
        }
        if (Status hooked = before_upload(upload); !hooked) {
            return hooked;
        }
        if (Status uploaded = bindings.upload(slot, upload); !uploaded) {
            return uploaded;
        }
        capture = Capture{assembly.resources().output, readback, options.width, options.height};
        if (read_back && capture.output != rendering::kInvalidResource) {
            rendering::BufferRequest capture_request;
            capture_request.name = "third-person capture";
            capture_request.size = u64{options.width} * options.height * 4U;
            capture_request.extra_usage = rhi::BufferUsage::TransferDestination;
            const rendering::ResourceId destination =
                graph.import_buffer(capture_request, readback);
            graph.add_pass("third-person capture", rhi::QueueKind::Graphics)
                .read(capture.output, rhi::Access::TransferRead)
                .write(destination, rhi::Access::TransferWrite)
                .record(&State::record_capture, &capture);
            graph.add_pass("third-person capture host", rhi::QueueKind::Graphics)
                .read(destination, rhi::Access::HostRead)
                .side_effect();
        }
        if (options.device != nullptr && capture.output != rendering::kInvalidResource) {
            // HANDED TO THE PRESENTER: the output left as a fragment shader samples it, which is
            // the use the play host imports it in.
            graph.add_pass("third-person hand off", rhi::QueueKind::Graphics)
                .read(capture.output, rhi::Access::FragmentSampledRead)
                .side_effect();
        }
        rendering::GraphExecutor executor(*allocator, *device);
        const auto submitted = std::chrono::steady_clock::now();
        Status executed = assembly.execute(executor, graph, out);
        const auto recorded = std::chrono::steady_clock::now();
        if (executed) {
            executed = device->wait_idle();
        }
        last_record_ms = std::chrono::duration<f64, std::milli>(recorded - submitted).count();
        last_wait_ms = since_ms(recorded);
        if (executed && read_back) {
            const auto* mapped = static_cast<const u32*>(device->buffer_mapped_pointer(readback));
            if (mapped == nullptr) {
                executed = cy::fail(cy::ErrorCode::Internal, "the readback is not mapped");
            } else {
                pixels.assign(mapped,
                              mapped + (static_cast<usize>(options.width) * options.height));
            }
        }
        executor.release();
        if (Status ended = device->end_frame(); !ended && executed) {
            return ended;
        }
        return executed;
    }

    [[nodiscard]] Status write_png(const std::string& path) noexcept {
        cy::render_test::Image image(cy::system_allocator(cy::MemoryDomain::Renderer));
        if (Status adopted =
                cy::render_test::adopt(image, cy::Span<const u32>(pixels.data(), pixels.size()),
                                       options.width, options.height);
            !adopted) {
            return adopted;
        }
        return cy::render_test::write_png(path.c_str(), image);
    }
};

// --- The renderer --------------------------------------------------------------------------------

Renderer::Renderer(cy::Allocator& allocator) noexcept : allocator_(&allocator) {}

Renderer::~Renderer() {
    if (state_ == nullptr) {
        return;
    }
    // THE DEVICE GOES LAST: everything that made a resource through it releases it first.
    rhi::Device* device = state_->device;
    if (device != nullptr) {
        (void)device->wait_idle();
        State& state = *state_;
        state.upscaler.destroy();
        state.gi.destroy();
        state.occlusion.destroy();
        state.skins.destroy();
        state.textures.shutdown();
        state.bindings.shutdown();
        state.pipelines.shutdown();
        if (!state.shadow_view.is_null()) {
            device->destroy_texture_view(state.shadow_view);
        }
        for (rhi::TextureHandle* texture :
             {&state.output, &state.shadow_color, &state.shadow_depth}) {
            if (!texture->is_null()) {
                device->destroy_texture(*texture);
            }
        }
        for (rhi::BufferHandle* buffer :
             {&state.positions, &state.normals, &state.uvs, &state.indices, &state.readback}) {
            if (!buffer->is_null()) {
                device->destroy_buffer(*buffer);
            }
        }
    }
    const bool owned = state_->options.device == nullptr;
    state_.reset();
    if (device != nullptr && owned) {
        rhi::destroy_device(cy::system_allocator(cy::MemoryDomain::Gpu), device);
    }
}

Status Renderer::start(ThirdPersonHost& host, const map::MapContent& content,
                       const RenderOptions& options) noexcept {
    state_ = std::make_unique<State>(*allocator_);
    State& state = *state_;
    state.options = options;
    state.host = &host;
    state.layout = &content.layout;
    state.sun_travel = content.layout.sun_travel;
    state.sun_lux = content.layout.sun_lux;
    if (Status made = state.create_device(); !made) {
        return made;
    }
    if (Status made =
            state.create_frame(static_cast<u32>(content.layout.materials.size()) + kSkyBands + 1U);
        !made) {
        return made;
    }
    std::vector<u32> slot_of_texture;
    if (Status made = state.create_textures(content, slot_of_texture); !made) {
        return made;
    }
    if (Status made = state.create_materials(content, slot_of_texture); !made) {
        return made;
    }
    if (Status made = state.create_geometry(content); !made) {
        return made;
    }
    if (Status made = state.create_targets(); !made) {
        return made;
    }
    if (Status sized = state.lights.resize(1); !sized) {
        return sized;
    }
    state.lights[0].kind = cy::render::LightKind::Directional;
    state.lights[0].intensity = state.sun_lux;
    state.lights[0].transform.rotation =
        cy::Quat::look_rotation(cy::normalize(state.sun_travel), Vec3{0.0F, 1.0F, 0.0F});
    state.lights[0].color[0] = 1.0F;
    state.lights[0].color[1] = 0.95F;
    state.lights[0].color[2] = 0.86F;
    state.lights[0].stable_id = 1;
    state.report.render_width = state.render.width;
    state.report.render_height = state.render.height;

    if (options.ambient_occlusion) {
        rendering::occlusion::AmbientOcclusionPassDescription description;
        description.width = state.render.width;
        description.height = state.render.height;
        if (Status made = state.occlusion.create(*allocator_, *state.device, description); !made) {
            return made;
        }
        rendering::occlusion::GtaoSettings settings;
        settings.shared.radius = 1.2F;
        state.occlusion.set_settings(settings);
    }
    if (!options.gi) {
        return cy::ok();
    }
    if (!gi_gpu::GpuGiFrame::supported(*state.device)) {
        return cy::fail(cy::ErrorCode::Unsupported, "the device cannot run the GI");
    }
    gi_gpu::GpuGiFrameDescription gi;
    gi.width = state.render.width;
    gi.height = state.render.height;
    return state.gi.create(*allocator_, *state.device, host.illumination().system(), gi);
}

Status Renderer::frame(ThirdPersonHost& host) noexcept {
    State& state = *state_;
    state.frame = host.frames();
    const auto started = std::chrono::steady_clock::now();
    state.set_camera(host);
    if (Status placed = state.place_hero(host); !placed) {
        return placed;
    }
    if (Status placed = state.place_sky(); !placed) {
        return placed;
    }
    state.place_rows();
    const bool played = state.options.device != nullptr;
    const bool still =
        !played &&
        (std::ranges::find(kStills, state.frame) != std::end(kStills) ||
         (state.options.stills_every != 0U && state.frame % state.options.stills_every == 0U));
    const bool video = !state.options.video_directory.empty() &&
                       state.frame % std::max<u32>(state.options.video_every, 1U) == 0U;
    rendering::assembly::AssemblyReport assembled{};
    if (Status rendered = state.render_frame(still || video, assembled); !rendered) {
        return rendered;
    }
    const f64 elapsed = since_ms(started);
    RenderReport& report = state.report;
    report.frames = state.frame;
    report.draws = state.recorder.report().draws();
    report.skinned_draws = state.recorder.report().skinned_draws;
    if (state.frame > kWarmFrames && !still && !video) {
        report.timed += 1;
        report.frame_ms += elapsed;
        report.worst_ms = std::max(report.worst_ms, elapsed);
        report.game_ms += host.last_frame_ms();
        report.record_ms += state.last_record_ms;
        report.device_wait_ms += state.last_wait_ms;
        const gi_gpu::GpuGiCost cost = state.gi.last_cost();
        if (state.options.gi && cost.any()) {
            report.gi_device_ms += cost.total_ms();
        }
    }
    if (video) {
        char name[64];
        (void)std::snprintf(name, sizeof(name), "/frame-%04llu.png",
                            static_cast<unsigned long long>(state.frame));
        if (Status written = state.write_png(state.options.video_directory + name); !written) {
            return written;
        }
    }
    if (still) {
        char name[64];
        (void)std::snprintf(name, sizeof(name), "/third-person-%04llu.png",
                            static_cast<unsigned long long>(state.frame));
        if (Status written = state.write_png(state.options.directory + name); !written) {
            return written;
        }
        std::fprintf(stderr, "wrote %s%s\n", state.options.directory.c_str(), name);
        report.stills += 1;
    }
    report.validation_errors = state.validation_errors;
    return cy::ok();
}

cy::rhi::TextureHandle Renderer::output() const noexcept {
    return state_ == nullptr ? cy::rhi::TextureHandle{} : state_->output;
}

cy::u32 Renderer::width() const noexcept {
    return state_ == nullptr ? 0U : state_->options.width;
}

cy::u32 Renderer::height() const noexcept {
    return state_ == nullptr ? 0U : state_->options.height;
}

const RenderReport& Renderer::report() const noexcept {
    static const RenderReport kNone{};
    return state_ == nullptr ? kNone : state_->report;
}

void Renderer::print_report() const noexcept {
    if (state_ == nullptr) {
        return;
    }
    const RenderReport& report = state_->report;
    const RenderOptions& options = state_->options;
    const f64 timed = static_cast<f64>(std::max<u64>(report.timed, 1));
    std::printf(
        "tps render width=%u height=%u render_width=%u render_height=%u frames=%llu timed=%llu "
        "frame_ms=%.3f worst_ms=%.3f game_ms=%.3f record_ms=%.3f device_wait_ms=%.3f "
        "gi_device_ms=%.3f draws=%u skinned_draws=%u instances=%u triangles=%u plants=%u "
        "stills=%u validation_errors=%u gi=%d ao=%d shadows=%d upscale=%s\n",
        options.width, options.height, report.render_width, report.render_height,
        static_cast<unsigned long long>(report.frames),
        static_cast<unsigned long long>(report.timed), report.frame_ms / timed, report.worst_ms,
        report.game_ms / timed, report.record_ms / timed, report.device_wait_ms / timed,
        report.gi_device_ms / timed, report.draws, report.skinned_draws, report.instances,
        report.triangles, report.plants, report.stills, report.validation_errors,
        options.gi ? 1 : 0, options.ambient_occlusion ? 1 : 0, options.shadows ? 1 : 0,
        options.upscale ? upscale::upscale_mode_name(options.upscale_mode) : "off");
}

}  // namespace sample::tps
