// SPDX-License-Identifier: MIT
#include <cy/rendering/gi/debug_views.h>

#include <cy/core/math/scalar.h>
#include <cy/rendering/gi/system.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace cy::rendering::gi {
namespace {

constexpr const char* kViewNames[kGiDebugViewCount] = {
    "GiProbes",
    "GiProbeIrradiance",
    "GiProbeAge",
    "GiProbeError",
    "GiProbeVisibility",
    "GiRadianceClipmap",
    "GiSurfaceCache",
    "GiDistanceField",
    "GiCards",
    "GiReflectionProbes",
    "GiScreenTrace",
    "GiConfidence",
    "GiTier",
    "GiRayCount",
    "GiDenoiserConfidence",
    "GiDisocclusion",
    "GiIndirectDiffuse",
    "GiIndirectSpecular",
    "GiEmissive",
};

constexpr Vec3 kAxes[6] = {
    Vec3{1.0F, 0.0F, 0.0F},  Vec3{-1.0F, 0.0F, 0.0F}, Vec3{0.0F, 1.0F, 0.0F},
    Vec3{0.0F, -1.0F, 0.0F}, Vec3{0.0F, 0.0F, 1.0F},  Vec3{0.0F, 0.0F, -1.0F},
};

constexpr Vec3 kGreen{0.15F, 0.85F, 0.25F};
constexpr Vec3 kRed{0.95F, 0.15F, 0.1F};
constexpr Vec3 kMagenta{1.0F, 0.0F, 1.0F};
constexpr Vec3 kDark{0.08F, 0.08F, 0.08F};

[[nodiscard]] u32 to_byte(f32 value) noexcept {
    return static_cast<u32>(std::lround(math::clamp(value, 0.0F, 1.0F) * 255.0F));
}

/// A distinct hue per integer: the golden-ratio walk around the colour wheel.
[[nodiscard]] Vec3 hue_of(u32 index) noexcept {
    const f32 hue = std::fmod(static_cast<f32>(index) * 0.618034F, 1.0F) * 6.0F;
    const f32 fraction = hue - std::floor(hue);
    constexpr f32 kValue = 0.9F;
    constexpr f32 kLow = kValue * (1.0F - 0.65F);
    const f32 falling = kValue * (1.0F - (0.65F * fraction));
    const f32 rising = kValue * (1.0F - (0.65F * (1.0F - fraction)));
    switch (static_cast<u32>(hue) % 6U) {
        case 0:
            return Vec3{kValue, rising, kLow};
        case 1:
            return Vec3{falling, kValue, kLow};
        case 2:
            return Vec3{kLow, kValue, rising};
        case 3:
            return Vec3{kLow, falling, kValue};
        case 4:
            return Vec3{rising, kLow, kValue};
        default:
            return Vec3{kValue, kLow, falling};
    }
}

/// Green at zero, red at one.
[[nodiscard]] Vec3 ramp(f32 t) noexcept {
    return lerp(kGreen, kRed, math::clamp(t, 0.0F, 1.0F));
}

[[nodiscard]] Vec3 tier_colour(RadianceSource source) noexcept {
    switch (source) {
        case RadianceSource::ScreenTrace:
            return Vec3{0.15F, 0.4F, 1.0F};
        case RadianceSource::SoftwareTrace:
            return kGreen;
        case RadianceSource::HardwareTrace:
            return Vec3{1.0F, 0.55F, 0.05F};
        case RadianceSource::Sky:
            return Vec3{1.0F, 1.0F, 1.0F};
        default:
            return Vec3{0.0F, 0.0F, 0.0F};
    }
}

struct Projected {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 w = 0.0F;
};

[[nodiscard]] Projected project(const GiViewCamera& camera, Vec3 point) noexcept {
    const Vec4 clip = camera.world_to_clip * Vec4{point.x, point.y, point.z, 1.0F};
    Projected out;
    out.w = clip.w;
    if (clip.w <= 1.0e-6F) {
        return out;
    }
    out.x = (((clip.x / clip.w) * 0.5F) + 0.5F) * static_cast<f32>(camera.width);
    out.y = (0.5F - ((clip.y / clip.w) * 0.5F)) * static_cast<f32>(camera.height);
    return out;
}

/// What the probe-disc views need of one probe: where, how big, how far, and its colour.
struct Disc {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 radius = 0.0F;
    f32 distance = 0.0F;
    Vec3 colour{0.0F, 0.0F, 0.0F};
};

/// One disc, only where it is nearer than the surface behind it, with a darker rim so a disc
/// reads as a disc against another of the same colour.
void splat_disc(const GiViewCamera& camera, const GiDebugInputs& inputs, const Disc& disc,
                Span<u32> out) noexcept {
    const i32 x0 = std::max(0, static_cast<i32>(std::floor(disc.x - disc.radius)));
    const i32 x1 = std::min(static_cast<i32>(camera.width) - 1,
                            static_cast<i32>(std::ceil(disc.x + disc.radius)));
    const i32 y0 = std::max(0, static_cast<i32>(std::floor(disc.y - disc.radius)));
    const i32 y1 = std::min(static_cast<i32>(camera.height) - 1,
                            static_cast<i32>(std::ceil(disc.y + disc.radius)));
    for (i32 y = y0; y <= y1; ++y) {
        for (i32 x = x0; x <= x1; ++x) {
            const f32 dx = (static_cast<f32>(x) + 0.5F) - disc.x;
            const f32 dy = (static_cast<f32>(y) + 0.5F) - disc.y;
            const f32 from_centre = std::sqrt((dx * dx) + (dy * dy));
            const usize index = (static_cast<usize>(y) * camera.width) + static_cast<usize>(x);
            const GiViewPixel& pixel = inputs.pixels[index];
            const bool hidden =
                pixel.depth > 0.0F && length(pixel.position - camera.position) < disc.distance;
            if (from_centre > disc.radius || hidden) {
                continue;
            }
            out[index] =
                pack_colour(disc.colour * (from_centre > disc.radius * 0.75F ? 0.6F : 1.0F));
        }
    }
}

/// Splat discs far to near.
void draw_discs(const GiViewCamera& camera, const GiDebugInputs& inputs, Array<Disc>& discs,
                Span<u32> out) noexcept {
    std::ranges::sort(discs, [](const Disc& a, const Disc& b) {
        if (a.distance != b.distance) {
            return a.distance > b.distance;
        }
        if (a.x != b.x) {
            return a.x < b.x;
        }
        return a.y < b.y;
    });
    for (const Disc& disc : discs) {
        splat_disc(camera, inputs, disc, out);
    }
}

[[nodiscard]] bool make_disc(const GiViewCamera& camera, const GiDebugInputs& inputs, Vec3 position,
                             Vec3 colour, Disc& disc) noexcept {
    // A probe at the eye, or behind it, would cover the screen: it is not drawn. Nor is one whose
    // disc lies wholly off the screen.
    const Projected centre = project(camera, position);
    if (centre.w <= 4.0F * inputs.probe_disc_metres) {
        return false;
    }
    const Vec3 view = normalized_or(position - camera.position, Vec3{0.0F, 0.0F, -1.0F});
    Vec3 side = cross(view, Vec3{0.0F, 1.0F, 0.0F});
    if (length_squared(side) < 1.0e-6F) {
        side = cross(view, Vec3{1.0F, 0.0F, 0.0F});
    }
    const Projected edge = project(camera, position + (normalize(side) * inputs.probe_disc_metres));
    disc.x = centre.x;
    disc.y = centre.y;
    disc.radius = math::clamp(std::hypot(edge.x - centre.x, edge.y - centre.y), 1.0F,
                              static_cast<f32>(camera.height) * 0.125F);
    if (disc.x + disc.radius < 0.0F || disc.y + disc.radius < 0.0F ||
        disc.x - disc.radius > static_cast<f32>(camera.width) ||
        disc.y - disc.radius > static_cast<f32>(camera.height)) {
        return false;
    }
    disc.distance = length(position - camera.position);
    disc.colour = colour;
    return true;
}

[[nodiscard]] Vec3 probe_mean(const RadianceCache& cache, u32 handle) noexcept {
    const Span<const f32> payload = cache.probe_payload(handle);
    Vec3 total{0.0F, 0.0F, 0.0F};
    for (const Vec3 axis : kAxes) {
        total = total + decode_payload(cache.settings().encoding, payload.data(), axis);
    }
    return total / 6.0F;
}

[[nodiscard]] Vec3 probe_colour(GiDebugView view, const RadianceCache& cache, u32 handle,
                                const GiDebugInputs& inputs) noexcept {
    const Probe& probe = cache.probe(handle);
    switch (view) {
        case GiDebugView::ProbeIrradiance: {
            if (!probe.valid) {
                return kMagenta;
            }
            const Vec3 mean = probe_mean(cache, handle) * inputs.exposure;
            return Vec3{mean.x / (1.0F + mean.x), mean.y / (1.0F + mean.y),
                        mean.z / (1.0F + mean.z)};
        }
        case GiDebugView::ProbeAge: {
            const u64 age =
                inputs.frame > probe.last_update_frame ? inputs.frame - probe.last_update_frame : 0;
            return probe.valid ? ramp(static_cast<f32>(age) /
                                      static_cast<f32>(std::max<u64>(inputs.max_age_frames, 1)))
                               : kMagenta;
        }
        case GiDebugView::ProbeError:
            return probe.valid ? ramp(probe.error / 0.25F) : kMagenta;
        default:
            return probe.valid ? kGreen : kRed;
    }
}

/// The probe overlays: every live probe as a disc in the colour the view gives it.
Status draw_probe_view(IlluminationSystem& system, GiDebugView view, const GiViewCamera& camera,
                       const GiDebugInputs& inputs, Span<u32> out) noexcept {
    const RadianceCache& cache = system.radiance();
    Array<Disc> discs;
    for (u32 handle = 0; handle < cache.probes().size(); ++handle) {
        const Probe& probe = cache.probe(handle);
        Disc disc;
        if (!probe.live || !make_disc(camera, inputs, probe.position,
                                      probe_colour(view, cache, handle, inputs), disc)) {
            continue;
        }
        if (Status pushed = discs.push_back(disc); !pushed) {
            return pushed;
        }
    }
    draw_discs(camera, inputs, discs, out);
    return ok();
}

/// One surface pixel of the leak view, and the suspects it found.
[[nodiscard]] u32 visibility_texel(const IlluminationQuery& query, Array<u8>& suspect) noexcept {
    if (query.probes_contributing == 0) {
        return pack_colour(kDark);
    }
    for (u32 probe = 0; probe < query.probe_count; ++probe) {
        if (query.probes[probe].leak_suspect) {
            suspect[query.probes[probe].probe] = 1;
        }
    }
    return pack_colour(lerp(kGreen, kRed, query.leak_weight) * 0.8F);
}

/// The leak view: the surface by how much of its probe weight is suspect, and the suspects.
Status draw_visibility_view(IlluminationSystem& system, const GiViewCamera& camera,
                            const GiDebugInputs& inputs, Span<u32> out) noexcept {
    const RadianceCache& cache = system.radiance();
    Array<u8> suspect;
    if (Status sized = suspect.resize(cache.probes().size()); !sized) {
        return sized;
    }
    for (u8& flag : suspect) {
        flag = 0;
    }
    ExplainSettings settings;
    settings.frame = inputs.frame;
    for (usize index = 0; index < inputs.pixels.size(); ++index) {
        const GiViewPixel& pixel = inputs.pixels[index];
        if (pixel.depth > 0.0F) {
            out[index] = visibility_texel(
                system.explain_probes(pixel.position, pixel.normal, settings), suspect);
        }
    }
    Array<Disc> discs;
    for (u32 handle = 0; handle < suspect.size(); ++handle) {
        Disc disc;
        if (suspect[handle] == 0 ||
            !make_disc(camera, inputs, cache.probe(handle).position, kRed, disc)) {
            continue;
        }
        if (Status pushed = discs.push_back(disc); !pushed) {
            return pushed;
        }
    }
    draw_discs(camera, inputs, discs, out);
    return ok();
}

[[nodiscard]] Vec3 clipmap_colour(const IlluminationSystem& system, Vec3 position) noexcept {
    const RadianceCache& cache = system.radiance();
    const ProbeCacheSettings& settings = cache.settings();
    for (u32 level = 0; level < settings.levels; ++level) {
        const f32 spacing = cache.level_spacing(level);
        const f32 half = spacing * static_cast<f32>(settings.half_extent_probes);
        const Vec3 offset = position - cache.camera();
        if (std::fabs(offset.x) > half || std::fabs(offset.y) > half ||
            std::fabs(offset.z) > half) {
            continue;
        }
        bool resident = false;
        for (const Probe& probe : cache.probes()) {
            if (probe.live && probe.valid && probe.level == level &&
                length(probe.position - position) <= spacing * 1.5F) {
                resident = true;
                break;
            }
        }
        return hue_of(level + 1U) * (resident ? 1.0F : 0.3F);
    }
    return kDark;
}

[[nodiscard]] Vec3 field_slice_colour(const IlluminationSystem& system, const GiViewCamera& camera,
                                      const GiDebugInputs& inputs, const GiViewPixel& pixel,
                                      bool& drawn) noexcept {
    drawn = false;
    const Vec3 toward = pixel.position - camera.position;
    const f32 surface = length(toward);
    if (surface <= 1.0e-6F) {
        return kDark;
    }
    const Vec3 direction = toward / surface;
    if (std::fabs(direction.y) < 1.0e-6F) {
        return kDark;
    }
    const f32 along = (inputs.field_slice_height - camera.position.y) / direction.y;
    if (along <= 0.0F || along >= surface) {
        return kDark;
    }
    drawn = true;
    const f32 distance = system.field().distance(camera.position + (direction * along));
    // Outside geometry blue, inside orange; a dark band every quarter metre of distance.
    const Vec3 base = distance >= 0.0F ? Vec3{0.2F, 0.45F, 1.0F} : Vec3{1.0F, 0.5F, 0.1F};
    const f32 band = std::fabs(distance) / 0.25F;
    const f32 fraction = band - std::floor(band);
    const f32 shade =
        fraction < 0.12F ? 0.35F : 1.0F - (0.5F * math::clamp(band / 8.0F, 0.0F, 1.0F));
    return base * shade;
}

[[nodiscard]] Vec3 reflection_colour(const IlluminationSystem& system, const GiViewCamera& camera,
                                     const GiViewPixel& pixel, f32 exposure) noexcept {
    u32 handles[4];
    f32 weights[4];
    const ReflectionProbeSet& set = system.reflection_probes();
    const u32 count = set.assign(pixel.position, Span<u32>{handles, 4}, Span<f32>{weights, 4});
    if (count == 0) {
        return kDark;
    }
    const Vec3 view = normalized_or(pixel.position - camera.position, Vec3{0.0F, 0.0F, -1.0F});
    const Vec3 reflection = reflect_vector(view, pixel.normal);
    const RadianceSample captured = set.sample(pixel.position, reflection, pixel.roughness);
    const Vec3 lit = captured.radiance * exposure;
    const Vec3 mapped{lit.x / (1.0F + lit.x), lit.y / (1.0F + lit.y), lit.z / (1.0F + lit.z)};
    return (hue_of(handles[0] + 7U) * (0.5F * weights[0])) + (mapped * 0.5F);
}

[[nodiscard]] Vec3 emissive_radiance(const IlluminationSystem& system,
                                     const GiViewPixel& pixel) noexcept {
    Vec3 total = pixel.emission;
    const Vec3 origin = pixel.position + (pixel.normal * 1.0e-2F);
    for (const GiLight& light : system.emitters().lights()) {
        const Vec3 arriving = direct_radiance(light, pixel.position, pixel.normal);
        if (length_squared(arriving) <= 0.0F ||
            system.world_tracer().occluded(origin, shadow_target(light, pixel.position))) {
            continue;
        }
        total = total + arriving;
    }
    return total;
}

/// What every per-pixel view knows about the pixel it is drawing.
struct PixelQuery {
    const GiViewCamera* camera = nullptr;
    const GiDebugInputs* inputs = nullptr;
    const GiViewPixel* pixel = nullptr;
    usize index = 0;
    Vec3 view_direction{0.0F, 0.0F, -1.0F};
    Vec3 reflection{0.0F, 1.0F, 0.0F};
    Vec3 origin{0.0F, 0.0F, 0.0F};
};

[[nodiscard]] u32 surface_cache_texel(IlluminationSystem& system, const PixelQuery& q) noexcept {
    Vec3 radiance{0.0F, 0.0F, 0.0F};
    u32 age = 0;
    return system.surfaces().radiance_at(q.pixel->position, q.pixel->normal, radiance, age)
               ? pack_radiance(radiance, q.inputs->exposure)
               : pack_colour(kMagenta);
}

[[nodiscard]] u32 screen_trace_texel(IlluminationSystem& system, const PixelQuery& q) noexcept {
    if (q.inputs->screen == nullptr) {
        return pack_colour(kRed);
    }
    const RadianceSample answer = ScreenTracer::trace(*q.inputs->screen, q.origin, q.reflection,
                                                      system.settings().max_ray_distance_metres);
    return answer.confidence > 0.0F ? pack_colour(kGreen * (0.25F + (0.75F * answer.confidence)))
                                    : pack_colour(kRed);
}

[[nodiscard]] u32 tier_texel(IlluminationSystem& system, const PixelQuery& q) noexcept {
    TraceBudget budget;
    const GiRegionSettings region = system.region_settings(q.pixel->position);
    const RadianceSample answer = system.tracer().trace(
        q.origin, q.reflection, system.settings().max_ray_distance_metres, budget, region.tier);
    return pack_colour(
        tier_colour(answer.confidence > 0.0F ? answer.source : RadianceSource::None));
}

[[nodiscard]] u32 ray_count_texel(IlluminationSystem& system, const PixelQuery& q) noexcept {
    const TracingDiagnostics before = system.tracer().diagnostics();
    TraceBudget budget;
    (void)system.indirect_specular(q.pixel->position, q.pixel->normal, q.view_direction,
                                   q.pixel->roughness, static_cast<u32>(q.inputs->frame), budget);
    const TracingDiagnostics& after = system.tracer().diagnostics();
    constexpr f32 kFullScale = 16.0F;
    return pack_colour(
        Vec3{static_cast<f32>(after.hardware_rays - before.hardware_rays) / kFullScale,
             static_cast<f32>(after.software_rays - before.software_rays) / kFullScale,
             static_cast<f32>(after.screen_rays - before.screen_rays) / kFullScale});
}

[[nodiscard]] bool reprojected(const GiDebugInputs& inputs, usize index) noexcept {
    return index < inputs.history.source.size() && inputs.history.source[index] >= 0;
}

[[nodiscard]] u32 denoiser_confidence_texel(const PixelQuery& q) noexcept {
    const bool known =
        reprojected(*q.inputs, q.index) && q.index < q.inputs->history.confidence.size();
    return pack_colour(ramp(1.0F - (known ? q.inputs->history.confidence[q.index] : 0.0F)));
}

[[nodiscard]] u32 specular_texel(IlluminationSystem& system, const PixelQuery& q) noexcept {
    TraceBudget budget;
    const ResolveResult resolved =
        system.indirect_specular(q.pixel->position, q.pixel->normal, q.view_direction,
                                 q.pixel->roughness, static_cast<u32>(q.inputs->frame), budget);
    return pack_radiance(resolved.radiance, q.inputs->exposure);
}

[[nodiscard]] u32 cards_texel(IlluminationSystem& system, const PixelQuery& q) noexcept {
    const u32 card = system.scene().nearest_surfel(q.pixel->position, q.pixel->normal,
                                                   system.surfaces().lookup_radius());
    return card == GiScene::kInvalidSurfel ? pack_colour(kDark) : pack_colour(hue_of(card));
}

/// One pixel of a per-pixel view, or false where the view draws nothing there.
[[nodiscard]] bool pixel_texel(IlluminationSystem& system, GiDebugView view, const PixelQuery& q,
                               u32& out) noexcept {
    const GiViewPixel& pixel = *q.pixel;
    switch (view) {
        case GiDebugView::RadianceClipmap:
            out = pack_colour(clipmap_colour(system, pixel.position));
            return true;
        case GiDebugView::SurfaceCache:
            out = surface_cache_texel(system, q);
            return true;
        case GiDebugView::DistanceField: {
            bool drawn = false;
            out = pack_colour(field_slice_colour(system, *q.camera, *q.inputs, pixel, drawn));
            return drawn;
        }
        case GiDebugView::Cards:
            out = cards_texel(system, q);
            return true;
        case GiDebugView::ReflectionProbes:
            out = pack_colour(reflection_colour(system, *q.camera, pixel, q.inputs->exposure));
            return true;
        case GiDebugView::ScreenTrace:
            out = screen_trace_texel(system, q);
            return true;
        case GiDebugView::Confidence:
            out = pack_colour(ramp(
                1.0F - system.indirect_diffuse(pixel.position, pixel.normal, SurfaceProperties{})
                           .confidence));
            return true;
        case GiDebugView::Tier:
            out = tier_texel(system, q);
            return true;
        case GiDebugView::RayCount:
            out = ray_count_texel(system, q);
            return true;
        case GiDebugView::DenoiserConfidence:
            out = denoiser_confidence_texel(q);
            return true;
        case GiDebugView::Disocclusion:
            out = pack_colour(reprojected(*q.inputs, q.index) ? kDark : kMagenta);
            return true;
        case GiDebugView::IndirectDiffuse:
            out = pack_radiance(
                system.indirect_diffuse(pixel.position, pixel.normal, SurfaceProperties{}).radiance,
                q.inputs->exposure);
            return true;
        case GiDebugView::IndirectSpecular:
            out = specular_texel(system, q);
            return true;
        case GiDebugView::Emissive:
            out = pack_radiance(emissive_radiance(system, pixel), q.inputs->exposure);
            return true;
        default:
            return false;
    }
}

/// The views that are one colour per surface pixel.
Status draw_pixel_view(IlluminationSystem& system, GiDebugView view, const GiViewCamera& camera,
                       const GiDebugInputs& inputs, Span<u32> out) noexcept {
    const ScreenView* previous_screen = system.tracer().screen();
    system.tracer().set_screen(inputs.screen);
    PixelQuery q;
    q.camera = &camera;
    q.inputs = &inputs;
    for (usize index = 0; index < inputs.pixels.size(); ++index) {
        const GiViewPixel& pixel = inputs.pixels[index];
        if (pixel.depth <= 0.0F) {
            continue;
        }
        q.pixel = &pixel;
        q.index = index;
        q.view_direction = normalized_or(pixel.position - camera.position, Vec3{0.0F, 0.0F, -1.0F});
        q.reflection = reflect_vector(q.view_direction, pixel.normal);
        q.origin = pixel.position + (pixel.normal * 1.0e-3F);
        u32 texel = 0;
        if (pixel_texel(system, view, q, texel)) {
            out[index] = texel;
        }
    }
    system.tracer().set_screen(previous_screen);
    return ok();
}

}  // namespace

