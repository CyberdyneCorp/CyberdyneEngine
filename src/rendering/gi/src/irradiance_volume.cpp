// SPDX-License-Identifier: MIT
// Irradiance volumes. See irradiance_volume.h for what is here and what is not.

#include <cy/rendering/gi/irradiance_volume.h>
#include <cy/rendering/gi/radiance_cache.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace cy::rendering::gi {
namespace {

constexpr Vec3 kAxes[6] = {Vec3{1.0F, 0.0F, 0.0F}, Vec3{-1.0F, 0.0F, 0.0F},
                           Vec3{0.0F, 1.0F, 0.0F}, Vec3{0.0F, -1.0F, 0.0F},
                           Vec3{0.0F, 0.0F, 1.0F}, Vec3{0.0F, 0.0F, -1.0F}};

/// Where a capture ray starts, off the probe, so a probe placed exactly on a surface does not see
/// that surface at distance zero in every direction.
constexpr f32 kRayStartMetres = 1.0e-3F;
/// The largest stored coefficient, after the layout's scale, that the packed texture will hold. A
/// half float reaches 65504; this leaves room for the device's own rounding.
constexpr f32 kLargestStoredCoefficient = 16384.0F;
/// The backface factor's floor. DDGI's value: a probe behind the surface still contributes a
/// little, so a surface whose every neighbour is behind it is dimmed rather than black.
constexpr f32 kBackfaceFloor = 0.2F;
constexpr f32 kWeightEpsilon = 1.0e-6F;

/// The i-th of `count` directions on the Fibonacci sphere. Near-uniform, deterministic, and the
/// same set on every capture.
Vec3 fibonacci_direction(u32 index, u32 count) noexcept {
    const f32 golden = std::numbers::pi_v<f32> * (3.0F - std::sqrt(5.0F));
    const f32 z = 1.0F - (((2.0F * static_cast<f32>(index)) + 1.0F) / static_cast<f32>(count));
    const f32 radius = std::sqrt(std::max(0.0F, 1.0F - (z * z)));
    const f32 phi = golden * static_cast<f32>(index);
    return Vec3{radius * std::cos(phi), radius * std::sin(phi), z};
}

/// The radiance one capture ray brings back, and whether it hit the inside of something.
Vec3 ray_radiance(const VolumeCaptureContext& context, Vec3 origin, Vec3 direction,
                  f32 max_distance, bool& backface) noexcept {
    backface = false;
    SceneHit hit;
    if (context.tracer == nullptr || !context.tracer->trace(origin, direction, max_distance, hit) ||
        !hit.hit) {
        return context.sky.radiance(direction);
    }
    if (dot(hit.normal, direction) > 0.0F) {
        // The ray left through a surface's back: the probe is inside something.
        backface = true;
        return Vec3{0.0F, 0.0F, 0.0F};
    }
    Vec3 radiance{0.0F, 0.0F, 0.0F};
    u32 age = 0;
    if (context.radiance == nullptr ||
        !context.radiance->radiance_at(hit.position, hit.normal, radiance, age)) {
        return Vec3{0.0F, 0.0F, 0.0F};
    }
    return radiance;
}

f32 axis_distance(const VolumeCaptureContext& context, Vec3 origin, Vec3 axis,
                  f32 max_distance) noexcept {
    SceneHit hit;
    if (context.tracer == nullptr || !context.tracer->trace(origin, axis, max_distance, hit) ||
        !hit.hit) {
        return max_distance;
    }
    return std::min(hit.t + kRayStartMetres, max_distance);
}

/// A grid coordinate's cell and the fraction across it, for one axis of `count` probes.
struct AxisCell {
    u32 base = 0;
    f32 fraction = 0.0F;
};

AxisCell locate(f32 grid, u32 count) noexcept {
    const f32 last = static_cast<f32>(count - 1U);
    const f32 clamped = std::clamp(grid, 0.0F, last);
    AxisCell cell;
    if (count == 1U) {
        return cell;
    }
    cell.base = std::min(static_cast<u32>(std::floor(clamped)), count - 2U);
    cell.fraction = clamped - static_cast<f32>(cell.base);
    return cell;
}

/// How far outside the grid a coordinate is, in cells.
f32 outside_by(f32 grid, u32 count) noexcept {
    return std::max({0.0F, -grid, grid - static_cast<f32>(count - 1U)});
}

/// One probe's `kVolumeTexelsPerProbe` texels: one channel's four coefficients in each of the first
/// three, so the shader's evaluation is a dot product per channel, then validity and the six axis
/// distances.
void write_probe_texels(const VolumeProbe& probe, f32 inverse_scale, f32* texel) noexcept {
    for (u32 channel = 0; channel < 3U; ++channel) {
        for (u32 coefficient = 0; coefficient < 4U; ++coefficient) {
            texel[(channel * 4U) + coefficient] =
                probe.payload[(coefficient * 3U) + channel] * inverse_scale;
        }
    }
    texel[12] = probe.validity;
    for (u32 axis = 0; axis < 6U; ++axis) {
        texel[13U + axis] = probe.axis_distance[axis];
    }
    texel[19] = 0.0F;
}

}  // namespace

