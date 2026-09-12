#pragma once
// CLOUDS ARE RECONSTRUCTED, NOT STORED. M10 task 3.3.
//
// `atmosphere-sky-and-clouds` — "Cloud representation": "Clouds SHALL be represented as a
// low-resolution weather map — coverage, type, and moisture over kilometres — combined with
// procedural detail: noise, erosion, and a height profile per layer. A world-scale volumetric cloud
// field SHALL NOT be stored, streamed, or replicated. Density SHALL be RECONSTRUCTED PROCEDURALLY
// during evaluation from the map and the noise."
//
// ================================================================================================
// THE ARITHMETIC THAT SENTENCE IS ABOUT
// ================================================================================================
//
// A hundred-kilometre world with clouds from 1 km to 8 km, voxelised at the resolution a ray march
// actually resolves — say 32 m — is 3125 x 3125 x 219 voxels. At one byte each that is 2.1 GB, it
// changes every frame, and `networking-and-replication` would be asked to replicate it.
//
// `CloudWeatherMap` over the same world at 1 km cells is 100 x 100 x 5 bytes = 50 KB, and
// `tests/test_clouds.cpp` asserts that number rather than describing it. Everything between the
// map and the pixel is `cloud_density()`, which is a pure function of (position, time, field) and
// holds nothing.
//
// ================================================================================================
// WHAT IS STATE AND WHAT IS DRAWING, BECAUSE THE CAPABILITY'S HEADLINE RULE IS ABOUT THAT LINE
// ================================================================================================
//
// "THE SAME ENVIRONMENTAL STATE SHALL DRIVE EVERY TIER. A tier SHALL change how the sky is drawn,
// never what the weather is" — and, as a scenario, "WHEN cloud quality is reduced under budget
// pressure THEN rain intensity, wind, and visibility SHALL be unchanged."
//
// So the split is drawn in the types:
//
//   STATE      `CloudWeatherState` (what `weather-and-wind` publishes), `CloudWeatherMap`, and the
//              `CloudLayer` parameters `drive_cloud_layers()` derives from them. None of these
//              functions takes a `CloudQuality`, and that is the enforcement: a tier cannot reach
//              them because it is not in their signatures.
//
//   DRAWING    `CloudQuality`, the octave count, the step counts and `march_clouds()`. A lower tier
//              changes the reconstructed DENSITY (fewer octaves of detail) and the integral over
//              it, and changes nothing a gameplay system reads.
//
// `CloudDensitySample` carries both halves for exactly this reason: `coverage` and `type` come from
// the map and are identical at every tier, `density` is the reconstruction and is not.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/sky/atmosphere.h>
#include <cy/rendering/sky/tables.h>
#include <cy/rendering/temporal/reprojection.h>

namespace cy::rendering::sky {

// ================================================================================================
// THE WEATHER MAP
// ================================================================================================

/// One cell of the map, and the version that makes staleness cheap to detect.
struct CloudMapSample {
    /// How much of the sky this cell's clouds cover, in [0, 1].
    f32 coverage = 0.0F;
    /// The cloud's kind as a CONTINUOUS parameter: 0 is a flat stratus sheet, 0.5 a cumulus, 1 a
    /// cumulonimbus tower. Continuous rather than an enumeration because a storm front is a place
    /// where the type changes across a few kilometres, and an enumeration would make that a seam.
    f32 type = 0.0F;
    /// Water available to the reconstruction, in [0, 1]. Separate from coverage because a thin
    /// overcast is high coverage and low moisture, and a lone thunderhead is the opposite.
    f32 moisture = 0.0F;
    /// The version of the cell this sample came from. Compared, never interpolated: a version
    /// halfway between two versions names no state.
    u32 version = 0;
};

/// The coarse map: what is stored, cooked, streamed and replicated. Kilometres per cell.
///
/// IT WRAPS. A world is finite and a sky is not — a ray march towards the horizon leaves the mapped
/// extent within a few kilometres and must still find clouds there. Wrapping keeps the storage
/// bounded and is invisible at the resolution a cloud is seen from the ground; clamping would put a
/// visible edge in the sky at the world's boundary.
class CloudWeatherMap {
public:
    explicit CloudWeatherMap(Allocator& allocator = current_allocator()) noexcept
        : coverage_(allocator), type_(allocator), moisture_(allocator), version_(allocator) {}

