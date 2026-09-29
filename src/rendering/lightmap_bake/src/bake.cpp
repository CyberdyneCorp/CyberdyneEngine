// SPDX-License-Identifier: MIT
// The lightmap bake. See bake.h for the pipeline and the units.

#include "internal.h"

#include <cy/rendering/lightmap_bake/asset.h>
#include <cy/rendering/lightmap_bake/bake.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cy::rendering::lightmap_bake {
namespace {

using detail::Canvas;
using detail::TexelState;
using detail::TexelSurface;

/// A distinct, reproducible sequence per texel from the bake's seed.
[[nodiscard]] u32 texel_sequence(u32 seed, usize texel) noexcept {
    u32 hash = seed ^ (static_cast<u32>(texel) * 0x9E3779B9U);
    hash ^= hash >> 16U;
    hash *= 0x7FEB352DU;
    hash ^= hash >> 15U;
    hash *= 0x846CA68BU;
    hash ^= hash >> 16U;
    return hash;
}

[[nodiscard]] f32 triangle_area(const BakeMesh& mesh, const Mat4& transform, usize at) noexcept {
    const Vec3 a = transform_point(transform, mesh.positions[mesh.indices[at]]);
    const Vec3 b = transform_point(transform, mesh.positions[mesh.indices[at + 1U]]);
    const Vec3 c = transform_point(transform, mesh.positions[mesh.indices[at + 2U]]);
    return 0.5F * length(cross(b - a, c - a));
}

[[nodiscard]] Status validate(const LightmapScene& scene) noexcept {
    for (const BakeInstance& instance : scene.instances) {
        if (instance.mesh >= scene.meshes.size() || instance.material >= scene.materials.size()) {
            return fail(ErrorCode::InvalidArgument,
                        "a lightmap instance names a mesh or a material the scene does not have");
        }
        const BakeMesh& mesh = scene.meshes[instance.mesh];
        for (const u32 index : mesh.indices) {
            if (index >= mesh.positions.size()) {
                return fail(ErrorCode::InvalidArgument,
                            "a lightmap mesh index is outside its vertex array");
            }
        }
        if (instance.receives_lightmap && mesh.uv2.size() < mesh.positions.size()) {
            return fail(ErrorCode::InvalidArgument,
                        "a mesh that receives a lightmap has no UV2 channel; import it with "
                        "generate-lightmap-uvs");
        }
    }
    return ok();
}

/// Pack every receiving instance, and fill the lightmap's layout fields and addresses.
[[nodiscard]] Status pack(const LightmapScene& scene, const LightmapBakeSettings& settings,
                          BakedLightmap& out, LightmapBakeReport& report) noexcept {
    Array<AtlasObject> objects;
    Array<u32> owners;
    for (u32 index = 0; index < scene.instances.size(); ++index) {
        const BakeInstance& instance = scene.instances[index];
        if (!instance.receives_lightmap) {
            continue;
        }
        const BakeMesh& mesh = scene.meshes[instance.mesh];
        AtlasObject object;
        object.surface_area = 0.0F;
        for (usize at = 0; at + 2U < mesh.indices.size(); at += 3U) {
            object.surface_area += triangle_area(mesh, instance.transform, at);
        }
        object.uv_coverage = mesh.uv_coverage;
        object.aspect = mesh.uv_aspect;
        object.resolution_scale = instance.resolution_scale;
        if (Status pushed = objects.push_back(object); !pushed) {
            return pushed;
        }
        if (Status pushed = owners.push_back(index); !pushed) {
            return pushed;
        }
    }
    AtlasLayout layout;
    if (Status packed = pack_atlas(objects.span(), settings.atlas, layout); !packed) {
        return packed;
    }
    out.mode = settings.mode;
    out.page_size = settings.atlas.page_size;
    out.pages = std::max(layout.pages, 1U);
    out.gutter_texels = layout.gutter_texels;
    out.mip_levels = layout.mip_levels;
    if (Status sized = out.addresses.resize(scene.instances.size()); !sized) {
        return sized;
    }
    for (u32& address : out.addresses) {
        address = kNoLightmapAddress;
    }
    for (usize object = 0; object < owners.size(); ++object) {
        out.addresses[owners[object]] = encode_address(layout.placements[object]);
    }
    report.objects = static_cast<u32>(owners.size());
    report.pages = out.pages;
    report.atlas_occupancy = layout.occupancy;
    // Two texels of the coarsest protected mip level between two charts: one on each side, so a
    // bilinear tap at either chart's edge, at any of those levels, reads only its own chart.
    report.required_chart_gap = 2U << layout.mip_levels;
    return ok();
}

/// The lights the path tracer is built with: the caller's, divided by pi (see bake.h), and without
/// the `Movable` ones, whose bounce the dynamic GI owns.
[[nodiscard]] Status frame_lights(Span<const gi::GiLight> lights,
                                  Array<gi::GiLight>& out) noexcept {
    out.clear();
    for (gi::GiLight light : lights) {
        if (light.mobility == gi::LightMobility::Movable) {
            continue;
        }
        light.intensity /= std::numbers::pi_v<f32>;
        if (Status pushed = out.push_back(light); !pushed) {
            return pushed;
        }
    }
    return ok();
}

/// The lights that get a shadow-mask channel: every `Stationary` one, unless the content bakes
/// their direct term anyway. More than the plane has channels is refused by name.
[[nodiscard]] Status stationary_lights(Span<const gi::GiLight> lights,
                                       const LightmapBakeSettings& settings,
                                       Array<gi::GiLight>& out) noexcept {
    out.clear();
    if (settings.content == LightmapContent::DirectAndIndirect) {
        return ok();
    }
    for (const gi::GiLight& light : lights) {
        if (light.mobility != gi::LightMobility::Stationary) {
            continue;
        }
        if (out.size() == kMaxShadowMaskLights) {
            return fail(ErrorCode::OutOfRange,
                        "a level has more stationary lights than the shadow mask has channels "
                        "(four); make the rest Static or Movable");
        }
        if (Status pushed = out.push_back(light); !pushed) {
            return pushed;
        }
    }
    return ok();
}

/// The path tracer and everything it resolves through, built once per bake or reference.
struct TraceWorld {
    MeshSceneTracer tracer;
    gi::GiScene surfels;
    Array<gi::GiLight> lights;
    /// The shadow mask's lights, in channel order, as the caller gave them.
    Array<gi::GiLight> stationary;

