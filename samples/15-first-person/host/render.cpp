// SPDX-License-Identifier: MIT
// render.cpp — the first-person session drawn. See render.h.

#include "render.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/assembly/frame_assembly.h>
#include <cy/rendering/decals/decal_table.h>
#include <cy/rendering/gi/system.h>
#include <cy/rendering/gi_gpu/gpu_frame.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/lighting/soft_shadows.h>
#include <cy/rendering/material/standard.h>
#include <cy/rendering/occlusion/occlusion_pass.h>
#include <cy/rendering/particles/particle_renderer.h>
#include <cy/rendering/pipeline/frame_recorder.h>
#include <cy/rendering/pipeline/material_textures.h>
#include <cy/rendering/upscale/upscale.h>
#include <cy/rendering/upscale/upscale_pass.h>
#include <cy/rendering/upscale/upscaler.h>
#include <cy/rendering/viewmodel/viewmodel.h>
#include <cy/servers/render/server.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "arena.h"
#include "fps_host.h"
#include "golden.h"
#include "render_content.h"
#include "targets.h"

namespace sample::fps {
namespace {

namespace rendering = cy::rendering;
namespace pipeline = cy::rendering::pipeline;
namespace gi_gpu = cy::rendering::gi_gpu;
namespace upscale = cy::rendering::upscale;
namespace viewmodel = cy::rendering::viewmodel;
namespace rhi = cy::rhi;
using cy::Aabb;
using cy::f32;
using cy::f64;
using cy::Mat4;
using cy::Status;
using cy::u16;
using cy::u32;
using cy::u64;
using cy::usize;
using cy::Vec2;
using cy::Vec3;
using cy::Vec4;

/// The frame-texture slots the frame's own targets are bound at.
constexpr u32 kGiDiffuseSlot = 118;
constexpr u32 kGiSpecularSlot = 119;
constexpr u32 kOcclusionSlot = 120;
constexpr u32 kShadowSlot = 121;
constexpr u32 kDecalSlot = 123;
constexpr u32 kShadowExtent = 2048;
/// A shadow box round the courtyard: the whole of it, so a wall behind the eye still shades.
constexpr f32 kShadowRadius = 22.0F;
/// A near plane a weapon pushed into a wall is still clear of (`guaranteed_clear_distance`).
constexpr f32 kNear = 0.05F;
constexpr f32 kFar = 200.0F;
constexpr rhi::Format kOutputFormat = rhi::Format::Rgba8Unorm;
/// The frame holds illuminance in physical units under a 22 000 lux sun.
constexpr f32 kExposureStops = -11.2F;
constexpr u64 kWarmFrames = 10;
/// The decal table's rows: a header and the budget's every record.
constexpr u32 kDecalRows = 4;
/// Room in the frame's light list: the sun and every light the game's pool can hold.
constexpr u32 kLightCapacity = 48;

[[nodiscard]] f64 since_ms(std::chrono::steady_clock::time_point start) noexcept {
    return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
        std::fprintf(stderr, "graphics validation error: %s\n", message != nullptr ? message : "");
    }
}

[[nodiscard]] Aabb transform_box(const Mat4& m, Vec3 low, Vec3 high) noexcept {
    Aabb out = Aabb::empty();
    for (u32 corner = 0; corner < 8U; ++corner) {
        const Vec3 p{(corner & 1U) != 0U ? high.x : low.x, (corner & 2U) != 0U ? high.y : low.y,
                     (corner & 4U) != 0U ? high.z : low.z};
        const Vec4 w = m * Vec4{p.x, p.y, p.z, 1.0F};
        const Vec3 at{w.x, w.y, w.z};
        out = Aabb::from_min_max(cy::cwise_min(out.min, at), cy::cwise_max(out.max, at));
    }
    return out;
}

/// A box's placement: `placement` scaled to `half`'s extents round the unit cube.
[[nodiscard]] Mat4 box_matrix(const cy::Transform& placement, Vec3 half) noexcept {
    return Mat4::from_trs(placement.translation, placement.rotation, half * 2.0F);
}

}  // namespace

// --- The state -----------------------------------------------------------------------------------

struct FpsRenderer::State {
    explicit State(cy::Allocator& memory) noexcept
        : allocator(&memory),
          assembly(memory),
          index(memory),
          graph(memory),
          program(memory),
          server(cy::system_allocator(cy::MemoryDomain::Gpu)),
          lights(memory),
          particles(memory),
          decal_words(memory) {}

    /// One placed box: its matrix, its material and its layers.
    struct Instance {
        Mat4 world = Mat4::identity();
        Surface surface = Surface::Concrete;
        /// Where its box starts in the streams: zero, the unit cube, for a box that moves.
        cy::i32 vertex_offset = 0;
        u32 index_slot = 0;
        rendering::DrawSurface draw;
    };

    cy::Allocator* allocator;
    RenderOptions options;
    rhi::Device* device = nullptr;
    rhi::BackendSelection selection{};
    u32 validation_errors = 0;
    u32 render_width = 0;
    u32 render_height = 0;
    bool upscaling = false;
    upscale::UpscaleMode upscale_mode = upscale::UpscaleMode::Quality;

    rendering::assembly::FrameAssembly assembly;
    rendering::SpatialIndex index;
    rendering::RenderGraph graph;
    rendering::MaterialProgram program;
    pipeline::FramePipelines pipelines;
    pipeline::FrameBindings bindings;
    pipeline::FrameRecorder recorder;
    rendering::particles::ParticleRenderer effect;
    rendering::occlusion::AmbientOcclusionPass occlusion;
    gi_gpu::GpuGiFrame gi;
    upscale::TemporalUpscalePass upscaler;
    rendering::decals::DecalTableTexture decal_table;
    viewmodel::ViewmodelFrame weapon_lens;
    viewmodel::ViewmodelSettings weapon;
    cy::render::RenderServer server;
    pipeline::MaterialTextureTable textures;
    std::vector<pipeline::MaterialTextureSlot> resident;
    /// Each surface's texture slot, or none, and the factor its texture is multiplied by.
    u32 surface_texture[static_cast<usize>(Surface::Count)] = {};
    Vec3 surface_factor[static_cast<usize>(Surface::Count)] = {};

    std::vector<Instance> instances;
    std::vector<pipeline::InstanceTransform> rows;
    u32 first_prop = 0;
    u32 first_target = 0;
    u32 first_weapon = 0;
    rhi::BufferHandle positions;
    rhi::BufferHandle normals;
    rhi::BufferHandle uvs;
    rhi::BufferHandle indices;
    rhi::BufferHandle readback;
    rhi::TextureHandle output;
    rhi::TextureHandle shadow_color;
    rhi::TextureHandle shadow_depth;
    rhi::TextureViewHandle shadow_view;
    rhi::QueryPoolHandle timer;
    u32 index_count = 0;

