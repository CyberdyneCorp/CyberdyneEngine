#include "frame.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/gi/distance_field.h>
#include <cy/rendering/gi/system.h>
#include <cy/rendering/graph/executor.h>
#include <cy/rendering/graph/graph.h>
#include <cy/rendering/virtual_geometry/visbuffer.h>

#if defined(CY_SAMPLE_FIDELITY_VULKAN)
#    include <cy/backends/rhi/vulkan/vulkan_backend.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace cy::sample::fidelity {
namespace {

using rendering::CompileOptions;
using rendering::ExecuteOptions;
using rendering::ExecutionResult;
using rendering::GraphExecutor;
using rendering::RenderGraph;

constexpr f32 kFovY = 1.0471975512F;

void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
        std::fprintf(stderr, "vulkan validation error: %s\n", message != nullptr ? message : "");
    }
}

/// The device, or the honest reason there is not one. The null backend is registered too, so a
/// machine without Vulkan gets a device that works and a `backend()` that says what it is — which
/// is what lets the frame act report NOT EVALUATED rather than fail.
class DeviceHolder {
public:
    DeviceHolder() noexcept : allocator_(system_allocator(MemoryDomain::Gpu)) {
#if defined(CY_SAMPLE_FIDELITY_VULKAN)
        (void)rhi::vulkan::register_vulkan_backend();
#endif
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_sample_fidelity";
        description.enable_validation = true;
        description.enable_synchronisation_validation = true;
        description.request_async_compute = false;
        device_ = rhi::create_device(allocator_, "vulkan", description, selection_);
        if (device_.has_value()) {
            device_.value()->set_validation_callback(&count_validation, &errors_);
        }
    }

    ~DeviceHolder() {
        if (device_.has_value()) {
            (void)device_.value()->wait_idle();
            rhi::destroy_device(allocator_, device_.value());
        }
    }

    DeviceHolder(const DeviceHolder&) = delete;
    DeviceHolder& operator=(const DeviceHolder&) = delete;
    DeviceHolder(DeviceHolder&&) = delete;
    DeviceHolder& operator=(DeviceHolder&&) = delete;

    [[nodiscard]] bool real() const noexcept {
        return device_.has_value() &&
               device_.value()->capabilities().backend() == rhi::BackendKind::Vulkan;
    }
    [[nodiscard]] rhi::Device& device() const noexcept { return *device_.value(); }
    [[nodiscard]] Allocator& allocator() const noexcept { return allocator_; }
    [[nodiscard]] u32 errors() const noexcept { return errors_; }
    [[nodiscard]] const char* reason() const noexcept {
        return selection_.reason != nullptr ? selection_.reason : "no reason given";
    }

private:
    Allocator& allocator_;
    rhi::BackendSelection selection_{};
    u32 errors_ = 0;
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
};

/// The scene's payloads, concatenated, and where each asset's starts. `VisbufferPass` reads the
/// pages as bytes and needs both.
struct Payload {
    explicit Payload(Allocator& allocator) noexcept : bytes(allocator), offsets(allocator) {}
    Array<u8> bytes;
    Array<u32> offsets;
};

[[nodiscard]] Status gather_payload(const Scene& scene, Payload& out) noexcept {
    if (Status reserved = out.offsets.reserve(scene.decoded.size()); !reserved) {
        return reserved;
    }
    usize total = 0;
    for (const rendering::vg::DecodedAsset& asset : scene.decoded) {
        total += asset.payload.size();
    }
    if (Status sized = out.bytes.reserve(total); !sized) {
        return sized;
    }
    for (const rendering::vg::DecodedAsset& asset : scene.decoded) {
        if (Status added = out.offsets.push_back(static_cast<u32>(out.bytes.size())); !added) {
            return added;
        }
        if (Status appended = out.bytes.append(asset.payload); !appended) {
            return appended;
        }
    }
    return ok();
}

[[nodiscard]] f32 percentile(Array<f32>& sorted, f32 fraction) noexcept {
    if (sorted.empty()) {
        return 0.0F;
    }
    const auto last = static_cast<f32>(sorted.size() - 1U);
    const auto index = static_cast<usize>(std::lround(fraction * last));
    return sorted[index < sorted.size() ? index : sorted.size() - 1U];
}

}  // namespace

