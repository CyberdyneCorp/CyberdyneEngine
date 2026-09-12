// The shoreline water owns, and the four environment fields it produces. M10 task 2.3.

#include <cy/water/shoreline.h>

#include <cy/core/math/scalar.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cy::water {

namespace {

using environment::FieldDeclaration;
using environment::FieldEncoding;
using environment::FieldInterpolation;
using environment::FieldLevel;
using environment::FieldResidency;
using environment::FieldType;
using environment::FieldValue;

/// The declaration every one of water's fields shares: three resolutions with the macro level
/// resident everywhere, CPU-produced, persistent, and gameplay-visible.
///
/// Gameplay-visible is not a stylistic choice. A boat is carried by `water-flow`, a swimmer's state
/// is derived from `water-depth`, and navigation costs are modified by both — so they are
/// authoritative state by `simulation-and-determinism`'s own classification, and declaring them
/// `Presentation` would make every one of those reads a firewall violation. The consequence is the
/// substrate's: a gameplay-visible field's `gameplay_level` must be resident everywhere, which is
/// why the macro level below carries `resident_everywhere = true` and why publishing the macro
/// level is not optional.
[[nodiscard]] FieldDeclaration base_declaration(const WaterFieldOptions& options) noexcept {
    FieldDeclaration declaration;
    declaration.type = FieldType::Scalar;
    declaration.encoding = FieldEncoding::UNorm16;
    declaration.interpolation = FieldInterpolation::Linear;
    declaration.cadence = environment::FieldCadence::SlowlyVarying;
    declaration.production = environment::FieldProduction::Cpu;
    declaration.levels[static_cast<u32>(FieldResidency::Local)] =
        FieldLevel{options.local_cell_metres, false};
    declaration.levels[static_cast<u32>(FieldResidency::Regional)] =
        FieldLevel{options.regional_cell_metres, false};
    declaration.levels[static_cast<u32>(FieldResidency::Macro)] =
        FieldLevel{options.macro_cell_metres, true};
    declaration.classification = determinism::SimulationClass::Persistent;
    declaration.gameplay_level = FieldResidency::Macro;
    declaration.persistent = true;
    return declaration;
}

[[nodiscard]] FieldDeclaration depth_declaration(const WaterFieldOptions& options) noexcept {
    FieldDeclaration declaration = base_declaration(options);
    declaration.name = environment::fields::kWaterDepth;
    declaration.unit = "metres";
    declaration.semantics =
        "thickness of the water column above the bed, 0 where there is no water";
    declaration.range_min = 0.0F;
    declaration.range_max = options.max_water_depth_metres;
    declaration.default_value = FieldValue::scalar(0.0F);
    return declaration;
}

[[nodiscard]] FieldDeclaration distance_declaration(const WaterFieldOptions& options) noexcept {
    FieldDeclaration declaration = base_declaration(options);
    declaration.name = environment::fields::kWaterDistance;
    declaration.unit = "metres";
    declaration.semantics =
        "horizontal distance to the nearest water, 0 in water, saturating at the declared maximum";
    declaration.range_min = 0.0F;
    declaration.range_max = options.max_water_distance_metres;
    // The default is the far end of the range and not zero: a region with no data is a region with
    // no water in it, and a default of zero would tell every consumer that the whole unstreamed
    // world is a shoreline.
    declaration.default_value = FieldValue::scalar(options.max_water_distance_metres);
    return declaration;
}

[[nodiscard]] FieldDeclaration flow_declaration(const WaterFieldOptions& options) noexcept {
    FieldDeclaration declaration = base_declaration(options);
    declaration.name = environment::fields::kWaterFlow;
    declaration.unit = "m/s";
    declaration.semantics = "horizontal water velocity, world axes, x in .x and z in .y";
    declaration.type = FieldType::Vec2;
    // f32 rather than quantised: flow is signed and small, and a UNorm16 over [-12, 12] would put
    // its zero between two representable values — a still lake would drift.
    declaration.encoding = FieldEncoding::F32;
    declaration.range_min = -options.max_flow_mps;
    declaration.range_max = options.max_flow_mps;
    declaration.default_value = FieldValue::vec2(0.0F, 0.0F);
    return declaration;
}

[[nodiscard]] FieldDeclaration wetness_declaration(const WaterFieldOptions& options) noexcept {
    FieldDeclaration declaration = base_declaration(options);
    declaration.name = (options.wetness_owner == WetnessOwner::Water)
                           ? environment::fields::kWetness
                           : kShoreWetnessField;
    declaration.unit = "fraction";
    declaration.semantics =
        (options.wetness_owner == WetnessOwner::Water)
            ? "surface wetness, 0 dry to 1 saturated"
            : "the shore's contribution to wetness, 0 dry to 1 saturated, for the wetness "
              "producer to compose with precipitation";
    declaration.encoding = FieldEncoding::UNorm8;
    declaration.range_min = 0.0F;
    declaration.range_max = 1.0F;
    declaration.default_value = FieldValue::scalar(0.0F);
    // The delta layer is where a runtime change lands over a cooked base, combined by MAX: a shore
    // that is wet from the sea and wet from the rain is wet, not twice wet.
    declaration.layer_rule = environment::FieldLayerRule::Max;
    return declaration;
}

}  // namespace

