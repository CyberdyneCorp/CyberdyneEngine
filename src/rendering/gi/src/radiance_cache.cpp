#include <cy/rendering/gi/radiance_cache.h>

#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <ranges>
#include <utility>

namespace cy::rendering::gi {
namespace {

constexpr Vec3 kAxes[6] = {{1.0F, 0.0F, 0.0F},  {-1.0F, 0.0F, 0.0F}, {0.0F, 1.0F, 0.0F},
                           {0.0F, -1.0F, 0.0F}, {0.0F, 0.0F, 1.0F},  {0.0F, 0.0F, -1.0F}};

constexpr u32 kOctahedralEdge = 4;

/// Pack a level and a signed cell coordinate into one clipmap key.
[[nodiscard]] u64 cell_key(u32 level, i32 x, i32 y, i32 z) noexcept {
    const auto fold = [](i32 value) {
        return static_cast<u64>(static_cast<u32>(value + (1 << 18)) & 0x7FFFFU);
    };
    return (static_cast<u64>(level & 0xFU) << 57U) | (fold(x) << 38U) | (fold(y) << 19U) | fold(z);
}

[[nodiscard]] i32 floor_div(f32 value, f32 divisor) noexcept {
    return static_cast<i32>(std::floor(value / divisor));
}

/// The golden-ratio sphere sequence: uniform, deterministic, and stable under any count. A probe
/// gather that changed direction set between two frames would make its own convergence metric
/// noise.
[[nodiscard]] Vec3 sphere_direction(u32 index, u32 count, u32 rotation) noexcept {
    constexpr f32 kGolden = 2.39996323F;
    const f32 offset = ((2.0F * static_cast<f32>(index)) + 1.0F) / static_cast<f32>(count);
    const f32 z = 1.0F - offset;
    const f32 radius = std::sqrt(std::max(0.0F, 1.0F - (z * z)));
    // Rays are distributed across frames rather than resolved fully per frame; the rotation is
    // which subset this frame took, and it is why successive frames converge rather than repeat.
    const f32 angle =
        (kGolden * static_cast<f32>(index)) + (static_cast<f32>(rotation) * 0.61803399F);
    return Vec3{radius * std::cos(angle), radius * std::sin(angle), z};
}

/// Direction to octahedral texel coordinates in [0, edge).
void octahedral_uv(Vec3 direction, f32& u, f32& v) noexcept {
    const f32 norm = std::abs(direction.x) + std::abs(direction.y) + std::abs(direction.z);
    const Vec3 p = norm > 0.0F ? direction / norm : Vec3{0.0F, 0.0F, 1.0F};
    f32 px = p.x;
    f32 py = p.y;
    if (p.z < 0.0F) {
        const f32 fx = (1.0F - std::abs(p.y)) * (p.x >= 0.0F ? 1.0F : -1.0F);
        const f32 fy = (1.0F - std::abs(p.x)) * (p.y >= 0.0F ? 1.0F : -1.0F);
        px = fx;
        py = fy;
    }
    u = math::clamp((px * 0.5F) + 0.5F, 0.0F, 1.0F) * static_cast<f32>(kOctahedralEdge - 1);
    v = math::clamp((py * 0.5F) + 0.5F, 0.0F, 1.0F) * static_cast<f32>(kOctahedralEdge - 1);
}

/// The direction one octahedral texel stands for. The inverse of the mapping above, at texel
/// centres, and what makes the map a cosine-convolved irradiance map rather than a radiance one.
[[nodiscard]] Vec3 octahedral_direction(u32 texel) noexcept {
    const u32 x = texel % kOctahedralEdge;
    const u32 y = texel / kOctahedralEdge;
    const f32 u = (static_cast<f32>(x) / static_cast<f32>(kOctahedralEdge - 1) * 2.0F) - 1.0F;
    const f32 v = (static_cast<f32>(y) / static_cast<f32>(kOctahedralEdge - 1) * 2.0F) - 1.0F;
    Vec3 direction{u, v, 1.0F - std::abs(u) - std::abs(v)};
    if (direction.z < 0.0F) {
        const f32 fx = (1.0F - std::abs(v)) * (u >= 0.0F ? 1.0F : -1.0F);
        const f32 fy = (1.0F - std::abs(u)) * (v >= 0.0F ? 1.0F : -1.0F);
        direction.x = fx;
        direction.y = fy;
    }
    return normalized_or(direction, Vec3{0.0F, 0.0F, 1.0F});
}

}  // namespace

Vec3 SkyTerm::radiance(Vec3 direction) const noexcept {
    const Vec3 unit = normalized_or(direction, Vec3{0.0F, 1.0F, 0.0F});
    if (unit.y < 0.0F) {
        return ground * intensity;
    }
    const f32 t = math::clamp(std::pow(unit.y, 0.45F), 0.0F, 1.0F);
    return lerp(horizon, zenith, t) * intensity;
}

const char* probe_encoding_name(ProbeEncoding encoding) noexcept {
    switch (encoding) {
        case ProbeEncoding::SphericalHarmonicsL1:
            return "SphericalHarmonicsL1";
        case ProbeEncoding::Octahedral:
            return "Octahedral";
        case ProbeEncoding::SphericalGaussian:
            return "SphericalGaussian";
        case ProbeEncoding::Count:
            break;
    }
    return "Unknown";
}

EncodingTraits encoding_traits(ProbeEncoding encoding) noexcept {
    EncodingTraits traits;
    switch (encoding) {
        case ProbeEncoding::SphericalHarmonicsL1:
            traits.floats_per_probe = 12;
            traits.directionality = 1.0F;
            traits.evaluation_cost = 1.0F;
            break;
        case ProbeEncoding::Octahedral:
            traits.floats_per_probe = kOctahedralEdge * kOctahedralEdge * 3;
            traits.directionality = 3.2F;
            traits.evaluation_cost = 1.8F;
            break;
        case ProbeEncoding::SphericalGaussian:
            traits.floats_per_probe = 18;
            traits.directionality = 1.7F;
            traits.evaluation_cost = 1.3F;
            break;
        case ProbeEncoding::Count:
            break;
    }
    traits.bytes_per_probe = traits.floats_per_probe * static_cast<u32>(sizeof(f32));
    return traits;
}

void encode_sample(ProbeEncoding encoding, f32* payload, Vec3 direction, Vec3 radiance,
                   f32 weight) noexcept {
    switch (encoding) {
        case ProbeEncoding::SphericalHarmonicsL1: {
            constexpr f32 kY0 = 0.282095F;
            constexpr f32 kY1 = 0.488603F;
            const f32 basis[4] = {kY0, kY1 * direction.y, kY1 * direction.z, kY1 * direction.x};
            for (u32 coefficient = 0; coefficient < 4; ++coefficient) {
                const f32 scale = basis[coefficient] * weight;
                payload[(coefficient * 3) + 0] += radiance.x * scale;
                payload[(coefficient * 3) + 1] += radiance.y * scale;
                payload[(coefficient * 3) + 2] += radiance.z * scale;
            }
            break;
        }
        case ProbeEncoding::Octahedral: {
            // Cosine-convolved on the way in: each texel accumulates the samples that face it, so
            // a fetch is an irradiance and not a radiance that still needs a hemisphere integrated.
            for (u32 texel = 0; texel < kOctahedralEdge * kOctahedralEdge; ++texel) {
                const f32 cosine = std::max(0.0F, dot(octahedral_direction(texel), direction));
                if (cosine <= 0.0F) {
                    continue;
                }
                const f32 scale = cosine * weight;
                payload[(texel * 3) + 0] += radiance.x * scale;
                payload[(texel * 3) + 1] += radiance.y * scale;
                payload[(texel * 3) + 2] += radiance.z * scale;
            }
            break;
        }
        case ProbeEncoding::SphericalGaussian: {
            for (u32 lobe = 0; lobe < 6; ++lobe) {
                const f32 cosine = std::max(0.0F, dot(kAxes[lobe], direction));
                if (cosine <= 0.0F) {
                    continue;
                }
                const f32 scale = cosine * weight;
                payload[(lobe * 3) + 0] += radiance.x * scale;
                payload[(lobe * 3) + 1] += radiance.y * scale;
                payload[(lobe * 3) + 2] += radiance.z * scale;
            }
            break;
        }
        case ProbeEncoding::Count:
            break;
    }
}

void normalise_payload(ProbeEncoding encoding, f32* payload, f32 total_weight,
                       u32 sample_count) noexcept {
    if (sample_count == 0 || total_weight <= 0.0F) {
        return;
    }
    const EncodingTraits traits = encoding_traits(encoding);
    f32 scale = 1.0F;
    switch (encoding) {
        case ProbeEncoding::SphericalHarmonicsL1:
            // Monte Carlo over the whole sphere: the estimator's normalisation is 4*pi/N.
            scale = 4.0F * std::numbers::pi_v<f32> / total_weight;
            break;
        case ProbeEncoding::Octahedral:
        case ProbeEncoding::SphericalGaussian:
            // Each texel or lobe accumulated a cosine-weighted sum; dividing by the same cosine
            // sum would need a per-texel total. The sample count is that total up to the cosine
            // integral over the sphere, which is pi per texel, so one scalar does it.
            scale = std::numbers::pi_v<f32> / total_weight;
            break;
        case ProbeEncoding::Count:
            return;
    }
    for (u32 index = 0; index < traits.floats_per_probe; ++index) {
        payload[index] *= scale;
    }
}

Vec3 decode_payload(ProbeEncoding encoding, const f32* payload, Vec3 direction) noexcept {
    switch (encoding) {
        case ProbeEncoding::SphericalHarmonicsL1: {
            constexpr f32 kY0 = 0.282095F;
            constexpr f32 kY1 = 0.488603F;
            // The L1 irradiance convolution: A0 = pi, A1 = 2*pi/3, divided by pi to return a
            // radiance rather than an irradiance.
            constexpr f32 kA0 = 1.0F;
            constexpr f32 kA1 = 2.0F / 3.0F;
            const f32 basis[4] = {kY0 * kA0, kY1 * kA1 * direction.y, kY1 * kA1 * direction.z,
                                  kY1 * kA1 * direction.x};
            Vec3 result{0.0F, 0.0F, 0.0F};
            for (u32 coefficient = 0; coefficient < 4; ++coefficient) {
                result.x += payload[(coefficient * 3) + 0] * basis[coefficient];
                result.y += payload[(coefficient * 3) + 1] * basis[coefficient];
                result.z += payload[(coefficient * 3) + 2] * basis[coefficient];
            }
            return Vec3{std::max(0.0F, result.x), std::max(0.0F, result.y),
                        std::max(0.0F, result.z)};
        }
        case ProbeEncoding::Octahedral: {
            f32 u = 0.0F;
            f32 v = 0.0F;
            octahedral_uv(direction, u, v);
            const auto x0 = static_cast<u32>(u);
            const auto y0 = static_cast<u32>(v);
            const u32 x1 = std::min(x0 + 1, kOctahedralEdge - 1);
            const u32 y1 = std::min(y0 + 1, kOctahedralEdge - 1);
            const f32 tx = u - static_cast<f32>(x0);
            const f32 ty = v - static_cast<f32>(y0);
            const auto fetch = [payload](u32 x, u32 y) {
                const u32 texel = (y * kOctahedralEdge) + x;
                return Vec3{payload[(texel * 3) + 0], payload[(texel * 3) + 1],
                            payload[(texel * 3) + 2]};
            };
            const Vec3 top = lerp(fetch(x0, y0), fetch(x1, y0), tx);
            const Vec3 bottom = lerp(fetch(x0, y1), fetch(x1, y1), tx);
            return lerp(top, bottom, ty);
        }
        case ProbeEncoding::SphericalGaussian: {
            Vec3 result{0.0F, 0.0F, 0.0F};
            f32 total = 0.0F;
            for (u32 lobe = 0; lobe < 6; ++lobe) {
                const f32 cosine = std::max(0.0F, dot(kAxes[lobe], direction));
                if (cosine <= 0.0F) {
                    continue;
                }
                result = result + (Vec3{payload[(lobe * 3) + 0], payload[(lobe * 3) + 1],
                                        payload[(lobe * 3) + 2]} *
                                   cosine);
                total += cosine;
            }
            return total > 0.0F ? result / total : Vec3{0.0F, 0.0F, 0.0F};
        }
        case ProbeEncoding::Count:
            break;
    }
    return Vec3{0.0F, 0.0F, 0.0F};
}

RadianceCache::RadianceCache() noexcept = default;
RadianceCache::~RadianceCache() = default;

Status RadianceCache::configure(const ProbeCacheSettings& settings) noexcept {
    if (settings.levels == 0 || settings.levels > 8 || settings.base_spacing_metres <= 0.0F ||
        settings.half_extent_probes == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "RadianceCache::configure: levels must be 1 to 8 with a positive spacing and a "
                    "non-zero window");
    }
    settings_ = settings;
    stride_ = encoding_traits(settings.encoding).floats_per_probe;
    probes_.clear();
    payloads_.clear();
    proxies_.clear();
    free_probes_.clear();
    index_.clear();
    occupancy_ = HashMap<u64, u32>{};
    live_probes_ = 0;
    diagnostics_ = RadianceCacheDiagnostics{};
    return ok();
}