IrradianceVolume::IrradianceVolume() noexcept = default;
IrradianceVolume::~IrradianceVolume() = default;

Status IrradianceVolume::configure(const IrradianceVolumeSettings& settings) noexcept {
    if (settings.count_x == 0 || settings.count_y == 0 || settings.count_z == 0 ||
        settings.spacing_metres <= 0.0F || settings.rays_per_probe == 0 ||
        settings.max_ray_distance_metres <= 0.0F || settings.visibility_slack_metres <= 0.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "IrradianceVolume::configure: counts, spacing, rays, reach and the visibility "
                    "slack must all be positive");
    }
    settings_ = settings;
    const usize count = static_cast<usize>(settings.count_x) * settings.count_y * settings.count_z;
    probes_.clear();
    if (Status sized = probes_.resize(count); !sized) {
        return sized;
    }
    queue_.clear();
    if (Status reserved = queue_.reserve(count); !reserved) {
        return reserved;
    }
    for (u32 z = 0; z < settings.count_z; ++z) {
        for (u32 y = 0; y < settings.count_y; ++y) {
            for (u32 x = 0; x < settings.count_x; ++x) {
                const u32 index = probe_index(x, y, z);
                VolumeProbe& probe = probes_[index];
                probe = VolumeProbe{};
                probe.position = settings.origin + (Vec3{static_cast<f32>(x), static_cast<f32>(y),
                                                         static_cast<f32>(z)} *
                                                    settings.spacing_metres);
                probe.queued = true;
                (void)queue_.push_back(index);
            }
        }
    }
    queue_head_ = 0;
    queue_depth_ = static_cast<u32>(count);
    refresh_cursor_ = 0;

    directions_.clear();
    if (Status sized = directions_.resize(settings.rays_per_probe); !sized) {
        return sized;
    }
    for (u32 index = 0; index < settings.rays_per_probe; ++index) {
        directions_[index] = fibonacci_direction(index, settings.rays_per_probe);
    }
    generation_ += 1;
    return ok();
}

u32 IrradianceVolume::probe_index(u32 x, u32 y, u32 z) const noexcept {
    return x + (settings_.count_x * (y + (settings_.count_y * z)));
}

void IrradianceVolume::capture_probe(VolumeProbe& probe, const VolumeCaptureContext& context,
                                     u64& rays) const noexcept {
    for (f32& value : probe.payload) {
        value = 0.0F;
    }
    u32 backfaces = 0;
    for (const Vec3 direction : directions_) {
        bool backface = false;
        const Vec3 radiance = ray_radiance(context, probe.position + (direction * kRayStartMetres),
                                           direction, settings_.max_ray_distance_metres, backface);
        backfaces += backface ? 1U : 0U;
        encode_sample(ProbeEncoding::SphericalHarmonicsL1, probe.payload, direction, radiance,
                      1.0F);
    }
    const auto count = static_cast<u32>(directions_.size());
    normalise_payload(ProbeEncoding::SphericalHarmonicsL1, probe.payload, static_cast<f32>(count),
                      count);
    for (u32 axis = 0; axis < 6U; ++axis) {
        probe.axis_distance[axis] =
            axis_distance(context, probe.position, kAxes[axis], settings_.max_ray_distance_metres);
    }
    rays += count + 6U;
    const f32 inside = static_cast<f32>(backfaces) / static_cast<f32>(count);
    probe.validity = inside > settings_.inside_fraction ? 0.0F : 1.0F;
    probe.captured = true;
    probe.captured_frame = context.frame;
}

void IrradianceVolume::commit(u32 index, const VolumeProbe& captured) noexcept {
    const bool queued = probes_[index].queued;
    probes_[index] = captured;
    probes_[index].queued = queued;
    generation_ += 1;
}

VolumeUpdateReport IrradianceVolume::capture_all(const VolumeCaptureContext& context) noexcept {
    VolumeUpdateReport report;
    // STAGED, THEN COMMITTED. A lookup that feeds the volume back into itself would otherwise see
    // the probes this pass already replaced, and the bake would depend on the order of the grid.
    Array<VolumeProbe> staged;
    if (!staged.resize(probes_.size())) {
        return report;
    }
    for (usize index = 0; index < probes_.size(); ++index) {
        staged[index] = probes_[index];
        capture_probe(staged[index], context, report.rays);
    }
    for (usize index = 0; index < probes_.size(); ++index) {
        staged[index].queued = false;
        probes_[index] = staged[index];
    }
    queue_.clear();
    queue_head_ = 0;
    queue_depth_ = 0;
    generation_ += 1;
    report.probes_captured = static_cast<u32>(probes_.size());
    report.invalid_probes = count_invalid();
    return report;
}