    [[nodiscard]] Status build(const LightmapScene& scene,
                               const LightmapBakeSettings& settings) noexcept {
        if (Status built = tracer.build(scene); !built) {
            return built;
        }
        if (Status built = detail::build_surfels(scene, tracer, settings.surfel_spacing, surfels);
            !built) {
            return built;
        }
        if (Status framed = frame_lights(scene.lights, lights); !framed) {
            return framed;
        }
        return stationary_lights(scene.lights, settings, stationary);
    }
};

/// Report a step, and say whether the bake may go on. Always true without a `progress`.
[[nodiscard]] bool checkpoint(const LightmapBakeProgress* progress, LightmapBakeStage stage,
                              u32 done, u32 total) noexcept {
    if (progress == nullptr) {
        return true;
    }
    if (progress->cancelled()) {
        return false;
    }
    progress->step(stage, done, total);
    return true;
}

[[nodiscard]] Status cancelled(LightmapBakeReport& report) noexcept {
    report.cancelled = true;
    return fail(ErrorCode::Unavailable, "the lightmap bake was cancelled");
}

/// Trace every surface texel. False when the bake was cancelled part way.
[[nodiscard]] bool trace_canvas(const detail::TraceContext& context,
                                const LightmapBakeSettings& settings, Canvas& canvas,
                                LightmapBakeReport& report,
                                const LightmapBakeProgress* progress) noexcept {
    const bool masked = !canvas.shadow.empty();
    const auto total = static_cast<u32>(canvas.surfaces.size());
    for (usize index = 0; index < canvas.surfaces.size(); ++index) {
        if (index % kProgressTexels == 0U &&
            !checkpoint(progress, LightmapBakeStage::Trace, static_cast<u32>(index), total)) {
            return false;
        }
        TexelSurface& texel = canvas.surfaces[index];
        if (texel.state != TexelState::Surface) {
            continue;
        }
        const u32 sequence = texel_sequence(settings.trace.seed, index);
        if (detail::buried(context, texel.position, texel.normal, sequence ^ 0xB5297A4DU)) {
            texel.state = TexelState::Buried;
            report.texels_buried += 1U;
            continue;
        }
        canvas.moments[index] = detail::trace_moments(context, texel.position, texel.normal,
                                                      sequence, settings.trace.samples);
        if (masked) {
            canvas.shadow[index] = detail::trace_shadow_mask(context, texel.position, texel.normal,
                                                             sequence ^ 0x2C1B3C6DU);
        }
        report.texels_covered += 1U;
    }
    return checkpoint(progress, LightmapBakeStage::Trace, total, total);
}

/// Every surface texel of an owner not in `active` becomes empty for the post-process: the
/// denoiser neither filters nor borrows from it, and the dilation, bounded by owner, cannot either.
/// A rebake copies those texels from the previous bake afterwards.
void keep_only(Canvas& canvas, const Array<u8>& active) noexcept {
    for (TexelSurface& texel : canvas.surfaces) {
        if (texel.owner != detail::kNoOwner && active[texel.owner] == 0U) {
            texel.state = TexelState::Empty;
        }
    }
}

/// The owner of the texel under an atlas coordinate.
[[nodiscard]] u32 owner_at(const Canvas& canvas, Vec2 coordinate) noexcept {
    const auto x =
        static_cast<u32>(std::clamp(coordinate.x, 0.0F, static_cast<f32>(canvas.width - 1U)));
    const auto y =
        static_cast<u32>(std::clamp(coordinate.y, 0.0F, static_cast<f32>(canvas.height - 1U)));
    return canvas.surfaces[canvas.index(x, y)].owner;
}

/// The seams both of whose sides are re-solved; the rest are counted and left as they were.
[[nodiscard]] Status seams_inside(const Canvas& canvas, const Array<u8>& active,
                                  const Array<detail::SeamEdge>& seams,
                                  Array<detail::SeamEdge>& out, u32& boundary) noexcept {
    out.clear();
    boundary = 0;
    for (const detail::SeamEdge& seam : seams) {
        const u32 a = owner_at(canvas, (seam.a0 + seam.a1) * 0.5F);
        const u32 b = owner_at(canvas, (seam.b0 + seam.b1) * 0.5F);
        const bool a_in = a != detail::kNoOwner && active[a] != 0U;
        const bool b_in = b != detail::kNoOwner && active[b] != 0U;
        if (a_in && b_in) {
            if (Status pushed = out.push_back(seam); !pushed) {
                return pushed;
            }
        } else if (a_in || b_in) {
            boundary += 1U;
        }
    }
    return ok();
}

/// Passes until every texel that can be filled is: the dilation stops on the first pass that fills
/// nothing, so the bound only has to be one no rectangle can need more than — its longest side. A
/// fixed count (four gutters) left the strip of a floor under a wall standing on it black in its
/// middle, and that black bled into the floor visible beside the wall.
[[nodiscard]] u32 dilation_passes(const LightmapBakeSettings& settings,
                                  const BakedLightmap& lightmap) noexcept {
    if (settings.dilation_passes != 0) {
        return settings.dilation_passes;
    }
    return lightmap.page_size;
}

[[nodiscard]] Status seed(const gi::PathTracer& path, const TraceWorld& world,
                          const LightmapScene& scene, const CacheSeedTargets& seeds,
                          LightmapBakeReport& report) noexcept {
    if (seeds.surfaces == nullptr || seeds.radiance == nullptr) {
        return ok();
    }
    Expected<gi::BakeReport, Error> seeded =
        gi::seed_caches(path, *seeds.surfaces, *seeds.radiance, seeds.reflections, scene.lights,
                        &world.tracer, seeds.settings);
    if (!seeded.has_value()) {
        return make_unexpected(seeded.error());
    }
    report.seeds = seeded.value();
    return ok();
}

}  // namespace