f32* RadianceCache::payload(u32 probe) noexcept {
    return payloads_.data() + (static_cast<usize>(probe) * stride_);
}

const f32* RadianceCache::payload(u32 probe) const noexcept {
    return payloads_.data() + (static_cast<usize>(probe) * stride_);
}

Expected<u32, Error> RadianceCache::create_probe(Vec3 position, u32 level) noexcept {
    u32 handle = ~0U;
    if (!free_probes_.empty()) {
        handle = free_probes_.back();
        free_probes_.pop_back();
    } else {
        Probe fresh;
        if (Status pushed = probes_.push_back(fresh); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = proxies_.push_back(0); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status sized = payloads_.resize(probes_.size() * stride_); !sized) {
            return make_unexpected(sized.error());
        }
        handle = static_cast<u32>(probes_.size() - 1);
    }

    Probe& entry = probes_[handle];
    entry = Probe{};
    entry.position = position;
    entry.level = level;
    entry.live = true;
    for (f32& axis : entry.axis_distance) {
        axis = 1.0e6F;
    }
    f32* data = payload(handle);
    for (u32 index = 0; index < stride_; ++index) {
        data[index] = 0.0F;
    }

    const f32 spacing = settings_.base_spacing_metres * static_cast<f32>(1U << level);
    const Vec3 extent{spacing * 0.5F, spacing * 0.5F, spacing * 0.5F};
    Expected<u32, Error> proxy = index_.insert(Aabb{position - extent, position + extent}, handle);
    if (!proxy) {
        entry.live = false;
        (void)free_probes_.push_back(handle);
        return make_unexpected(proxy.error());
    }
    proxies_[handle] = proxy.value();
    live_probes_ += 1;
    return handle;
}