u32 IrradianceVolume::invalidate(const Aabb& region) noexcept {
    // A probe's influence reaches one cell in every direction, so the region is widened by one
    // spacing: a probe whose cell touches the change is a probe whose answer the change moves.
    const Vec3 reach{settings_.spacing_metres, settings_.spacing_metres, settings_.spacing_metres};
    const Aabb widened = Aabb::from_min_max(region.min - reach, region.max + reach);
    u32 queued = 0;
    for (usize index = 0; index < probes_.size(); ++index) {
        VolumeProbe& probe = probes_[index];
        const Vec3 p = probe.position;
        const bool inside = p.x >= widened.min.x && p.x <= widened.max.x && p.y >= widened.min.y &&
                            p.y <= widened.max.y && p.z >= widened.min.z && p.z <= widened.max.z;
        if (!inside || probe.queued) {
            continue;
        }
        if (!queue_.push_back(static_cast<u32>(index))) {
            break;
        }
        probe.queued = true;
        queued += 1;
    }
    queue_depth_ += queued;
    return queued;
}

u32 IrradianceVolume::capture_queued(const VolumeCaptureContext& context, u32 budget,
                                     u64& rays) noexcept {
    u32 captured = 0;
    while (captured < budget && queue_head_ < queue_.size()) {
        const u32 index = queue_[queue_head_];
        queue_head_ += 1;
        VolumeProbe probe = probes_[index];
        capture_probe(probe, context, rays);
        commit(index, probe);
        probes_[index].queued = false;
        captured += 1;
    }
    if (queue_head_ >= queue_.size()) {
        queue_.clear();
        queue_head_ = 0;
    }
    queue_depth_ = static_cast<u32>(queue_.size() - queue_head_);
    return captured;
}

u32 IrradianceVolume::refresh_stalest(const VolumeCaptureContext& context, u32 budget,
                                      u64& rays) noexcept {
    // Round-robin over the grid: with no probe queued, the one the cursor names is the one captured
    // longest ago, because every probe before it was captured more recently in this same sweep.
    const auto count = static_cast<u32>(probes_.size());
    u32 captured = 0;
    while (captured < budget && captured < count) {
        const u32 index = refresh_cursor_ % count;
        refresh_cursor_ = (index + 1U) % count;
        VolumeProbe probe = probes_[index];
        capture_probe(probe, context, rays);
        commit(index, probe);
        captured += 1;
    }
    return captured;
}

VolumeUpdateReport IrradianceVolume::update(const VolumeCaptureContext& context) noexcept {
    VolumeUpdateReport report;
    const u32 budget = settings_.probes_per_update;
    report.probes_captured = capture_queued(context, budget, report.rays);
    if (settings_.policy == VolumeUpdatePolicy::Amortised && report.probes_captured < budget) {
        report.probes_captured +=
            refresh_stalest(context, budget - report.probes_captured, report.rays);
    }
    report.queue_depth = queue_depth_;
    report.invalid_probes = count_invalid();
    return report;
}

u32 IrradianceVolume::count_invalid() const noexcept {
    u32 invalid = 0;
    for (const VolumeProbe& probe : probes_) {
        invalid += probe.validity <= 0.0F ? 1U : 0U;
    }
    return invalid;
}

Vec3 IrradianceVolume::probe_radiance(u32 probe, Vec3 normal) const noexcept {
    return decode_payload(ProbeEncoding::SphericalHarmonicsL1, probes_[probe].payload, normal);
}

f32 IrradianceVolume::visibility(const VolumeProbe& probe, Vec3 position, Vec3 normal,
                                 Vec3 query) const noexcept {
    // BACKFACE: a probe behind the surface's own plane saw the other side of it. Attenuated, never
    // zeroed — see kBackfaceFloor.
    const Vec3 to_probe = normalized_or(probe.position - position, normal);
    const f32 facing = std::max(1.0e-4F, (dot(to_probe, normal) + 1.0F) * 0.5F);
    const f32 backface = (facing * facing) + kBackfaceFloor;

    // DISTANCE: the six axis distances bound a box of free space around the probe, and a query
    // outside that box, on any axis, is behind something. Per axis rather than averaged over the
    // axes by cosine: an average lets the open sky above a probe outvote the wall beside it, which
    // is precisely the leak this term exists to stop.
    const Vec3 offset = query - probe.position;
    const f32 components[3] = {offset.x, offset.y, offset.z};
    const f32 slack = settings_.visibility_slack_metres;
    f32 reach = 1.0F;
    for (u32 axis = 0; axis < 3U; ++axis) {
        const f32 component = components[axis];
        const f32 free = probe.axis_distance[(axis * 2U) + (component >= 0.0F ? 0U : 1U)];
        reach =
            std::min(reach, std::clamp((free + slack - std::fabs(component)) / slack, 0.0F, 1.0F));
    }
    return backface * reach;
}