const char* lightmap_mode_name(LightmapMode mode) noexcept {
    switch (mode) {
        case LightmapMode::Irradiance:
            return "irradiance";
        case LightmapMode::Directional:
            return "directional";
        case LightmapMode::ShL1:
            return "sh-l1";
        case LightmapMode::Count:
            break;
    }
    return "unknown";
}

const char* lightmap_bake_stage_name(LightmapBakeStage stage) noexcept {
    switch (stage) {
        case LightmapBakeStage::Prepare:
            return "prepare";
        case LightmapBakeStage::Trace:
            return "trace";
        case LightmapBakeStage::Filter:
            return "filter";
        case LightmapBakeStage::Finish:
            return "finish";
        case LightmapBakeStage::Count:
            break;
    }
    return "unknown";
}

u32 lightmap_planes(LightmapMode mode) noexcept {
    switch (mode) {
        case LightmapMode::Directional:
            return 2;
        case LightmapMode::ShL1:
            return 3;
        case LightmapMode::Irradiance:
        case LightmapMode::Count:
            break;
    }
    return 1;
}

namespace {

/// One run's working state: the tracer and cards, the canvas, and its seams.
struct BakeRun {
    TraceWorld world;
    Canvas canvas;
    Array<detail::SeamEdge> seams;
    const LightmapBakeProgress* progress = nullptr;
};

/// Everything after the packing and before the tracing: the tracer, the raster, the shadow mask's
/// canvas and the chart-padding check.
[[nodiscard]] Status prepare(const LightmapScene& scene, const LightmapBakeSettings& settings,
                             const BakedLightmap& layout, BakeRun& run,
                             LightmapBakeReport& report) noexcept {
    if (Status built = run.world.build(scene, settings); !built) {
        return built;
    }
    if (Status drawn = detail::rasterise(scene, layout, run.canvas, run.seams); !drawn) {
        return drawn;
    }
    if (!run.world.stationary.empty()) {
        if (Status sized = run.canvas.shadow.resize(run.canvas.surfaces.size()); !sized) {
            return sized;
        }
        for (Vec4& texel : run.canvas.shadow) {
            texel = Vec4{1.0F, 1.0F, 1.0F, 1.0F};
        }
    }
    if (Status measured =
            detail::short_padding(run.canvas, report.required_chart_gap, report.padding_short);
        !measured) {
        return measured;
    }
    if (settings.refuse_short_padding && !report.padding_short.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "an object's charts land closer together in its lightmap rectangle than "
                    "bilinear filtering and the protected mip levels need (report.padding_short "
                    "names them); unwrap with more padding or bake at a higher density");
    }
    return ok();
}