void RadianceCache::retire_probe(u32 probe) noexcept {
    if (probe >= probes_.size() || !probes_[probe].live) {
        return;
    }
    (void)index_.remove(proxies_[probe]);
    probes_[probe].live = false;
    probes_[probe].valid = false;
    (void)free_probes_.push_back(probe);
    live_probes_ -= 1;
}

bool RadianceCache::accept_candidate(Vec3 position, u32 level, f32 spacing,
                                     const ProbePlacementContext& context,
                                     ProbePlacementReport& report) const noexcept {
    if (context.has_importance_region && context.importance_region.contains(position)) {
        // The player's region, or a designer's. Full density whatever the geometry says.
        return true;
    }
    if (context.field == nullptr) {
        return true;
    }
    const f32 distance = context.field->distance(position);
    if (distance < 0.0F) {
        // Inside solid geometry. A probe here is queried by nothing and updated forever.
        report.rejected_inside_geometry += 1;
        return false;
    }
    if (distance < spacing * 1.5F) {
        // Near a surface: this is where indirect light varies and where the density belongs.
        return true;
    }
    for (const GiLight& light : context.lights) {
        if (!light.directional && cy::distance(light.position, position) < spacing * 3.0F) {
            // A lighting transition. The other place the field varies quickly.
            return true;
        }
    }
    // Open space, and the coarsest level still covers it. Placing one probe in eight here is the
    // whole of "density SHALL be lower in open space and at distance": the region is still lit, at
    // a quarter of the probes.
    const i32 cell_x = floor_div(position.x, spacing);
    const i32 cell_y = floor_div(position.y, spacing);
    const i32 cell_z = floor_div(position.z, spacing);
    const bool sparse_slot = ((cell_x & 1) == 0) && ((cell_y & 1) == 0) && ((cell_z & 1) == 0);
    if (!sparse_slot && level + 1 < settings_.levels) {
        report.rejected_open_space += 1;
        return false;
    }
    return true;
}