    u32 material_offsets[4] = {};
    u32 material_texture_offsets[4] = {pipeline::kNoMaterialTexture, pipeline::kNoMaterialTexture,
                                       pipeline::kNoMaterialTexture, pipeline::kNoMaterialTexture};
    u32 material_slots[static_cast<usize>(Surface::Count)] = {};
    cy::Array<cy::render::LightDescription> lights;
    cy::Array<rendering::particles::ParticleInstance> particles;
    cy::Array<u32> decal_words;
    std::vector<rendering::DecalInstance> decals;
    std::vector<u32> decal_order;
    rendering::ResourceId decal_resource = rendering::kInvalidResource;

    // The view.
    Mat4 view = Mat4::identity();
    Mat4 relative_view = Mat4::identity();
    Mat4 projection = Mat4::identity();
    Vec3 eye{0.0F, 0.0F, 0.0F};
    Vec3 forward{0.0F, 0.0F, -1.0F};
    f32 fov = 1.3F;
    Mat4 shadow_to_clip = Mat4::identity();
    rendering::FrameResourceRead opaque_reads[2];
    u32 opaque_read_count = 0;

    // What the report says.
    u64 frame = 0;
    u64 timed = 0;
    f64 frame_ms = 0.0;
    f64 worst_ms = 0.0;
    f64 record_ms = 0.0;
    f64 device_wait_ms = 0.0;
    f64 game_ms = 0.0;
    f64 gpu_ms = 0.0;
    u64 gpu_timed = 0;
    f64 gi_ms = 0.0;
    bool stamped = false;
    u32 draws = 0;
    u32 viewmodel_draws = 0;
    u32 decals_assigned = 0;
    u32 particles_drawn = 0;
    u32 peak_lights = 0;
    u32 stills = 0;
    f64 last_record_ms = 0.0;
    f64 last_wait_ms = 0.0;
    std::vector<u32> pixels;

    // --- Building ------------------------------------------------------------------------------