/// Trace, denoise, dilate and stitch the canvas over `seams`.
[[nodiscard]] Status solve(const LightmapScene& scene, const LightmapBakeSettings& settings,
                           BakeRun& run, Span<const detail::SeamEdge> seams,
                           const BakedLightmap& layout, LightmapBakeReport& report) noexcept {
    const gi::PathTracer path(run.world.tracer, run.world.surfels, run.world.lights.span(),
                              scene.sky, &run.world.tracer);
    detail::TraceContext context;
    context.path = &path;
    context.tracer = &run.world.tracer;
    context.lights = run.world.lights.span();
    context.stationary = run.world.stationary.span();
    context.settings = &settings;
    if (!trace_canvas(context, settings, run.canvas, report, run.progress) ||
        !checkpoint(run.progress, LightmapBakeStage::Filter, 0, 1)) {
        return cancelled(report);
    }
    if (settings.denoise) {
        if (Status denoised =
                detail::denoise_moments(run.canvas, settings.mode, settings.denoise_passes);
            !denoised) {
            return denoised;
        }
    }
    report.texels_dilated = detail::dilate(run.canvas, dilation_passes(settings, layout));
    report.seam_edges = static_cast<u32>(seams.size());
    report.seam_samples = detail::reconcile_seams(
        run.canvas, seams, settings.seam_iterations, settings.reconcile_seams,
        report.seam_error_before, report.seam_error_after);
    return checkpoint(run.progress, LightmapBakeStage::Filter, 1, 1) ? ok() : cancelled(report);
}

[[nodiscard]] f32 half_rounded(f32 value) noexcept {
    return float_from_half(half_from_float(value));
}