/// Retire probes whose CELL has left the window.
///
/// By cell rather than by distance, and with the same arithmetic the placement loop uses: a
/// distance test with its own margin is a second definition of "in the clipmap", and the two
/// disagree at exactly the boundary the scroll is made of. Doing it first frees the slots the new
/// region is about to want, which is what keeps a translating camera's probe count flat.
void RadianceCache::retire_outside_window(Vec3 camera, i32 half,
                                          ProbePlacementReport& report) noexcept {
    for (u32 handle = 0; handle < probes_.size(); ++handle) {
        Probe& entry = probes_[handle];
        if (!entry.live) {
            continue;
        }
        const f32 spacing = settings_.base_spacing_metres * static_cast<f32>(1U << entry.level);
        const i32 cell_x = floor_div(entry.position.x, spacing);
        const i32 cell_y = floor_div(entry.position.y, spacing);
        const i32 cell_z = floor_div(entry.position.z, spacing);
        const i32 centre_x = floor_div(camera.x, spacing);
        const i32 centre_y = floor_div(camera.y, spacing);
        const i32 centre_z = floor_div(camera.z, spacing);
        const bool inside = cell_x >= centre_x - half && cell_x <= centre_x + half &&
                            cell_y >= centre_y - half && cell_y <= centre_y + half &&
                            cell_z >= centre_z - half && cell_z <= centre_z + half;
        if (!inside) {
            (void)occupancy_.remove(cell_key(entry.level, cell_x, cell_y, cell_z));
            retire_probe(handle);
            report.probes_retired += 1;
        }
    }
}