    [[nodiscard]] Status create_device() noexcept {
        if (options.device != nullptr) {
            device = options.device;  // the play host's: its swapchain presents what is drawn
            return cy::ok();
        }
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_sample_first-person";
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

    [[nodiscard]] Status create_frame() noexcept {
        rendering::assembly::AssemblyDescription description;
        description.width = options.width;
        description.height = options.height;
        description.near_plane = kNear;
        description.far_plane = kFar;
        description.clusters = rendering::ClusterGridConfig{32, 18, 32};
        description.material_capacity = static_cast<u32>(Surface::Count) + 4U;
        description.max_draws = 1024;
        description.max_instances = 256;
        // The weapon view is culled on the host beside the world: a device-culled frame refuses it.
        description.gpu_culling = false;
        description.post.temporal_antialiasing = !upscaling;
        description.post.ambient_occlusion = options.ambient_occlusion;
        description.global_illumination = options.gi;
        if (upscaling) {
            description.post.temporal_upscaling = true;
            description.produced_temporal_stage = true;
            // The sparks are translucent and carry no motion of their own: the reactive mask keeps
            // the reconstruction from smearing them.
            description.reactive_mask = true;
            description.render_width = render_width;
            description.render_height = render_height;
            description.temporal.jitter =
                upscale::jitter_config_for(upscale::Extent2D{options.width, options.height},
                                           upscale::Extent2D{render_width, render_height});
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
        setup.viewmodel = true;
        if (Status made = pipelines.initialize(*device, setup); !made) {
            return made;
        }
        cy::Expected<rendering::ClusterGrid, cy::Error> grid =
            rendering::make_cluster_grid(description.clusters, render_width, render_height,
                                         description.near_plane, description.far_plane);
        if (!grid) {
            return cy::make_unexpected(grid.error());
        }
        const pipeline::BindingCapacity capacity = pipeline::BindingCapacity::for_grid(
            *grid, description.max_draws, description.max_instances, description.material_capacity,
            kLightCapacity);
        if (Status made = bindings.initialize(*device, pipelines, capacity); !made) {
            return made;
        }
        if (Status made = recorder.initialize(pipelines, bindings); !made) {
            return made;
        }
        return effect.initialize(*device, pipelines, kParticleRing);
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
            return cy::fail(cy::ErrorCode::Internal, "an arena buffer is not mapped");
        }
        std::memcpy(mapped, data, bytes);
        return cy::ok();
    }

    [[nodiscard]] Status set_material(u32 slot, Surface surface) noexcept {
        const rendering::StandardParameters ids;
        rendering::MaterialTable& table = assembly.materials();
        if (Status defaults = rendering::apply_standard_defaults(program, table, slot, ids);
            !defaults) {
            return defaults;
        }
        const auto which = static_cast<usize>(surface);
        const Vec3 colour = surface_factor[which];
        const Vec3 emission = surface_emission(surface);
        if (Status set = table.set_color(program, slot, ids.base_color_factor,
                                         Vec4{colour.x, colour.y, colour.z, 1.0F});
            !set) {
            return set;
        }
        if (Status set =
                table.set_float(program, slot, ids.roughness_factor, surface_roughness(surface));
            !set) {
            return set;
        }
        if (Status set =
                table.set_float(program, slot, ids.metallic_factor, surface_metallic(surface));
            !set) {
            return set;
        }
        if (Status set = table.set_color(program, slot, ids.emission_color,
                                         Vec4{emission.x, emission.y, emission.z, 1.0F});
            !set) {
            return set;
        }
        return table.set_texture(program, slot, ids.base_color_texture, surface_texture[which]);
    }

    /// The textures, resident on the device, and each surface's slot and factor: its texture's
    /// mean brought to its albedo, or the albedo itself where it has no texture.
    [[nodiscard]] Status create_textures() noexcept {
        for (u32 surface = 0; surface < static_cast<u32>(Surface::Count); ++surface) {
            surface_texture[surface] = pipeline::kNoMaterialTexture;
            surface_factor[surface] = surface_albedo(static_cast<Surface>(surface));
        }
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
        sampler.name = "arena material sampler";
        sampler.max_anisotropy = 8.0F;
        if (Status made = textures.initialize(*device, *allocator, sampler); !made) {
            return made;
        }
        std::vector<DetailTexture> loaded;
        std::vector<cy::render::TextureHandle> handles;
        std::vector<pipeline::TextureUpload> uploads;
        for (u32 detail = 1; detail < static_cast<u32>(Detail::Count); ++detail) {
            cy::Expected<DetailTexture, cy::Error> texture =
                load_detail(static_cast<Detail>(detail), CY_FIRST_PERSON_CONTENT_ROOT);
            if (!texture) {
                // No importer: the flat colours are the picture.
                return texture.error().code == cy::ErrorCode::Unsupported
                           ? cy::ok()
                           : cy::make_unexpected(texture.error());
            }
            loaded.push_back(std::move(*texture));
        }
        for (const DetailTexture& texture : loaded) {
            cy::render::TextureRecord record;
            record.name = cy::Name::intern("arena texture");
            record.format = cy::render::TextureFormat::Rgba8Srgb;
            record.usage_class = cy::render::TextureUsageClass::Color;
            record.width = texture.width;
            record.height = texture.height;
            record.mip_levels = static_cast<cy::u8>(texture.mips);
            cy::Expected<cy::render::TextureHandle, cy::Error> handle =
                server.create_texture(record);
            if (!handle) {
                return cy::make_unexpected(handle.error());
            }
            handles.push_back(*handle);
            uploads.push_back(pipeline::TextureUpload{
                *handle, cy::Span<const cy::u8>(texture.pixels.data(), texture.pixels.size())});
        }
        if (Status uploaded = textures.upload(
                server, cy::Span<const pipeline::TextureUpload>(uploads.data(), uploads.size()));
            !uploaded) {
            return uploaded;
        }
        resident.resize(textures.resident());
        (void)textures.slots(
            cy::Span<pipeline::MaterialTextureSlot>(resident.data(), resident.size()));
        for (u32 surface = 0; surface < static_cast<u32>(Surface::Count); ++surface) {
            const Detail detail = detail_of(static_cast<Surface>(surface));
            if (detail == Detail::None) {
                continue;
            }
            const auto loaded_index = static_cast<usize>(detail) - 1U;
            const Vec3 mean = loaded[loaded_index].mean;
            const Vec3 albedo = surface_albedo(static_cast<Surface>(surface));
            surface_texture[surface] = textures.slot_of(handles[loaded_index]);
            surface_factor[surface] =
                Vec3{albedo.x / std::max(mean.x, 1e-3F), albedo.y / std::max(mean.y, 1e-3F),
                     albedo.z / std::max(mean.z, 1e-3F)};
        }
        return cy::ok();
    }

    [[nodiscard]] Status create_materials() noexcept {
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
        for (u32 surface = 0; surface < static_cast<u32>(Surface::Count); ++surface) {
            cy::Expected<u32, cy::Error> slot = assembly.materials().allocate();
            if (!slot) {
                return cy::make_unexpected(slot.error());
            }
            material_slots[surface] = *slot;
            if (Status set = set_material(*slot, static_cast<Surface>(surface)); !set) {
                return set;
            }
        }
        return cy::ok();
    }

    [[nodiscard]] Status add(const Mat4& world, Surface surface, cy::render::LayerMask layers,
                             bool casts_shadow = true, const BakedBox* baked = nullptr) {
        Instance instance;
        instance.world = world;
        instance.surface = surface;
        instance.vertex_offset = baked == nullptr ? 0 : baked->vertex_offset;
        instance.draw.material = material_slots[static_cast<usize>(surface)];
        instance.draw.mesh = 0;
        instance.draw.surface = 0;
        rendering::SpatialEntry entry;
        entry.bounds = baked != nullptr ? baked->bounds
                                        : transform_box(world, Vec3{-0.5F, -0.5F, -0.5F},
                                                        Vec3{0.5F, 0.5F, 0.5F});
        entry.stable_id = 7000U + instances.size();
        entry.gpu_slot = static_cast<u32>(instances.size());
        entry.radius = cy::length(entry.bounds.max - entry.bounds.min) * 0.5F;
        entry.layer_mask = layers;
        if (!casts_shadow) {
            entry.flags &= ~rendering::kSpatialCastsShadow;
        }
        cy::Expected<u32, cy::Error> slot = index.insert(entry);
        if (!slot) {
            return cy::make_unexpected(slot.error());
        }
        instance.index_slot = *slot;
        instances.push_back(instance);
        return cy::ok();
    }

    [[nodiscard]] Status move(u32 which, const Mat4& world) noexcept {
        instances[which].world = world;
        return index.update(
            instances[which].index_slot,
            transform_box(world, Vec3{-0.5F, -0.5F, -0.5F}, Vec3{0.5F, 0.5F, 0.5F}));
    }

    [[nodiscard]] Status create_geometry() noexcept {
        // THE STATIC BOXES BAKED: the arena's and the sky's, after the unit cube.
        std::vector<ArenaBox> statics(arena_boxes().begin(), arena_boxes().end());
        statics.insert(statics.end(), sky_panels().begin(), sky_panels().end());
        ArenaStreams streams;
        std::vector<BakedBox> baked;
        build_streams(cy::Span<const ArenaBox>(statics.data(), statics.size()), streams, baked);
        index_count = static_cast<u32>(streams.indices.size());
        if (Status made = upload_buffer("arena positions", streams.positions.data(),
                                        streams.positions.size() * sizeof(Vec3),
                                        rhi::BufferUsage::Vertex, positions);
            !made) {
            return made;
        }
        if (Status made = upload_buffer("arena normals", streams.normals.data(),
                                        streams.normals.size() * sizeof(u16),
                                        rhi::BufferUsage::Vertex, normals);
            !made) {
            return made;
        }
        if (Status made =
                upload_buffer("arena uvs", streams.uvs.data(), streams.uvs.size() * sizeof(Vec2),
                              rhi::BufferUsage::Vertex, uvs);
            !made) {
            return made;
        }
        if (Status made = upload_buffer("arena indices", streams.indices.data(),
                                        streams.indices.size() * sizeof(u32),
                                        rhi::BufferUsage::Index, indices);
            !made) {
            return made;
        }
        // THE INSTANCES: the static boxes at the identity — they are in world space already — and
        // the sky, which casts no shadow; then the props, the targets' bodies and heads, and the
        // weapon's parts on the weapon view's layer, all of them the unit cube placed.
        for (usize box = 0; box < baked.size(); ++box) {
            const bool sky = box >= arena_boxes().size();
            if (Status added = add(Mat4::identity(), baked[box].surface, cy::render::kDefaultLayer,
                                   !sky, &baked[box]);
                !added) {
                return added;
            }
        }
        first_prop = static_cast<u32>(instances.size());
        for (const PropBox& prop : prop_boxes()) {
            if (Status added =
                    add(box_matrix(cy::Transform::from_translation(prop.centre), prop.half),
                        prop.surface, cy::render::kDefaultLayer);
                !added) {
                return added;
            }
        }
        first_target = static_cast<u32>(instances.size());
        for (u32 target = 0; target < kTargetCount * 2U; ++target) {
            if (Status added =
                    add(Mat4::identity(), target % 2U == 0U ? Surface::Target : Surface::TargetHead,
                        cy::render::kDefaultLayer);
                !added) {
                return added;
            }
        }
        first_weapon = static_cast<u32>(instances.size());
        for (const WeaponPart& part : weapon_parts()) {
            if (Status added = add(Mat4::identity(), part.surface, 1U << kWeaponLayerBit); !added) {
                return added;
            }
        }
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

    [[nodiscard]] Status create_targets() noexcept {
        rhi::BufferDescription host_copy;
        host_copy.name = "fps readback";
        host_copy.size = u64{options.width} * options.height * 4U;
        host_copy.usage = rhi::BufferUsage::TransferDestination;
        host_copy.memory = rhi::MemoryUse::Readback;
        cy::Expected<rhi::BufferHandle, cy::Error> buffer = device->create_buffer(host_copy);
        if (!buffer) {
            return cy::make_unexpected(buffer.error());
        }
        readback = *buffer;
        rhi::TextureDescription out;
        out.name = "fps output";
        out.format = kOutputFormat;
        out.extent = rhi::Extent3D{options.width, options.height, 1};
        out.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSource |
                    rhi::TextureUsage::Sampled;
        cy::Expected<rhi::TextureHandle, cy::Error> made = device->create_texture(out);
        if (!made) {
            return cy::make_unexpected(made.error());
        }
        output = *made;
        rhi::QueryPoolDescription stamps;
        stamps.name = "fps frame timestamps";
        stamps.kind = rhi::QueryKind::Timestamp;
        stamps.count = 2;
        if (cy::Expected<rhi::QueryPoolHandle, cy::Error> pool = device->create_query_pool(stamps);
            pool) {
            timer = *pool;
        }
        if (!options.shadows) {
            return cy::ok();
        }
        rhi::TextureDescription colour;
        colour.name = "fps shadow map";
        colour.format = rhi::Format::R32Sfloat;
        colour.extent = rhi::Extent3D{kShadowExtent, kShadowExtent, 1};
        colour.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::Sampled;
        cy::Expected<rhi::TextureHandle, cy::Error> map_colour = device->create_texture(colour);
        if (!map_colour) {
            return cy::make_unexpected(map_colour.error());
        }
        shadow_color = *map_colour;
        rhi::TextureDescription depth;
        depth.name = "fps shadow depth";
        depth.format = rhi::Format::D32Sfloat;
        depth.extent = rhi::Extent3D{kShadowExtent, kShadowExtent, 1};
        depth.usage = rhi::TextureUsage::DepthStencilAttachment;
        cy::Expected<rhi::TextureHandle, cy::Error> map_depth = device->create_texture(depth);
        if (!map_depth) {
            return cy::make_unexpected(map_depth.error());
        }
        shadow_depth = *map_depth;
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

    // --- The frame's lookups ---------------------------------------------------------------------

    static cy::Span<const rendering::DrawSurface> surfaces(
        const rendering::VisibleInstance& visible, void* user) noexcept {
        auto* self = static_cast<State*>(user);
        if (visible.gpu_slot >= self->instances.size()) {
            return {};
        }
        return {&self->instances[visible.gpu_slot].draw, 1};
    }

    static bool geometry(const cy::render::DrawItem& /*item*/,
                         const rendering::GpuDrawInstance& draw, void* user,
                         pipeline::DrawGeometry& out) noexcept {
        auto* self = static_cast<State*>(user);
        if (draw.instance_slot >= self->instances.size()) {
            return false;
        }
        out.indices = self->indices;
        out.wide_indices = true;
        out.index_count = self->index_count;
        out.first_index = 0;
        out.vertex_offset = self->instances[draw.instance_slot].vertex_offset;
        return true;
    }

    static void stamp(const pipeline::ExtensionContext& context, void* user) noexcept {
        auto* self = static_cast<State*>(user);
        if (context.kind == rendering::FramePassKind::Prepare) {
            context.commands->reset_queries(self->timer, 0, 2);
            context.commands->write_timestamp(self->timer, 0);
            return;
        }
        context.commands->write_timestamp(self->timer, 1);
        self->stamped = true;
    }

    // --- One frame -------------------------------------------------------------------------------

    void set_camera(const FpsHost& host) noexcept {
        const cy::Transform& placed = host.eye();
        eye = placed.translation;
        forward = cy::normalize(placed.rotation * Vec3{0.0F, 0.0F, -1.0F});
        const Vec3 up = cy::normalize(placed.rotation * Vec3{0.0F, 1.0F, 0.0F});
        fov = host.fov();
        const f32 aspect = static_cast<f32>(options.width) / static_cast<f32>(options.height);
        view = cy::look_at(eye, eye + forward, up);
        relative_view = cy::look_at(Vec3{0.0F, 0.0F, 0.0F}, forward, up);
        projection = cy::perspective_reversed_z(fov, aspect, kNear, kFar);
        // THE SUN'S MAP covers the courtyard, centred on it: a camera-relative transform as the
        // frame's rows are.
        const Vec3 travel = cy::normalize(kSunTravel);
        const Vec3 centre = Vec3{0.0F, 0.0F, 0.0F} - eye;
        const Vec3 light_eye = centre - (travel * (kShadowRadius * 2.0F));
        const Mat4 light_view = cy::look_at(light_eye, centre, Vec3{0.0F, 1.0F, 0.0F});
        const Mat4 light_projection =
            cy::orthographic_reversed_z(-kShadowRadius, kShadowRadius, -kShadowRadius,
                                        kShadowRadius, 0.5F, kShadowRadius * 4.0F);
        shadow_to_clip = light_projection * light_view;
    }

    [[nodiscard]] Status place(const FpsHost& host) noexcept {
        const cy::Span<const PropBox> props = prop_boxes();
        for (u32 prop = 0; prop < kPropCount; ++prop) {
            if (Status moved =
                    move(first_prop + prop, box_matrix(host.drawn_prop(prop), props[prop].half));
                !moved) {
                return moved;
            }
        }
        for (u32 target = 0; target < kTargetCount; ++target) {
            const cy::Transform body = target_placement(host.drawn_target(target));
            if (Status moved = move(first_target + (target * 2U), box_matrix(body, kTargetHalf));
                !moved) {
                return moved;
            }
            if (Status moved = move(first_target + (target * 2U) + 1U,
                                    box_matrix(head_placement(body), kTargetHeadHalf));
                !moved) {
                return moved;
            }
        }
        // THE WEAPON, held where the first-person camera holds it: camera-attached, so it is lit
        // where it is. Without the weapon view it is world geometry, and goes into walls.
        const cy::Span<const WeaponPart> parts = weapon_parts();
        const cy::Transform& held = host.weapon();
        for (u32 part = 0; part < parts.size(); ++part) {
            cy::Transform placed = held;
            placed.translation = held.translation + (held.rotation * parts[part].centre);
            if (Status moved = move(first_weapon + part, box_matrix(placed, parts[part].half));
                !moved) {
                return moved;
            }
        }
        return cy::ok();
    }

    void place_rows() noexcept {
        for (usize slot = 0; slot < instances.size(); ++slot) {
            const Mat4& world = instances[slot].world;
            pipeline::InstanceTransform& row = rows[slot];
            for (u32 column = 0; column < 4U; ++column) {
                row.row0[column] = world.at(0, column);
                row.row1[column] = world.at(1, column);
                row.row2[column] = world.at(2, column);
            }
            row.row0[3] -= eye.x;
            row.row1[3] -= eye.y;
            row.row2[3] -= eye.z;
            row.tint[0] = 1.0F;
            row.tint[1] = 1.0F;
            row.tint[2] = 1.0F;
            row.tint[3] = 1.0F;
        }
    }

    [[nodiscard]] Status gather(FpsHost& host) noexcept {
        // The sun, then every light the game's pool holds alive: the muzzle flashes.
        if (Status sized = lights.resize(1); !sized) {
            return sized;
        }
        lights[0].kind = cy::render::LightKind::Directional;
        lights[0].intensity = kSunLux;
        lights[0].transform.rotation =
            cy::Quat::look_rotation(cy::normalize(kSunTravel), Vec3{0.0F, 1.0F, 0.0F});
        lights[0].color[0] = 1.0F;
        lights[0].color[1] = 0.95F;
        lights[0].color[2] = 0.86F;
        lights[0].stable_id = 1;
        if (options.flashes) {
            if (Status appended = host.append_lights(lights); !appended) {
                return appended;
            }
        }
        peak_lights = std::max(peak_lights, static_cast<u32>(lights.size()));
        particles.clear();
        if (options.particles) {
            if (Status published = host.effects().publish(eye, particles); !published) {
                return published;
            }
        }
        decals.clear();
        if (options.decals) {
            const cy::Span<const rendering::DecalInstance> live = host.effects().decals().decals();
            decals.assign(live.begin(), live.end());
        }
        weapon = options.weapon_view ? host.weapon_view() : viewmodel::ViewmodelSettings{};
        return cy::ok();
    }

    /// The decal table for this frame's view: the budget's marks in application order, relative to
    /// the eye. Packed before the frame is assembled — the order is the decals' own, so it is the
    /// order the assembly ranks them in — and staged into the frame's graph.
    [[nodiscard]] Status pack_decals() noexcept {
        decal_order.resize(decals.size());
        (void)rendering::decal_application_order(
            cy::Span<const rendering::DecalInstance>(decals.data(), decals.size()),
            cy::Span<u32>(decal_order.data(), decal_order.size()));
        rendering::decals::DecalTableInput input;
        input.decals = cy::Span<const rendering::DecalInstance>(decals.data(), decals.size());
        input.order = cy::Span<const u32>(decal_order.data(), decal_order.size());
        input.materials = decal_materials_;
        input.origin = eye;
        return rendering::decals::pack_decal_table(input, decal_words);
    }

    cy::Span<const rendering::decals::DecalMaterial> decal_materials_;

    [[nodiscard]] Status before_assemble(rendering::assembly::AssemblyView& assembly_view,
                                         rendering::assembly::FrameSinks& sinks) noexcept {
        opaque_read_count = 0;
        const f32 aspect = static_cast<f32>(options.width) / static_cast<f32>(options.height);
        if (weapon.enabled()) {
            assembly_view.viewmodel = viewmodel::assembly_view(weapon, view, aspect);
        }
        if (options.decals && decal_table.ready()) {
            assembly_view.decals =
                cy::Span<const rendering::DecalInstance>(decals.data(), decals.size());
            cy::Expected<rendering::ResourceId, cy::Error> declared =
                decal_table.declare_upload(graph);
            if (!declared) {
                return cy::make_unexpected(declared.error());
            }
            decal_resource = *declared;
            opaque_reads[opaque_read_count++] =
                rendering::FrameResourceRead{decal_resource, rhi::Access::FragmentSampledRead};
        }
        if (options.shadows) {
            rendering::TextureRequest request;
            request.name = "fps shadow map";
            request.format = rhi::Format::R32Sfloat;
            request.width = kShadowExtent;
            request.height = kShadowExtent;
            assembly_view.shadow_color =
                graph.import_texture(request, shadow_color, rhi::ImageUse::Undefined);
            request.name = "fps shadow depth";
            request.format = rhi::Format::D32Sfloat;
            assembly_view.shadow_depth =
                graph.import_texture(request, shadow_depth, rhi::ImageUse::Undefined);
            recorder.set_shadow_targets(assembly_view.shadow_color, assembly_view.shadow_depth,
                                        kShadowExtent);
            const auto shadow = static_cast<usize>(rendering::FramePassKind::Shadow);
            sinks.passes[shadow] = recorder.sinks().passes[shadow];
            opaque_reads[opaque_read_count++] = rendering::FrameResourceRead{
                assembly_view.shadow_color, rhi::Access::FragmentSampledRead};
        }
        sinks.passes[static_cast<usize>(rendering::FramePassKind::Opaque)].reads =
            cy::Span<const rendering::FrameResourceRead>(opaque_reads, opaque_read_count);
        if (options.ambient_occlusion) {
            rendering::occlusion::GtaoView gtao;
            gtao.projection = projection;
            gtao.relative_to_view = relative_view;
            gtao.width = render_width;
            gtao.height = render_height;
            if (Status set = occlusion.set_view(gtao); !set) {
                return set;
            }
            assembly_view.ambient_occlusion = occlusion.import_target(graph);
            sinks.ambient_occlusion = occlusion.stage();
        }
        if (upscaling) {
            upscale::UpscaleSettings settings;
            settings.mode = upscale_mode;
            settings.exposure = std::exp2(kExposureStops);
            upscaler.set_settings(settings);
            sinks.temporal = upscale::stage_of(upscaler);
            sinks.opaque_copy = upscaler.opaque_copy_stage();
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
        if (Status prepared = gi.prepare(graph, gi_view, host_frame, targets); !prepared) {
            return prepared;
        }
        assembly_view.screen_space_gi = targets.diffuse;
        assembly_view.screen_space_gi_specular = targets.specular;
        sinks.screen_space_gi = gi.stage();
        return cy::ok();
    }

    u64 host_frame = 0;

    /// The sun's shadow map in the view block: which light is the sun, the map's transform, and the
    /// soft filter's words.
    void write_shadows(pipeline::FrameUpload& upload) const noexcept {
        u32 sun = static_cast<u32>(upload.lights.size());
        for (usize light = 0; light < upload.lights.size(); ++light) {
            if (upload.lights[light].kind == rendering::kGpuLightDirectional) {
                sun = static_cast<u32>(light);
            }
        }
        for (u32 row = 0; row < 4U; ++row) {
            for (u32 column = 0; column < 4U; ++column) {
                upload.view.shadow_to_clip[(row * 4U) + column] = shadow_to_clip.at(row, column);
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
        footprint.depth_range_metres = (kShadowRadius * 4.0F) - 0.5F;
        footprint.width_metres = kShadowRadius * 2.0F;
        footprint.extent = kShadowExtent;
        rendering::write_soft_shadow_words(pipeline::kSoftShadowPcss, pipeline::kNoMaterialTexture,
                                           rendering::make_pcss_shape(soft, footprint),
                                           upload.view.soft_shadow_control,
                                           upload.view.soft_shadow_shape);
    }

    [[nodiscard]] Status before_upload(pipeline::FrameUpload& upload) noexcept {
        std::vector<pipeline::MaterialTextureSlot> slots = resident;
        const Vec2 jitter{upload.view.temporal_jitter[0], upload.view.temporal_jitter[1]};
        if (weapon.enabled()) {
            const f32 aspect = static_cast<f32>(options.width) / static_cast<f32>(options.height);
            const viewmodel::ViewmodelMatrices matrices =
                weapon_lens.advance(weapon, aspect, relative_view, assembly.temporal());
            viewmodel::write_viewmodel(matrices, weapon, assembly.temporal().jitter().current(),
                                       assembly.render_width(), assembly.render_height(),
                                       upload.view);
        }
        if (options.ambient_occlusion) {
            rendering::occlusion::GtaoView gtao;
            gtao.projection = projection;
            gtao.relative_to_view = relative_view;
            gtao.width = render_width;
            gtao.height = render_height;
            gtao.jitter = jitter;
            if (Status set = occlusion.set_view(gtao); !set) {
                return set;
            }
            slots.push_back(pipeline::MaterialTextureSlot{kOcclusionSlot, occlusion.target_view()});
            rendering::occlusion::write_occlusion_control(
                kOcclusionSlot, occlusion.settings().shared, upload.view.occlusion_control);
        }
        if (options.shadows) {
            write_shadows(upload);
            slots.push_back(pipeline::MaterialTextureSlot{kShadowSlot, shadow_view});
        }
        if (options.decals && decal_table.ready()) {
            slots.push_back(decal_table.slot(kDecalSlot));
            rendering::decals::write_decal_frame(kDecalSlot, decal_table.rows(), 0U, upload.view);
        }
        if (options.gi) {
            gi.set_jitter(jitter);
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

    void declare_capture() noexcept {
        rendering::BufferRequest capture_request;
        capture_request.name = "fps capture";
        capture_request.size = u64{options.width} * options.height * 4U;
        capture_request.extra_usage = rhi::BufferUsage::TransferDestination;
        const rendering::ResourceId destination = graph.import_buffer(capture_request, readback);
        graph.add_pass("fps capture", rhi::QueueKind::Graphics)
            .read(capture.output, rhi::Access::TransferRead)
            .write(destination, rhi::Access::TransferWrite)
            .record(&State::record_capture, &capture);
        graph.add_pass("fps capture host", rhi::QueueKind::Graphics)
            .read(destination, rhi::Access::HostRead)
            .side_effect();
    }

    [[nodiscard]] Status attach_effects(u32 slot) noexcept {
        recorder.clear_extensions();
        effect.reset_report();
        if (!timer.is_null()) {
            for (const rendering::FramePassKind kind :
                 {rendering::FramePassKind::Prepare, rendering::FramePassKind::PostProcess}) {
                if (Status added =
                        recorder.add_extension(pipeline::PassExtension{kind, &State::stamp, this});
                    !added) {
                    return added;
                }
            }
        }
        if (particles.empty()) {
            return cy::ok();
        }
        if (Status uploaded = effect.upload(slot, particles.span()); !uploaded) {
            return uploaded;
        }
        return recorder.add_extension(effect.extension());
    }

    /// This frame's view: the camera, its cull, the lights, the sun and the imported output.
    [[nodiscard]] rendering::assembly::AssemblyView describe_view() noexcept {
        rendering::assembly::AssemblyView frame_view;
        frame_view.fov_y_radians = fov;
        frame_view.view = view;
        frame_view.projection = projection;
        frame_view.cull.frustum = cy::Frustum::from_view_projection(projection * view);
        frame_view.cull.camera_position = eye;
        frame_view.cull.camera_forward = forward;
        frame_view.cull.fov_y_radians = fov;
        frame_view.lights = lights.span();
        frame_view.sun_direction = cy::normalize(kSunTravel * -1.0F);
        rendering::TextureRequest request;
        request.name = "fps output";
        request.format = kOutputFormat;
        request.width = options.width;
        request.height = options.height;
        request.extra_usage = rhi::TextureUsage::TransferSource | rhi::TextureUsage::Sampled;
        frame_view.output = graph.import_texture(request, output, rhi::ImageUse::Undefined);
        return frame_view;
    }

    /// The frame's passes declared, its decal table staged and its view block written.
    [[nodiscard]] Status assemble_and_upload(u32 slot,
                                             rendering::assembly::AssemblyView& frame_view,
                                             rendering::assembly::AssemblyReport& out) noexcept {
        rendering::assembly::FrameSinks sinks = recorder.sinks();
        sinks.surfaces = &State::surfaces;
        sinks.surfaces_user = this;
        Status executed = before_assemble(frame_view, sinks);
        if (executed) {
            executed = assembly.assemble(index, frame_view, sinks, graph, out);
        }
        if (executed && options.decals && decal_table.ready()) {
            executed = decal_table.stage(decal_words.span());
        }
        pipeline::GlobalsData globals;
        globals.exposure_stops = kExposureStops;
        if (executed) {
            pipeline::FrameUpload upload = pipeline::upload_for(
                assembly, out, projection * relative_view, relative_view,
                cy::Span<const pipeline::InstanceTransform>(rows.data(), rows.size()), globals,
                material_offsets);
            for (u32 word = 0; word < 4U; ++word) {
                upload.view.material_textures[word] = material_texture_offsets[word];
            }
            executed = before_upload(upload);
            if (executed) {
                executed = bindings.upload(slot, upload);
            }
        }
        return executed;
    }

    [[nodiscard]] Status render(bool read_back, rendering::assembly::AssemblyReport& out) noexcept {
        cy::Expected<u32, cy::Error> began = device->begin_frame();
        if (!began) {
            return cy::make_unexpected(began.error());
        }
        const u32 slot = *began;
        graph.reset();
        stamped = false;
        if (Status attached = attach_effects(slot); !attached) {
            (void)device->end_frame();
            return attached;
        }
        rendering::assembly::AssemblyView frame_view = describe_view();
        Status executed = recorder.bind(assembly);
        if (executed) {
            executed = assemble_and_upload(slot, frame_view, out);
        }
        capture = Capture{assembly.resources().output, readback, options.width, options.height};
        if (executed && read_back && capture.output != rendering::kInvalidResource) {
            declare_capture();
        }
        if (executed && options.device != nullptr &&
            capture.output != rendering::kInvalidResource) {
            // HANDED TO THE PRESENTER: the output left as a fragment shader samples it, which is
            // the use the play host imports it in.
            graph.add_pass("fps hand off", rhi::QueueKind::Graphics)
                .read(capture.output, rhi::Access::FragmentSampledRead)
                .side_effect();
        }
        rendering::GraphExecutor executor(*allocator, *device);
        const auto submitted = std::chrono::steady_clock::now();
        if (executed) {
            executed = assembly.execute(executor, graph, out);
        }
        const auto recorded = std::chrono::steady_clock::now();
        if (executed) {
            executed = device->wait_idle();
        }
        last_record_ms = std::chrono::duration<f64, std::milli>(recorded - submitted).count();
        last_wait_ms = since_ms(recorded);
        if (executed && read_back) {
            const auto* mapped = static_cast<const u32*>(device->buffer_mapped_pointer(readback));
            if (mapped == nullptr) {
                executed = cy::fail(cy::ErrorCode::Internal, "the fps readback is not mapped");
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

    [[nodiscard]] f64 device_ms() const noexcept {
        if (timer.is_null() || !stamped) {
            return -1.0;
        }
        u64 stamps[2] = {};
        cy::Expected<u32, cy::Error> read =
            device->read_query_results(timer, 0, 2, cy::Span<u64>(stamps, 2));
        if (!read || *read != 2U || stamps[1] < stamps[0]) {
            return -1.0;
        }
        return static_cast<f64>(stamps[1] - stamps[0]) / 1.0e6;
    }

    /// Where a world point lands in the output, in pixels from the top left, through this frame's
    /// unjittered view: false behind the eye.
    [[nodiscard]] bool project(Vec3 point, f32& x, f32& y) const noexcept {
        const Vec4 clip = projection * view * Vec4{point.x, point.y, point.z, 1.0F};
        if (clip.w <= 1e-4F) {
            return false;
        }
        x = (((clip.x / clip.w) * 0.5F) + 0.5F) * static_cast<f32>(options.width);
        y = (0.5F - ((clip.y / clip.w) * 0.5F)) * static_cast<f32>(options.height);
        return true;
    }

    /// At a still: where each live mark and the last shot's hit point land on screen — what
    /// `tests/test_first_person_render.cpp` holds the drawn marks and sparks to.
    void print_marks(const FpsHost& host) const noexcept {
        u32 which = 0;
        for (const rendering::DecalInstance& decal : decals) {
            f32 x = 0.0F;
            f32 y = 0.0F;
            if (project(decal.center, x, y)) {
                std::printf("fps mark     frame=%llu decal=%u x=%.1f y=%.1f\n",
                            static_cast<unsigned long long>(frame), which, static_cast<double>(x),
                            static_cast<double>(y));
            }
            ++which;
        }
        const cy::Span<const ShotRecord> shots = host.shots();
        if (!shots.empty()) {
            const ShotRecord& last = shots[shots.size() - 1U];
            f32 x = 0.0F;
            f32 y = 0.0F;
            if (last.kind != 0U && project(last.point, x, y)) {
                std::printf("fps mark     frame=%llu shot=%u fired=%llu x=%.1f y=%.1f\n",
                            static_cast<unsigned long long>(frame),
                            static_cast<u32>(shots.size() - 1U),
                            static_cast<unsigned long long>(last.frame), static_cast<double>(x),
                            static_cast<double>(y));
            }
        }
    }

    // --- The stages beside the frame ---------------------------------------------------------

    /// The render extent: the output's, or below it when the upscaler reconstructs.
    [[nodiscard]] Status choose_extent() noexcept {
        render_width = options.width;
        render_height = options.height;
        if (options.upscale == "off") {
            return cy::ok();
        }
        if (!upscale::parse_upscale_mode(options.upscale.c_str(), upscale_mode)) {
            return cy::fail(cy::ErrorCode::InvalidArgument,
                            "--upscale is off, native, quality, balanced or performance");
        }
        const upscale::Extent2D extent =
            upscale::render_extent_for(upscale::Extent2D{options.width, options.height},
                                       upscale::upscale_mode_scale(upscale_mode));
        render_width = extent.width;
        render_height = extent.height;
        upscaling = true;
        return cy::ok();
    }

    /// The decal table, the upscaler and ambient occlusion, each only when the run asked for it.
    [[nodiscard]] Status create_passes() noexcept {
        if (options.decals) {
            decal_table.initialize(*device, *allocator);
            if (Status reserved = decal_table.reserve(kDecalRows); !reserved) {
                return reserved;
            }
        }
        if (upscaling) {
            upscale::UpscalePassDescription description;
            description.output = upscale::Extent2D{options.width, options.height};
            if (Status made = upscaler.create(*allocator, *device, description); !made) {
                return made;
            }
            if (Status bound = upscaler.bind_temporal(assembly.temporal()); !bound) {
                return bound;
            }
        }
        if (!options.ambient_occlusion) {
            return cy::ok();
        }
        rendering::occlusion::AmbientOcclusionPassDescription description;
        description.width = render_width;
        description.height = render_height;
        if (Status made = occlusion.create(*allocator, *device, description); !made) {
            return made;
        }
        rendering::occlusion::GtaoSettings settings;
        settings.shared.radius = 1.2F;
        occlusion.set_settings(settings);
        return cy::ok();
    }

    /// The device GI over the arena's system, at the render extent.
    [[nodiscard]] Status create_gi(rendering::gi::IlluminationSystem& system) noexcept {
        if (!options.gi) {
            return cy::ok();
        }
        if (!gi_gpu::GpuGiFrame::supported(*device)) {
            return cy::fail(cy::ErrorCode::Unsupported, "the device cannot run the GI");
        }
        gi_gpu::GpuGiFrameDescription description;
        description.width = render_width;
        description.height = render_height;
        return gi.create(*allocator, *device, system, description);
    }

    /// One frame's numbers into the report: the frames that are timed are past the warm-up and
    /// write no picture.
    void account(const FpsHost& host, const rendering::assembly::AssemblyReport& assembled,
                 f64 elapsed, bool written) noexcept {
        draws = recorder.report().draws();
        viewmodel_draws = std::max(viewmodel_draws, assembled.viewmodel_draws);
        decals_assigned = std::max(decals_assigned, assembled.decals);
        particles_drawn = std::max(particles_drawn, effect.report().particles);
        if (frame <= kWarmFrames || written) {
            return;
        }
        timed += 1;
        frame_ms += elapsed;
        worst_ms = std::max(worst_ms, elapsed);
        record_ms += last_record_ms;
        device_wait_ms += last_wait_ms;
        game_ms += host.last_frame_ms();
        const f64 measured = device_ms();
        if (measured >= 0.0) {
            gpu_ms += measured;
            ++gpu_timed;
        }
        const gi_gpu::GpuGiCost cost = gi.last_cost();
        if (options.gi && cost.any()) {
            gi_ms += cost.total_ms();
        }
    }

    /// Write this frame as `directory/<prefix><frame number, zero-padded to digits>.png`.
    [[nodiscard]] Status write_frame(const std::string& directory, const char* prefix,
                                     int digits) noexcept {
        char number[32];
        (void)std::snprintf(number, sizeof(number), "%0*llu", digits,
                            static_cast<unsigned long long>(frame));
        return write_png(directory + "/" + prefix + number + ".png");
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

FpsRenderer::FpsRenderer(cy::Allocator& allocator) noexcept : allocator_(&allocator) {}

FpsRenderer::~FpsRenderer() {
    if (state_ == nullptr) {
        return;
    }
    // THE DEVICE GOES LAST: everything that made a resource through it releases it first.
    rhi::Device* device = state_->device;
    if (device != nullptr) {
        (void)device->wait_idle();
        State& state = *state_;
        state.gi.destroy();
        state.upscaler.destroy();
        state.occlusion.destroy();
        state.decal_table.shutdown();
        state.effect.shutdown();
        state.textures.shutdown();
        state.bindings.shutdown();
        state.pipelines.shutdown();
        if (!state.timer.is_null()) {
            device->destroy_query_pool(state.timer);
        }
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

cy::Status FpsRenderer::start(FpsHost& host, const RenderOptions& options) noexcept {
    state_ = std::make_unique<State>(*allocator_);
    State& state = *state_;
    state.options = options;
    state.decal_materials_ = host.effects().decal_materials();
    using Step = Status (State::*)() noexcept;
    for (const Step step :
         {&State::choose_extent, &State::create_device, &State::create_frame,
          &State::create_textures, &State::create_materials, &State::create_geometry,
          &State::create_targets, &State::create_passes}) {
        if (Status made = (state.*step)(); !made) {
            return made;
        }
    }
    return state.create_gi(host.illumination().system());
}

cy::Status FpsRenderer::frame(FpsHost& host) noexcept {
    State& state = *state_;
    state.frame = host.frames();
    state.host_frame = host.illumination().frame();
    const auto started = std::chrono::steady_clock::now();
    state.set_camera(host);
    if (Status placed = state.place(host); !placed) {
        return placed;
    }
    if (Status gathered = state.gather(host); !gathered) {
        return gathered;
    }
    if (Status packed = state.options.decals ? state.pack_decals() : cy::ok(); !packed) {
        return packed;
    }
    state.place_rows();
    const bool played = state.options.device != nullptr;
    const bool still =
        !played && std::ranges::find(kStills, state.frame) != std::end(kStills);
    const bool video = !state.options.video_directory.empty() &&
                       state.frame % std::max<u32>(state.options.video_every, 1U) == 0U;
    rendering::assembly::AssemblyReport assembled{};
    if (Status rendered = state.render(still || video, assembled); !rendered) {
        return rendered;
    }
    state.account(host, assembled, since_ms(started), still || video);
    if (video) {
        if (Status written = state.write_frame(state.options.video_directory, "frame-", 4);
            !written) {
            return written;
        }
    }
    if (!still) {
        return cy::ok();
    }
    if (Status written = state.write_frame(state.options.directory, "fps-", 3); !written) {
        return written;
    }
    std::fprintf(stderr, "wrote %s/fps-%03llu.png\n", state.options.directory.c_str(),
                 static_cast<unsigned long long>(state.frame));
    state.stills += 1;
    state.print_marks(host);
    return cy::ok();
}

cy::rhi::TextureHandle FpsRenderer::output() const noexcept {
    return state_ == nullptr ? cy::rhi::TextureHandle{} : state_->output;
}

cy::u32 FpsRenderer::width() const noexcept {
    return state_ == nullptr ? 0U : state_->options.width;
}

cy::u32 FpsRenderer::height() const noexcept {
    return state_ == nullptr ? 0U : state_->options.height;
}

cy::f64 FpsRenderer::last_frame_ms() const noexcept {
    return state_ == nullptr ? 0.0 : state_->last_record_ms + state_->last_wait_ms;
}

void FpsRenderer::print_report() const noexcept {
    if (state_ == nullptr) {
        return;
    }
    const State& state = *state_;
    const f64 timed = static_cast<f64>(std::max<u64>(state.timed, 1));
    const f64 gpu_timed = static_cast<f64>(std::max<u64>(state.gpu_timed, 1));
    std::printf(
        "fps render   width=%u height=%u render_width=%u render_height=%u frames=%llu timed=%llu "
        "frame_ms=%.3f worst_ms=%.3f game_ms=%.3f total_ms=%.3f record_ms=%.3f "
        "device_wait_ms=%.3f gpu_ms=%.3f "
        "gi_device_ms=%.3f draws=%u viewmodel_draws=%u decals=%u particles=%u lights=%u "
        "stills=%u validation_errors=%u upscale=%s gi=%d ao=%d shadows=%d weapon_view=%d "
        "decals_on=%d particles_on=%d flashes_on=%d\n",
        state.options.width, state.options.height, state.render_width, state.render_height,
        static_cast<unsigned long long>(state.frame), static_cast<unsigned long long>(state.timed),
        state.frame_ms / timed, state.worst_ms, state.game_ms / timed,
        (state.frame_ms + state.game_ms) / timed, state.record_ms / timed,
        state.device_wait_ms / timed, state.gpu_ms / gpu_timed, state.gi_ms / timed, state.draws,
        state.viewmodel_draws, state.decals_assigned, state.particles_drawn, state.peak_lights,
        state.stills, state.validation_errors, state.options.upscale.c_str(),
        state.options.gi ? 1 : 0, state.options.ambient_occlusion ? 1 : 0,
        state.options.shadows ? 1 : 0, state.options.weapon_view ? 1 : 0,
        state.options.decals ? 1 : 0, state.options.particles ? 1 : 0,
        state.options.flashes ? 1 : 0);
}

}  // namespace sample::fps
