// The coarse world-scale cloud shadow field, produced into CyberField.

#include <cy/rendering/sky/cloud_shadows.h>

#include <cy/core/math/math.h>

#include "internal.h"

#include <cmath>
#include <utility>

namespace cy::rendering::sky {
namespace {

/// Octaves the shadow reconstruction uses.
///
/// TWO, and never the view march's count. A cloud shadow is low-frequency BY DEFINITION — that is
/// the requirement's own justification for not using virtual shadow pages — so the fine octaves of
/// erosion are integrated away by a 128-metre cell before anything can see them. Spending them
/// would cost four times as much for a field that is quantised to one part in 255 anyway.
inline constexpr u32 kShadowOctaves = 2;

/// Which residency level carries which cell size, and how far out each is written. Two levels and
/// not three: `Local` would be finer than `kMinimumCellMetres`, which is the resolution this field
/// is specified not to have.
struct ShadowLevel {
    environment::FieldResidency residency;
    f32 cell_metres;
    f32 radius_metres;
};

[[nodiscard]] i64 floor_div(i64 value, i64 divisor) noexcept {
    const i64 quotient = value / divisor;
    return (value % divisor != 0 && ((value < 0) != (divisor < 0))) ? quotient - 1 : quotient;
}

}  // namespace

Expected<environment::FieldDeclaration, Error> cloud_shadow_declaration(
    const CloudShadowQuality& quality) noexcept {
    if (quality.regional_cell_metres < kMinimumCellMetres) {
        return fail(ErrorCode::InvalidArgument,
                    "cloud_shadow_declaration: a cell below 64 m is a shadow map, not a cloud "
                    "shadow field — a cloud's shadow edge is softened by the sun's own disc to "
                    "tens of metres, and `virtual-shadows` is where shadow maps belong");
    }
    if (quality.macro_cell_metres <= quality.regional_cell_metres) {
        return fail(ErrorCode::InvalidArgument,
                    "cloud_shadow_declaration: the macro level must be coarser than the regional "
                    "one — CyberField declares its levels finest first");
    }

    environment::FieldDeclaration declaration;
    declaration.name = kCloudShadowFieldName;
    declaration.unit = "fraction";
    declaration.semantics =
        "the fraction of direct sunlight reaching the surface through the cloud layers, 1 in full "
        "sun and 0 under a storm";
    declaration.type = environment::FieldType::Scalar;
    // UNorm8: the quantity is a fraction, one part in 255 is far below what a surface lit through
    // it can show, and a world-scale f32 field would cost four times as much for nothing.
    declaration.encoding = environment::FieldEncoding::UNorm8;
    declaration.interpolation = environment::FieldInterpolation::Linear;
    declaration.cadence = environment::FieldCadence::PerFrame;
    declaration.production = environment::FieldProduction::Cpu;
    declaration.range_min = 0.0F;
    declaration.range_max = 1.0F;
    // FULL SUN where nothing has been written. A default of zero would put a black world under an
    // unstreamed sky, and `environment-fields` requires a sample outside resident data to return
    // the declared default rather than to block — so the default is the one that is invisible.
    declaration.default_value = environment::FieldValue::scalar(1.0F);
    declaration.levels[static_cast<u32>(environment::FieldResidency::Regional)] =
        environment::FieldLevel{quality.regional_cell_metres, false};
    declaration.levels[static_cast<u32>(environment::FieldResidency::Macro)] =
        environment::FieldLevel{quality.macro_cell_metres, false};

    // PRESENTATION, and the classification is the load-bearing decision in this declaration.
    //
    // A cloud shadow is derived from the cloud reconstruction, which is a rendering-side function
    // of the weather and of the frame's own time. The GAMEPLAY-VISIBLE quantity is the weather —
    // `weather-and-wind`'s wind, wetness, precipitation and their fields — and a shadow computed
    // from it at render cadence is a picture of that state, not a second copy of it. Declaring it
    // `Authoritative` would oblige every machine to agree on a value that a quality tier is allowed
    // to change, which is exactly the contradiction `simulation-and-determinism`'s firewall exists
    // to catch. `determinism::may_read()` then keeps gameplay out of it by configuration
    // validation, which is what `declare_consumers()` below gives it something to validate.
    declaration.classification = determinism::SimulationClass::Presentation;
    declaration.persistent = false;
    return declaration;
}

Status CloudShadowField::attach(environment::FieldRegistry& registry,
                                const CloudShadowQuality& quality,
                                const char* producer_name) noexcept {
    auto declaration = cloud_shadow_declaration(quality);
    if (!declaration) {
        return fail(declaration.error().code, declaration.error().message);
    }
    if (auto status = registry.declare(declaration.value()); !status) {
        return status;
    }
    auto token =
        registry.claim(cloud_shadow_field_id(), producer_name, environment::ProducerKind::System);
    if (!token) {
        // The refusal names both producers in its own message; passing it through unchanged is what
        // makes "a second producer for the same field fails, naming both producers" visible at the
        // call site that caused it.
        return fail(token.error().code, token.error().message);
    }
    token_ = std::move(token).value();
    quality_ = quality;
    accumulated_ = 0.0F;
    stats_ = CloudShadowStats{};
    return {};
}

Status CloudShadowField::declare_consumers(environment::FieldRegistry& registry) noexcept {
    // The four the requirement names, each as the class it reads in. All four are presentation: a
    // cloud shadow darkens a surface and modulates a light, and none of them is a gameplay input.
    static constexpr const char* kConsumers[] = {"terrain-materials", "foliage", "water-shading",
                                                 "illumination"};
    for (const char* consumer : kConsumers) {
        if (auto status = registry.declare_consumer(
                consumer, determinism::SimulationClass::Presentation, cloud_shadow_field_id());
            !status) {
            return status;
        }
    }
    return {};
}

void CloudShadowField::set_levers(f32 updates_per_second, u32 steps) noexcept {
    quality_.updates_per_second = math::max(updates_per_second, 0.0F);
    quality_.steps = math::max(steps, 2U);
}

f32 CloudShadowField::shadow_at(const CloudField& clouds, Vec3 sun, f64 time_seconds, f64 world_x,
                                f64 world_z, u64& samples) const noexcept {
    if (sun.y <= kMinimumSunElevation) {
        return 1.0F;
    }
    const f32 base = clouds.layers.lowest_base();
    const f32 top = clouds.layers.highest_top();
    if (!(top > base)) {
        return 1.0F;
    }

    // The ray is parameterised by ALTITUDE rather than by distance, because the two ends of the
    // integral are altitudes — the bottom and the top of the deck — and dividing by the sun's
    // elevation once is cheaper and clearer than intersecting two spheres per cell.
    const f32 start = base / sun.y;
    const f32 end = top / sun.y;
    const u32 steps = math::max(quality_.steps, 2U);
    const f32 step = (end - start) / static_cast<f32>(steps);

    f32 optical_depth = 0.0F;
    for (u32 index = 0; index < steps; ++index) {
        const f32 distance = start + (step * (static_cast<f32>(index) + 0.5F));
        const Vec3 position{static_cast<f32>(world_x) + (sun.x * distance), sun.y * distance,
                            static_cast<f32>(world_z) + (sun.z * distance)};
        optical_depth += cloud_density(clouds, position, time_seconds, kShadowOctaves).density;
        ++samples;
    }
    return math::saturate(std::exp(-optical_depth * step));
}

Expected<bool, Error> CloudShadowField::update(environment::FieldStore& store,
                                               const CloudField& clouds, Vec3 sun_direction,
                                               f64 time_seconds, const world::WorldVec3d& centre,
                                               f32 delta_seconds) noexcept {
    if (!token_.valid()) {
        return fail(ErrorCode::Unavailable,
                    "CloudShadowField::update: attach() has not claimed the cloud-shadow field");
    }
    if (clouds.map == nullptr || !clouds.map->configured()) {
        return fail(ErrorCode::Unavailable,
                    "CloudShadowField::update: the cloud field has no weather map, so there is "
                    "nothing to cast a shadow");
    }

    // THE UPDATE RATE IS A BUDGET LEVER, so the decision to skip lives here and is counted. A
    // renderer that called this every frame and a renderer that called it every eighth frame would
    // otherwise produce different fields for the same lever setting.
    accumulated_ += math::max(delta_seconds, 0.0F);
    const f32 period =
        quality_.updates_per_second > 0.0F ? 1.0F / quality_.updates_per_second : 0.0F;
    if (period > 0.0F && accumulated_ < period && stats_.updates > 0) {
        ++stats_.skipped;
        return false;
    }
    accumulated_ = 0.0F;

    const Vec3 sun = normalized_or(sun_direction, Vec3{0.0F, 1.0F, 0.0F});
    const ShadowLevel levels[] = {
        ShadowLevel{environment::FieldResidency::Regional, quality_.regional_cell_metres,
                    quality_.radius_metres},
        // The macro level reaches four times as far at eight times the cell size, so it costs a
        // quarter of the regional level's cells and answers every sample the regional one does not
        // cover. A field whose coarse level stopped where its fine one did would return the
        // declared default just beyond the viewer, and the transition would be a visible edge.
        ShadowLevel{environment::FieldResidency::Macro, quality_.macro_cell_metres,
                    quality_.radius_metres * 4.0F},
    };

    auto writer = store.open_writer(token_);
    if (!writer) {
        return fail(writer.error().code, writer.error().message);
    }

    f32 darkest = 1.0F;
    f32 brightest = 0.0F;
    f64 total = 0.0;
    u64 cells = 0;
    u64 samples = 0;
    u32 tiles = 0;

    for (const ShadowLevel& level : levels) {
        const auto span = static_cast<i64>(environment::kTileCells);
        const f64 cell = static_cast<f64>(level.cell_metres);
        const auto min_cell_x =
            static_cast<i64>(std::floor((centre.x - static_cast<f64>(level.radius_metres)) / cell));
        const auto max_cell_x =
            static_cast<i64>(std::floor((centre.x + static_cast<f64>(level.radius_metres)) / cell));
        const auto min_cell_z =
            static_cast<i64>(std::floor((centre.z - static_cast<f64>(level.radius_metres)) / cell));
        const auto max_cell_z =
            static_cast<i64>(std::floor((centre.z + static_cast<f64>(level.radius_metres)) / cell));

        for (i64 tile_z = floor_div(min_cell_z, span); tile_z <= floor_div(max_cell_z, span);
             ++tile_z) {
            for (i64 tile_x = floor_div(min_cell_x, span); tile_x <= floor_div(max_cell_x, span);
                 ++tile_x) {
                environment::TileAddress address;
                address.field = cloud_shadow_field_id();
                address.x = static_cast<i32>(tile_x);
                address.z = static_cast<i32>(tile_z);
                address.level = static_cast<u8>(level.residency);
                address.layer = static_cast<u8>(environment::FieldLayer::Base);

                if (auto status = writer.value().stage(address); !status) {
                    return fail(status.error().code, status.error().message);
                }
                ++tiles;

                for (u32 local_z = 0; local_z < environment::kTileCells; ++local_z) {
                    for (u32 local_x = 0; local_x < environment::kTileCells; ++local_x) {
                        // Values sit at CELL CENTRES, which is what `FieldStore`'s own sampler
                        // assumes; writing them at corners would shift the whole field by half a
                        // cell and nothing would report it.
                        const f64 world_x =
                            ((static_cast<f64>((tile_x * span) + local_x)) + 0.5) * cell;
                        const f64 world_z =
                            ((static_cast<f64>((tile_z * span) + local_z)) + 0.5) * cell;
                        const f32 value =
                            shadow_at(clouds, sun, time_seconds, world_x, world_z, samples);
                        darkest = math::min(darkest, value);
                        brightest = math::max(brightest, value);
                        total += static_cast<f64>(value);
                        ++cells;
                        if (auto status =
                                writer.value().set(address, local_x, 0, local_z,
                                                   environment::FieldValue::scalar(value));
                            !status) {
                            return fail(status.error().code, status.error().message);
                        }
                    }
                }
            }
        }
    }

    if (auto status = writer.value().publish(); !status) {
        return fail(status.error().code, status.error().message);
    }

    ++stats_.updates;
    stats_.cells_evaluated += cells;
    stats_.density_samples += samples;
    stats_.tiles_written = tiles;
    stats_.darkest = darkest;
    stats_.brightest = cells > 0 ? brightest : 1.0F;
    stats_.mean = cells > 0 ? static_cast<f32>(total / static_cast<f64>(cells)) : 1.0F;
    return true;
}

f32 CloudShadowField::sample(const environment::FieldStore& store,
                             const world::WorldVec3d& at) noexcept {
    return store.sample(cloud_shadow_field_id(), at).value.x();
}

}  // namespace cy::rendering::sky