const char* wetness_owner_name(WetnessOwner owner) noexcept {
    return (owner == WetnessOwner::Water) ? "water" : "external";
}

WaterFields::WaterFields(Allocator& allocator) noexcept
    : allocator_(&allocator), points_(allocator) {}

Status WaterFields::declare(environment::FieldRegistry& registry,
                            const WaterFieldOptions& options) noexcept {
    if (options.local_cell_metres <= 0.0F ||
        options.regional_cell_metres <= options.local_cell_metres ||
        options.macro_cell_metres <= options.regional_cell_metres) {
        return fail(ErrorCode::InvalidArgument,
                    "water: the three field resolutions must be positive and coarsening outward");
    }
    if (options.max_water_depth_metres <= 0.0F || options.max_water_distance_metres <= 0.0F ||
        options.max_flow_mps <= 0.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "water: the declared depth, distance and flow ranges must be positive");
    }
    options_ = options;

    const FieldDeclaration depth = depth_declaration(options);
    const FieldDeclaration distance = distance_declaration(options);
    const FieldDeclaration flow = flow_declaration(options);
    const FieldDeclaration wetness = wetness_declaration(options);
    if (Status declared = registry.declare(depth); !declared) {
        return declared;
    }
    if (Status declared = registry.declare(distance); !declared) {
        return declared;
    }
    if (Status declared = registry.declare(flow); !declared) {
        return declared;
    }
    if (Status declared = registry.declare(wetness); !declared) {
        return declared;
    }
    depth_ = depth.id();
    distance_ = distance.id();
    flow_ = flow.id();
    wetness_ = wetness.id();
    declared_ = true;
    return ok();
}

Status WaterFields::claim(environment::FieldRegistry& registry,
                          environment::FieldStore& store) noexcept {
    if (!declared_) {
        return fail(ErrorCode::Unavailable,
                    "water: the water fields must be declared before they are claimed");
    }
    // Four claims, and any of them may be refused. The refusal is the substrate's and it names both
    // producers; this module passes it through unchanged rather than wrapping it, because the
    // message a developer needs is "weather.precipitation already produces wetness" and nothing
    // here can say it better.
    struct Claim {
        environment::FieldId field;
        environment::ProducerToken* token;
    };
    const Claim claims[4] = {{depth_, &depth_token_},
                             {distance_, &distance_token_},
                             {flow_, &flow_token_},
                             {wetness_, &wetness_token_}};
    // EVERY FIELD IS CHECKED BEFORE ANY IS CLAIMED. A claim that failed halfway would leave three
    // of the four produced by water and the configuration refused — a state in which the refusal is
    // correct and the registry no longer describes anything anybody chose. The refusal returned is
    // still the substrate's own, obtained by asking for the contested field: nothing here can name
    // both producers better than the registry that holds them.
    for (const Claim& claim : claims) {
        const environment::FieldRecord* record = registry.find(claim.field);
        if (record == nullptr || !record->claimed) {
            continue;
        }
        Expected<environment::ProducerToken, Error> refusal =
            registry.claim(claim.field, "water.shoreline", environment::ProducerKind::System);
        if (!refusal) {
            return make_unexpected(refusal.error());
        }
        // Claimed by water already: re-claiming its own field is not a conflict to report, and the
        // token just issued is the one this module keeps.
        *claim.token = std::move(*refusal);
    }
    for (const Claim& claim : claims) {
        if (claim.token->valid()) {
            continue;
        }
        Expected<environment::ProducerToken, Error> token =
            registry.claim(claim.field, "water.shoreline", environment::ProducerKind::System);
        if (!token) {
            return make_unexpected(token.error());
        }
        *claim.token = std::move(*token);
    }
    store_ = &store;
    claimed_ = true;
    return ok();
}