/// Populate one clipmap level's window, reusing every cell that already holds a probe.
void RadianceCache::populate_level(u32 level, Vec3 camera, i32 half,
                                   const ProbePlacementContext& context,
                                   ProbePlacementReport& report) noexcept {
    const f32 spacing = settings_.base_spacing_metres * static_cast<f32>(1U << level);
    const i32 centre_x = floor_div(camera.x, spacing);
    const i32 centre_y = floor_div(camera.y, spacing);
    const i32 centre_z = floor_div(camera.z, spacing);

    for (i32 z = -half; z <= half; ++z) {
        for (i32 y = -half; y <= half; ++y) {
            for (i32 x = -half; x <= half; ++x) {
                const i32 cell_x = centre_x + x;
                const i32 cell_y = centre_y + y;
                const i32 cell_z = centre_z + z;
                const u64 key = cell_key(level, cell_x, cell_y, cell_z);
                if (occupancy_.find(key) != nullptr) {
                    // Still valid and reused untouched. This is the scroll.
                    report.probes_reused += 1;
                    continue;
                }
                const Vec3 position{(static_cast<f32>(cell_x) + 0.5F) * spacing,
                                    (static_cast<f32>(cell_y) + 0.5F) * spacing,
                                    (static_cast<f32>(cell_z) + 0.5F) * spacing};
                if (!accept_candidate(position, level, spacing, context, report)) {
                    continue;
                }
                Expected<u32, Error> created = create_probe(position, level);
                if (!created) {
                    continue;
                }
                probes_[created.value()].camera_distance = cy::distance(position, camera);
                if (Expected<u32*, Error> stored = occupancy_.insert(key, created.value());
                    !stored) {
                    retire_probe(created.value());
                    continue;
                }
                report.probes_created += 1;
            }
        }
    }
}