VolumeSample IrradianceVolume::sample(Vec3 position, Vec3 normal) const noexcept {
    VolumeSample answer;
    if (probes_.empty()) {
        return answer;
    }
    const Vec3 query = position + (normal * settings_.normal_offset_metres);
    const Vec3 grid = (query - settings_.origin) / settings_.spacing_metres;
    const u32 counts[3] = {settings_.count_x, settings_.count_y, settings_.count_z};
    const f32 coordinates[3] = {grid.x, grid.y, grid.z};
    f32 outside = 0.0F;
    AxisCell cells[3];
    for (u32 axis = 0; axis < 3U; ++axis) {
        outside = std::max(outside, outside_by(coordinates[axis], counts[axis]));
        cells[axis] = locate(coordinates[axis], counts[axis]);
    }
    answer.coverage = std::clamp(1.0F - outside, 0.0F, 1.0F);

    Vec3 total{0.0F, 0.0F, 0.0F};
    for (u32 corner = 0; corner < 8U; ++corner) {
        u32 index[3] = {};
        f32 trilinear = 1.0F;
        for (u32 axis = 0; axis < 3U; ++axis) {
            const u32 step = (corner >> axis) & 1U;
            index[axis] = std::min(cells[axis].base + step, counts[axis] - 1U);
            trilinear *= step != 0U ? cells[axis].fraction : 1.0F - cells[axis].fraction;
        }
        const VolumeProbe& probe = probes_[probe_index(index[0], index[1], index[2])];
        const f32 weight = trilinear * probe.validity;
        if (weight <= 0.0F) {
            continue;
        }
        const f32 visible = weight * visibility(probe, position, normal, query);
        total =
            total +
            (decode_payload(ProbeEncoding::SphericalHarmonicsL1, probe.payload, normal) * visible);
        answer.weight += visible;
    }
    if (answer.weight > kWeightEpsilon) {
        answer.radiance = total / answer.weight;
    }
    return answer;
}

Vec3 IrradianceVolume::gather(Vec3 position, Vec3 normal) const noexcept {
    return sample(position, normal).radiance;
}

Vec3 IrradianceVolume::ambient(Vec3 position, Vec3 normal, Vec3 flat) const noexcept {
    const VolumeSample answer = sample(position, normal);
    const Vec3 volume = answer.weight > kWeightEpsilon ? answer.radiance : flat;
    return lerp(flat, volume, answer.coverage);
}

VolumeTextureLayout IrradianceVolume::texture_layout() const noexcept {
    VolumeTextureLayout layout;
    layout.width = settings_.count_x * kVolumeTexelsPerProbe;
    layout.height = settings_.count_y * settings_.count_z;
    f32 largest = 0.0F;
    for (const VolumeProbe& probe : probes_) {
        for (const f32 value : probe.payload) {
            largest = std::max(largest, std::fabs(value));
        }
    }
    layout.coefficient_scale =
        largest > kLargestStoredCoefficient ? largest / kLargestStoredCoefficient : 1.0F;
    return layout;
}

Status IrradianceVolume::pack_texels(Span<f32> out) const noexcept {
    const VolumeTextureLayout layout = texture_layout();
    const usize wanted = static_cast<usize>(layout.width) * layout.height * 4U;
    if (out.size() != wanted) {
        return fail(ErrorCode::InvalidArgument,
                    "IrradianceVolume::pack_texels: the output is not width * height * 4 floats");
    }
    const f32 inverse = 1.0F / layout.coefficient_scale;
    for (u32 z = 0; z < settings_.count_z; ++z) {
        for (u32 y = 0; y < settings_.count_y; ++y) {
            for (u32 x = 0; x < settings_.count_x; ++x) {
                const usize row =
                    static_cast<usize>(y) + (static_cast<usize>(settings_.count_y) * z);
                const usize first =
                    (row * layout.width) + (static_cast<usize>(x) * kVolumeTexelsPerProbe);
                write_probe_texels(probes_[probe_index(x, y, z)], inverse,
                                   out.data() + (first * 4U));
            }
        }
    }
    return ok();
}

}  // namespace cy::rendering::gi