Status render_frames(const Scene& scene, const FrameOptions& options, FrameReport& out) noexcept {
    DeviceHolder holder;
    out.reason = holder.reason();
    if (!holder.real()) {
        out.backend = "null";
        return ok();
    }
    out.backend = "vulkan";
    Allocator& allocator = holder.allocator();

    rendering::vg::GpuScene gpu_scene(allocator);
    for (const rendering::vg::DecodedAsset& asset : scene.decoded) {
        Expected<u32, Error> added = gpu_scene.add_asset(asset);
        if (!added) {
            return Status{make_unexpected(added.error())};
        }
    }
    if (Status set = gpu_scene.set_instances(scene.instances.span()); !set) {
        return set;
    }

    // Sized for a film-detail set rather than for a test: the defaults are a sixteenth of what a
    // hundred instances of a thousand-cluster asset put through a 1280x720 view, and an overflow
    // here is a truncated visible list rather than an error.
    rendering::vg::GpuTraversalOptions traversal_options;
    traversal_options.queue_capacity = 1U << 19U;
    traversal_options.visible_capacity = 1U << 19U;
    traversal_options.request_capacity = 1U << 14U;

    rendering::vg::GpuTraversal traversal(allocator, holder.device());
    if (Status started = traversal.initialise(gpu_scene, traversal_options); !started) {
        return started;
    }

    // Every page resident. `virtual-geometry`'s residency is exercised by its own suite and by the
    // geometry cache; what this artefact is measuring is the traversal and the rasteriser, and a
    // page table that reported half the scene missing would measure the streamer instead.
    Array<rendering::vg::PageTableEntry> table(allocator);
    if (Status sized = table.resize(scene.pages); !sized) {
        return sized;
    }
    for (rendering::vg::PageTableEntry& entry : table) {
        entry.generation = 1;
        entry.flags = rendering::vg::PageFlags::kResident;
    }
    if (Status uploaded = traversal.upload_page_table(table.span()); !uploaded) {
        return uploaded;
    }

    Payload payload(allocator);
    if (Status gathered = gather_payload(scene, payload); !gathered) {
        return gathered;
    }

    Array<const rendering::vg::DecodedAsset*> asset_pointers(allocator);
    if (Status sized = asset_pointers.reserve(scene.decoded.size()); !sized) {
        return sized;
    }
    for (const rendering::vg::DecodedAsset& asset : scene.decoded) {
        if (Status added = asset_pointers.push_back(&asset); !added) {
            return added;
        }
    }

    rendering::vg::VisbufferOptions visbuffer_options;
    visbuffer_options.width = options.width;
    visbuffer_options.height = options.height;
    visbuffer_options.material_count = 8;
    rendering::vg::VisbufferPass visbuffer(allocator, holder.device());
    if (Status started =
            visbuffer.initialise(gpu_scene, asset_pointers.span(), payload.bytes.span(),
                                 payload.offsets.span(), visbuffer_options);
        !started) {
        return started;
    }

    GraphExecutor executor(allocator, holder.device());
    rendering::vg::TraversalReadback traversal_readback(allocator);
    rendering::vg::VisbufferReadback visbuffer_readback(allocator);
    if (Status sized = out.frame_ms.reserve(options.frames); !sized) {
        return sized;
    }

    const f32 aspect = static_cast<f32>(options.width) /
                       static_cast<f32>(options.height > 0 ? options.height : 1U);
    const auto span = static_cast<f32>(options.frames > 1U ? options.frames - 1U : 1U);
    u32 material_bits = 0;

    // The warm-up frames run through the identical path and are then discarded: the device has to
    // do the work to clock up, so skipping them would defeat the purpose. See `warmup_frames`.
    for (u32 frame = 0; frame < options.frames + options.warmup_frames; ++frame) {
        const bool timed = frame >= options.warmup_frames;
        // The shot parameter runs 0..1 over the TIMED frames. Deriving it from the raw loop index
        // would spend the warm-up on the interior half and leave the timed frames all exterior,
        // which is the shape the smoke test caught: `interior 0, exterior 17,016,037`.
        const f32 t = static_cast<f32>(timed ? frame - options.warmup_frames : 0U) / span;
        const Vec3 camera = camera_at(scene, t);
        const Mat4 view = look_at(camera, camera_target(scene, t), Vec3{0.0F, 1.0F, 0.0F});
        const Mat4 projection = perspective_reversed_z_infinite(kFovY, aspect, 0.05F);
        const Mat4 world_to_clip = projection * view;

        rendering::vg::TraversalView traversal_view;
        traversal_view.frustum = Frustum::from_view_projection(world_to_clip);
        traversal_view.frustum.refresh_corner_masks();
        traversal_view.projection.camera_position = camera;
        traversal_view.projection.viewport_height = static_cast<f32>(options.height);
        traversal_view.projection.fov_y_radians = kFovY;
        traversal_view.threshold_pixels = options.threshold_pixels;
        traversal_view.minimum_instance_pixels = 1.0F;
        traversal_view.cone_culling = true;

        RenderGraph graph(allocator);
        if (Status recorded =
                traversal.record(graph, traversal_view, static_cast<u32>(scene.instances.size()));
            !recorded) {
            return recorded;
        }
        if (Status recorded = visbuffer.record(graph, traversal, world_to_clip); !recorded) {
            return recorded;
        }
        if (Status built = graph.status(); !built) {
            return built;
        }

        const auto started = std::chrono::steady_clock::now();
        Expected<ExecutionResult, Error> executed =
            executor.execute(graph, CompileOptions{}, ExecuteOptions{});
        if (!executed) {
            return Status{make_unexpected(executed.error())};
        }
        if (Status idle = holder.device().wait_idle(); !idle) {
            return idle;
        }
        const f32 elapsed_ms =
            std::chrono::duration<f32, std::milli>(std::chrono::steady_clock::now() - started)
                .count();

        if (Status read = traversal.read_back(traversal_readback); !read) {
            return read;
        }
        if (Status read = visbuffer.read_back(visbuffer_readback); !read) {
            return read;
        }

        if (!timed) {
            continue;
        }
        if (Status added = out.frame_ms.push_back(elapsed_ms); !added) {
            return added;
        }
        const u32 covered = visbuffer_readback.covered_pixels();
        out.covered_pixels += covered;
        out.visible_clusters += traversal_readback.visible.size();
        out.overflowed = out.overflowed || traversal_readback.overflowed;
        out.levels_exhausted = out.levels_exhausted || traversal_readback.levels_exhausted;
        (t < 0.5F ? out.interior_covered : out.exterior_covered) += covered;
        for (u32 material = 0; material < visbuffer_options.material_count; ++material) {
            if (visbuffer_readback.bin_counts[material] > 0U) {
                material_bits |= 1U << material;
            }
        }
        ++out.frames;
    }

    out.materials_seen = static_cast<u32>(__builtin_popcount(material_bits));
    out.validation_errors = holder.errors();
    out.device = true;

    Expected<Array<f32>, Error> sorted = out.frame_ms.clone();
    if (!sorted) {
        return Status{make_unexpected(sorted.error())};
    }
    std::sort(sorted->begin(), sorted->end());
    out.median_ms = percentile(*sorted, 0.5F);
    out.p90_ms = percentile(*sorted, 0.9F);
    out.worst_ms = percentile(*sorted, 1.0F);
    return ok();
}

