// The weather map, the reconstruction, the march, and the cloud half of temporal reprojection.

#include <cy/rendering/sky/clouds.h>

#include <cy/core/math/math.h>
#include <cy/rendering/post/effects.h>

#include "internal.h"

#include <cmath>

namespace cy::rendering::sky {

using detail::entry_distance;
using detail::exit_distance;

namespace {

// ================================================================================================
// NOISE — TRANSLITERATED FROM `cy/noise.slang`, NOT INVENTED HERE
// ================================================================================================
//
// `src/rendering/shaders/cy/noise.slang` already defines the engine's hash and value noise, and it
// says why they are integer hashes rather than sin-based ones: "a sin-based hash produces different
// values on different drivers, and `rendering-architecture` requires the frame to be reproducible."
//
// The arithmetic below is that module's, operation for operation, so that the day a cloud shader is
// written the CPU and the GPU reconstruct THE SAME CLOUD rather than two clouds that look alike.
// The one addition is the seed: `valueNoise()` has no seed because a shader's noise is global,
// and a world's clouds have to differ between two seeds, so the seed offsets the lattice.

[[nodiscard]] u32 hash_pcg(u32 value) noexcept {
    const u32 state = (value * 747796405U) + 2891336453U;
    const u32 word = ((state >> ((state >> 28U) + 4U)) ^ state) * 277803737U;
    return (word >> 22U) ^ word;
}

struct U32x3 {
    u32 x = 0;
    u32 y = 0;
    u32 z = 0;
};

[[nodiscard]] U32x3 hash_pcg3d(U32x3 value) noexcept {
    U32x3 v{(value.x * 1664525U) + 1013904223U, (value.y * 1664525U) + 1013904223U,
            (value.z * 1664525U) + 1013904223U};
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v.x ^= v.x >> 16U;
    v.y ^= v.y >> 16U;
    v.z ^= v.z >> 16U;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return v;
}

/// Value noise over a 3D lattice, smoothstep-interpolated. `cy/noise.slang`'s `valueNoise()` with
/// the seed folded into the lattice origin.
[[nodiscard]] f32 value_noise(Vec3 position, u32 seed) noexcept {
    const Vec3 cell{std::floor(position.x), std::floor(position.y), std::floor(position.z)};
    const Vec3 fraction = position - cell;
    const Vec3 weight{fraction.x * fraction.x * (3.0F - (2.0F * fraction.x)),
                      fraction.y * fraction.y * (3.0F - (2.0F * fraction.y)),
                      fraction.z * fraction.z * (3.0F - (2.0F * fraction.z))};
    // +1024 keeps the lattice index non-negative for the worlds this is used in, exactly as the
    // shader does; the seed is mixed in on top of it so two worlds get two skies.
    const U32x3 base{static_cast<u32>(static_cast<i32>(cell.x) + 1024) + seed,
                     static_cast<u32>(static_cast<i32>(cell.y) + 1024) + hash_pcg(seed),
                     static_cast<u32>(static_cast<i32>(cell.z) + 1024) + hash_pcg(seed + 1U)};

    f32 result = 0.0F;
    for (u32 corner = 0; corner < 8U; ++corner) {
        const U32x3 offset{corner & 1U, (corner >> 1U) & 1U, (corner >> 2U) & 1U};
        const U32x3 hashed =
            hash_pcg3d(U32x3{base.x + offset.x, base.y + offset.y, base.z + offset.z});
        const f32 sample = static_cast<f32>(hashed.x >> 8U) * (1.0F / 16777216.0F);
        const f32 bx = offset.x != 0U ? weight.x : 1.0F - weight.x;
        const f32 by = offset.y != 0U ? weight.y : 1.0F - weight.y;
        const f32 bz = offset.z != 0U ? weight.z : 1.0F - weight.z;
        result += sample * bx * by * bz;
    }
    return result;
}

/// Fractal sum. Amplitude halves and frequency doubles, and the result is normalised so that an
/// fbm of one octave and an fbm of four have the same range — which is what makes the octave count
/// a DRAWING lever rather than an exposure change.
[[nodiscard]] f32 fbm(Vec3 position, u32 octaves, u32 seed) noexcept {
    f32 sum = 0.0F;
    f32 amplitude = 1.0F;
    f32 total = 0.0F;
    Vec3 sample = position;
    for (u32 octave = 0; octave < math::max(octaves, 1U); ++octave) {
        sum += value_noise(sample, seed + (octave * 7919U)) * amplitude;
        total += amplitude;
        amplitude *= 0.5F;
        sample = sample * 2.02F;  // not exactly 2, so the octaves' lattices do not line up
    }
    return sum / math::max(total, 1.0e-6F);
}

/// Rescale `value` from [low, high] into [0, 1], clamped.
///
/// THE INTERVAL MAY RUN BACKWARDS, and that is not a curiosity: `height_profile()` fades a layer's
/// top out with `remap01(h, 1.0F, 0.88F)`, which is a DESCENDING interval on purpose. An earlier
/// version guarded the divisor with `max(high - low, 1e-5)`, which turns a span of -0.12 into
/// +1e-5, sends every descending remap to zero, and produces a sky with no clouds in it anywhere —
/// which is exactly what it did, and what `test_clouds.cpp`'s reconstruction case caught. The guard
/// has to protect against a span of zero without changing its SIGN.
[[nodiscard]] f32 remap01(f32 value, f32 low, f32 high) noexcept {
    const f32 span = high - low;
    const f32 floor = span < 0.0F ? -1.0e-5F : 1.0e-5F;
    const f32 safe = std::fabs(span) >= 1.0e-5F ? span : floor;
    return math::saturate((value - low) / safe);
}

/// The height profile: what fraction of the layer's density exists at a fraction of its thickness.
///
/// Interpolated by TYPE rather than switched on it, because a storm front is a place where the type
/// changes across a few kilometres and a switch would put a seam there. A stratus is a slab with
/// soft faces; a cumulonimbus is narrow at the base, widest in the middle third and anvil-flat at
/// the top.
[[nodiscard]] f32 height_profile(f32 height_fraction, f32 type) noexcept {
    const f32 h = math::saturate(height_fraction);
    const f32 stratus = remap01(h, 0.0F, 0.08F) * remap01(h, 1.0F, 0.88F);
    const f32 cumulus = remap01(h, 0.0F, 0.28F) * remap01(h, 1.0F, 0.62F);
    return math::lerp(stratus, cumulus, math::saturate(type));
}

/// How much water sits at a height. A cumulus carries most of its mass high; a stratus is even.
[[nodiscard]] f32 density_profile(f32 height_fraction, f32 type) noexcept {
    const f32 h = math::saturate(height_fraction);
    return math::lerp(1.0F, 0.25F + (1.35F * h), math::saturate(type));
}

[[nodiscard]] u8 quantise(f32 value) noexcept {
    // `lround` rather than `+ 0.5F` and a cast: the latter rounds a value already exactly on a
    // half in whichever direction the double conversion happened to land, and the boundary between
    // two adjacent coverage values is precisely where that shows.
    return static_cast<u8>(std::lround(math::clamp(value, 0.0F, 1.0F) * 255.0F));
}

[[nodiscard]] f32 dequantise(u8 value) noexcept {
    return static_cast<f32>(value) * (1.0F / 255.0F);
}

/// Wrap a cell coordinate into the map. See the class comment: a sky has no edge.
[[nodiscard]] u32 wrap_cell(i32 value, u32 count) noexcept {
    const auto extent = static_cast<i32>(count);
    i32 wrapped = value % extent;
    if (wrapped < 0) {
        wrapped += extent;
    }
    return static_cast<u32>(wrapped);
}

/// The multiple-scattering approximation inside a cloud: three octaves of (attenuation, phase
/// eccentricity, contribution), each cheaper and broader than the last.
///
/// It is the standard energy-conserving approximation and it is here rather than in the atmosphere
/// tables because the two are different physics: `MultipleScatteringTable` integrates a sphere of
/// thin air once per table entry, and a cloud's multiple scattering varies within one step of one
/// ray. What the two share is the honesty about what they are — neither claims to be a path trace.
[[nodiscard]] Vec3 cloud_scattering(f32 optical_depth_to_sun, f32 cos_theta, f32 anisotropy,
                                    Vec3 sun_illuminance, bool multiple) noexcept {
    Vec3 total{0.0F, 0.0F, 0.0F};
    f32 attenuation = 1.0F;
    f32 contribution = 1.0F;
    f32 eccentricity = anisotropy;
    const u32 octaves = multiple ? 3U : 1U;
    for (u32 octave = 0; octave < octaves; ++octave) {
        const f32 phase = henyey_greenstein(eccentricity, cos_theta);
        const f32 transmitted = std::exp(-optical_depth_to_sun * attenuation);
        total = total + (sun_illuminance * (transmitted * phase * contribution));
        attenuation *= 0.5F;
        contribution *= 0.5F;
        eccentricity *= 0.5F;
    }
    return total;
}

}  // namespace

// ================================================================================================
// CloudWeatherMap
// ================================================================================================

usize CloudWeatherMap::index_of(i32 x, i32 z) const noexcept {
    return (static_cast<usize>(wrap_cell(z, cells_)) * cells_) + wrap_cell(x, cells_);
}

Status CloudWeatherMap::configure(u32 cells_per_edge, f32 cell_metres) noexcept {
    if (cells_per_edge == 0) {
        return fail(ErrorCode::InvalidArgument, "CloudWeatherMap: a map needs at least one cell");
    }
    if (cell_metres < kMinimumCellMetres) {
        // The requirement is "coverage, type, and moisture OVER KILOMETRES". A map at tens of
        // metres is a stored volume with extra steps, which is the representation this one exists
        // to avoid, so it is refused here rather than discovered in a memory report.
        return fail(ErrorCode::InvalidArgument,
                    "CloudWeatherMap: a weather map is coarse by definition — a cell below 250 m "
                    "is a stored cloud volume, which is what the coverage map replaces");
    }
    cells_ = cells_per_edge;
    cell_metres_ = cell_metres;
    epoch_ = 0;
    const usize count = static_cast<usize>(cells_per_edge) * cells_per_edge;
    coverage_.clear();
    type_.clear();
    moisture_.clear();
    version_.clear();
    if (auto status = coverage_.resize(count); !status) {
        return status;
    }
    if (auto status = type_.resize(count); !status) {
        return status;
    }
    if (auto status = moisture_.resize(count); !status) {
        return status;
    }
    return version_.resize(count);
}

Status CloudWeatherMap::set(i32 x, i32 z, f32 coverage, f32 type, f32 moisture) noexcept {
    if (!configured()) {
        return fail(ErrorCode::Unavailable, "CloudWeatherMap::set: the map has no cells");
    }
    const usize index = index_of(x, z);
    coverage_[index] = quantise(coverage);
    type_[index] = quantise(type);
    moisture_[index] = quantise(moisture);
    ++version_[index];
    ++epoch_;
    return {};
}

Status CloudWeatherMap::generate(u64 seed, f32 base_coverage, f32 storminess) noexcept {
    if (!configured()) {
        return fail(ErrorCode::Unavailable, "CloudWeatherMap::generate: configure the map first");
    }
    const auto noise_seed = static_cast<u32>(seed ^ (seed >> 32U));
    const f32 kilometres = cell_metres_ / 1000.0F;
    for (u32 z = 0; z < cells_; ++z) {
        for (u32 x = 0; x < cells_; ++x) {
            // Two octaves at tens of kilometres: weather systems, not clouds. The clouds are the
            // reconstruction's job and this map has no business resolving them.
            const Vec3 position{static_cast<f32>(x) * kilometres * 0.04F, 0.0F,
                                static_cast<f32>(z) * kilometres * 0.04F};
            const f32 systems = fbm(position, 3, noise_seed);
            const f32 fronts = fbm(position * 0.35F, 2, noise_seed + 104729U);
            const f32 coverage = math::saturate(base_coverage + ((systems - 0.5F) * 0.9F));
            const f32 type = math::saturate((fronts * 0.6F) + (storminess * 0.7F));
            const f32 moisture = math::saturate((coverage * 0.6F) + (fronts * 0.4F));
            if (auto status =
                    set(static_cast<i32>(x), static_cast<i32>(z), coverage, type, moisture);
                !status) {
                return status;
            }
        }
    }
    return {};
}

CloudMapSample CloudWeatherMap::sample(f32 world_x, f32 world_z) const noexcept {
    CloudMapSample result;
    if (!configured()) {
        return result;
    }
    const f32 fx = (world_x / cell_metres_) - 0.5F;
    const f32 fz = (world_z / cell_metres_) - 0.5F;
    const auto x0 = static_cast<i32>(std::floor(fx));
    const auto z0 = static_cast<i32>(std::floor(fz));
    const f32 tx = fx - static_cast<f32>(x0);
    const f32 tz = fz - static_cast<f32>(z0);

    const usize i00 = index_of(x0, z0);
    const usize i10 = index_of(x0 + 1, z0);
    const usize i01 = index_of(x0, z0 + 1);
    const usize i11 = index_of(x0 + 1, z0 + 1);

    const auto blend = [&](const Array<u8>& channel) {
        const f32 a = math::lerp(dequantise(channel[i00]), dequantise(channel[i10]), tx);
        const f32 b = math::lerp(dequantise(channel[i01]), dequantise(channel[i11]), tx);
        return math::lerp(a, b, tz);
    };
    result.coverage = blend(coverage_);
    result.type = blend(type_);
    result.moisture = blend(moisture_);
    // NEAREST for the version: it is an identity, and an identity halfway between two identities
    // names no state. The nearest cell is the one a rejection should be attributed to.
    const usize nearest = index_of(x0 + (tx >= 0.5F ? 1 : 0), z0 + (tz >= 0.5F ? 1 : 0));
    result.version = version_[nearest];
    return result;
}

u64 CloudWeatherMap::bytes() const noexcept {
    const auto cells = static_cast<u64>(cells_) * cells_;
    return cells * (3U + sizeof(u16));
}

// ================================================================================================
// Layers
// ================================================================================================

const char* cloud_layer_kind_name(CloudLayerKind kind) noexcept {
    switch (kind) {
        case CloudLayerKind::Low:
            return "low";
        case CloudLayerKind::Middle:
            return "middle";
        case CloudLayerKind::High:
            return "high";
        case CloudLayerKind::Storm:
            return "storm";
        case CloudLayerKind::Project:
            return "project";
        case CloudLayerKind::Count:
            break;
    }
    return "unknown";
}

Status CloudLayerSet::add(const CloudLayer& layer) noexcept {
    if (count >= kMaxCloudLayers) {
        return fail(ErrorCode::OutOfRange,
                    "CloudLayerSet: eight layers is the declared maximum — four named kinds and "
                    "four a project may add");
    }
    if (!(layer.thickness > 0.0F)) {
        return fail(ErrorCode::InvalidArgument,
                    "CloudLayerSet: a layer with no thickness has no volume to reconstruct");
    }
    layers[count] = layer;
    ++count;
    return {};
}

f32 CloudLayerSet::lowest_base() const noexcept {
    f32 lowest = 1.0e9F;
    for (u32 index = 0; index < count; ++index) {
        if (layers[index].enabled) {
            lowest = math::min(lowest, layers[index].base_altitude);
        }
    }
    return lowest;
}

f32 CloudLayerSet::highest_top() const noexcept {
    f32 highest = 0.0F;
    for (u32 index = 0; index < count; ++index) {
        if (layers[index].enabled) {
            highest = math::max(highest, layers[index].base_altitude + layers[index].thickness);
        }
    }
    return highest;
}

CloudLayerSet default_cloud_layers() noexcept {
    CloudLayerSet set;
    CloudLayer low;
    low.name = "cumulus";
    low.kind = CloudLayerKind::Low;
    low.base_altitude = 1500.0F;
    low.thickness = 1400.0F;
    low.density = 0.055F;
    low.type = 0.45F;
    low.base_scale = 3200.0F;
    low.detail_scale = 240.0F;
    low.erosion = 0.38F;
    low.wind = Vec3{8.0F, 0.0F, 2.0F};
    (void)set.add(low);

    CloudLayer middle;
    middle.name = "altostratus";
    middle.kind = CloudLayerKind::Middle;
    middle.base_altitude = 4200.0F;
    middle.thickness = 900.0F;
    middle.density = 0.028F;
    middle.type = 0.12F;
    middle.base_scale = 6000.0F;
    middle.detail_scale = 420.0F;
    middle.erosion = 0.2F;
    middle.wind = Vec3{12.0F, 0.0F, 3.0F};
    (void)set.add(middle);

    CloudLayer high;
    high.name = "cirrus";
    high.kind = CloudLayerKind::High;
    high.base_altitude = 8000.0F;
    high.thickness = 700.0F;
    high.density = 0.010F;
    high.type = 0.05F;
    high.base_scale = 9000.0F;
    high.detail_scale = 700.0F;
    high.erosion = 0.55F;
    high.wind = Vec3{22.0F, 0.0F, 5.0F};
    (void)set.add(high);

    CloudLayer storm;
    storm.name = "cumulonimbus";
    storm.kind = CloudLayerKind::Storm;
    storm.base_altitude = 1200.0F;
    storm.thickness = 7000.0F;
    storm.density = 0.11F;
    storm.type = 1.0F;
    storm.base_scale = 5200.0F;
    storm.detail_scale = 300.0F;
    storm.erosion = 0.3F;
    storm.wind = Vec3{10.0F, 0.0F, 2.5F};
    // Present and empty until the weather asks for it: `drive_cloud_layers()` raises its coverage
    // with `storm_intensity`, and a project that never has storms pays for one disabled layer.
    storm.coverage = 0.0F;
    (void)set.add(storm);
    return set;
}

void drive_cloud_layers(const CloudWeatherState& state, CloudLayerSet& layers) noexcept {
    for (u32 index = 0; index < layers.count; ++index) {
        CloudLayer& layer = layers.layers[index];

        // WIND: every layer's own, from one state plus the declared shear. Layers therefore shear
        // past each other without a project authoring three wind vectors, and the shear is a
        // weather parameter rather than a per-layer constant somebody has to keep consistent.
        const f32 kilometres = (layer.base_altitude + (layer.thickness * 0.5F)) / 1000.0F;
        layer.wind = state.wind * (1.0F + (state.shear_per_km * kilometres));

        switch (layer.kind) {
            case CloudLayerKind::Low:
                layer.coverage = math::saturate(0.15F + (state.humidity * 0.95F));
                layer.type = math::saturate(0.2F + (state.storm_intensity * 0.6F));
                break;
            case CloudLayerKind::Middle:
                layer.coverage = math::saturate(state.humidity * 0.8F);
                layer.type = math::saturate(0.1F + (state.storm_intensity * 0.3F));
                break;
            case CloudLayerKind::High:
                // Cirrus does not follow humidity at the surface: it follows the approach of a
                // front, which is what `storm_intensity` stands for here. A high deck thickening
                // hours before the storm arrives is the one cue a player reads as weather.
                layer.coverage = math::saturate(0.2F + (state.storm_intensity * 0.7F));
                break;
            case CloudLayerKind::Storm:
                layer.coverage = math::saturate((state.storm_intensity - 0.25F) * 1.6F);
                layer.density = 0.06F + (state.storm_intensity * 0.09F);
                break;
            case CloudLayerKind::Project:
            case CloudLayerKind::Count:
                // A project's layer keeps its authored coverage and type: this function knows what
                // "low" and "storm" mean and does not know what a project's layer is for.
                break;
        }
    }
}

// ================================================================================================
// Reconstruction
// ================================================================================================

CloudDensitySample cloud_density(const CloudField& field, Vec3 world_position, f64 time_seconds,
                                 u32 octaves) noexcept {
    CloudDensitySample result;
    if (field.map == nullptr || !field.map->configured()) {
        return result;
    }
    const f32 altitude = world_position.y;
    const auto seed = static_cast<u32>(field.seed ^ (field.seed >> 32U));

    f32 best_density = 0.0F;
    for (u32 index = 0; index < field.layers.count; ++index) {
        const CloudLayer& layer = field.layers.layers[index];
        if (!layer.enabled) {
            continue;
        }
        const f32 height_fraction = (altitude - layer.base_altitude) / layer.thickness;
        if (height_fraction < 0.0F || height_fraction > 1.0F) {
            continue;
        }

        // ADVECTION. The map and the noise are both sampled at a position offset by how far this
        // layer's wind has carried the air. Moving the sample rather than the cloud is what makes
        // the reconstruction a pure function of (position, time) with no per-frame state.
        const auto drift_x = static_cast<f32>(static_cast<f64>(layer.wind.x) * time_seconds);
        const auto drift_z = static_cast<f32>(static_cast<f64>(layer.wind.z) * time_seconds);
        const f32 sample_x = world_position.x - drift_x;
        const f32 sample_z = world_position.z - drift_z;

        const CloudMapSample map = field.map->sample(sample_x, sample_z);
        // STATE, and reported before anything that depends on the octave count touches it: the
        // coverage and the type a gameplay system would read are the map's, at every tier.
        if (map.coverage > result.coverage) {
            result.coverage = map.coverage;
            result.type = map.type;
            result.version = map.version;
        }

        const f32 coverage = math::saturate((map.coverage * layer.coverage) + field.coverage_bias);
        if (coverage <= 0.0F) {
            continue;
        }
        const f32 type = math::saturate((layer.type + map.type) * 0.5F);

        const Vec3 base_sample{sample_x / layer.base_scale, altitude / layer.base_scale,
                               sample_z / layer.base_scale};
        const f32 base_noise = fbm(base_sample, math::min(octaves, 2U), seed + index);
        const f32 profile = height_profile(height_fraction, type);
        f32 shape = remap01(base_noise * profile, 1.0F - coverage, 1.0F);

        if (octaves > 1U && shape > 0.0F) {
            // EROSION. The detail eats into the base shape rather than being added to it, which is
            // what makes a cumulus cauliflower instead of a blob with speckles. It bites hardest at
            // the bottom of the layer, where a real cloud frays.
            const Vec3 detail_sample{sample_x / layer.detail_scale, altitude / layer.detail_scale,
                                     sample_z / layer.detail_scale};
            const f32 detail = fbm(detail_sample, octaves - 1U, seed + index + 977U);
            const f32 bite = layer.erosion * (1.0F - (0.6F * height_fraction));
            shape = remap01(shape, detail * bite, 1.0F);
        }

        const f32 density = shape * layer.density * density_profile(height_fraction, type) *
                            (0.45F + (0.55F * map.moisture));
        result.density += density;
        if (density > best_density) {
            best_density = density;
            result.dominant_layer = index;
        }
    }
    return result;
}

// ================================================================================================
// The march
// ================================================================================================

namespace {

/// The optical depth from a point towards the sun, by a short march. Self-shadowing: the reason a
/// cumulus has a bright top and a dark base, and the single most expensive thing in a cloud
/// renderer, which is why `light_steps` is a declared lever.
///
/// The steps GROW: the first are short because the shadow's shape is set by the metres nearest the
/// sample, and the last are long because by then the march is only asking whether there is a cloud
/// somewhere above.
[[nodiscard]] f32 optical_depth_to_sun(const CloudField& field, Vec3 position, Vec3 sun,
                                       f64 time_seconds, const CloudQuality& quality,
                                       CloudMarchStats& stats) noexcept {
    // ONE OCTAVE FEWER than the view march. The shadow is an integral of the density over tens of
    // metres, so the finest octave of erosion averages out of it; paying for that octave once per
    // light step, at six light steps per view step, is the single largest avoidable cost in a cloud
    // renderer.
    const u32 light_octaves = quality.octaves > 1U ? quality.octaves - 1U : 1U;

    f32 depth = 0.0F;
    f32 step = 40.0F;
    Vec3 sample = position;
    for (u32 index = 0; index < quality.light_steps; ++index) {
        sample = sample + (sun * step);
        const CloudDensitySample cloud = cloud_density(field, sample, time_seconds, light_octaves);
        depth += cloud.density * step;
        ++stats.light_samples;
        step *= 1.75F;
    }
    return depth;
}

/// Where the cloud slab begins and ends along a ray, in metres from the eye.
struct SlabSpan {
    f32 start = 0.0F;
    f32 end = 0.0F;
    bool valid = false;
};

[[nodiscard]] SlabSpan cloud_slab_span(const Atmosphere& atmosphere, const CloudLayerSet& layers,
                                       Vec3 planet_relative, Vec3 direction) noexcept {
    SlabSpan span;
    const f32 base = layers.lowest_base();
    const f32 top = layers.highest_top();
    if (!(top > base)) {
        return span;
    }
    const f32 inner = atmosphere.planet_radius + base;
    const f32 outer = atmosphere.planet_radius + top;

    // A ray that meets the ground never reaches the clouds above it: the horizon is the boundary
    // and it is a ray-sphere test rather than a `direction.y > 0` one, because at altitude the
    // horizon is below the observer.
    if (entry_distance(planet_relative, direction, atmosphere.planet_radius) >= 0.0F) {
        return span;
    }

    const f32 to_inner = exit_distance(planet_relative, direction, inner);
    const f32 to_outer = exit_distance(planet_relative, direction, outer);
    if (to_outer <= 0.0F) {
        return span;
    }
    const f32 radius = length(planet_relative);
    if (radius < inner) {
        span.start = math::max(to_inner, 0.0F);  // below the deck, looking up through it
        span.end = to_outer;
    } else if (radius <= outer) {
        span.start = 0.0F;  // inside the deck
        span.end = to_outer;
    } else {
        // Above the deck: the ray has to come back down into it, which is the near root of the
        // outer shell, and it leaves through the inner one.
        const f32 down = entry_distance(planet_relative, direction, outer);
        if (down < 0.0F) {
            return span;
        }
        span.start = down;
        const f32 leave = entry_distance(planet_relative, direction, inner);
        span.end = leave >= 0.0F ? leave : to_outer;
    }
    span.valid = span.end > span.start;
    return span;
}

/// The farthest a cloud march runs, in metres. A ray along the horizon stays inside the slab for
/// hundreds of kilometres, and marching all of it spends the whole budget on air nobody can
/// resolve. Declared here rather than buried in the loop because it is a real limit on what the
/// renderer can show — beyond it, clouds fade into the aerial perspective, which is where they
/// belong.
inline constexpr f32 kMaxMarchMetres = 60000.0F;

}  // namespace

CloudMarchResult march_clouds(const Atmosphere& atmosphere, const AtmosphereTables& tables,
                              const CloudField& field, Vec3 view_position, Vec3 view_direction,
                              Vec3 sun_direction, Vec3 sun_illuminance, Vec3 ambient,
                              f64 time_seconds, const CloudQuality& quality) noexcept {
    CloudMarchResult result;
    if (field.map == nullptr || !field.map->configured() || field.layers.count == 0) {
        return result;
    }
    const Vec3 view = normalized_or(view_direction, Vec3{0.0F, 1.0F, 0.0F});
    const Vec3 sun = normalized_or(sun_direction, Vec3{0.0F, 1.0F, 0.0F});

    // The march runs in PLANET-RELATIVE metres for the geometry and in WORLD metres for the
    // reconstruction. `view_position` is world — its `y` is the altitude — and the planet-relative
    // position is that altitude lifted onto the planet's radius. Keeping them apart is what lets
    // the slab test be a ray-sphere intersection (correct at altitude and at the horizon) while the
    // cloud field stays addressed by the world coordinates the weather map is cooked in.
    const Vec3 planet_relative = ground_position(atmosphere, view_position.y);

    // THE LAYER LEVER IS APPLIED BEFORE THE SLAB IS MEASURED, and the order is the whole of what
    // the lever buys. A disabled layer is absent from `lowest_base()` and `highest_top()` too, so a
    // tier that draws only the low deck marches the fourteen hundred metres that deck occupies
    // rather than the eight kilometres up to the cirrus. Disabling the layers after computing the
    // span — which an earlier version of this function did — gives a tier that costs the same as
    // the one above it and looks worse, which is the wrong half of the trade.
    CloudField limited = field;
    for (u32 index = quality.max_layers; index < limited.layers.count; ++index) {
        limited.layers.layers[index].enabled = false;
    }

    const SlabSpan span = cloud_slab_span(atmosphere, limited.layers, planet_relative, view);
    if (!span.valid) {
        return result;
    }

    const f32 end = math::min(span.end, span.start + kMaxMarchMetres);
    const u32 steps = math::max(quality.steps, 4U);
    const f32 step = (end - span.start) / static_cast<f32>(steps);
    if (!(step > 0.0F)) {
        return result;
    }
    result.stats.marched_metres = end - span.start;

    const f32 cos_theta = dot(view, sun);

    f32 dominant_extinction = 0.0F;
    for (u32 index = 0; index < steps; ++index) {
        const f32 distance = span.start + (step * (static_cast<f32>(index) + 0.5F));
        const Vec3 planet_sample = planet_relative + (view * distance);
        const Vec3 world_sample = view_position + (view * distance);
        // The reconstruction is addressed by altitude above the surface, which at these distances
        // is NOT `view_position.y + view.y * distance`: the planet curves away. Reading the
        // altitude off the planet-relative position is what keeps a cloud deck at 1500 m from
        // rising above the horizon thirty kilometres out.
        const Vec3 reconstruction_position{world_sample.x, altitude_of(atmosphere, planet_sample),
                                           world_sample.z};

        ++result.stats.steps;
        const CloudDensitySample cloud =
            cloud_density(limited, reconstruction_position, time_seconds, quality.octaves);
        ++result.stats.density_samples;
        if (cloud.density <= 1.0e-6F) {
            ++result.stats.empty_steps;
            continue;
        }
        if (cloud.dominant_layer < kMaxCloudLayers) {
            ++result.stats.steps_in_layer[cloud.dominant_layer];
        }
        if (cloud.density > dominant_extinction) {
            dominant_extinction = cloud.density;
            result.dominant_layer = cloud.dominant_layer;
        }

        const f32 to_sun = optical_depth_to_sun(limited, reconstruction_position, sun, time_seconds,
                                                quality, result.stats);
        const Vec3 direct = cloud_scattering(to_sun, cos_theta, 0.62F, sun_illuminance,
                                             quality.multiple_scattering);

        // The atmosphere between the eye and this step, from the table. Without it a cloud at
        // twenty kilometres is as saturated as one overhead, which reads as a cut-out.
        const Vec3 through_air =
            segment_transmittance_tabulated(atmosphere, tables, planet_relative, planet_sample);

        // Energy-conserving integration over the step: the analytic integral of a constant source
        // through a constant extinction, rather than a rectangle rule that loses light as the step
        // grows.
        const f32 step_transmittance = std::exp(-cloud.density * step);
        const Vec3 source = direct + ambient;
        const Vec3 integrated = (source - (source * step_transmittance));
        result.scattering =
            result.scattering + cwise_mul(through_air, integrated * result.transmittance);
        result.transmittance *= step_transmittance;

        if (result.half_transmittance_depth < 0.0F && result.transmittance < 0.5F) {
            result.half_transmittance_depth = distance;
        }
        if (result.transmittance < 0.01F) {
            break;  // nothing behind this survives; the remaining steps would add nothing visible
        }
    }

    for (const u32 steps_here : result.stats.steps_in_layer) {
        if (steps_here > 0) {
            ++result.stats.layers_touched;
        }
    }
    return result;
}

// ================================================================================================
// Temporal reprojection
// ================================================================================================

namespace {

/// How fast confidence falls with the distance a history sample had to be fetched from. A quarter
/// of the screen of motion is where a bilinear refetch of a reduced-resolution cloud buffer stops
/// being worth blending, and it is a declared constant rather than a magic number in the middle of
/// the function.
inline constexpr f32 kConfidenceFalloff = 4.0F;

}  // namespace

CloudHistory reproject_cloud(const CloudHistoryInputs& inputs) noexcept {
    CloudHistory history;

    // THE CLOUD'S OWN MOTION. The parcel now at this pixel was, one frame ago, at `p - v*dt`;
    // projecting that point is where the history sample is. A camera-only motion vector — which is
    // what every other temporal consumer in the engine uses — puts the fetch at the same pixel when
    // the camera is still, and the result is a cloud that smears rather than moves.
    const f32 depth = math::max(inputs.depth_metres, 1.0F);
    const f32 ndc_x = (inputs.current_uv.x * 2.0F) - 1.0F;
    const f32 ndc_y = (inputs.current_uv.y * 2.0F) - 1.0F;
    const Vec3 point = (inputs.view_forward * depth) +
                       (inputs.view_right * (ndc_x * inputs.tan_half_fov_x * depth)) +
                       (inputs.view_up * (ndc_y * inputs.tan_half_fov_y * depth));
    const Vec3 previous = point - (inputs.cloud_velocity * inputs.delta_seconds);

    const f32 previous_depth = dot(previous, inputs.view_forward);
    Vec2 cloud_motion{0.0F, 0.0F};
    bool representable = previous_depth > 1.0F;
    if (representable) {
        const f32 previous_x =
            dot(previous, inputs.view_right) / (previous_depth * inputs.tan_half_fov_x);
        const f32 previous_y =
            dot(previous, inputs.view_up) / (previous_depth * inputs.tan_half_fov_y);
        cloud_motion = Vec2{((previous_x + 1.0F) * 0.5F) - inputs.current_uv.x,
                            ((previous_y + 1.0F) * 0.5F) - inputs.current_uv.y};
    }

    ReprojectionInputs classify;
    classify.current_uv = inputs.current_uv;
    classify.motion = inputs.camera_motion + cloud_motion;
    classify.representable = representable;
    // A cloud has no surface, so there is no depth discontinuity for the classifier to find: both
    // depths are the marched one. Disocclusion for clouds is expressed by the weather rejection
    // below, which is the cloud-specific semantics the requirement asks for.
    classify.current_depth = depth;
    classify.history_depth = depth;
    classify.history_valid = inputs.history_valid;

    const ReprojectionResult classified = classify_history(classify);
    history.state = classified.state;
    history.history_uv = classified.history_uv;

    // "History SHALL be rejected where weather changed." Two granularities, because they fail
    // differently: the CELL's version catches a front moving across one part of the sky, and the
    // STATE's epoch catches a change to the layer parameters that no cell records.
    if (inputs.weather_version != inputs.history_weather_version ||
        inputs.weather_epoch != inputs.history_weather_epoch) {
        history.state = HistoryState::Unrepresentable;
        history.rejected_by_weather = true;
        history.confidence = 0.0F;
        return history;
    }

    if (!history_usable(history.state)) {
        history.confidence = 0.0F;
        return history;
    }
    const f32 travelled = length(classify.motion);
    history.confidence = math::saturate(1.0F - (travelled * kConfidenceFalloff));
    return history;
}

}  // namespace cy::rendering::sky
