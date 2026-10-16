// SPDX-License-Identifier: MIT
#include "gi.h"

#include <cy/core/assets/file.h>
#include <cy/core/math/scalar.h>
#include <cy/import/image_codecs.h>
#include <cy/rendering/gi/bake.h>
#include <cy/rendering/gi/mesh_cards.h>
#include <cy/rendering/gi/screen_resolve.h>
#include <cy/rendering/gi/system.h>
#include <cy/rendering/gi_gpu/gpu_radiance_cache.h>
#include <cy/rendering/gi_gpu/gpu_scene.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/lightmap_bake/scene.h>
#include <cy/rendering/raytracing/geometry.h>
#include <cy/rendering/raytracing_gpu/device_structures.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <thread>

namespace cy::sample::beauty {

namespace gi = rendering::gi;
namespace gi_gpu = rendering::gi_gpu;
namespace rt = rendering::rt;
namespace rt_gpu = rendering::rt_gpu;
namespace lightmap_bake = rendering::lightmap_bake;

namespace {

// --- The GI scene's scale, stated once ----------------------------------------------------------
//
// THE FIELD: three levels of 64 voxels, a quarter metre at the finest, so its window is 64 metres
// across at the coarsest — the colonnade's far end is 33 metres from the camera. THE PROBES: three
// levels from a metre apart, six either way, so the finest covers the foreground objects and the
// coarsest reaches the far columns. THE CARDS: half a metre over every object, a metre over the
// ground, and the ground only where the camera's frame can see it — 96 metres of gravel the frame
// never shows would be cards lit for nothing.

constexpr u32 kFieldLevels = 3;
constexpr u32 kFieldResolution = 64;
constexpr f32 kFieldExtent = 16.0F;
constexpr u32 kProbeLevels = 3;
constexpr u32 kProbeHalfExtent = 6;
constexpr u32 kRaysPerProbe = 32;
constexpr f32 kMaxRayDistance = 40.0F;
constexpr f32 kObjectCardSpacing = 0.5F;
constexpr f32 kGroundCardSpacing = 1.0F;
/// The ground's cards, in WORLD space — the shot's own coordinates, where the ground is y = 0 —
/// the frame's width either side, from just behind the camera to past the last column. Moved into
/// the baked space by the camera's offset where it is used.
const Aabb kGroundRegion =
    Aabb::from_min_max(Vec3{-48.0F, -1.0F, -48.0F}, Vec3{48.0F, 1.0F, 48.0F});
constexpr f32 kLookupRadius = 0.75F;
constexpr u32 kMeshFieldDimension = 32;
/// Converged mode's bar and its ceiling: `IlluminationSystem::advance_to_convergence`'s metric.
constexpr f32 kConvergedThreshold = 0.98F;
constexpr u32 kConvergedFrames = 32;
constexpr f32 kMeshFieldMargin = 0.5F;

[[nodiscard]] f64 now_millis() noexcept {
    using clock = std::chrono::steady_clock;
    return std::chrono::duration<f64, std::milli>(clock::now().time_since_epoch()).count();
}

[[nodiscard]] f32 srgb_to_linear(f32 value) noexcept {
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

/// The mean of an image's first three channels, in linear light when `srgb`.
[[nodiscard]] Expected<Vec3, Error> image_mean(const std::string& path, bool srgb) noexcept {
    Array<u8> bytes;
    if (Status read = assets::fs::read_whole(path.c_str(), bytes); !read) {
        return make_unexpected(read.error());
    }
    auto decoded = import::decode_png(bytes.span());
    if (!decoded) {
        return make_unexpected(decoded.error());
    }
    const import::ImageData& image = decoded.value();
    if (image.channels < 3 || image.is_empty()) {
        return fail(ErrorCode::InvalidArgument, "a material map with fewer than three channels");
    }
    // A table, because a 1024-square map is a million `pow` calls otherwise.
    f32 table[256];
    for (u32 value = 0; value < 256; ++value) {
        const f32 unit = static_cast<f32>(value) / 255.0F;
        table[value] = srgb ? srgb_to_linear(unit) : unit;
    }
    f64 sum[3] = {0.0, 0.0, 0.0};
    const usize count = static_cast<usize>(image.width) * image.height;
    for (usize pixel = 0; pixel < count; ++pixel) {
        for (u32 channel = 0; channel < 3; ++channel) {
            sum[channel] +=
                static_cast<f64>(table[image.pixels[(pixel * image.channels) + channel]]);
        }
    }
    const f64 inverse = 1.0 / static_cast<f64>(count);
    return Vec3{static_cast<f32>(sum[0] * inverse), static_cast<f32>(sum[1] * inverse),
                static_cast<f32>(sum[2] * inverse)};
}

/// A material's diffuse albedo as a card sees it: the albedo map's mean in linear light times the
/// authored base colour, less the share its metallic channel takes — the graph's own
/// `albedo * base_color * (1 - metallic)`, averaged.
[[nodiscard]] Expected<Vec3, Error> card_albedo(const ShotMaterial& material) noexcept {
    auto albedo = image_mean(material.albedo_path, true);
    if (!albedo) {
        return albedo;
    }
    auto data = image_mean(material.data_path, false);
    if (!data) {
        return data;
    }
    const Vec4 base = material.parameter_defaults.empty() ? Vec4{1.0F, 1.0F, 1.0F, 0.0F}
                                                          : material.parameter_defaults[0];
    const f32 metallic_parameter =
        material.parameter_defaults.size() > 2 ? material.parameter_defaults[2].x : 0.0F;
    const f32 diffuse_share = 1.0F - std::clamp(metallic_parameter * data.value().z, 0.0F, 1.0F);
    return Vec3{albedo.value().x * base.x, albedo.value().y * base.y, albedo.value().z * base.z} *
           diffuse_share;
}

/// The instance's model to baked-space transform: the yaw `build_geometry` rotates by, then the
/// position less the shot camera's.
[[nodiscard]] Mat4 instance_transform(const Instance& instance, Vec3 camera) noexcept {
    const f32 radians = instance.yaw_degrees * std::numbers::pi_v<f32> / 180.0F;
    const f32 cosine = std::cos(radians);
    const f32 sine = std::sin(radians);
    Mat4 rotation = Mat4::identity();
    rotation.at(0, 0) = cosine;
    rotation.at(0, 2) = sine;
    rotation.at(2, 0) = -sine;
    rotation.at(2, 2) = cosine;
    const Vec3 offset{
        static_cast<f32>(static_cast<f64>(instance.position.x) - static_cast<f64>(camera.x)),
        static_cast<f32>(static_cast<f64>(instance.position.y) - static_cast<f64>(camera.y)),
        static_cast<f32>(static_cast<f64>(instance.position.z) - static_cast<f64>(camera.z))};
    return Mat4::from_translation(offset) * rotation;
}

/// THE GI'S SKY, FITTED TO THE ENGINE'S: the gradient's shape from the atmosphere's zenith and
/// horizon, and its scale, channel by channel, such that an upward face receives exactly the mean
/// radiance `compose_sky_lighting` answered — the number the published frame's ambient term is.
/// The gradient alone has no sun aureole, and a low sun's aureole is most of a clear sky's light:
/// unscaled, the GI's sky lit an upward face with a third of the frame's.
[[nodiscard]] gi::SkyTerm fit_sky(const BeautyGiLight& light) noexcept {
    gi::SkyTerm sky;
    sky.zenith = light.zenith;
    sky.horizon = light.horizon;
    sky.ground = Vec3{0.0F, 0.0F, 0.0F};
    sky.intensity = 1.0F;
    constexpr u32 kSamples = 1024;
    Vec3 mean{0.0F, 0.0F, 0.0F};
    for (u32 sample = 0; sample < kSamples; ++sample) {
        const Vec3 direction = gi::screen_cosine_direction(
            Vec3{0.0F, 1.0F, 0.0F}, (static_cast<f32>(sample) + 0.5F) / static_cast<f32>(kSamples),
            std::fmod(static_cast<f32>(sample) * 0.618034F, 1.0F));
        mean = mean + (sky.radiance(direction) / static_cast<f32>(kSamples));
    }
    const Vec3 scale{light.mean_sky.x / std::max(mean.x, 1.0e-6F),
                     light.mean_sky.y / std::max(mean.y, 1.0e-6F),
                     light.mean_sky.z / std::max(mean.z, 1.0e-6F)};
    sky.zenith = Vec3{light.zenith.x * scale.x, light.zenith.y * scale.y, light.zenith.z * scale.z};
    sky.horizon =
        Vec3{light.horizon.x * scale.x, light.horizon.y * scale.y, light.horizon.z * scale.z};
    // Below the horizon is the ground the field holds; this is only what escapes past its edge.
    sky.ground = light.mean_sky * (1.0F / 3.0F);
    return sky;
}

[[nodiscard]] bool is_plane(const import::MeshData& mesh) noexcept {
    f32 lo = mesh.positions[0].y;
    f32 hi = lo;
    for (const Vec3 p : mesh.positions.span()) {
        lo = std::min(lo, p.y);
        hi = std::max(hi, p.y);
    }
    return hi - lo < 1.0e-4F;
}

}  // namespace

struct BeautyGi::State {
    explicit State(Allocator& allocator) noexcept : cards(allocator) {}