Status WaterFields::sample_rectangle(const ShorelineInputs& inputs, f32 cell_metres, i64 min_i,
                                     i64 min_k, u32 width, u32 height) noexcept {
    if (Status sized = points_.resize(static_cast<usize>(width) * height); !sized) {
        return sized;
    }
    const auto metres = static_cast<f64>(cell_metres);
    for (u32 k = 0; k < height; ++k) {
        for (u32 i = 0; i < width; ++i) {
            // Lattice points sit at CELL CENTRES — the half-cell offset `environment::store.cpp`
            // samples with. A publication on cell corners would be half a cell out of step with
            // every reader, which is the kind of error a screenshot shows and a test does not.
            const f64 x = (static_cast<f64>(min_i + i) + 0.5) * metres;
            const f64 z = (static_cast<f64>(min_k + k) + 0.5) * metres;
            ShorePoint point;
            point.bed = (inputs.bed_at == nullptr) ? 0.0 : inputs.bed_at(inputs.bed_user, x, z);

            WaterSample water;
            inputs.water_at(inputs.water_user, world::WorldVec3d{x, point.bed, z}, water);
            point.surface = water.surface_height;
            point.depth = water.column_thickness;
            point.flow_x = water.velocity.x;
            point.flow_z = water.velocity.z;
            point.in_water = water.column_thickness > 0.0F;
            points_[(static_cast<usize>(k) * width) + i] = point;
        }
    }
    return ok();
}

namespace {

/// The chamfer sweep's one relaxation step: a candidate distance through a neighbour, taken when it
/// is shorter. A free function over the span rather than a lambda inside the sweep, so that the two
/// passes below are two loops rather than two loops plus a capture.
struct Chamfer {
    f32 straight = 0.0F;
    f32 diagonal = 0.0F;
};

}  // namespace

void WaterFields::relax(usize target, usize source, f32 step) noexcept {
    const f32 candidate = points_[source].distance + step;
    if (candidate < points_[target].distance) {
        points_[target].distance = candidate;
    }
}

void WaterFields::sweep_forward(u32 width, u32 height, f32 straight, f32 diagonal) noexcept {
    // From the top-left neighbourhood: every point takes the best route through a neighbour the
    // sweep has already settled.
    for (u32 k = 0; k < height; ++k) {
        for (u32 i = 0; i < width; ++i) {
            const usize target = (static_cast<usize>(k) * width) + i;
            if (i > 0) {
                relax(target, target - 1, straight);
            }
            if (k == 0) {
                continue;
            }
            relax(target, target - width, straight);
            if (i > 0) {
                relax(target, target - width - 1, diagonal);
            }
            if (i + 1 < width) {
                relax(target, target - width + 1, diagonal);
            }
        }
    }
}