// --- The light ---------------------------------------------------------------------------------

namespace {

using rendering::gi::IlluminationSettings;
using rendering::gi::IlluminationSystem;

/// A box distance field for one asset, sampled onto a dense grid. `Shape::Hall` is the complement
/// of a solid — the distance a ray INSIDE the room has left to travel — because that is the shell
/// the camera stands within and a solid field there would report every interior point occluded.
struct ShapeField {
    explicit ShapeField(Allocator& allocator) noexcept : distances(allocator) {}

    u32 dimension = 13;
    Aabb bounds{};
    Array<f32> distances;

    [[nodiscard]] Status build(Vec3 half_extents, bool hollow) noexcept {
        const Vec3 extent = half_extents + Vec3{1.0F, 1.0F, 1.0F};
        bounds = Aabb::from_min_max(extent * -1.0F, extent);
        if (Status sized = distances.resize(static_cast<usize>(dimension) * dimension * dimension);
            !sized) {
            return sized;
        }
        const Vec3 size = bounds.size();
        const auto last = static_cast<f32>(dimension - 1U);
        for (u32 z = 0; z < dimension; ++z) {
            for (u32 y = 0; y < dimension; ++y) {
                for (u32 x = 0; x < dimension; ++x) {
                    const Vec3 point{
                        bounds.min.x + (size.x * static_cast<f32>(x) / last),
                        bounds.min.y + (size.y * static_cast<f32>(y) / last),
                        bounds.min.z + (size.z * static_cast<f32>(z) / last),
                    };
                    const f32 distance = rendering::gi::box_distance(point, half_extents);
                    distances[(((static_cast<usize>(z) * dimension) + y) * dimension) + x] =
                        hollow ? -distance : distance;
                }
            }
        }
        return ok();
    }