    struct Mesh {
        std::string name;
        std::vector<Vec3> positions;
        std::vector<u32> indices;
        std::unique_ptr<gi::MeshDistanceField> field;
        bool plane = false;
    };

    std::vector<Mesh> meshes;
    std::vector<Vec3> albedo;
    Array<gi::Surfel> cards;
    std::vector<gi::GiLight> lights;
    gi::IlluminationSystem system;
    BeautyGiLight light;
    BeautyGiSettings settings;

    rhi::Device* device = nullptr;
    gi_gpu::GpuGiScene scene;
    gi_gpu::GpuRadianceCache cache;
    gi_gpu::GpuScreenResolve resolve;
    rt_gpu::DeviceStructures structures;
    bool hardware = false;
    u32 width = 0;
    u32 height = 0;
    u32 placements = 0;

    // For the reference: the shot's instances as the bake's triangle tracer holds them.
    std::vector<lightmap_bake::BakeMesh> bake_meshes;
    std::vector<lightmap_bake::BakeMaterial> bake_materials;
    std::vector<lightmap_bake::BakeInstance> bake_instances;
    lightmap_bake::MeshSceneTracer exact;
    bool exact_built = false;
};

BeautyGi::BeautyGi(Allocator& allocator) noexcept : allocator_(&allocator) {}

BeautyGi::~BeautyGi() {
    destroy();
}

void BeautyGi::destroy() noexcept {
    if (state_ == nullptr) {
        return;
    }
    if (state_->device != nullptr) {
        (void)state_->device->wait_idle();
    }
    state_->system.acceleration().set_mirror(nullptr);
    state_->resolve.destroy();
    state_->cache.destroy();
    state_->scene.destroy();
    state_->structures.destroy();
    state_.reset();
}

Status BeautyGi::build(const Shot& shot,
                       const std::vector<std::pair<std::string, import::MeshData>>& meshes,
                       const BeautyGiLight& light, const BeautyGiSettings& settings,
                       rhi::Device& device, u32 width, u32 height) noexcept {
    if (!gi_gpu::GpuScreenResolve::supported(device)) {
        return fail(ErrorCode::Unsupported,
                    "the device GI needs compute and a shader format it ships");
    }
    state_ = std::make_unique<State>(*allocator_);
    State& state = *state_;
    state.light = light;
    state.settings = settings;
    state.width = width;
    state.height = height;
    state.hardware = settings.hardware && rt_gpu::DeviceStructures::supported(device);

    // --- The caches
    // -------------------------------------------------------------------------------
    gi::IlluminationSettings configured;
    configured.field.levels = kFieldLevels;
    configured.field.resolution = kFieldResolution;
    configured.field.base_extent_metres = kFieldExtent;
    configured.probes.levels = kProbeLevels;
    configured.probes.half_extent_probes = kProbeHalfExtent;
    configured.rays_per_probe = kRaysPerProbe;
    configured.max_ray_distance_metres = kMaxRayDistance;
    configured.surface_bounce = gi::SurfaceBounce::Cards;
    configured.card_gather.max_distance_metres = kMaxRayDistance;
    configured.ray_tracing.device_supports_ray_tracing = state.hardware;
    // THE SKY THE ENGINE'S ATMOSPHERE ANSWERED, as the GI's gradient: straight up, round the
    // horizon, and below it a third of the mean — the ground the field holds answers every ray
    // that meets it, so this is only what escapes past the gravel's edge.
    configured.sky = fit_sky(light);
    if (Status made = state.system.configure(configured); !made) {
        return made;
    }

    // --- The meshes: a distance grid each, and their triangles for the tiers that need them
    // -------
    for (const auto& entry : meshes) {
        State::Mesh mesh;
        mesh.name = entry.first;
        mesh.positions.assign(entry.second.positions.begin(), entry.second.positions.end());
        mesh.indices.assign(entry.second.indices.begin(), entry.second.indices.end());
        mesh.plane = is_plane(entry.second);
        mesh.field = std::make_unique<gi::MeshDistanceField>();
        gi::MeshFieldSettings field;
        field.dimension = kMeshFieldDimension;
        field.margin_metres = kMeshFieldMargin;
        if (Status built = mesh.field->build({mesh.positions.data(), mesh.positions.size()},
                                             {mesh.indices.data(), mesh.indices.size()}, field);
            !built) {
            return built;
        }
        state.meshes.push_back(std::move(mesh));
    }
    for (const ShotMaterial& material : shot.materials) {
        auto albedo = card_albedo(material);
        if (!albedo) {
            std::fprintf(stderr, "cy_sample_beauty: gi: %s: %s\n", material.key.c_str(),
                         albedo.error().message);
            return make_unexpected(albedo.error());
        }
        state.albedo.push_back(albedo.value());
        lightmap_bake::BakeMaterial bake;
        bake.albedo = albedo.value();
        state.bake_materials.push_back(bake);
    }
    for (const State::Mesh& mesh : state.meshes) {
        lightmap_bake::BakeMesh bake;
        bake.positions = {mesh.positions.data(), mesh.positions.size()};
        bake.indices = {mesh.indices.data(), mesh.indices.size()};
        state.bake_meshes.push_back(bake);
        if (state.hardware) {
            rt::TriangleGeometry triangles;
            triangles.positions = bake.positions;
            triangles.indices = bake.indices;
            if (Status declared = state.system.acceleration().declare_geometry(
                    static_cast<rt::GeometryId>(state.bake_meshes.size()),
                    rt::adapt_static_mesh(triangles, 0));
                !declared) {
                return declared;
            }
        }
    }

    // --- The instances: placed in the field, carded, and in the tracers -------------------------
    u32 instance_id = 0;
    for (const Instance& instance : shot.instances) {
        ++instance_id;
        u32 mesh_index = 0;
        while (mesh_index < state.meshes.size() && state.meshes[mesh_index].name != instance.mesh) {
            ++mesh_index;
        }
        u32 material_index = 0;
        while (material_index < shot.materials.size() &&
               shot.materials[material_index].key != instance.material) {
            ++material_index;
        }
        if (mesh_index == state.meshes.size() || material_index == shot.materials.size()) {
            return fail(ErrorCode::NotFound,
                        "a GI instance names a mesh or material the shot lacks");
        }
        const State::Mesh& mesh = state.meshes[mesh_index];
        const Mat4 transform = instance_transform(instance, shot.camera_position);
        if (Status placed = state.system.field().place(instance_id, mesh.field->asset(), transform);
            !placed) {
            return placed;
        }
        ++state.placements;

        gi::MeshCardSettings cards;
        cards.spacing_metres = mesh.plane ? kGroundCardSpacing : kObjectCardSpacing;
        cards.albedo = state.albedo[material_index];
        cards.roughness = shot.materials[material_index].parameter_defaults.size() > 1
                              ? shot.materials[material_index].parameter_defaults[1].x
                              : 1.0F;
        cards.instance_id = instance_id;
        cards.material_id = material_index;
        cards.clip_to_region = mesh.plane;
        cards.region = Aabb::from_min_max(kGroundRegion.min - shot.camera_position,
                                          kGroundRegion.max - shot.camera_position);
        if (Status carded = gi::append_mesh_cards({mesh.positions.data(), mesh.positions.size()},
                                                  {mesh.indices.data(), mesh.indices.size()},
                                                  transform, cards, state.cards);
            !carded) {
            return carded;
        }

        lightmap_bake::BakeInstance bake;
        bake.mesh = mesh_index;
        bake.material = material_index;
        bake.transform = transform;
        bake.receives_lightmap = false;
        bake.id = instance_id;
        state.bake_instances.push_back(bake);
        if (state.hardware) {
            rt::InstanceDescriptor descriptor;
            descriptor.geometry = static_cast<rt::GeometryId>(mesh_index) + 1U;
            descriptor.transform = transform;
            descriptor.instance_id = instance_id;
            descriptor.material_id = material_index;
            if (auto added = state.system.acceleration().add_instance(descriptor); !added) {
                return make_unexpected(added.error());
            }
        }
    }
    if (state.cards.empty()) {
        return fail(ErrorCode::InvalidArgument, "the shot's meshes gave the GI scene no card");
    }
    Aabb bounds = Aabb::from_min_max(state.cards[0].position, state.cards[0].position);
    for (const gi::Surfel& card : state.cards.span()) {
        bounds = Aabb::from_min_max(cwise_min(bounds.min, card.position),
                                    cwise_max(bounds.max, card.position));
    }
    bounds = Aabb::from_min_max(bounds.min - Vec3{1.0F, 1.0F, 1.0F},
                                bounds.max + Vec3{1.0F, 1.0F, 1.0F});
    if (Status ingested = state.system.ingest_cell(1, bounds, state.cards.span(), 0); !ingested) {
        return ingested;
    }
    state.system.surfaces().set_lookup_radius(kLookupRadius);

    // THE SHOT'S SUN, the one the frame shades with: `to_sun` turned into the direction it
    // travels, its illuminance split into a colour and a figure — DIVIDED BY PI, as
    // `lightmap_bake` hands its path tracer every light (bake.h): the GI shades a card as
    // `albedo * direct`, so a light in lux would make every bounce pi times the frame's own
    // `albedo / pi * E`. Scaled, the ground's card is the radiance the frame draws the ground with.
    gi::GiLight sun;
    sun.directional = true;
    sun.direction = -light.to_sun;
    const f32 peak = std::max(
        {light.sun_illuminance.x, light.sun_illuminance.y, light.sun_illuminance.z, 1.0e-6F});
    sun.colour = light.sun_illuminance / peak;
    sun.intensity = peak / std::numbers::pi_v<f32>;
    sun.radius = light.sun_angular_radius;
    sun.id = 1;
    state.lights.push_back(sun);

    // --- The device half -------------------------------------------------------------------------
    state.device = &device;
    gi_gpu::GpuGiSceneDescription scene;
    scene.max_levels = kFieldLevels;
    scene.max_window_bricks = kFieldResolution / 4U;
    scene.max_brick_slots = 32768;
    scene.max_cards = 16384;
    scene.max_selection = 4096;
    if (Status made = state.scene.create(*allocator_, device, scene); !made) {
        return made;
    }
    // Every probe the three levels can hold: thirteen a side, a cube a level.
    gi_gpu::GpuRadianceCacheDescription cache;
    const u32 side = (kProbeHalfExtent * 2U) + 1U;
    cache.max_probes = kProbeLevels * side * side * side;
    if (Status made = state.cache.create(*allocator_, device, state.scene, cache); !made) {
        return made;
    }
    state.cache.bind_field(state.system.field());
    state.cache.bind_host_surfaces(&state.system.surfaces());
    if (state.hardware) {
        if (Status made = state.structures.create(*allocator_, device, {}); !made) {
            return made;
        }
        state.system.acceleration().set_mirror(&state.structures);
        (void)state.system.acceleration().update();
        // THE STRUCTURES ARE BUILT ONCE, BEFORE THE FIRST FRAME, so the probe gather that frame
        // runs and the resolve's ray queries both find a traceable scene.
        if (Status begun =
                device.begin_frame().has_value()
                    ? ok()
                    : fail(ErrorCode::Internal, "the GI's structure frame did not begin");
            !begun) {
            return begun;
        }
        rendering::RenderGraph graph(*allocator_);
        Status declared = state.structures.declare(graph);
        if (declared) {
            rendering::GraphExecutor executor(*allocator_, device);
            auto executed =
                executor.execute(graph, rendering::CompileOptions{}, rendering::ExecuteOptions{});
            if (!executed) {
                declared = make_unexpected(executed.error());
            }
        }
        (void)device.wait_idle();
        (void)device.end_frame();
        if (!declared) {
            return declared;
        }
        if (Status bound =
                state.cache.bind_hardware(&state.system.acceleration(), &state.structures);
            !bound) {
            return bound;
        }
    }
    gi_gpu::GpuScreenResolveDescription resolve;
    resolve.width = width;
    resolve.height = height;
    resolve.readback = settings.readback;
    if (Status made = state.resolve.create(*allocator_, device, state.cache, resolve); !made) {
        return made;
    }
    gi::ScreenResolveSettings screen;
    screen.mode = gi::GiMode::Dynamic;
    screen.max_ray_distance_metres = kMaxRayDistance;
    screen.gather = settings.per_pixel ? gi::GiGather::PerPixel : gi::GiGather::ScreenProbes;
    screen.probes.denoiser = settings.shared_denoiser ? gi::ScreenProbeDenoiser::Shared
                                                      : gi::ScreenProbeDenoiser::ProbeSpace;
    state.resolve.set_settings(screen);
    state.resolve.set_denoiser(state.system.denoiser());
    state.resolve.set_field(state.system.field());
    state.resolve.set_sky(state.system.sky_term());
    state.resolve.set_timing(true);
    if (Status set = state.resolve.set_divisor(std::max(settings.divisor, 1U)); !set) {
        return set;
    }
    return ok();
}

Status BeautyGi::prepare(rendering::RenderGraph& graph, rendering::assembly::AssemblyView& view,
                         rendering::assembly::FrameSinks& sinks, const BeautyGiView& frame,
                         u64 frame_index) noexcept {
    if (state_ == nullptr) {
        return fail(ErrorCode::InvalidArgument, "the GI was not built");
    }
    State& state = *state_;
    gi::FrameContext context;
    context.camera = frame.eye;
    context.lights = {state.lights.data(), state.lights.size()};
    context.frame = frame_index;
    if (frame_index == 0 && state.settings.converged) {
        // CONVERGED MODE FOR A CAPTURE: `rendering-global-illumination` asks for it for "cinematic
        // capture, and reference comparison", so the still is the caches at convergence rather
        // than wherever a budgeted schedule had reached by its frame. Every frame after this one
        // runs at the budget's rates, as a game's does.
        const f64 converge_mark = now_millis();
        converged_frames_ =
            state.system.advance_to_convergence(context, kConvergedThreshold, kConvergedFrames);
        converge_ms_ = now_millis() - converge_mark;
        context.frame = converged_frames_;
    }
    const f64 mark = now_millis();
    (void)state.system.update(context);
    update_ms_ = now_millis() - mark;
    if (Status synced = state.resolve.sync(state.system.radiance()); !synced) {
        return synced;
    }
    state.resolve.set_cache_frame(frame_index);
    gi_gpu::GpuResolveView resolve_view;
    resolve_view.camera = frame.eye;
    resolve_view.world_to_clip = frame.world_to_clip;
    resolve_view.projection[0] = frame.projection.columns[0].x;
    resolve_view.projection[1] = frame.projection.columns[1].y;
    resolve_view.projection[2] = frame.projection.columns[2].z;
    resolve_view.projection[3] = frame.projection.columns[3].z;
    resolve_view.relative_to_view = frame.camera;
    if (Status began = state.resolve.begin_frame(resolve_view, frame_index); !began) {
        return began;
    }
    if (state.hardware) {
        if (Status declared = state.structures.declare(graph); !declared) {
            return declared;
        }
    }
    view.screen_space_gi = state.resolve.import_diffuse(graph);
    view.screen_space_gi_specular = state.resolve.import_specular(graph);
    sinks.screen_space_gi = state.resolve.stage();
    return ok();
}

rhi::TextureViewHandle BeautyGi::diffuse_view() const noexcept {
    return state_ != nullptr ? state_->resolve.diffuse_view() : rhi::TextureViewHandle{};
}

rhi::TextureViewHandle BeautyGi::specular_view() const noexcept {
    return state_ != nullptr ? state_->resolve.specular_view() : rhi::TextureViewHandle{};
}

gi_gpu::GpuResolveTimes BeautyGi::last_times() const noexcept {
    return state_ != nullptr ? state_->resolve.last_times() : gi_gpu::GpuResolveTimes{};
}

const char* BeautyGi::tier_name() const noexcept {
    if (state_ == nullptr) {
        return "none";
    }
    return state_->resolve.last_tier() == gi::RadianceSource::HardwareTrace ? "hardware ray queries"
                                                                            : "software field";
}

u32 BeautyGi::card_count() const noexcept {
    return state_ != nullptr ? static_cast<u32>(state_->cards.size()) : 0U;
}

u32 BeautyGi::placements() const noexcept {
    return state_ != nullptr ? state_->placements : 0U;
}

Expected<BeautyGiReference, Error> BeautyGi::measure_reference(u32 pixels, u32 samples,
                                                               u32 bounces) noexcept {
    if (state_ == nullptr || !state_->settings.readback) {
        return fail(ErrorCode::InvalidArgument, "the GI reference needs a readback run");
    }
    State& state = *state_;
    if (!state.exact_built) {
        lightmap_bake::LightmapScene scene;
        scene.meshes = {state.bake_meshes.data(), state.bake_meshes.size()};
        scene.materials = {state.bake_materials.data(), state.bake_materials.size()};
        scene.instances = {state.bake_instances.data(), state.bake_instances.size()};
        if (Status built = state.exact.build(scene); !built) {
            return make_unexpected(built.error());
        }
        state.exact_built = true;
    }
    const u32 grid_width = state.resolve.grid_width();
    const u32 grid_height = state.resolve.grid_height();
    const u32 divisor = state.resolve.divisor();
    const usize cells = static_cast<usize>(grid_width) * grid_height;
    std::vector<Vec3> position(cells);
    std::vector<f32> depth(cells);
    std::vector<Vec3> normal(cells);
    std::vector<f32> roughness(cells);
    if (Status read =
            state.resolve.read_back_surfaces({position.data(), cells}, {depth.data(), cells},
                                             {normal.data(), cells}, {roughness.data(), cells});
        !read) {
        return make_unexpected(read.error());
    }
    const usize frame_pixels = static_cast<usize>(state.width) * state.height;
    std::vector<Vec3> diffuse(frame_pixels);
    std::vector<Vec3> specular(frame_pixels);
    if (Status read = state.resolve.read_back_published({diffuse.data(), frame_pixels},
                                                        {specular.data(), frame_pixels});
        !read) {
        return make_unexpected(read.error());
    }

    const gi::PathTracer tracer(state.exact, state.system.scene(),
                                {state.lights.data(), state.lights.size()}, state.system.sky_term(),
                                &state.exact);
    gi::BakeSettings bake;
    bake.samples = samples;
    bake.bounces = bounces;
    bake.max_distance_metres = kMaxRayDistance;
    std::vector<Vec3> measured;
    std::vector<Vec3> flat;
    std::vector<Vec3> reference;
    // A lattice over the grid, every cell that sees a surface: deterministic, and even over the
    // frame rather than over the surfaces.
    const u32 side = std::max(1U, static_cast<u32>(std::sqrt(static_cast<f32>(pixels))));
    for (u32 row = 0; row < side; ++row) {
        for (u32 column = 0; column < side; ++column) {
            const u32 x = (((column * 2U) + 1U) * grid_width) / (side * 2U);
            const u32 y = (((row * 2U) + 1U) * grid_height) / (side * 2U);
            const usize cell = (static_cast<usize>(y) * grid_width) + x;
            if (depth[cell] <= 0.0F) {
                continue;
            }
            const Vec3 n = normal[cell];
            const Vec3 truth = tracer.irradiance(position[cell] + (n * 0.01F), n, bake);
            const usize pixel =
                (static_cast<usize>(std::min(y * divisor, state.height - 1U)) * state.width) +
                std::min(x * divisor, state.width - 1U);
            measured.push_back(diffuse[pixel]);
            // The published frame's ambient term: the sky's mean radiance, hemispherically weighted
            // by the normal — `beauty.slang`'s `frame.ambient.xyz * sky`.
            flat.push_back(state.light.mean_sky * ((n.y * 0.5F) + 0.5F));
            reference.push_back(truth);
        }
    }
    const gi::ReferenceComparison gi_error = gi::compare_against_reference(
        {measured.data(), measured.size()}, {reference.data(), reference.size()});
    const gi::ReferenceComparison flat_error = gi::compare_against_reference(
        {flat.data(), flat.size()}, {reference.data(), reference.size()});
    BeautyGiReference out;
    out.pixels = gi_error.samples;
    out.gi_relative_error = gi_error.relative_error;
    out.ambient_relative_error = flat_error.relative_error;
    out.mean_reference = gi_error.mean_reference_magnitude;
    return out;
}

Status BeautyGi::reference_image(u32 step, u32 samples, u32 bounces, u32 threads,
                                 BeautyGiReferenceImage& out) noexcept {
    if (state_ == nullptr || !state_->settings.readback || state_->resolve.divisor() != 1) {
        return fail(ErrorCode::InvalidArgument,
                    "the GI's reference picture needs a readback run on the full-resolution grid");
    }
    State& state = *state_;
    const u32 width = state.width;
    const u32 height = state.height;
    const usize pixels = static_cast<usize>(width) * height;
    std::vector<Vec3> position(pixels);
    std::vector<f32> depth(pixels);
    std::vector<Vec3> normal(pixels);
    std::vector<f32> roughness(pixels);
    if (Status read =
            state.resolve.read_back_surfaces({position.data(), pixels}, {depth.data(), pixels},
                                             {normal.data(), pixels}, {roughness.data(), pixels});
        !read) {
        return read;
    }
    std::vector<Vec3> published(pixels);
    std::vector<Vec3> specular(pixels);
    if (Status read = state.resolve.read_back_published({published.data(), pixels},
                                                        {specular.data(), pixels});
        !read) {
        return read;
    }
    gi::ScreenSurfaces frame;
    frame.width = width;
    frame.height = height;
    frame.depth = {depth.data(), pixels};
    frame.position = {position.data(), pixels};
    frame.normal = {normal.data(), pixels};
    frame.roughness = {roughness.data(), pixels};
    gi::ScreenGrid grid;
    if (Status built = grid.build(frame, std::max(step, 1U)); !built) {
        return built;
    }
    const gi::ScreenSurfaces cells = grid.surfaces();
    const usize cell_count = static_cast<usize>(cells.width) * cells.height;
    std::vector<Vec3> traced(cell_count, Vec3{0.0F, 0.0F, 0.0F});
    // Each pixel is traced as two independent halves, so the reference's own noise is measured
    // beside the GI's error: |a - b| / 2 is distributed as |(a + b) / 2 - truth|.
    std::vector<Vec3> half_apart(cell_count, Vec3{0.0F, 0.0F, 0.0F});

    // EACH THREAD ITS OWN TRACERS: `MeshSceneTracer` and `PathTracer` count their rays in a plain
    // member, so one shared between threads is a data race however read-only the tracing is.
    // A copy of the triangles a thread is a fraction of a second; the paths are minutes.
    lightmap_bake::LightmapScene scene;
    scene.meshes = {state.bake_meshes.data(), state.bake_meshes.size()};
    scene.materials = {state.bake_materials.data(), state.bake_materials.size()};
    scene.instances = {state.bake_instances.data(), state.bake_instances.size()};
    const u32 workers = std::clamp(threads, 1U, 32U);
    std::vector<Status> outcomes(workers, ok());
    const f64 mark = now_millis();
    const auto work = [&](u32 worker) noexcept {
        lightmap_bake::MeshSceneTracer exact;
        if (Status built = exact.build(scene); !built) {
            outcomes[worker] = built;
            return;
        }
        const gi::PathTracer tracer(exact, state.system.scene(),
                                    {state.lights.data(), state.lights.size()},
                                    state.system.sky_term(), &exact);
        gi::BakeSettings bake;
        bake.samples = std::max(samples / 2U, 1U);
        bake.bounces = bounces;
        bake.max_distance_metres = kMaxRayDistance;
        for (usize cell = worker; cell < cell_count; cell += workers) {
            if (cells.depth[cell] <= 0.0F) {
                continue;
            }
            // A sequence of its own a pixel and a half, so no two estimates share a pattern.
            const Vec3 n = cells.normal[cell];
            const Vec3 at = cells.position[cell] + (n * 0.01F);
            bake.seed = gi::screen_hash(static_cast<u32>(cell), 0, 17);
            const Vec3 first = tracer.irradiance(at, n, bake);
            bake.seed = gi::screen_hash(static_cast<u32>(cell), 0, 18);
            const Vec3 second = tracer.irradiance(at, n, bake);
            traced[cell] = (first + second) * 0.5F;
            half_apart[cell] = (first - second) * 0.5F;
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(workers);
    for (u32 worker = 0; worker < workers; ++worker) {
        pool.emplace_back(work, worker);
    }
    for (std::thread& thread : pool) {
        thread.join();
    }
    for (const Status& outcome : outcomes) {
        if (!outcome) {
            return outcome;
        }
    }
    out.trace_ms = now_millis() - mark;

    out.width = width;
    out.height = height;
    out.step = std::max(step, 1U);
    out.samples = samples;
    out.bounces = bounces;
    out.reference.assign(pixels, Vec3{0.0F, 0.0F, 0.0F});
    if (Status upsampled = gi::screen_upsample(frame, cells, out.step, {traced.data(), cell_count},
                                               {out.reference.data(), pixels});
        !upsampled) {
        return upsampled;
    }
    // The figure is over the pixels that were traced, where the reference is its own and not an
    // interpolation: `measure_reference`'s, at every traced pixel rather than a lattice of them.
    std::vector<Vec3> measured;
    std::vector<Vec3> reference;
    f64 noise = 0.0;
    for (u32 y = 0; y < cells.height; ++y) {
        for (u32 x = 0; x < cells.width; ++x) {
            const usize cell = (static_cast<usize>(y) * cells.width) + x;
            if (cells.depth[cell] <= 0.0F) {
                continue;
            }
            const usize pixel = (static_cast<usize>(std::min(y * out.step, height - 1U)) * width) +
                                std::min(x * out.step, width - 1U);
            measured.push_back(published[pixel]);
            reference.push_back(traced[cell]);
            noise += static_cast<f64>(length(half_apart[cell]));
        }
    }
    const gi::ReferenceComparison compared = gi::compare_against_reference(
        {measured.data(), measured.size()}, {reference.data(), reference.size()});
    out.traced = compared.samples;
    out.relative_error = compared.relative_error;
    out.reference_noise = compared.samples == 0 || compared.mean_reference_magnitude <= 0.0F
                              ? 0.0F
                              : static_cast<f32>(noise / static_cast<f64>(compared.samples)) /
                                    compared.mean_reference_magnitude;
    out.published = std::move(published);
    out.depth = std::move(depth);
    return ok();
}

}  // namespace cy::sample::beauty
