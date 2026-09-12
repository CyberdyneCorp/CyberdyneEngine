#pragma once
// CLOUD SHADOWS AS A COARSE WORLD-SCALE FIELD, PUBLISHED INTO CyberField. M10 task 3.3.
//
// `atmosphere-sky-and-clouds` — "Cloud shadows": "Clouds SHALL cast shadows onto the world through
// a COARSE WORLD-SCALE SHADOW REPRESENTATION — a low-frequency field or map covering a large area
// at low resolution — consumed by terrain, foliage, water, and illumination. Cloud shadows SHALL
// NOT be produced through the VIRTUAL SHADOW PAGE system, whose design assumes shadow detail
// correlates with screen pixels; a cloud shadow's footprint is kilometres wide and its detail is
// low-frequency. Cloud shadow resolution and update rate SHALL be budget levers."
//
// ================================================================================================
// THE "RIGHT MECHANISM" SCENARIO IS A BUILD FACT, NOT A PROMISE
// ================================================================================================
//
// "WHEN cloud shadows are implemented THEN they SHALL use the coarse field rather than allocating
// virtual shadow pages."
//
// `src/rendering/sky/CMakeLists.txt` does not link `cy::rendering-shadows`, so there is no
// expression in this module that can allocate a page: the mechanism is not merely unused, it is
// unreachable. What this module links instead is `cy::environment` — the substrate M10 section 1
// built — and the shadow is an ordinary field in it, sampled by terrain, foliage, water and
// illumination through `environment::FieldReader` like any other.
//
// The second half of the same guarantee is `kMinimumCellMetres`. A caller that asks for a
// two-metre cloud shadow is asking for a shadow map by another name, and
// `cloud_shadow_declaration()` refuses it and says so. The floor is what stops the coarse field
// quietly becoming the thing it replaced.
//
// ================================================================================================
// ONE PRODUCER, AND IT IS THIS ONE
// ================================================================================================
//
// `environment-fields` makes a second producer a configuration error refused at registration.
// `CloudShadowField::attach()` therefore CLAIMS the field and holds the
// `environment::ProducerToken` for its lifetime: the token is move-only and is the only thing a
// `FieldWriter` can be opened with, so no other system can write this field even by mistake. A
// second sky — two `CloudShadowField`s over one registry — fails at `attach()` naming both, and
// that is a test.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/environment/field.h>
#include <cy/environment/store.h>
#include <cy/rendering/sky/clouds.h>
#include <cy/world/coordinates.h>

namespace cy::rendering::sky {

/// The field's stable name.
///
/// It is NOT added to `cy::environment::fields::`. That list is the set of fields
/// `environment-fields` says the engine must define, and extending it from here would be one
/// producer row editing the substrate's vocabulary — exactly the coupling the substrate exists to
/// prevent. A field name is a string; two rows that name `cloud-shadow` get one field without
/// having been introduced, which is the whole point of `environment::field_id()` being a hash.
inline constexpr const char* kCloudShadowFieldName = "cloud-shadow";

[[nodiscard]] constexpr environment::FieldId cloud_shadow_field_id() noexcept {
    return environment::field_id(kCloudShadowFieldName);
}

/// The producer's name, as a refusal will print it.
inline constexpr const char* kCloudShadowProducerName = "atmosphere-sky-and-clouds";

/// The smallest cell a cloud shadow field may declare, in metres.
///
/// SIXTY-FOUR, and the number is an argument rather than a taste. A cumulus is on the order of a
/// kilometre across and its shadow's edge is softened by the sun's own half-degree disc — about
/// fifteen metres of penumbra per kilometre of cloud height, so a 1 500 m deck has a penumbra
/// roughly twenty metres wide. Sixty-metre cells already carry more detail than the phenomenon has.
/// Below that a caller is asking for a shadow map, and `virtual-shadows` is where shadow maps live.
inline constexpr f32 kMinimumCellMetres = 64.0F;

/// Below this the sun is too low for a shadow ray to mean anything, as a sine of its elevation.
///
/// HALF A DEGREE, and it is a guard against a very specific failure rather than a tidiness. The
/// shadow ray climbs from the bottom of the cloud deck to the top, so its length is the deck's
/// thickness divided by this number: at a hundredth it is eight hundred kilometres of nearly
/// horizontal travel through a weather map that WRAPS, which finds cloud everywhere and writes a
/// field that is black in every cell. The visible result is a world that goes dark at dusk and
/// comes back at dawn with no sunrise in between. Above the guard the ray is short enough to mean
/// what it says; below it there is no meaningful direct sunlight to block, so the field is full
/// sun.
inline constexpr f32 kMinimumSunElevation = 0.02F;

/// Resolution and update rate: "Cloud shadow resolution and update rate SHALL be budget levers."
/// Both are here, in one struct, so that the arbiter moves one object rather than reaching into the
/// field's declaration.
struct CloudShadowQuality {
    /// Metres per cell at the regional level — the one a surface actually samples.
    f32 regional_cell_metres = 128.0F;
    /// Metres per cell at the macro level, which exists so that a sample outside the updated radius
    /// still answers with something rather than with the default.
    f32 macro_cell_metres = 1024.0F;
    /// How often the field is rewritten, in hertz. Clouds move at tens of metres per second and the
    /// field's cells are over a hundred metres wide, so eight is already finer than the phenomenon.
    f32 updates_per_second = 8.0F;
    /// How far from the viewer the regional level is written, in metres.
    f32 radius_metres = 4096.0F;
    /// Steps of the shadow ray through the cloud deck. The cheapest lever and the first one a
    /// budget takes.
    u32 steps = 12;
};

/// Build the declaration, or refuse a quality that is asking for a shadow map.
[[nodiscard]] Expected<environment::FieldDeclaration, Error> cloud_shadow_declaration(
    const CloudShadowQuality& quality) noexcept;

/// What the last updates cost, and what they covered.
struct CloudShadowStats {
    u32 updates = 0;
    /// Calls that were declined because the update period had not elapsed. The update-rate lever,
    /// visible rather than assumed.
    u32 skipped = 0;
    u64 cells_evaluated = 0;
    u64 density_samples = 0;
    u32 tiles_written = 0;
    /// The darkest, brightest and mean transmittance written by the last update.
    ///
    /// ALL THREE, because the scenario is "a cloud shadow crosses a valley" and a field that is
    /// uniformly dark satisfies "darkest is low" while showing no shadow at all. The spread between
    /// the first two is what a shadow IS.
    f32 darkest = 1.0F;
    f32 brightest = 0.0F;
    f32 mean = 1.0F;
};

/// The producer of the `cloud-shadow` field.
class CloudShadowField {
public:
    CloudShadowField() = default;