    /// `cells_per_edge` cells of `cell_metres` each. The requirement says "over kilometres", and
    /// `configure()` refuses a cell below `kMinimumCellMetres` — a map at tens of metres is a
    /// volume with extra steps, and it is the mistake this representation exists to prevent.
    [[nodiscard]] Status configure(u32 cells_per_edge, f32 cell_metres) noexcept;

    /// The smallest cell a weather map may declare. 250 m: four cells to a kilometre, which is
    /// still "over kilometres" for a storm front and is an order of magnitude coarser than the
    /// detail the noise reconstructs.
    static constexpr f32 kMinimumCellMetres = 250.0F;

    /// Write one cell and bump its version and the map's epoch. The only way values change, so
    /// "history SHALL be rejected where weather changed" has exactly one place to hook into.
    [[nodiscard]] Status set(i32 x, i32 z, f32 coverage, f32 type, f32 moisture) noexcept;

    /// Fill the map procedurally from a seed and a weather state. Here because `weather-and-wind`
    /// is the row that will drive this map and a sky that could not be tested without it would be a
    /// sky nobody could test — and because "WHEN a project wants a procedural night sky" has a
    /// daytime counterpart nobody writes down.
    [[nodiscard]] Status generate(u64 seed, f32 base_coverage, f32 storminess) noexcept;

    /// Bilinear in coverage, type and moisture; NEAREST in version, because a version is an
    /// identity and an interpolated one names no state.
    [[nodiscard]] CloudMapSample sample(f32 world_x, f32 world_z) const noexcept;

    /// What is stored, in bytes. The number the "clouds are compact" scenario is about, reported
    /// rather than estimated.
    [[nodiscard]] u64 bytes() const noexcept;

    [[nodiscard]] u32 cells_per_edge() const noexcept { return cells_; }
    [[nodiscard]] f32 cell_metres() const noexcept { return cell_metres_; }
    /// Bumped by every write. What a temporal history is rejected against when nothing finer is
    /// available.
    [[nodiscard]] u32 epoch() const noexcept { return epoch_; }
    [[nodiscard]] bool configured() const noexcept { return cells_ > 0; }

private:
    [[nodiscard]] usize index_of(i32 x, i32 z) const noexcept;

    Array<u8> coverage_;
    Array<u8> type_;
    Array<u8> moisture_;
    /// One version per cell. `u16` and not `u64`: it is compared for equality and nothing else, and
    /// a wrap after 65 536 edits of one cell costs a single rejected history frame.
    Array<u16> version_;
    u32 cells_ = 0;
    f32 cell_metres_ = 1000.0F;
    u32 epoch_ = 0;
};

// ================================================================================================
// LAYERS
// ================================================================================================

/// "Multiple cloud layers SHALL be supported — low, middle, high, storm, and PROJECT-DEFINED."
/// `Project` is a kind rather than an escape hatch: a project's layer is driven by weather through
/// the same function as the four named ones.
enum class CloudLayerKind : u8 { Low = 0, Middle, High, Storm, Project, Count };

[[nodiscard]] const char* cloud_layer_kind_name(CloudLayerKind kind) noexcept;

/// Eight, because four are named and a project that wants more than four of its own is describing
/// an atmosphere this representation is the wrong shape for.
inline constexpr u32 kMaxCloudLayers = 8;

/// One layer. Every field is a parameter of the reconstruction; none of them is a rendering
/// setting, which is what keeps `CloudQuality` out of this struct.
struct CloudLayer {
    const char* name = "";
    CloudLayerKind kind = CloudLayerKind::Low;