/// The canvas into `out`'s planes, coverage and shadow mask.
[[nodiscard]] Status encode(const BakeRun& run, const LightmapBakeSettings& settings,
                            BakedLightmap& out) noexcept {
    const Canvas& canvas = run.canvas;
    out.texels.width = canvas.width;
    out.texels.height = canvas.height;
    out.texels.planes = lightmap_planes(settings.mode);
    if (Status sized =
            out.texels.texels.resize(usize{out.texels.planes} * canvas.width * canvas.height);
        !sized) {
        return sized;
    }
    detail::encode_planes(canvas, settings.mode, out.texels);
    if (Status sized = out.coverage.resize(canvas.surfaces.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < canvas.surfaces.size(); ++index) {
        out.coverage[index] = canvas.surfaces[index].state == TexelState::Surface ? 1U : 0U;
    }
    out.shadow_mask = LightmapTexels();
    out.shadow_lights.clear();
    if (canvas.shadow.empty()) {
        return ok();
    }
    out.shadow_mask.width = canvas.width;
    out.shadow_mask.height = canvas.height;
    out.shadow_mask.planes = 1;
    if (Status sized = out.shadow_mask.texels.resize(canvas.shadow.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < canvas.shadow.size(); ++index) {
        const Vec4 value = canvas.shadow[index];
        out.shadow_mask.texels[index] = Vec4{half_rounded(value.x), half_rounded(value.y),
                                             half_rounded(value.z), half_rounded(value.w)};
    }
    for (const gi::GiLight& light : run.world.stationary) {
        if (Status pushed = out.shadow_lights.push_back(light.id); !pushed) {
            return pushed;
        }
    }
    return ok();
}

/// What the frame needs besides the texels: which lights' direct term is baked, and the mip chain
/// over the charts the rasteriser drew.
[[nodiscard]] Status finish(const BakeRun& run, const LightmapScene& scene,
                            const LightmapBakeSettings& settings, BakedLightmap& out) noexcept {
    out.direct_lights.clear();
    for (const gi::GiLight& light : scene.lights) {
        if (detail::bakes_direct(light, settings.content)) {
            if (Status pushed = out.direct_lights.push_back(light.id); !pushed) {
                return pushed;
            }
        }
    }
    const Canvas& canvas = run.canvas;
    Array<u32> charts;
    if (Status sized = charts.resize(canvas.surfaces.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < charts.size(); ++index) {
        charts[index] = out.coverage[index] != 0U ? canvas.surfaces[index].chart : kNoChart;
    }
    return build_lightmap_mips(out, charts.span());
}

[[nodiscard]] Status seed_from(const BakeRun& run, const LightmapScene& scene,
                               const CacheSeedTargets* seeds, LightmapBakeReport& report) noexcept {
    // THE SEEDS, FROM THIS RUN'S TRACER AND CARDS. With the caller's own lights rather than the
    // frame's — every light, movable ones too: the dynamic caches keep the surface cache's
    // convention (see bake.h) and own a movable light's GI.
    if (seeds == nullptr) {
        return ok();
    }
    const gi::PathTracer seeding(run.world.tracer, run.world.surfels, scene.lights, scene.sky,
                                 &run.world.tracer);
    return seed(seeding, run.world, scene, *seeds, report);
}

}  // namespace

Status bake_lightmaps(const LightmapScene& scene, const LightmapBakeSettings& settings,
                      const CacheSeedTargets* seeds, BakedLightmap& out, LightmapBakeReport& report,
                      const LightmapBakeProgress* progress) noexcept {
    report = LightmapBakeReport();
    if (!checkpoint(progress, LightmapBakeStage::Prepare, 0, 1)) {
        return cancelled(report);
    }
    if (settings.mode >= LightmapMode::Count) {
        return fail(ErrorCode::InvalidArgument, "an unknown lightmap mode");
    }
    if (Status valid = validate(scene); !valid) {
        return valid;
    }
    if (Status packed = pack(scene, settings, out, report); !packed) {
        return packed;
    }
    BakeRun run;
    run.progress = progress;
    if (Status prepared = prepare(scene, settings, out, run, report); !prepared) {
        return prepared;
    }
    if (Status solved = solve(scene, settings, run, run.seams.span(), out, report); !solved) {
        return solved;
    }
    if (!checkpoint(progress, LightmapBakeStage::Finish, 0, 1)) {
        return cancelled(report);
    }
    if (Status encoded = encode(run, settings, out); !encoded) {
        return encoded;
    }
    if (Status finished = finish(run, scene, settings, out); !finished) {
        return finished;
    }
    if (Status seeded = seed_from(run, scene, seeds, report); !seeded) {
        return seeded;
    }
    report.rays = run.world.tracer.rays();
    (void)checkpoint(progress, LightmapBakeStage::Finish, 1, 1);
    return ok();
}

namespace {

// --- Incremental rebake -------------------------------------------------------------------------

/// Why `previous` cannot be reused, or null when it can.
[[nodiscard]] const char* layout_change(const BakedLightmap& previous, const BakedLightmap& now,
                                        const LightmapBakeSettings& settings,
                                        const TraceWorld& world) noexcept {
    if (previous.mode != settings.mode || previous.page_size != now.page_size ||
        previous.pages != now.pages || previous.gutter_texels != now.gutter_texels) {
        return "the mode or the page layout changed";
    }
    if (previous.addresses.size() != now.addresses.size() ||
        !std::equal(previous.addresses.begin(), previous.addresses.end(), now.addresses.begin())) {
        return "the level no longer packs to the same rectangles";
    }
    const bool same_lights =
        previous.shadow_lights.size() == world.stationary.size() &&
        std::equal(world.stationary.begin(), world.stationary.end(), previous.shadow_lights.begin(),
                   [](const gi::GiLight& light, u64 id) { return light.id == id; });
    if (!same_lights) {
        return "the stationary lights changed";
    }
    const usize texels = usize{now.page_size} * now.page_size * now.pages;
    if (previous.texels.texels.size() != texels * lightmap_planes(settings.mode)) {
        return "the previous bake's texels do not match its layout";
    }
    return nullptr;
}

[[nodiscard]] Aabb instance_bounds(const LightmapScene& scene,
                                   const BakeInstance& instance) noexcept {
    Aabb box = Aabb::empty();
    for (const Vec3 position : scene.meshes[instance.mesh].positions) {
        const Vec3 world = transform_point(instance.transform, position);
        box = Aabb::from_min_max(cwise_min(box.min, world), cwise_max(box.max, world));
    }
    return box;
}

[[nodiscard]] Aabb grown(Aabb box, f32 metres) noexcept {
    const Vec3 margin{metres, metres, metres};
    return Aabb::from_min_max(box.min - margin, box.max + margin);
}

[[nodiscard]] bool inside(const Aabb& box, Vec3 point) noexcept {
    return point.x >= box.min.x && point.y >= box.min.y && point.z >= box.min.z &&
           point.x <= box.max.x && point.y <= box.max.y && point.z <= box.max.z;
}

/// The owners a move reaches: every moved one, and every one with a surface texel inside a moved
/// object's old or new bounds grown by the influence distance.
[[nodiscard]] Status rebake_region(const LightmapScene& scene, const Canvas& canvas,
                                   const LightmapRebakeRequest& request,
                                   Array<u8>& active) noexcept {
    if (Status sized = active.resize(scene.instances.size()); !sized) {
        return sized;
    }
    std::ranges::fill(active, u8{0});
    Array<Aabb> dirty;
    for (usize at = 0; at < request.moved_instances.size(); ++at) {
        const u32 moved = request.moved_instances[at];
        active[moved] = 1U;
        const Aabb now = instance_bounds(scene, scene.instances[moved]);
        for (const Aabb box : {now, request.previous_bounds[at]}) {
            if (Status pushed = dirty.push_back(grown(box, request.influence_metres)); !pushed) {
                return pushed;
            }
        }
    }
    for (const TexelSurface& texel : canvas.surfaces) {
        if (texel.state != TexelState::Surface || texel.owner == detail::kNoOwner ||
            active[texel.owner] != 0U) {
            continue;
        }
        if (std::ranges::any_of(dirty,
                                [&](const Aabb& box) { return inside(box, texel.position); })) {
            active[texel.owner] = 1U;
        }
    }
    return ok();
}

/// Every texel whose owner was not re-solved, byte for byte from `previous`: planes, coverage and
/// shadow mask. Coverage comes from the raster instead when `previous` carries none — a decoded
/// asset does not — because a kept object rasterises to the same texels either way.
void copy_kept(const BakedLightmap& previous, const Canvas& canvas, const Array<u8>& active,
               BakedLightmap& out) noexcept {
    const usize texels = canvas.surfaces.size();
    const bool has_coverage = previous.coverage.size() == texels;
    const bool has_mask =
        previous.shadow_mask.texels.size() == texels && out.shadow_mask.texels.size() == texels;
    for (usize index = 0; index < texels; ++index) {
        const u32 owner = canvas.surfaces[index].owner;
        if (owner != detail::kNoOwner && active[owner] != 0U) {
            continue;
        }
        for (u32 plane = 0; plane < out.texels.planes; ++plane) {
            const usize at = (usize{plane} * texels) + index;
            out.texels.texels[at] = previous.texels.texels[at];
        }
        if (has_coverage) {
            out.coverage[index] = previous.coverage[index];
        }
        if (has_mask) {
            out.shadow_mask.texels[index] = previous.shadow_mask.texels[index];
        }
    }
}

[[nodiscard]] Status check_request(const LightmapScene& scene,
                                   const LightmapRebakeRequest& request) noexcept {
    if (request.moved_instances.size() != request.previous_bounds.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "a rebake needs one previous bound per moved instance");
    }
    for (const u32 moved : request.moved_instances) {
        if (moved >= scene.instances.size()) {
            return fail(ErrorCode::InvalidArgument,
                        "a rebake names a moved instance the scene does not have");
        }
    }
    return ok();
}

/// The rebake proper, once the level is known to pack as `previous` did.
[[nodiscard]] Status rebake_region_of(const LightmapScene& scene,
                                      const LightmapBakeSettings& settings,
                                      const BakedLightmap& previous,
                                      const LightmapRebakeRequest& request, BakeRun& run,
                                      BakedLightmap& out, LightmapBakeReport& report) noexcept {
    Array<u8> active;
    if (Status chosen = rebake_region(scene, run.canvas, request, active); !chosen) {
        return chosen;
    }
    report.objects_rebaked = static_cast<u32>(std::count(active.begin(), active.end(), u8{1}));
    keep_only(run.canvas, active);
    Array<detail::SeamEdge> seams;
    if (Status kept = seams_inside(run.canvas, active, run.seams, seams, report.boundary_seams);
        !kept) {
        return kept;
    }
    if (Status solved = solve(scene, settings, run, seams.span(), out, report); !solved) {
        return solved;
    }
    if (Status encoded = encode(run, settings, out); !encoded) {
        return encoded;
    }
    copy_kept(previous, run.canvas, active, out);
    if (Status finished = finish(run, scene, settings, out); !finished) {
        return finished;
    }
    report.incremental = true;
    report.rays = run.world.tracer.rays();
    return ok();
}

}  // namespace

Status rebake_lightmaps(const LightmapScene& scene, const LightmapBakeSettings& settings,
                        const BakedLightmap& previous, const LightmapRebakeRequest& request,
                        BakedLightmap& out, LightmapBakeReport& report,
                        const LightmapBakeProgress* progress) noexcept {
    report = LightmapBakeReport();
    if (settings.mode >= LightmapMode::Count) {
        return fail(ErrorCode::InvalidArgument, "an unknown lightmap mode");
    }
    if (Status valid = validate(scene); !valid) {
        return valid;
    }
    if (Status checked = check_request(scene, request); !checked) {
        return checked;
    }
    if (Status packed = pack(scene, settings, out, report); !packed) {
        return packed;
    }
    BakeRun run;
    run.progress = progress;
    if (Status prepared = prepare(scene, settings, out, run, report); !prepared) {
        return prepared;
    }
    if (const char* reason = layout_change(previous, out, settings, run.world); reason != nullptr) {
        if (Status baked = bake_lightmaps(scene, settings, nullptr, out, report, progress);
            !baked) {
            return baked;
        }
        report.fallback = reason;
        return ok();
    }
    return rebake_region_of(scene, settings, previous, request, run, out, report);
}

f32 sample_shadow_mask(const BakedLightmap& lightmap, u64 light_id, Vec2 coordinate) noexcept {
    for (usize channel = 0; channel < lightmap.shadow_lights.size(); ++channel) {
        if (lightmap.shadow_lights[channel] != light_id) {
            continue;
        }
        const Vec4 mask = sample_plane(lightmap.shadow_mask, 0, coordinate);
        const f32 values[4] = {mask.x, mask.y, mask.z, mask.w};
        return channel < 4U ? values[channel] : 1.0F;
    }
    return 1.0F;
}

Status reference_ambient(const LightmapScene& scene, const LightmapBakeSettings& settings,
                         Span<const Vec3> positions, Span<const Vec3> normals,
                         Array<Vec3>& out) noexcept {
    if (positions.size() != normals.size()) {
        return fail(ErrorCode::InvalidArgument, "a reference needs one normal per position");
    }
    TraceWorld world;
    if (Status built = world.build(scene, settings); !built) {
        return built;
    }
    const gi::PathTracer path(world.tracer, world.surfels, world.lights.span(), scene.sky,
                              &world.tracer);
    detail::TraceContext context;
    context.path = &path;
    context.tracer = &world.tracer;
    context.lights = world.lights.span();
    context.settings = &settings;
    out.clear();
    for (usize index = 0; index < positions.size(); ++index) {
        // A sequence the bake never uses, so the reference is an independent draw.
        const u32 sequence = texel_sequence(settings.trace.seed ^ 0x51ED270BU, index);
        const detail::TexelMoments moments = detail::trace_moments(
            context, positions[index], normalize(normals[index]), sequence, settings.trace.samples);
        if (Status pushed = out.push_back(moments.mean); !pushed) {
            return pushed;
        }
    }
    return ok();
}

// --- The CPU reference sampler ------------------------------------------------------------------

Vec4 sample_plane(const LightmapTexels& texels, u32 plane, Vec2 coordinate) noexcept {
    if (texels.width == 0 || texels.height == 0 || plane >= texels.planes) {
        return Vec4{0.0F, 0.0F, 0.0F, 0.0F};
    }
    const f32 x = coordinate.x - 0.5F;
    const f32 y = coordinate.y - 0.5F;
    const f32 fx = std::floor(x);
    const f32 fy = std::floor(y);
    const f32 tx = x - fx;
    const f32 ty = y - fy;
    const auto clamp_x = [&](f32 value) {
        return static_cast<u32>(std::clamp(value, 0.0F, static_cast<f32>(texels.width - 1U)));
    };
    const auto clamp_y = [&](f32 value) {
        return static_cast<u32>(std::clamp(value, 0.0F, static_cast<f32>(texels.height - 1U)));
    };
    const Vec4 a = texels.texels[texels.index(plane, clamp_x(fx), clamp_y(fy))];
    const Vec4 b = texels.texels[texels.index(plane, clamp_x(fx + 1.0F), clamp_y(fy))];
    const Vec4 c = texels.texels[texels.index(plane, clamp_x(fx), clamp_y(fy + 1.0F))];
    const Vec4 d = texels.texels[texels.index(plane, clamp_x(fx + 1.0F), clamp_y(fy + 1.0F))];
    const auto mix = [](Vec4 p, Vec4 q, f32 t) {
        return Vec4{p.x + ((q.x - p.x) * t), p.y + ((q.y - p.y) * t), p.z + ((q.z - p.z) * t),
                    p.w + ((q.w - p.w) * t)};
    };
    return mix(mix(a, b, tx), mix(c, d, tx), ty);
}

Vec3 sample_lightmap(const BakedLightmap& lightmap, u32 address, Vec2 uv2, Vec3 normal) noexcept {
    const Vec2 coordinate =
        atlas_coordinate(address, uv2, lightmap.page_size, lightmap.gutter_texels);
    const Vec4 first = sample_plane(lightmap.texels, 0, coordinate);
    switch (lightmap.mode) {
        case LightmapMode::Directional: {
            const Vec4 direction = sample_plane(lightmap.texels, 1, coordinate);
            const Vec3 v{direction.x, direction.y, direction.z};
            const f32 factor =
                std::max(0.0F, 1.0F + dot(v, normal)) / std::max(direction.w, 1.0e-3F);
            return Vec3{first.x, first.y, first.z} * factor;
        }
        case LightmapMode::ShL1: {
            const Vec4 green = sample_plane(lightmap.texels, 1, coordinate);
            const Vec4 blue = sample_plane(lightmap.texels, 2, coordinate);
            const auto channel = [&](Vec4 plane) {
                return std::max(0.0F, plane.w + dot(Vec3{plane.x, plane.y, plane.z}, normal));
            };
            return Vec3{channel(first), channel(green), channel(blue)};
        }
        case LightmapMode::Irradiance:
        case LightmapMode::Count:
            break;
    }
    return Vec3{first.x, first.y, first.z};
}

}  // namespace cy::rendering::lightmap_bake