void WaterFields::sweep_backward(u32 width, u32 height, f32 straight, f32 diagonal) noexcept {
    // And back from the bottom-right, which is the half that finds water lying BELOW or to the
    // RIGHT of a point. Without it the field is correct only where the water happens to be on the
    // side the forward pass came from — see `tests/test_shoreline.cpp`, where a spit with water on
    // both sides is what makes the omission visible.
    for (u32 k = height; k-- > 0;) {
        for (u32 i = width; i-- > 0;) {
            const usize target = (static_cast<usize>(k) * width) + i;
            if (i + 1 < width) {
                relax(target, target + 1, straight);
            }
            if (k + 1 >= height) {
                continue;
            }
            relax(target, target + width, straight);
            if (i + 1 < width) {
                relax(target, target + width + 1, diagonal);
            }
            if (i > 0) {
                relax(target, target + width - 1, diagonal);
            }
        }
    }
}

void WaterFields::sweep_distances(u32 width, u32 height, f32 cell_metres) noexcept {
    const f32 far_distance = options_.max_water_distance_metres;
    for (ShorePoint& point : points_) {
        point.distance = point.in_water ? 0.0F : far_distance;
    }

    // A two-pass chamfer transform. The diagonal step costs sqrt(2) cells, which is what keeps a
    // distance field from being a Manhattan distance wearing a metre label.
    const Chamfer chamfer{cell_metres, cell_metres * 1.41421356F};
    sweep_forward(width, height, chamfer.straight, chamfer.diagonal);
    sweep_backward(width, height, chamfer.straight, chamfer.diagonal);

    for (ShorePoint& point : points_) {
        if (point.distance > far_distance) {
            point.distance = far_distance;
        }
    }
}

namespace {

/// Which tile a lattice index belongs to. Floor division, spelled out because C++'s `/` truncates
/// toward zero and a world west of the origin has negative lattice indices — where truncation would
/// put two different columns in one tile.
[[nodiscard]] i64 tile_of_lattice(i64 lattice) noexcept {
    const auto span = static_cast<i64>(environment::kTileCells);
    return (lattice >= 0) ? (lattice / span) : (((lattice + 1) / span) - 1);
}

}  // namespace

Status WaterFields::write_point(const ShoreWriters& writers, environment::FieldResidency level,
                                i64 lattice_x, i64 lattice_z, const ShorePoint& point,
                                f32 wet_band) noexcept {
    const auto span = static_cast<i64>(environment::kTileCells);
    const i64 tile_x = tile_of_lattice(lattice_x);
    const i64 tile_z = tile_of_lattice(lattice_z);
    const auto local_x = static_cast<u32>(lattice_x - (tile_x * span));
    const auto local_z = static_cast<u32>(lattice_z - (tile_z * span));

    environment::TileAddress address;
    address.level = static_cast<u8>(level);
    // The DELTA layer, not the base: a published shoreline is a runtime contribution over whatever
    // was cooked, combined by the rule each field declares. Writing the base would overwrite cooked
    // data with a value computed from the water that happens to be resident.
    address.layer = static_cast<u8>(environment::FieldLayer::Delta);
    address.x = static_cast<i32>(tile_x);
    address.z = static_cast<i32>(tile_z);

    // Wetness: saturated in the water, falling off over the wet band beyond its edge. The run-up is
    // what makes a rough sea wet a wider strip of beach than a calm lake does — the specification's
    // "waves wash up a beach" as a function of the authoritative band amplitudes.
    const f32 wetness =
        point.in_water ? 1.0F : math::saturate(1.0F - (point.distance / std::max(wet_band, 0.01F)));

    environment::FieldWriter* targets[4] = {writers.depth, writers.distance, writers.flow,
                                            writers.wetness};
    const environment::FieldId ids[4] = {depth_, distance_, flow_, wetness_};
    const FieldValue values[4] = {
        FieldValue::scalar(point.depth), FieldValue::scalar(point.distance),
        FieldValue::vec2(point.flow_x, point.flow_z), FieldValue::scalar(wetness)};

    for (u32 index = 0; index < 4; ++index) {
        environment::TileAddress tile = address;
        tile.field = ids[index];
        if (Status staged = targets[index]->stage(tile); !staged) {
            return staged;
        }
        if (Status set = targets[index]->set(tile, local_x, 0, local_z, values[index]); !set) {
            return set;
        }
    }
    ++published_points_;
    return ok();
}