    /// Metres above the surface.
    f32 base_altitude = 1500.0F;
    f32 thickness = 1200.0F;
    /// Multiplied into the map's own coverage. A layer at zero is present and empty; a layer that
    /// is `enabled == false` is absent and costs no steps.
    f32 coverage = 1.0F;
    /// Extinction per metre at full density. Cloud water is grey — it scatters every wavelength
    /// nearly equally, which is why this is a scalar and `Atmosphere::rayleigh_scattering` is not.
    f32 density = 0.06F;
    /// 0 stratus, 1 cumulonimbus. Blended with the map's own type rather than replacing it.
    f32 type = 0.3F;
    /// The layer's own wind, in m/s. Per layer and not global, because layers shearing past each
    /// other is most of what makes a sky look alive — and because the cloud-motion half of temporal
    /// reprojection needs to know which layer a pixel came from.
    Vec3 wind{8.0F, 0.0F, 2.0F};

    /// Metres per period of the base shape and of the erosion detail.
    f32 base_scale = 3000.0F;
    f32 detail_scale = 220.0F;
    /// How hard the detail eats into the base shape, in [0, 1]. This is what makes a cumulus
    /// cauliflower rather than a blob.
    f32 erosion = 0.35F;

    bool enabled = true;
};

/// The layers, in altitude order. Order matters only for diagnostics and for the march's early
/// exit; the density at a point is the sum over layers, which is what makes a storm layer
/// overlapping a low layer look like weather rather than like a z-fight.
struct CloudLayerSet {
    CloudLayer layers[kMaxCloudLayers];
    u32 count = 0;