    CloudShadowField(const CloudShadowField&) = delete;
    CloudShadowField& operator=(const CloudShadowField&) = delete;
    CloudShadowField(CloudShadowField&&) noexcept = default;
    CloudShadowField& operator=(CloudShadowField&&) noexcept = default;
    ~CloudShadowField() = default;

    /// Declare the field and claim it. Fails, naming both producers, if something else already
    /// produces `cloud-shadow`.
    ///
    /// `producer_name` is a parameter rather than a constant because a project may replace the
    /// engine's sky with its own and the refusal has to be able to name the two of them apart. It
    /// must outlive the registry: the conflict message points at it.
    [[nodiscard]] Status attach(environment::FieldRegistry& registry,
                                const CloudShadowQuality& quality,
                                const char* producer_name = kCloudShadowProducerName) noexcept;

    /// Declare the four consumers the requirement names, so that
    /// `environment::FieldRegistry::validate()` has something to validate. Separate from `attach()`
    /// because a project may have no water and because a consumer list is a configuration rather
    /// than a property of the producer.
    [[nodiscard]] static Status declare_consumers(environment::FieldRegistry& registry) noexcept;

    /// Advance the clock and, if the update period has elapsed, rewrite the field around `centre`.
    ///
    /// Returns whether it wrote. `delta_seconds` is the renderer's frame time: the update RATE is a
    /// budget lever, so the decision to skip belongs here rather than at every call site.
    [[nodiscard]] Expected<bool, Error> update(environment::FieldStore& store,
                                               const CloudField& clouds, Vec3 sun_direction,
                                               f64 time_seconds, const world::WorldVec3d& centre,
                                               f32 delta_seconds) noexcept;

    /// The fraction of sunlight reaching a world position: 1 in full sun, 0 under a storm. Sampled
    /// through the substrate, so terrain, foliage, water and illumination all read the same
    /// number — which is the requirement's "consumed by" list made true by construction.
    [[nodiscard]] static f32 sample(const environment::FieldStore& store,
                                    const world::WorldVec3d& at) noexcept;

    [[nodiscard]] const CloudShadowStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const CloudShadowQuality& quality() const noexcept { return quality_; }
    /// Change the levers without re-claiming the field. The resolution is part of the declaration
    /// and cannot move at run time — a field's stored bytes would mean something else — so this
    /// takes the two levers that can: the update rate and the step count.
    void set_levers(f32 updates_per_second, u32 steps) noexcept;
    [[nodiscard]] bool attached() const noexcept { return token_.valid(); }

private:
    [[nodiscard]] f32 shadow_at(const CloudField& clouds, Vec3 sun, f64 time_seconds, f64 world_x,
                                f64 world_z, u64& samples) const noexcept;

    environment::ProducerToken token_;
    CloudShadowQuality quality_;
    CloudShadowStats stats_;
    f32 accumulated_ = 0.0F;
};

}  // namespace cy::rendering::sky