const char* gi_debug_view_name(GiDebugView view) noexcept {
    const auto index = static_cast<u32>(view);
    return index < kGiDebugViewCount ? kViewNames[index] : "<invalid>";
}

GiDebugView gi_debug_view_from_name(const char* name) noexcept {
    if (name == nullptr) {
        return GiDebugView::Count;
    }
    for (u32 index = 0; index < kGiDebugViewCount; ++index) {
        if (std::strcmp(name, kViewNames[index]) == 0) {
            return static_cast<GiDebugView>(index);
        }
    }
    return GiDebugView::Count;
}

Vec3 debug_view_hue(u32 index) noexcept {
    return hue_of(index);
}

Vec3 debug_view_ramp(f32 t) noexcept {
    return ramp(t);
}

u32 pack_colour(Vec3 colour) noexcept {
    return to_byte(colour.x) | (to_byte(colour.y) << 8U) | (to_byte(colour.z) << 16U) |
           (255U << 24U);
}

u32 pack_radiance(Vec3 radiance, f32 exposure) noexcept {
    const Vec3 lit = radiance * exposure;
    return pack_colour(Vec3{std::max(lit.x, 0.0F) / (1.0F + std::max(lit.x, 0.0F)),
                            std::max(lit.y, 0.0F) / (1.0F + std::max(lit.y, 0.0F)),
                            std::max(lit.z, 0.0F) / (1.0F + std::max(lit.z, 0.0F))});
}

Status render_gi_debug_view(IlluminationSystem& system, GiDebugView view,
                            const GiViewCamera& camera, const GiDebugInputs& inputs,
                            Span<u32> out) noexcept {
    const usize pixels = static_cast<usize>(camera.width) * camera.height;
    if (view >= GiDebugView::Count) {
        return fail(ErrorCode::InvalidArgument, "render_gi_debug_view: no such view");
    }
    if (pixels == 0 || inputs.pixels.size() != pixels || out.size() != pixels) {
        return fail(ErrorCode::InvalidArgument,
                    "render_gi_debug_view: the surface buffer and the output must both be "
                    "width * height");
    }
    for (u32& texel : out) {
        texel = 0;
    }
    switch (view) {
        case GiDebugView::Probes:
        case GiDebugView::ProbeIrradiance:
        case GiDebugView::ProbeAge:
        case GiDebugView::ProbeError:
            return draw_probe_view(system, view, camera, inputs, out);
        case GiDebugView::ProbeVisibility:
            return draw_visibility_view(system, camera, inputs, out);
        default:
            return draw_pixel_view(system, view, camera, inputs, out);
    }
}

}  // namespace cy::rendering::gi