    [[nodiscard]] Status add(const CloudLayer& layer) noexcept;
    [[nodiscard]] f32 lowest_base() const noexcept;
    [[nodiscard]] f32 highest_top() const noexcept;
};

/// The four the specification names, at Earth's altitudes.
[[nodiscard]] CloudLayerSet default_cloud_layers() noexcept;

// ================================================================================================
// WEATHER STATE — the input, owned by another row
// ================================================================================================

/// What `weather-and-wind` publishes and what this module consumes. It is declared HERE and
/// produced THERE, which is the direction that lets the sky be built and measured before that row
/// exists — and which keeps "layer parameters SHALL be driven by weather state rather than authored
/// per frame" a property of `drive_cloud_layers()` rather than of a convention.
struct CloudWeatherState {
    /// The wind at the reference altitude, in m/s.
    Vec3 wind{8.0F, 0.0F, 2.0F};
    /// How much faster the wind blows per kilometre of altitude. The shear that separates the
    /// layers.
    f32 shear_per_km = 0.15F;
    /// [0, 1].
    f32 humidity = 0.5F;
    /// [0, 1]. Drives the storm layer and, through it, the precipitation a gameplay system reads.
    f32 storm_intensity = 0.0F;
    /// Millimetres per hour. Gameplay-visible; carried here so that "rain intensity is unchanged by
    /// a quality tier" can be asserted on the same struct the tier does not touch.
    f32 precipitation = 0.0F;
    f32 temperature_celsius = 15.0F;
    /// Bumped by the producer whenever any of the above changes. The coarse half of the history
    /// rejection, for the frames where a pixel's own map cell did not change but the state did.
    u32 epoch = 0;
};

/// Derive every layer's coverage, density, type and wind from the weather state.
///
/// The layers are edited in place because a project AUTHORS the base shape — altitudes,
/// thicknesses, scales, which layers exist at all — and weather modulates it. Replacing the set
/// would throw away the authoring; leaving the authoring to modulate weather would make the sky
/// unresponsive to the storm that is coming.
void drive_cloud_layers(const CloudWeatherState& state, CloudLayerSet& layers) noexcept;

// ================================================================================================
// RECONSTRUCTION
// ================================================================================================

/// Everything `cloud_density()` reads. A value, not an object: the reconstruction holds no state,
/// which is what makes it a pure function of position and time and therefore reproducible on any
/// thread, in any order, and on a second machine.
struct CloudField {
    const CloudWeatherMap* map = nullptr;
    CloudLayerSet layers;
    /// The world's seed. The noise is derived from it, so two machines reconstruct the same cloud.
    u64 seed = 0x0C10'0D5EULL;
    /// Added to every layer's coverage before the map's. A director's dial, and the one place a
    /// human overrides the weather.
    f32 coverage_bias = 0.0F;
};

/// One reconstructed sample, split into the half that is STATE and the half that is DRAWING.
struct CloudDensitySample {
    /// Extinction per metre. The drawn quantity: it depends on the octave count, and therefore on
    /// the tier.
    f32 density = 0.0F;
    /// The map's coverage and type at this position. STATE: identical at every tier, because they
    /// come from the map and the map has no tier.
    f32 coverage = 0.0F;
    f32 type = 0.0F;
    /// Which layer contributed most of the density, or `kMaxCloudLayers` for none. What the
    /// diagnostic's "which layer produced this" answer is, and what the temporal reprojection uses
    /// to pick the wind that moved this pixel.
    u32 dominant_layer = kMaxCloudLayers;
    /// The map cell's version, for the history rejection.
    u32 version = 0;
};

/// Reconstruct the density at a world position and a time.
///
/// `octaves` is the drawing lever: 1 is the base shape alone, 4 is a cinematic tier's erosion. The
/// COVERAGE and TYPE it reports do not depend on it, which is the property
/// `tests/test_clouds.cpp` holds every tier to.
///
/// `time_seconds` advects the map and the noise by each layer's own wind, which is what makes
/// clouds drift and shear. It is `f64` for the reason `core-math`'s precision policy gives: an
/// accumulated time in `f32` stops moving clouds smoothly after about an hour of play.
[[nodiscard]] CloudDensitySample cloud_density(const CloudField& field, Vec3 world_position,
                                               f64 time_seconds, u32 octaves) noexcept;

// ================================================================================================
// RAY MARCHING
// ================================================================================================

/// The drawing levers, held by the renderer budget arbiter. See `profile.h` for the tier table and
/// the priced ladder.
struct CloudQuality {
    /// Steps along the view ray through the cloud slab.
    u32 steps = 64;
    /// Steps along the ray to the sun, for self-shadowing.
    u32 light_steps = 6;
    /// Octaves of detail in the reconstruction.
    u32 octaves = 3;
    /// Layers evaluated, lowest first. A mobile tier draws the low deck and nothing else.
    u32 max_layers = kMaxCloudLayers;
    /// Fraction of the render resolution clouds are marched at. Not used by `march_clouds()` — it
    /// marches one ray — and carried here because it is the lever the arbiter moves and the cost
    /// attribution divides by.
    f32 resolution_scale = 0.5F;
    /// Whether the multiple-scattering approximation is evaluated. The cheapest thing to drop and
    /// the first thing an artist notices, which is why it is a declared lever rather than a
    /// constant.
    bool multiple_scattering = true;
};

/// Where the cost went. "Cloud rendering cost SHALL be attributable to steps, resolution, and
/// layers, since cloud rendering is notoriously difficult to tune without that."
struct CloudMarchStats {
    u32 steps = 0;
    u32 density_samples = 0;
    u32 light_samples = 0;
    /// Steps whose dominant contribution came from each layer. The per-layer attribution the
    /// requirement names, counted rather than modelled.
    u32 steps_in_layer[kMaxCloudLayers] = {};
    u32 layers_touched = 0;
    f32 marched_metres = 0.0F;
    /// Steps that found nothing. The number that says whether an empty-space skip would pay.
    u32 empty_steps = 0;
};

struct CloudMarchResult {
    /// Radiance the clouds added along the ray.
    Vec3 scattering{0.0F, 0.0F, 0.0F};
    /// What survives of whatever was behind them.
    f32 transmittance = 1.0F;
    /// The depth at which the ray's transmittance first fell below a half, in metres, or -1 where
    /// it never did. What the temporal reprojection reprojects, because a cloud has no surface and
    /// a depth buffer has nothing to say about it.
    f32 half_transmittance_depth = -1.0F;
    /// Which layer contributed most of the extinction. What the pixel diagnostic names and what
    /// picks the wind for the reprojection.
    u32 dominant_layer = kMaxCloudLayers;
    CloudMarchStats stats;
};

/// March the clouds along one ray.
///
/// Lighting accounts for direct sunlight (self-shadowed by a short march towards the sun), an
/// approximation of multiple scattering inside the cloud, ambient sky light, and the atmospheric
/// transmittance between the eye and each step — all four of which the requirement names.
///
/// `sun_illuminance` and `ambient` come from the atmosphere rather than from a constant, so a cloud
/// at sunset is lit by a reddened sun without anything here knowing what sunset is.
[[nodiscard]] CloudMarchResult march_clouds(const Atmosphere& atmosphere,
                                            const AtmosphereTables& tables, const CloudField& field,
                                            Vec3 view_position, Vec3 view_direction,
                                            Vec3 sun_direction, Vec3 sun_illuminance, Vec3 ambient,
                                            f64 time_seconds, const CloudQuality& quality) noexcept;

// ================================================================================================
// TEMPORAL RECONSTRUCTION — the cloud-specific half
// ================================================================================================
//
// "Rendering SHALL run at reduced resolution with temporal reconstruction THROUGH THE FRAMEWORK IN
// `temporal-rendering`, with cloud-specific history semantics: clouds move independently of the
// camera, so reprojection SHALL account for cloud motion and history SHALL be rejected where
// weather changed."
//
// Both halves are here and neither is a second framework. `classify_history()` is the engine's own
// classifier and it is CALLED, not reimplemented; what this adds is the motion vector it is given
// — camera motion PLUS the screen-space motion of the cloud itself — and the one rejection the
// classifier cannot know about, which is a weather map cell that changed underneath a valid
// reprojection.

/// What reprojecting a cloud pixel needs. The camera half is what any temporal effect passes; the
/// cloud half is `cloud_velocity`, `depth_metres` and the two versions.
struct CloudHistoryInputs {
    Vec2 current_uv{0.5F, 0.5F};
    /// Screen-space motion from the camera alone, as `temporal-rendering` derives it for a surface
    /// at `depth_metres`.
    Vec2 camera_motion{0.0F, 0.0F};