    [[nodiscard]] rendering::gi::AssetDistanceField asset() const noexcept {
        rendering::gi::AssetDistanceField field;
        field.dimension = dimension;
        field.bounds = bounds;
        field.distances = distances.span();
        return field;
    }
};

/// The convergence the shot is driven to before anything is resolved, and the cap it is allowed.
///
/// `advance_to_convergence` is what `rendering-global-illumination` prescribes for a capture —
/// "converged mode SHALL be used by golden-image tests, cinematic capture, and reference
/// comparison". `LightReport::converged` says whether the run reached the target or hit the cap,
/// which is the distinction that matters: a capture that stopped at the cap is a capture whose
/// figures are still moving, and reporting the metric alone would hide that.
///
/// MEASURED, on this scene: 0.85 is reached in ten frames and 0.90 is not reached in 320 (it
/// arrives at 0.879). The asymptote is the surface cache's, and 0.85 is the target this artefact
/// claims rather than the one that would read better.
constexpr f32 kConvergenceTarget = 0.85F;
constexpr u32 kConvergenceFrameCap = 320;

[[nodiscard]] f32 luminance(Vec3 colour) noexcept {
    return (0.2126F * colour.x) + (0.7152F * colour.y) + (0.0722F * colour.z);
}

[[nodiscard]] IlluminationSettings shot_settings() noexcept {
    IlluminationSettings settings;
    // Two clipmap levels: the interior is metres across and the district is hundreds, which is the
    // case a clipmap exists for and the reason this is not one level.
    settings.field.levels = 2;
    settings.field.resolution = 48;
    settings.field.base_extent_metres = 32.0F;
    settings.probes.levels = 2;
    settings.probes.base_spacing_metres = 2.5F;
    settings.probes.half_extent_probes = 3;
    settings.max_ray_distance_metres = 48.0F;
    settings.rays_per_probe = 24;
    settings.convergence_region_metres = 6.0F;
    return settings;
}

}  // namespace