Status WaterFields::write_tiles(environment::FieldResidency level, i64 min_i, i64 min_k, u32 width,
                                u32 height, const ShorelineInputs& inputs) noexcept {
    Expected<environment::FieldWriter, Error> depth_writer = store_->open_writer(depth_token_);
    if (!depth_writer) {
        return make_unexpected(depth_writer.error());
    }
    Expected<environment::FieldWriter, Error> distance_writer =
        store_->open_writer(distance_token_);
    if (!distance_writer) {
        return make_unexpected(distance_writer.error());
    }
    Expected<environment::FieldWriter, Error> flow_writer = store_->open_writer(flow_token_);
    if (!flow_writer) {
        return make_unexpected(flow_writer.error());
    }
    Expected<environment::FieldWriter, Error> wetness_writer = store_->open_writer(wetness_token_);
    if (!wetness_writer) {
        return make_unexpected(wetness_writer.error());
    }

    const ShoreWriters writers{&*depth_writer, &*distance_writer, &*flow_writer, &*wetness_writer};
    const f32 wet_band = options_.wet_band_metres + inputs.run_up_metres;

    for (u32 k = 0; k < height; ++k) {
        for (u32 i = 0; i < width; ++i) {
            const ShorePoint& point = points_[(static_cast<usize>(k) * width) + i];
            if (Status written = write_point(writers, level, min_i + i, min_k + k, point, wet_band);
                !written) {
                return written;
            }
        }
    }

    // Published together: every reader that samples two of these fields at one position sees one
    // shoreline rather than a depth from this frame beside a distance from the last.
    if (Status published = depth_writer->publish(); !published) {
        return published;
    }
    if (Status published = distance_writer->publish(); !published) {
        return published;
    }
    if (Status published = flow_writer->publish(); !published) {
        return published;
    }
    return wetness_writer->publish();
}

Status WaterFields::publish(const ShorelineInputs& inputs, environment::FieldResidency level,
                            f64 min_x, f64 min_z, f64 max_x, f64 max_z) noexcept {
    if (!claimed_ || store_ == nullptr) {
        return fail(ErrorCode::Unavailable,
                    "water: the water fields must be claimed before they are published");
    }
    if (!inputs.complete()) {
        return fail(ErrorCode::InvalidArgument,
                    "water: publishing the shoreline needs a water sampler");
    }
    if (max_x <= min_x || max_z <= min_z) {
        return fail(ErrorCode::InvalidArgument, "water: an empty rectangle publishes nothing");
    }

    const environment::FieldDeclaration* declaration = store_->registry().declaration(depth_);
    if (declaration == nullptr) {
        return fail(ErrorCode::NotFound, "water: the depth field is not declared");
    }
    const f32 cell_metres = declaration->levels[static_cast<u32>(level)].cell_metres;
    if (cell_metres <= 0.0F) {
        return fail(ErrorCode::InvalidArgument,
                    "water: that residency level is not declared for the water fields");
    }

    const auto metres = static_cast<f64>(cell_metres);
    const auto min_i = static_cast<i64>(std::floor(min_x / metres));
    const auto min_k = static_cast<i64>(std::floor(min_z / metres));
    const auto max_i = static_cast<i64>(std::ceil(max_x / metres));
    const auto max_k = static_cast<i64>(std::ceil(max_z / metres));
    const auto width = static_cast<u32>(max_i - min_i);
    const auto height = static_cast<u32>(max_k - min_k);
    if (width == 0 || height == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "water: the rectangle is smaller than one cell of that level");
    }

    published_points_ = 0;
    if (Status sampled = sample_rectangle(inputs, cell_metres, min_i, min_k, width, height);
        !sampled) {
        return sampled;
    }
    sweep_distances(width, height, cell_metres);
    return write_tiles(level, min_i, min_k, width, height, inputs);
}

}  // namespace cy::water