    /// The dominant layer's wind, in m/s, in world axes. The whole of "clouds move independently of
    /// the camera".
    Vec3 cloud_velocity{0.0F, 0.0F, 0.0F};
    f32 delta_seconds = 1.0F / 60.0F;

    /// The view basis and projection, to turn a world velocity into a screen offset.
    Vec3 view_forward{0.0F, 0.0F, -1.0F};
    Vec3 view_right{1.0F, 0.0F, 0.0F};
    Vec3 view_up{0.0F, 1.0F, 0.0F};
    /// A 45-degree vertical field of view on a 16:9 frame: tan(22.5 degrees) and that times
    /// 16/9. Written as the tangents rather than as an angle because that is what a projection
    /// uses, and stated here because they are the only camera parameters this module has.
    f32 tan_half_fov_x = 0.7364F;
    f32 tan_half_fov_y = 0.4142F;
    /// Where along the ray the cloud is. `CloudMarchResult::half_transmittance_depth`.
    f32 depth_metres = 4000.0F;

    /// The weather map version under this pixel now, and the one the history was rendered with.
    u32 weather_version = 0;
    u32 history_weather_version = 0;
    /// The weather state's epoch, same pair. A cell that did not change while the state did is
    /// still a rejection: the layer parameters moved underneath it.
    u32 weather_epoch = 0;
    u32 history_weather_epoch = 0;

    bool history_valid = true;
};

struct CloudHistory {
    Vec2 history_uv{0.0F, 0.0F};
    HistoryState state = HistoryState::Unrepresentable;
    /// How much of the history may be blended, in [0, 1]. One for a clean reprojection, falling
    /// with the residual motion the reprojection could not explain, zero for a rejection. Reported
    /// so that the diagnostic's "temporal history confidence" view has something to draw.
    f32 confidence = 0.0F;
    /// True when the rejection was the weather changing rather than the geometry. Separated because
    /// the two are fixed by different things and a single "invalid" count cannot tell a tuner
    /// which.
    bool rejected_by_weather = false;
};

/// Reproject one cloud pixel. Calls `classify_history()`; adds the cloud's own motion and the
/// weather rejection.
[[nodiscard]] CloudHistory reproject_cloud(const CloudHistoryInputs& inputs) noexcept;

}  // namespace cy::rendering::sky
