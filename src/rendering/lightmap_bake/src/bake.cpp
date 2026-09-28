// SPDX-License-Identifier: MIT
// The lightmap bake. See bake.h for the pipeline and the units.

#include "internal.h"

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
    return ok();
}

/// The lights the path tracer is built with: the caller's, divided by pi. See bake.h.
[[nodiscard]] Status frame_lights(Span<const gi::GiLight> lights,
                                  Array<gi::GiLight>& out) noexcept {
    out.clear();
    for (gi::GiLight light : lights) {
        light.intensity /= std::numbers::pi_v<f32>;
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

    [[nodiscard]] Status build(const LightmapScene& scene,
                               const LightmapBakeSettings& settings) noexcept {
        if (Status built = tracer.build(scene); !built) {
            return built;
        }
        if (Status built = detail::build_surfels(scene, tracer, settings.surfel_spacing, surfels);
            !built) {
            return built;
        }
        return frame_lights(scene.lights, lights);
    }
};

void trace_canvas(const detail::TraceContext& context, const LightmapBakeSettings& settings,
                  Canvas& canvas, LightmapBakeReport& report) noexcept {
    for (usize index = 0; index < canvas.surfaces.size(); ++index) {
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
        report.texels_covered += 1U;
    }
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

Status bake_lightmaps(const LightmapScene& scene, const LightmapBakeSettings& settings,
                      const CacheSeedTargets* seeds, BakedLightmap& out,
                      LightmapBakeReport& report) noexcept {
    report = LightmapBakeReport{};
    if (settings.mode >= LightmapMode::Count) {
        return fail(ErrorCode::InvalidArgument, "an unknown lightmap mode");
    }
    if (Status valid = validate(scene); !valid) {
        return valid;
    }
    if (Status packed = pack(scene, settings, out, report); !packed) {
        return packed;
    }

    TraceWorld world;
    if (Status built = world.build(scene, settings); !built) {
        return built;
    }
    const gi::PathTracer path(world.tracer, world.surfels, world.lights.span(), scene.sky,
                              &world.tracer);

    Canvas canvas;
    Array<detail::SeamEdge> seams;
    if (Status drawn = detail::rasterise(scene, out, canvas, seams); !drawn) {
        return drawn;
    }
    detail::TraceContext context;
    context.path = &path;
    context.tracer = &world.tracer;
    context.lights = world.lights.span();
    context.settings = &settings;
    trace_canvas(context, settings, canvas, report);

    if (settings.denoise) {
        if (Status denoised =
                detail::denoise_moments(canvas, settings.mode, settings.denoise_passes);
            !denoised) {
            return denoised;
        }
    }
    report.texels_dilated = detail::dilate(canvas, dilation_passes(settings, out));
    report.seam_edges = static_cast<u32>(seams.size());
    report.seam_samples = detail::reconcile_seams(
        canvas, seams.span(), settings.seam_iterations, settings.reconcile_seams,
        report.seam_error_before, report.seam_error_after);

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

    // THE SEEDS, FROM THIS RUN'S TRACER AND CARDS. With the caller's own lights rather than the
    // frame's: the dynamic caches keep the surface cache's convention (see bake.h).
    if (seeds != nullptr) {
        const gi::PathTracer seeding(world.tracer, world.surfels, scene.lights, scene.sky,
                                     &world.tracer);
        if (Status seeded = seed(seeding, world, scene, *seeds, report); !seeded) {
            return seeded;
        }
    }
    report.rays = world.tracer.rays();
    return ok();
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