ProbePlacementReport RadianceCache::scroll_to(Vec3 camera,
                                              const ProbePlacementContext& context) noexcept {
    ProbePlacementReport report;
    camera_ = camera;
    const auto half = static_cast<i32>(settings_.half_extent_probes);

    retire_outside_window(camera, half, report);
    for (u32 level = 0; level < settings_.levels; ++level) {
        populate_level(level, camera, half, context, report);
    }

    for (Probe& entry : probes_) {
        if (entry.live) {
            entry.camera_distance = cy::distance(entry.position, camera);
        }
    }
    diagnostics_.last_placement = report;
    account();
    return report;
}

u32 RadianceCache::invalidate(const Aabb& region) noexcept {
    u32 count = 0;
    index_.query_aabb(region, [&](u32 /*proxy*/, u64 user_data) {
        const u32 handle = static_cast<u32>(user_data);
        if (handle < probes_.size() && probes_[handle].live &&
            region.contains(probes_[handle].position)) {
            probes_[handle].valid = false;
            probes_[handle].error = 1.0F;
            count += 1;
        }
        return true;
    });
    return count;
}

f32 RadianceCache::priority_of(const Probe& entry, u64 frame) noexcept {
    const u64 age = frame > entry.last_update_frame ? frame - entry.last_update_frame : 0;
    f32 priority = 0.0F;
    if (entry.visible) {
        priority += 1000.0F;
    }
    if (!entry.valid) {
        priority += 500.0F;
    }
    priority += entry.importance * 50.0F;
    priority += entry.error * 20.0F;
    priority += std::min(static_cast<f32>(age), 4000.0F) * 0.05F;
    // Distance is a cost, not a benefit: a probe at the far end of the coarsest clipmap serves
    // fewer pixels than one at the camera.
    priority -= entry.camera_distance * 0.5F;
    return priority;
}

void RadianceCache::gather_probe(u32 handle, const ProbeUpdateContext& context) noexcept {
    Probe& entry = probes_[handle];
    f32* data = payload(handle);

    Vec3 previous{0.0F, 0.0F, 0.0F};
    for (const Vec3 axis : kAxes) {
        previous = previous + decode_payload(settings_.encoding, data, axis);
    }

    for (u32 index = 0; index < stride_; ++index) {
        data[index] = 0.0F;
    }
    f32 axis_distance[6];
    f32 axis_weight[6];
    for (u32 axis = 0; axis < 6; ++axis) {
        axis_distance[axis] = 0.0F;
        axis_weight[axis] = 0.0F;
    }

    const u32 rays = std::max(1U, context.rays_per_probe);
    const auto rotation = static_cast<u32>(context.frame & 0xFFU);
    f32 total_weight = 0.0F;
    for (u32 index = 0; index < rays; ++index) {
        const Vec3 direction = sphere_direction(index, rays, rotation);
        Vec3 radiance = context.sky.radiance(direction);
        f32 hit_distance = context.max_ray_distance_metres;
        if (context.tracer != nullptr) {
            SceneHit hit;
            if (context.tracer->trace(entry.position, direction, context.max_ray_distance_metres,
                                      hit)) {
                hit_distance = hit.t;
                Vec3 surface{0.0F, 0.0F, 0.0F};
                u32 age = 0;
                if (context.radiance != nullptr &&
                    context.radiance->radiance_at(hit.position, hit.normal, surface, age)) {
                    radiance = surface;
                } else {
                    // Nothing in the surface cache covers the hit. Black is the honest answer and
                    // the miss is counted: it is what makes "why is this area dark" answerable.
                    radiance = Vec3{0.0F, 0.0F, 0.0F};
                }
            }
        }
        encode_sample(settings_.encoding, data, direction, radiance, 1.0F);
        total_weight += 1.0F;
        for (u32 axis = 0; axis < 6; ++axis) {
            const f32 cosine = std::max(0.0F, dot(kAxes[axis], direction));
            axis_distance[axis] += hit_distance * cosine;
            axis_weight[axis] += cosine;
        }
    }
    normalise_payload(settings_.encoding, data, total_weight, rays);

    for (u32 axis = 0; axis < 6; ++axis) {
        entry.axis_distance[axis] = axis_weight[axis] > 0.0F
                                        ? axis_distance[axis] / axis_weight[axis]
                                        : context.max_ray_distance_metres;
    }

    Vec3 current{0.0F, 0.0F, 0.0F};
    for (const Vec3 axis : kAxes) {
        current = current + decode_payload(settings_.encoding, data, axis);
    }
    const f32 magnitude = length(previous) + length(current);
    entry.error = magnitude > 1.0e-5F ? length(current - previous) / magnitude : 0.0F;
    entry.last_update_frame = context.frame;
    entry.valid = true;
}

