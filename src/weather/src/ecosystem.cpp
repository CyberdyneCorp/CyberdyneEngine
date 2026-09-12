// Macro ecosystem state: the knock-backs weather owns, the recovery the substrate owns, and the
// biome thresholds a project declares. M10 task 3.2.

#include <cy/weather/ecosystem.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace cy::weather {

namespace {

[[nodiscard]] f32 clamp01(f32 value) noexcept {
    return std::clamp(value, 0.0F, 1.0F);
}

[[nodiscard]] i64 tile_of_lattice(i64 lattice) noexcept {
    const auto span = static_cast<i64>(environment::kTileCells);
    return (lattice >= 0) ? (lattice / span) : (((lattice + 1) / span) - 1);
}

[[nodiscard]] Status stage_scalar(environment::FieldWriter& writer, environment::FieldId field,
                                  environment::FieldResidency level, i64 lattice_x, i64 lattice_z,
                                  const environment::FieldValue& value) noexcept {
    const auto span = static_cast<i64>(environment::kTileCells);
    const i64 tile_x = tile_of_lattice(lattice_x);
    const i64 tile_z = tile_of_lattice(lattice_z);
    environment::TileAddress address;
    address.field = field;
    address.level = static_cast<u8>(level);
    // THE BASE LAYER, and this is the one place in the module that is not the delta.
    //
    // Two reasons, and they point the same way. First, `FieldStore::advance_recovery()` — the
    // substrate's own recovery, which is where "a burned forest regrows toward its potential"
    // actually happens — walks the BASE layer's tiles and reads the potential field at the same
    // address; a current state written into the delta would never be recovered, and a suite would
    // report a forest that stayed burned. Second, ecosystem state has no cooked base to preserve:
    // unlike temperature or wind, which modulate whatever a cooker baked, vegetation density IS the
    // state, so there is nothing for a delta to be a delta of.
    address.layer = static_cast<u8>(environment::FieldLayer::Base);
    address.x = static_cast<i32>(tile_x);
    address.z = static_cast<i32>(tile_z);
    if (Status staged = writer.stage(address); !staged) {
        return staged;
    }
    return writer.set(address, static_cast<u32>(lattice_x - (tile_x * span)), 0,
                      static_cast<u32>(lattice_z - (tile_z * span)), value);
}

/// The lattice rectangle a region covers at a level. Shared by every walk in this file, so that a
/// seeding, an event and a step all touch exactly the same points.
struct LatticeRect {
    i64 min_i = 0;
    i64 max_i = 0;
    i64 min_k = 0;
    i64 max_k = 0;
    f64 metres = 1.0;
};

/// The smallest difference a field's STORAGE can hold, which is not the same thing as the agreement
/// two readers are held to.
///
/// `FieldDeclaration::resolved_precision()` answers the second question — how far apart two
/// evaluations of one blend may legitimately be — and for `F32` it returns a relative allowance
/// over the declared range, which is far larger than the mantissa's own step. An accumulator needs
/// the first question, so this asks it: a quantised encoding's quantum, and for f32 the spacing of
/// the representable numbers near the top of the declared range.
[[nodiscard]] f32 storage_quantum(const environment::FieldDeclaration& declaration) noexcept {
    const f32 span = declaration.range_max - declaration.range_min;
    switch (declaration.encoding) {
        case environment::FieldEncoding::UNorm8:
            return span / 255.0F;
        case environment::FieldEncoding::UNorm16:
            return span / 65535.0F;
        case environment::FieldEncoding::Uint8:
        case environment::FieldEncoding::Uint16:
            return 1.0F;
        case environment::FieldEncoding::F32:
            break;
    }
    const f32 magnitude = (span > 1.0F) ? span : 1.0F;
    return magnitude * 1.2e-7F;
}

[[nodiscard]] LatticeRect rect_of(const PublishRegion& region, f32 cell_metres) noexcept {
    LatticeRect rect;
    rect.metres = static_cast<f64>(cell_metres);
    rect.min_i = static_cast<i64>(std::floor(region.min_x / rect.metres));
    rect.max_i = static_cast<i64>(std::floor(region.max_x / rect.metres));
    rect.min_k = static_cast<i64>(std::floor(region.min_z / rect.metres));
    rect.max_k = static_cast<i64>(std::floor(region.max_z / rect.metres));
    return rect;
}

}  // namespace