Status light_shot(const Scene& scene, const FrameOptions& options, LightReport& out) noexcept {
    Allocator& allocator = scene.allocator;
    IlluminationSystem system;
    if (Status configured = system.configure(shot_settings()); !configured) {
        return configured;
    }

    // One field per SHAPE, placed once per instance: the fields are the shells' own, which is what
    // a cook produces, and placing them is what makes a wall in the district occlude a ray in it.
    Array<ShapeField> fields(allocator);
    if (Status sized = fields.reserve(scene.decoded.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < scene.decoded.size(); ++index) {
        ShapeField field(allocator);
        if (Status built = field.build(scene.decoded[index].bounds.half_extents(),
                                       scene.assets[index].shape == Shape::Hall);
            !built) {
            return built;
        }
        if (Status stored = fields.push_back(std::move(field)); !stored) {
            return stored;
        }
    }
    u64 placement = 1;
    for (const rendering::vg::GeometryInstance& instance : scene.instances) {
        if (Status placed = system.field().place(placement++, fields[instance.asset].asset(),
                                                 Mat4::from_translation(instance.translation));
            !placed) {
            return placed;
        }
    }

    // The surfaces, as one cell per shape so that an eviction is a thing the scene can express.
    for (u32 shape = 0; shape < static_cast<u32>(Shape::Count); ++shape) {
        Array<rendering::gi::Surfel> cell(allocator);
        Aabb bounds = Aabb::empty();
        for (const rendering::gi::Surfel& surfel : scene.surfels) {
            if (surfel.material_id != shape) {
                continue;
            }
            if (Status added = cell.push_back(surfel); !added) {
                return added;
            }
            bounds = merge(bounds, Aabb::from_point(surfel.position));
        }
        if (cell.empty()) {
            continue;
        }
        if (Status ingested =
                system.scene().ingest_cell(shape + 1U, bounds.expanded(2.0F), cell.span(), 0);
            !ingested) {
            return ingested;
        }
    }

    rendering::gi::FrameContext context;
    context.camera = camera_at(scene, 0.25F);
    context.lights = scene.lights.span();
    const auto started = std::chrono::steady_clock::now();
    out.frames_to_converge =
        system.advance_to_convergence(context, kConvergenceTarget, kConvergenceFrameCap);
    out.gi_ms =
        std::chrono::duration<f32, std::milli>(std::chrono::steady_clock::now() - started).count();

    rendering::gi::IlluminationFrameReport report = system.update(context);
    out.tier = rendering::gi::radiance_source_name(report.world_tier);
    out.convergence = report.convergence;
    out.converged = out.frames_to_converge < kConvergenceFrameCap;
    out.surfels = static_cast<u32>(scene.surfels.size());
    out.lights = static_cast<u32>(scene.lights.size());

    // A stratified walk of the scene's own surfaces: every `stride`-th card, lifted off the surface
    // along its normal so the query is in the room rather than in the wall.
    const u32 wanted = options.shaded_samples > 0U ? options.shaded_samples : 1U;
    const usize stride =
        scene.surfels.size() > wanted ? scene.surfels.size() / wanted : static_cast<usize>(1);
    Vec3 lowest{1.0e30F, 1.0e30F, 1.0e30F};
    Vec3 highest{-1.0e30F, -1.0e30F, -1.0e30F};
    f32 indirect_total = 0.0F;
    f32 reflection_total = 0.0F;
    for (usize index = 0; index < scene.surfels.size(); index += stride) {
        const rendering::gi::Surfel& surfel = scene.surfels[index];
        const Vec3 point = surfel.position + (surfel.normal * 0.15F);
        rendering::gi::SurfaceProperties surface;
        const rendering::gi::ResolveResult diffuse =
            system.indirect_diffuse(point, surfel.normal, surface);
        rendering::gi::TraceBudget budget;
        const Vec3 view_direction = normalize(context.camera - point);
        const rendering::gi::ResolveResult specular = system.indirect_specular(
            point, surfel.normal, view_direction, surfel.roughness, 1, budget);

        ++out.shaded;
        const f32 diffuse_luminance = luminance(diffuse.radiance);
        const f32 specular_luminance = luminance(specular.radiance);
        indirect_total += diffuse_luminance;
        reflection_total += specular_luminance;
        out.with_indirect += diffuse_luminance > 0.0F ? 1U : 0U;
        out.with_reflection += specular_luminance > 0.0F ? 1U : 0U;
        if (diffuse_luminance > 0.0F) {
            lowest =
                Vec3{std::min(lowest.x, diffuse.radiance.x), std::min(lowest.y, diffuse.radiance.y),
                     std::min(lowest.z, diffuse.radiance.z)};
            highest = Vec3{std::max(highest.x, diffuse.radiance.x),
                           std::max(highest.y, diffuse.radiance.y),
                           std::max(highest.z, diffuse.radiance.z)};
        }
    }
    const rendering::gi::TracingDiagnostics diagnostics = system.tracer().diagnostics();
    out.software_rays = diagnostics.software_rays;
    out.hardware_rays = diagnostics.hardware_rays;
    if (out.shaded > 0U) {
        out.mean_indirect = indirect_total / static_cast<f32>(out.shaded);
        out.mean_reflection = reflection_total / static_cast<f32>(out.shaded);
    }
    if (out.with_indirect > 0U) {
        const Vec3 spread = highest - lowest;
        out.indirect_colour_spread = std::max({spread.x, spread.y, spread.z});
    }
    return ok();
}

}  // namespace cy::sample::fidelity