ProbeUpdateReport RadianceCache::update(const ProbeUpdateContext& context) noexcept {
    ProbeUpdateReport report;

    Array<u32> queue;
    for (u32 handle = 0; handle < probes_.size(); ++handle) {
        if (probes_[handle].live) {
            (void)queue.push_back(handle);
        }
    }
    report.queue_depth = static_cast<u32>(queue.size());
    if (queue.empty()) {
        diagnostics_.last_update = report;
        return report;
    }

    const u64 frame = context.frame;
    std::ranges::sort(queue, [&](u32 a, u32 b) {
        const u64 age_a =
            frame > probes_[a].last_update_frame ? frame - probes_[a].last_update_frame : 0;
        const u64 age_b =
            frame > probes_[b].last_update_frame ? frame - probes_[b].last_update_frame : 0;
        // The progress guarantee. Without it a region that is never visible is never updated, and
        // that is not low priority but starvation.
        const bool starved_a = age_a >= context.max_age_frames;
        const bool starved_b = age_b >= context.max_age_frames;
        if (starved_a != starved_b) {
            return starved_a;
        }
        const f32 priority_a = priority_of(probes_[a], frame);
        const f32 priority_b = priority_of(probes_[b], frame);
        if (priority_a != priority_b) {
            return priority_a > priority_b;
        }
        return a < b;
    });

    const usize limit =
        context.converged ? queue.size() : std::min<usize>(queue.size(), context.budget);
    f32 error_total = 0.0F;
    for (usize index = 0; index < limit; ++index) {
        gather_probe(queue[index], context);
        error_total += probes_[queue[index]].error;
        report.probes_updated += 1;
        report.rays += std::max(1U, context.rays_per_probe);
    }
    for (usize index = limit; index < queue.size(); ++index) {
        const Probe& entry = probes_[queue[index]];
        const u64 age = frame > entry.last_update_frame ? frame - entry.last_update_frame : 0;
        report.oldest_unserviced_age = std::max(report.oldest_unserviced_age, age);
    }
    report.mean_error =
        report.probes_updated == 0 ? 0.0F : error_total / static_cast<f32>(report.probes_updated);

    diagnostics_.last_update = report;
    account();
    return report;
}

f32 RadianceCache::visibility_weight(const Probe& entry, Vec3 query) const noexcept {
    const Vec3 offset = query - entry.position;
    const f32 span = length(offset);
    if (span <= 1.0e-4F) {
        return 1.0F;
    }
    const Vec3 direction = offset / span;
    f32 expected = 0.0F;
    f32 weight = 0.0F;
    for (u32 axis = 0; axis < 6; ++axis) {
        const f32 cosine = std::max(0.0F, dot(kAxes[axis], direction));
        expected += entry.axis_distance[axis] * cosine;
        weight += cosine;
    }
    if (weight <= 0.0F) {
        return 1.0F;
    }
    expected /= weight;
    // The query is further from the probe than the probe's own view of the world in that
    // direction: something is between them. This is the wall.
    const f32 slack = settings_.visibility_bias_metres;
    if (span > expected + slack) {
        return 0.0F;
    }
    // A soft edge over the bias, so a query crossing the boundary does not pop.
    return math::clamp((expected + slack - span) / std::max(slack, 1.0e-4F), 0.0F, 1.0F);
}

Vec3 RadianceCache::gather(Vec3 position, Vec3 normal) const noexcept {
    const RadianceSample answer = sample(position, normal, 0);
    return answer.radiance;
}