const char* ecosystem_event_kind_name(EcosystemEventKind kind) noexcept {
    switch (kind) {
        case EcosystemEventKind::Fire:
            return "fire";
        case EcosystemEventKind::Deforestation:
            return "deforestation";
        case EcosystemEventKind::Drought:
            return "drought";
        case EcosystemEventKind::Pollution:
            return "pollution";
        case EcosystemEventKind::Terraforming:
            return "terraforming";
        default:
            return "project";
    }
}

Ecosystem::Ecosystem(Allocator& allocator) noexcept : allocator_(&allocator), rules_(allocator) {}

Status Ecosystem::set_biome_rules(Span<const BiomeRule> rules) noexcept {
    if (Status sized = rules_.resize(rules.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < rules.size(); ++index) {
        rules_[index] = rules[index];
    }
    return ok();
}

Status Ecosystem::set_default_biome_rules(const WeatherFields& fields) noexcept {
    // THE TABLE IS DATA, and this is only the engine's default row set. Nothing in this file
    // switches on a biome index; a project replaces these with its own and adds a biome without an
    // engine change, which is what "expressible as declared conditions over fields" has to mean.
    const environment::FieldId moisture = fields.id(WeatherField::Moisture);
    const environment::FieldId vegetation = fields.id(WeatherField::VegetationDensity);

    BiomeRule rules[5];
    rules[0].biome = biomes::kBarren;
    rules[0].name = "barren";
    rules[0].conditions[0] = FieldCondition{vegetation, 0.0F, 0.04F};
    rules[0].condition_count = 1;

    rules[1].biome = biomes::kDesert;
    rules[1].name = "desert";
    rules[1].conditions[0] = FieldCondition{moisture, 0.0F, 0.12F};
    rules[1].condition_count = 1;

    rules[2].biome = biomes::kGrassland;
    rules[2].name = "grassland";
    rules[2].conditions[0] = FieldCondition{moisture, 0.12F, 0.32F};
    rules[2].conditions[1] = FieldCondition{vegetation, 0.04F, 0.45F};
    rules[2].condition_count = 2;

    rules[3].biome = biomes::kSavanna;
    rules[3].name = "savanna";
    rules[3].conditions[0] = FieldCondition{moisture, 0.32F, 0.5F};
    rules[3].condition_count = 1;

    rules[4].biome = biomes::kForest;
    rules[4].name = "forest";
    rules[4].conditions[0] = FieldCondition{moisture, 0.5F, 1.01F};
    rules[4].conditions[1] = FieldCondition{vegetation, 0.3F, 1.01F};
    rules[4].condition_count = 2;

    return set_biome_rules(Span<const BiomeRule>(rules, 5));
}

u32 Ecosystem::classify(const environment::FieldStore& store,
                        const world::WorldVec3d& at) const noexcept {
    for (const BiomeRule& rule : rules_.span()) {
        bool matched = true;
        for (u32 index = 0; index < rule.condition_count && matched; ++index) {
            const FieldCondition& condition = rule.conditions[index];
            const f32 value = store.sample_deterministic(condition.field, at).value.x();
            matched = value >= condition.min_value && value < condition.max_value;
        }
        if (matched && rule.condition_count > 0) {
            return rule.biome;
        }
    }
    return biomes::kBarren;
}

Expected<EcosystemReport, Error> Ecosystem::seed_potential(WeatherFields& fields,
                                                           const ClimateMap& climate,
                                                           const PublishRegion& region) noexcept {
    EcosystemReport report;
    if (!fields.claimed() || fields.store() == nullptr) {
        return fail(ErrorCode::Unavailable, "weather: seeding potential needs claimed fields");
    }
    if (!fields.declared(WeatherField::VegetationPotential)) {
        return report;
    }
    if (!region.is_valid()) {
        return fail(ErrorCode::InvalidArgument, "weather: an empty region seeds nothing");
    }

    const WeatherField wanted[3] = {WeatherField::VegetationPotential,
                                    WeatherField::MoisturePotential, WeatherField::BiomePotential};
    Array<environment::FieldWriter> writers(*allocator_);
    for (WeatherField field : wanted) {
        const environment::ProducerToken* token = fields.token(field);
        if (token == nullptr) {
            return fail(ErrorCode::PermissionDenied,
                        "weather: seeding potential needs the potential fields' tokens");
        }
        Expected<environment::FieldWriter, Error> writer = fields.store()->open_writer(*token);
        if (!writer) {
            return make_unexpected(writer.error());
        }
        if (Status pushed = writers.push_back(std::move(*writer)); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    const LatticeRect rect = rect_of(region, fields.level_metres(region.level));
    for (i64 k = rect.min_k; k <= rect.max_k; ++k) {
        for (i64 i = rect.min_i; i <= rect.max_i; ++i) {
            const f64 x = (static_cast<f64>(i) + 0.5) * rect.metres;
            const f64 z = (static_cast<f64>(k) + 0.5) * rect.metres;
            // THE CLIMATE IS READ HERE AND NOWHERE ELSE IN THE RUNTIME. Seeding is not answering a
            // question about the current state; it is writing the potential the current state will
            // recover toward, and it runs when a world is created or its climate is edited.
            const BiomePotential potential = climate.potential(x, z);
            const environment::FieldValue values[3] = {
                environment::FieldValue::scalar(potential.vegetation),
                environment::FieldValue::scalar(potential.moisture),
                environment::FieldValue::category(potential.biome)};
            for (u32 index = 0; index < 3; ++index) {
                if (Status staged = stage_scalar(writers[index], fields.id(wanted[index]),
                                                 region.level, i, k, values[index]);
                    !staged) {
                    return make_unexpected(staged.error());
                }
            }
            ++report.lattice_points;
        }
    }
    for (environment::FieldWriter& writer : writers) {
        if (Status published = writer.publish(); !published) {
            return make_unexpected(published.error());
        }
    }
    return report;
}

namespace {

/// What one knock-back does to one point's six ecosystem values. A pure function of the event and
/// the current values, so the walk below carries no branching of its own — and so a suite can drive
/// it directly.
struct EcosystemPoint {
    f32 vegetation = 0.0F;
    f32 moisture = 0.0F;
    f32 soil = 0.0F;
    f32 biomass = 0.0F;
    f32 age = 0.0F;
    f32 burn = 0.0F;
};

[[nodiscard]] EcosystemPoint knock_back(const EcosystemModel& model, const EcosystemEvent& event,
                                        const EcosystemPoint& current) noexcept {
    EcosystemPoint out = current;
    const f32 magnitude = clamp01(event.magnitude);
    switch (event.kind) {
        case EcosystemEventKind::Fire:
            out.vegetation =
                clamp01(out.vegetation * (1.0F - (model.fire_vegetation_loss * magnitude)));
            // Ash is fertiliser, so a fire takes far less soil than it takes canopy — which is
            // exactly why the specification's own scenario is that the forest comes BACK.
            out.soil = clamp01(out.soil * (1.0F - (model.fire_soil_loss * magnitude)));
            out.burn = clamp01(out.burn + magnitude);
            out.age = 0.0F;
            out.biomass = 0.0F;
            break;
        case EcosystemEventKind::Deforestation:
            out.vegetation = clamp01(out.vegetation *
                                     (1.0F - (model.deforestation_vegetation_loss * magnitude)));
            out.age = 0.0F;
            out.biomass = 0.0F;
            break;
        case EcosystemEventKind::Drought:
            out.moisture =
                clamp01(out.moisture * (1.0F - (model.drought_moisture_loss * magnitude)));
            out.vegetation = clamp01(out.vegetation * (1.0F - (0.4F * magnitude)));
            break;
        case EcosystemEventKind::Pollution:
            out.soil = clamp01(out.soil * (1.0F - (model.pollution_soil_loss * magnitude)));
            out.vegetation = clamp01(out.vegetation * (1.0F - (0.3F * magnitude)));
            break;
        case EcosystemEventKind::Terraforming:
            // The one event that moves the land TOWARD something rather than away from it. Soil and
            // moisture are driven to the declared targets, and the biome follows from the declared
            // thresholds over them — which is the "terraforming changes a biome" scenario.
            out.soil = clamp01(out.soil + ((event.target_soil_health - out.soil) * magnitude));
            out.moisture =
                clamp01(out.moisture + ((event.target_moisture - out.moisture) * magnitude));
            break;
        default:
            break;
    }
    return out;
}

}  // namespace

Expected<u64, Error> Ecosystem::edit_region(WeatherFields& fields, Span<const WeatherField> which,
                                            const PublishRegion& region, PointOp op,
                                            void* user) noexcept {
    if (!fields.claimed() || fields.store() == nullptr) {
        return fail(ErrorCode::Unavailable, "weather: editing the ecosystem needs claimed fields");
    }
    if (!region.is_valid()) {
        return fail(ErrorCode::InvalidArgument, "weather: an empty region edits nothing");
    }
    Array<environment::FieldWriter> writers(*allocator_);
    for (WeatherField field : which) {
        const environment::ProducerToken* token = fields.token(field);
        if (token == nullptr) {
            return fail(ErrorCode::PermissionDenied,
                        "weather: editing the ecosystem needs the fields' producer tokens");
        }
        Expected<environment::FieldWriter, Error> writer = fields.store()->open_writer(*token);
        if (!writer) {
            return make_unexpected(writer.error());
        }
        if (Status pushed = writers.push_back(std::move(*writer)); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    const LatticeRect rect = rect_of(region, fields.level_metres(region.level));
    u64 points = 0;
    f32 values[16] = {};
    const auto count = static_cast<u32>(which.size());
    for (i64 k = rect.min_k; k <= rect.max_k; ++k) {
        for (i64 i = rect.min_i; i <= rect.max_i; ++i) {
            const f64 x = (static_cast<f64>(i) + 0.5) * rect.metres;
            const f64 z = (static_cast<f64>(k) + 0.5) * rect.metres;
            const world::WorldVec3d at{x, 0.0, z};
            for (u32 index = 0; index < count; ++index) {
                values[index] =
                    fields.store()->sample_at(fields.id(which[index]), at, region.level).value.x();
            }
            op(user, x, z, values, count);
            for (u32 index = 0; index < count; ++index) {
                const environment::FieldDeclaration* declaration =
                    fields.store()->registry().declaration(fields.id(which[index]));
                const bool category =
                    declaration != nullptr && declaration->type == environment::FieldType::Category;
                const environment::FieldValue value =
                    category ? environment::FieldValue::category(static_cast<u32>(values[index]))
                             : environment::FieldValue::scalar(values[index]);
                if (Status staged = stage_scalar(writers[index], fields.id(which[index]),
                                                 region.level, i, k, value);
                    !staged) {
                    return make_unexpected(staged.error());
                }
            }
            ++points;
        }
    }
    for (environment::FieldWriter& writer : writers) {
        if (Status published = writer.publish(); !published) {
            return make_unexpected(published.error());
        }
    }
    return points;
}

namespace {

/// The six ecosystem fields an event or a step edits, in the order `EcosystemPoint`'s members are.
constexpr WeatherField kStateFields[6] = {WeatherField::VegetationDensity, WeatherField::Moisture,
                                          WeatherField::SoilHealth,        WeatherField::Biomass,
                                          WeatherField::ForestAge,         WeatherField::BurnState};

/// The two potentials a terraforming event moves, in the order `terraform_potential_op()` reads
/// them.
constexpr WeatherField kTerraformFields[2] = {WeatherField::MoisturePotential,
                                              WeatherField::VegetationPotential};

struct EventContext {
    const EcosystemModel* model;
    const EcosystemEvent* event;
};

void apply_event_op(void* user, f64 x, f64 z, f32* values, u32 count) {
    (void)x;
    (void)z;
    if (count < 6) {
        return;
    }
    auto* context = static_cast<EventContext*>(user);
    EcosystemPoint current{values[0], values[1], values[2], values[3], values[4], values[5]};
    const EcosystemPoint next = knock_back(*context->model, *context->event, current);
    values[0] = next.vegetation;
    values[1] = next.moisture;
    values[2] = next.soil;
    values[3] = next.biomass;
    values[4] = next.age;
    values[5] = next.burn;
}

/// Terraforming's second half: the POTENTIAL, not only the current state.
///
/// Without it a terraforming programme is a puddle. Every other event knocks the current state away
/// from a potential the climate still holds, and the substrate's recovery pulls it back — which is
/// exactly right for a fire and exactly wrong for an irrigation scheme, because irrigating a desert
/// changes what the land can SUPPORT. So this event writes the potentials as well, and the
/// specification's "desert to savanna to forest" becomes a state the world stays in rather than one
/// it visits.
void terraform_potential_op(void* user, f64 x, f64 z, f32* values, u32 count) {
    (void)x;
    (void)z;
    if (count < 2) {
        return;
    }
    const auto* event = static_cast<const EcosystemEvent*>(user);
    const f32 magnitude = clamp01(event->magnitude);
    // values[0] is the moisture potential, values[1] the vegetation potential — the order
    // `kTerraformFields` declares them in.
    values[0] = clamp01(values[0] + ((event->target_moisture - values[0]) * magnitude));
    // What a place can grow is bounded by its moisture, and raising the moisture raises that bound.
    // It never LOWERS the vegetation potential: draining a marsh does not make the surrounding
    // forest unable to grow, and a project that wants that writes the climate instead.
    const f32 supported = clamp01(values[0] * 1.15F);
    values[1] = (values[1] > supported) ? values[1] : supported;
}

struct StepContext {
    const EcosystemModel* model;
    f64 seconds;
};

void step_op(void* user, f64 x, f64 z, f32* values, u32 count) {
    (void)x;
    (void)z;
    if (count < 6) {
        return;
    }
    auto* context = static_cast<StepContext*>(user);
    const EcosystemModel& model = *context->model;
    const auto seconds = static_cast<f32>(context->seconds);

    // The BURN SCAR heals slower than the grass grows back, which is what a burned forest actually
    // looks like a decade later. Exponential, so the step size does not change the outcome.
    values[5] = clamp01(values[5] * std::exp(-model.burn_decay_per_second * seconds));

    // FOREST AGE advances with time and is reset by fire (above, in `knock_back`).
    values[4] += seconds / (365.25F * 24.0F * 3600.0F);

    // BIOMASS follows density and age together: a dense young stand and a sparse old one carry
    // different biomass, which is why this is a product rather than a function of either.
    const f32 maturity = clamp01(values[4] / std::max(1.0F, model.maturity_years));
    values[3] = model.biomass_at_maturity * values[0] * maturity;
}

}  // namespace

Expected<EcosystemReport, Error> Ecosystem::apply_event(WeatherFields& fields,
                                                        const EcosystemEvent& event) noexcept {
    EcosystemReport report;
    if (!event.is_valid()) {
        return fail(ErrorCode::InvalidArgument, "weather: an ecosystem event needs an extent");
    }
    if (!fields.declared(WeatherField::VegetationDensity)) {
        return report;
    }
    PublishRegion region;
    region.min_x = event.min_x;
    region.min_z = event.min_z;
    region.max_x = event.max_x;
    region.max_z = event.max_z;
    region.level = environment::FieldResidency::Macro;

    EventContext context{&model_, &event};
    Expected<u64, Error> points = edit_region(fields, Span<const WeatherField>(kStateFields, 6),
                                              region, apply_event_op, &context);
    if (!points) {
        return make_unexpected(points.error());
    }
    report.lattice_points = *points;

    if (event.kind == EcosystemEventKind::Terraforming &&
        fields.declared(WeatherField::MoisturePotential)) {
        // The second half, and only for this one kind. See `terraform_potential_op()`.
        Expected<u64, Error> shaped =
            edit_region(fields, Span<const WeatherField>(kTerraformFields, 2), region,
                        terraform_potential_op, const_cast<EcosystemEvent*>(&event));
        if (!shaped) {
            return make_unexpected(shaped.error());
        }
    }
    return report;
}

Status Ecosystem::check_resolution(const WeatherFields& fields) const noexcept {
    const environment::FieldStore* store = fields.store();
    if (store == nullptr) {
        return ok();
    }
    // The smallest change one macro step makes to each accumulating field, against what that field
    // can represent. The recovery figures are the substrate's own rate times the step; the age
    // figure is the step in years; the burn figure is its declared decay. See the header note.
    struct Movement {
        WeatherField field;
        f32 per_step;
    };
    const WeatherFieldOptions& options = fields.options();
    const auto step = static_cast<f32>(kEcosystemStepSeconds);
    const Movement movements[3] = {
        {WeatherField::VegetationDensity, options.vegetation_recovery_per_second * step},
        {WeatherField::ForestAge, step / (365.25F * 24.0F * 3600.0F)},
        {WeatherField::BurnState, model_.burn_decay_per_second * step}};

    for (const Movement& movement : movements) {
        const environment::FieldDeclaration* declaration =
            store->registry().declaration(fields.id(movement.field));
        if (declaration == nullptr) {
            continue;
        }
        const f32 quantum = storage_quantum(*declaration);
        if (movement.per_step >= quantum) {
            continue;
        }
        // The message NAMES THE FIELD and carries both numbers, because a developer reading
        // "something does not resolve" has to go and find which something. `FieldRegistry`'s
        // producer conflict is the shape, and the buffer has the same lifetime contract: it lives
        // as long as this object and the `Error::message` points into it.
        std::snprintf(resolution_message_, sizeof(resolution_message_),
                      "weather: '%s' moves by %g per macro step and its storage quantum is %g, so "
                      "every write would round back and the field would never evolve at all — "
                      "widen its encoding, narrow its declared range, or raise its rate",
                      declaration->name, static_cast<f64>(movement.per_step),
                      static_cast<f64>(quantum));
        return make_unexpected(Error{ErrorCode::InvalidArgument, resolution_message_, 0});
    }
    return ok();
}

/// One macro step: the model's own edits, then the substrate's recovery.
///
/// Split out of `advance()` because the two are separate claims and the loop around them is a
/// third — and because `advance()` written as one function scored 44 of cognitive complexity, past
/// what this project allows even for systems code.
Status Ecosystem::run_step(WeatherFields& fields, const PublishRegion& region,
                           EcosystemReport& report) noexcept {
    StepContext context{&model_, kEcosystemStepSeconds};
    Expected<u64, Error> points =
        edit_region(fields, Span<const WeatherField>(kStateFields, 6), region, step_op, &context);
    if (!points) {
        return make_unexpected(points.error());
    }
    report.lattice_points = *points;
    ++report.steps;

    // THE RECOVERY IS THE SUBSTRATE'S. `FieldStore::advance_recovery()` closes the gap to the
    // declared potential exponentially, for every field that declared one. There is no recovery
    // curve in this module at all — see ecosystem.h's header note.
    const WeatherField recovering[2] = {WeatherField::VegetationDensity, WeatherField::Moisture};
    for (WeatherField field : recovering) {
        const environment::ProducerToken* token = fields.token(field);
        if (token == nullptr) {
            continue;
        }
        if (Status recovered =
                fields.store()->advance_recovery(*token, static_cast<f32>(kEcosystemStepSeconds));
            !recovered) {
            return recovered;
        }
    }
    return ok();
}

/// The biome pass, over the fields the steps have just moved.
///
/// It runs ONCE per `advance()` rather than once per step: a classification is a pure function of
/// the fields it reads, so running it after every hour of a ninety-day fast-forward would produce
/// the same answer two thousand times and cost two thousand walks of the region.
Status Ecosystem::classify_region(WeatherFields& fields, const PublishRegion& region,
                                  EcosystemReport& report) noexcept {
    if (rules_.empty() || !fields.declared(WeatherField::Biome)) {
        return ok();
    }
    const environment::ProducerToken* token = fields.token(WeatherField::Biome);
    if (token == nullptr) {
        return ok();
    }
    Expected<environment::FieldWriter, Error> writer = fields.store()->open_writer(*token);
    if (!writer) {
        return make_unexpected(writer.error());
    }
    const LatticeRect rect = rect_of(region, fields.level_metres(region.level));
    for (i64 k = rect.min_k; k <= rect.max_k; ++k) {
        for (i64 i = rect.min_i; i <= rect.max_i; ++i) {
            const world::WorldVec3d at{(static_cast<f64>(i) + 0.5) * rect.metres, 0.0,
                                       (static_cast<f64>(k) + 0.5) * rect.metres};
            const u32 before = fields.store()
                                   ->sample_at(fields.id(WeatherField::Biome), at, region.level)
                                   .value.index();
            const u32 after = classify(*fields.store(), at);
            report.biome_changes += (after != before) ? 1U : 0U;
            if (Status staged = stage_scalar(*writer, fields.id(WeatherField::Biome), region.level,
                                             i, k, environment::FieldValue::category(after));
                !staged) {
                return staged;
            }
        }
    }
    return writer->publish();
}

Expected<EcosystemReport, Error> Ecosystem::advance(WeatherFields& fields,
                                                    const PublishRegion& region,
                                                    f64 seconds) noexcept {
    EcosystemReport report;
    if (!fields.declared(WeatherField::VegetationDensity)) {
        return report;
    }
    if (seconds < 0.0) {
        return fail(ErrorCode::InvalidArgument, "weather: the ecosystem cannot run backwards");
    }
    if (!checked_) {
        if (Status resolves = check_resolution(fields); !resolves) {
            return make_unexpected(resolves.error());
        }
        checked_ = true;
    }
    // WHOLE STEPS ONLY, REMAINDER CARRIED — the same rule `WeatherCells::advance()` uses, and for
    // the same reason: ninety one-day calls and one ninety-day call run the same steps in the same
    // order and reach the same state. That is what makes the editor's fast-forward the runtime's
    // own model rather than a preview of it, and `test_ecosystem.cpp` compares the two.
    carry_ += seconds;
    while (carry_ >= kEcosystemStepSeconds) {
        if (Status stepped = run_step(fields, region, report); !stepped) {
            return make_unexpected(stepped.error());
        }
        carry_ -= kEcosystemStepSeconds;
    }
    if (report.steps == 0) {
        return report;
    }
    if (Status classified = classify_region(fields, region, report); !classified) {
        return make_unexpected(classified.error());
    }
    return report;
}

Expected<EcosystemReport, Error> Ecosystem::advance_days(WeatherFields& fields,
                                                         const PublishRegion& region,
                                                         f64 days) noexcept {
    // A unit conversion, and nothing else. "Ninety days in a moment" is `advance()` — see
    // ecosystem.h — so there is no preview model here for the runtime to disagree with.
    return advance(fields, region, days * kSecondsPerDay);
}

}  // namespace cy::weather