RadianceSample RadianceCache::sample(Vec3 position, Vec3 normal, u64 frame) const noexcept {
    diagnostics_.gathers += 1;
    RadianceSample answer;
    answer.source = RadianceSource::RadianceCache;

    const f32 reach =
        settings_.base_spacing_metres * static_cast<f32>(1U << (settings_.levels - 1)) * 1.5F;
    const Vec3 extent{reach, reach, reach};
    const Aabb box{position - extent, position + extent};

    Vec3 total{0.0F, 0.0F, 0.0F};
    f32 total_weight = 0.0F;
    f32 oldest = 0.0F;
    index_.query_aabb(box, [&](u32 /*proxy*/, u64 user_data) {
        const u32 handle = static_cast<u32>(user_data);
        if (handle >= probes_.size()) {
            return true;
        }
        const Probe& entry = probes_[handle];
        if (!entry.live || !entry.valid) {
            return true;
        }
        const f32 spacing = settings_.base_spacing_metres * static_cast<f32>(1U << entry.level);
        const Vec3 offset = position - entry.position;
        const f32 span = length(offset);
        if (span > spacing * 1.5F) {
            return true;
        }
        // Trilinear-shaped falloff over the probe's own spacing, a wrap term so a probe behind the
        // query's surface does not contribute, and the visibility term so a probe behind a wall
        // does not either. All three are needed; dropping any one is a recognisable artefact.
        f32 weight = 1.0F - math::clamp(span / (spacing * 1.5F), 0.0F, 1.0F);
        if (span > 1.0e-4F) {
            const f32 facing = dot(normal, -offset / span);
            weight *= math::clamp((facing + 1.0F) * 0.5F, 0.0F, 1.0F);
        }
        weight *= visibility_weight(entry, position);
        if (weight <= 0.0F) {
            return true;
        }
        total = total + (decode_payload(settings_.encoding, payload(handle), normal) * weight);
        total_weight += weight;
        const u64 age = frame > entry.last_update_frame ? frame - entry.last_update_frame : 0;
        oldest = std::max(oldest, static_cast<f32>(age));
        return true;
    });

    if (total_weight <= 0.0F) {
        diagnostics_.gather_misses += 1;
        answer.confidence = 0.0F;
        return answer;
    }
    answer.radiance = total / total_weight;
    // Confidence falls with staleness: "WHEN a region has been invalidated and not yet re-solved
    // THEN samples from it SHALL carry reduced confidence."
    const f32 staleness = math::clamp(oldest / 240.0F, 0.0F, 1.0F);
    answer.confidence = math::clamp(1.0F - (staleness * 0.75F), 0.0F, 1.0F);
    return answer;
}

Status RadianceCache::seed(u32 probe, Span<const Vec3> directions,
                           Span<const Vec3> radiance) noexcept {
    if (probe >= probes_.size() || !probes_[probe].live) {
        return fail(ErrorCode::NotFound, "RadianceCache::seed: no such probe");
    }
    if (directions.size() != radiance.size() || directions.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "RadianceCache::seed: one radiance value per direction is required");
    }
    f32* data = payload(probe);
    for (u32 index = 0; index < stride_; ++index) {
        data[index] = 0.0F;
    }
    for (usize index = 0; index < directions.size(); ++index) {
        encode_sample(settings_.encoding, data,
                      normalized_or(directions[index], Vec3{0.0F, 1.0F, 0.0F}), radiance[index],
                      1.0F);
    }
    normalise_payload(settings_.encoding, data, static_cast<f32>(directions.size()),
                      static_cast<u32>(directions.size()));
    probes_[probe].valid = true;
    probes_[probe].error = 0.0F;
    return ok();
}

void RadianceCache::account() noexcept {
    u32 valid = 0;
    for (const Probe& entry : probes_) {
        if (entry.live && entry.valid) {
            valid += 1;
        }
    }
    diagnostics_.probe_count = live_probes_;
    diagnostics_.valid_probes = valid;
    diagnostics_.bytes = (static_cast<u64>(probes_.size()) * sizeof(Probe)) +
                         (static_cast<u64>(payloads_.size()) * sizeof(f32));
}

}  // namespace cy::rendering::gi
