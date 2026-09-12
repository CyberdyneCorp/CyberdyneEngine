// Foliage's seat at the table: the field it produces, the fields it reads, the regional state it
// derives, and the one call that turns a region into a cluster. See system.h for the two notes.

#include <cy/foliage/system.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::foliage {

environment::FieldDeclaration vegetation_field_declaration(f32 macro_cell_metres,
                                                           f32 recovery_per_second) noexcept {
    environment::FieldDeclaration declaration;
    declaration.name = kVegetationField;
    declaration.unit = "fraction";
    declaration.semantics =
        "how much plant life this position currently carries, 0 bare to 1 fully vegetated; "
        "produced by foliage, read by placement, materials, audio and navigation";
    declaration.type = environment::FieldType::Scalar;
    // UNorm8: a world-scale vegetation field at f32 would cost four times what a quantity nobody
    // can distinguish at 1/255 needs.
    declaration.encoding = environment::FieldEncoding::UNorm8;
    declaration.interpolation = environment::FieldInterpolation::Linear;
    declaration.cadence = environment::FieldCadence::SlowlyVarying;
    declaration.production = environment::FieldProduction::Cpu;
    declaration.range_min = 0.0F;
    declaration.range_max = 1.0F;
    declaration.default_value = environment::FieldValue::scalar(1.0F);
    // The macro level is resident everywhere. Both halves of the design depend on it: the
    // determinism requirement, and the fact that a region nobody has loaded still has an ecosystem
    // state that can evolve.
    declaration.levels[static_cast<u32>(environment::FieldResidency::Macro)] =
        environment::FieldLevel{macro_cell_metres, true};
    declaration.levels[static_cast<u32>(environment::FieldResidency::Regional)] =
        environment::FieldLevel{macro_cell_metres * 0.25F, false};
    declaration.layer_rule = environment::FieldLayerRule::Multiply;
    // Persistent: a burned forest must survive a save. `Persistent` is `Authoritative` plus the
    // overlay, which is exactly what this field is.
    declaration.classification = determinism::SimulationClass::Persistent;
    declaration.gameplay_level = environment::FieldResidency::Macro;
    declaration.persistent = true;
    declaration.potential = environment::field_id(kVegetationPotentialField);
    declaration.recovery_per_second = recovery_per_second;
    return declaration;
}

environment::FieldDeclaration vegetation_potential_declaration(f32 macro_cell_metres) noexcept {
    environment::FieldDeclaration declaration;
    declaration.name = kVegetationPotentialField;
    declaration.unit = "fraction";
    declaration.semantics =
        "the vegetation this position would carry given time; what the current state recovers "
        "toward, so a burned forest is still forest country";
    declaration.type = environment::FieldType::Scalar;
    declaration.encoding = environment::FieldEncoding::UNorm8;
    declaration.interpolation = environment::FieldInterpolation::Linear;
    declaration.cadence = environment::FieldCadence::Static;
    declaration.production = environment::FieldProduction::Cpu;
    declaration.range_min = 0.0F;
    declaration.range_max = 1.0F;
    declaration.default_value = environment::FieldValue::scalar(1.0F);
    declaration.levels[static_cast<u32>(environment::FieldResidency::Macro)] =
        environment::FieldLevel{macro_cell_metres, true};
    declaration.classification = determinism::SimulationClass::Persistent;
    declaration.gameplay_level = environment::FieldResidency::Macro;
    return declaration;
}

const char* regional_state_name(RegionalState state) noexcept {
    switch (state) {
        case RegionalState::Normal:
            return "normal";
        case RegionalState::Wet:
            return "wet";
        case RegionalState::Dry:
            return "dry";
        case RegionalState::Burning:
            return "burning";
        case RegionalState::Burned:
            return "burned";
        case RegionalState::SnowCovered:
            return "snow-covered";
        case RegionalState::kCount:
            break;
    }
    return "unknown";
}

StateBindings StateBindings::standard() noexcept {
    StateBindings bindings;
    bindings.wetness = environment::field_id(environment::fields::kWetness);
    bindings.moisture = environment::field_id(environment::fields::kMoisture);
    bindings.burn_state = environment::field_id(environment::fields::kBurnState);
    bindings.snow_depth = environment::field_id(environment::fields::kSnowDepth);
    return bindings;
}

namespace {

struct FieldReading {
    f32 value = 0.0F;
    u64 version = 0;
    bool present = false;
};

[[nodiscard]] FieldReading read_state_field(const environment::FieldStore& fields,
                                            environment::FieldId field,
                                            const world::WorldVec3d& at) noexcept {
    FieldReading reading;
    if (!field.is_valid() || fields.registry().declaration(field) == nullptr) {
        return reading;
    }
    // Deterministic, for the reason system.h gives: a material that shaded a forest burned on one
    // machine and unburned on another because one had streamed the fine tile is exactly the
    // streaming dependence the substrate forbids of a gameplay-visible field.
    const environment::FieldSample sample = fields.sample_deterministic(field, at);
    reading.value = sample.value.x();
    reading.version = sample.version;
    reading.present = true;
    return reading;
}

}  // namespace

RegionalStateSample regional_state_at(const environment::FieldStore& fields,
                                      const StateBindings& bindings,
                                      const StateThresholds& thresholds,
                                      const world::WorldVec3d& at) noexcept {
    const FieldReading wetness = read_state_field(fields, bindings.wetness, at);
    const FieldReading moisture = read_state_field(fields, bindings.moisture, at);
    const FieldReading burn = read_state_field(fields, bindings.burn_state, at);
    const FieldReading snow = read_state_field(fields, bindings.snow_depth, at);

    RegionalStateSample sample;
    sample.wetness = wetness.value;
    sample.moisture = moisture.value;
    sample.burn = burn.value;
    sample.snow_depth = snow.value;

    // Ordered most decisive first. Burning outranks snow, because a burning tree under snow is
    // burning; snow outranks wet, because what a material shows is the snow.
    if (burn.present && burn.value >= thresholds.burning_above) {
        sample.state = RegionalState::Burning;
        sample.version = burn.version;
    } else if (burn.present && burn.value >= thresholds.burned_above) {
        sample.state = RegionalState::Burned;
        sample.version = burn.version;
    } else if (snow.present && snow.value >= thresholds.snow_depth_above_metres) {
        sample.state = RegionalState::SnowCovered;
        sample.version = snow.version;
    } else if (wetness.present && wetness.value >= thresholds.wet_above) {
        sample.state = RegionalState::Wet;
        sample.version = wetness.version;
    } else if (moisture.present && moisture.value <= thresholds.dry_below) {
        sample.state = RegionalState::Dry;
        sample.version = moisture.version;
    } else {
        sample.state = RegionalState::Normal;
        sample.version = moisture.present ? moisture.version : 0;
    }
    return sample;
}

// --- FoliageSystem
// -------------------------------------------------------------------------------

FoliageSystem::FoliageSystem(Allocator& allocator, const SpeciesLibrary& library) noexcept
    : allocator_(&allocator), library_(&library) {}

Status FoliageSystem::register_producer(environment::FieldRegistry& registry,
                                        const char* producer_name, f32 macro_cell_metres,
                                        f32 recovery_per_second) noexcept {
    // The potential is declared first, so the current field's `potential` link resolves to a field
    // the registry knows about. Nothing claims it here: it is cooked, and a cooker is its producer.
    if (Status declared = registry.declare(vegetation_potential_declaration(macro_cell_metres));
        !declared) {
        return declared;
    }
    if (Status declared =
            registry.declare(vegetation_field_declaration(macro_cell_metres, recovery_per_second));
        !declared) {
        return declared;
    }
    vegetation_ = environment::field_id(kVegetationField);
    potential_ = environment::field_id(kVegetationPotentialField);
    // The refusal is NOT caught here: a second foliage system claiming `vegetation` must fail with
    // both producers named, and swallowing it would be foliage giving itself an exemption from the
    // rule it is the second user of.
    Expected<environment::ProducerToken, Error> token =
        registry.claim(vegetation_, producer_name, environment::ProducerKind::System);
    if (!token) {
        return make_unexpected(token.error());
    }
    token_ = static_cast<environment::ProducerToken&&>(token.value());
    return ok();
}

Status FoliageSystem::declare_consumption(environment::FieldRegistry& registry,
                                          const char* consumer_name, const PlacementRuleSet& rules,
                                          const FieldBindings& bindings,
                                          const StateBindings& state) noexcept {
    // Placement is gameplay-visible: a forest that differed between two machines would be different
    // cover. The class it declares is therefore `Persistent`, and `FieldRegistry::validate()` will
    // report a crossing if any rule reads a field declared `Presentation`.
    const determinism::SimulationClass reader = determinism::SimulationClass::Persistent;
    // Each (consumer, field) is declared ONCE. Two rules that both read moisture, or a rule and the
    // state bindings that do, are one read of one field — and declaring it twice would make
    // `validate()` report a count of declarations rather than a count of crossings.
    Array<environment::FieldId> declared_fields(*allocator_);
    const auto declare_once = [&](environment::FieldId field) noexcept -> Status {
        if (!field.is_valid() || registry.declaration(field) == nullptr) {
            return ok();
        }
        for (environment::FieldId already : declared_fields) {
            if (already == field) {
                return ok();
            }
        }
        if (Status pushed = declared_fields.push_back(field); !pushed) {
            return pushed;
        }
        return registry.declare_consumer(consumer_name, reader, field);
    };

    for (const PlacementRule& rule : rules.rules) {
        for (const RuleTest& test : rule.active_tests()) {
            if (Status declared = declare_once(bindings.field_for(test.input)); !declared) {
                return declared;
            }
        }
    }
    const environment::FieldId state_fields[] = {state.wetness, state.moisture, state.burn_state,
                                                 state.snow_depth};
    for (environment::FieldId field : state_fields) {
        if (Status declared = declare_once(field); !declared) {
            return declared;
        }
    }
    return ok();
}

Expected<MaterialisedRegion, Error> FoliageSystem::materialise(const GenerationContext& context,
                                                               ClusterCoord region,
                                                               const ExceptionStore* exceptions,
                                                               const ResolutionPolicy& policy,
                                                               ClusterId anchored_to) noexcept {
    Expected<FoliagePopulation, Error> population = generate_region(*allocator_, context, region);
    if (!population) {
        return make_unexpected(population.error());
    }

    // ADDED exceptions change slot numbering, so the cluster has to be rebuilt with them in rather
    // than patched afterwards. The builder is the only place slots are decided; see exceptions.h.
    const ClusterId anchor = anchored_to.is_valid() ? anchored_to : population.value().cluster.id();
    if (exceptions != nullptr && !exceptions->of_cluster(anchor).empty()) {
        const ClusterBounds bounds = population.value().cluster.bounds();
        ClusterBuilder builder(*allocator_, context.policy, population.value().cluster.id(), region,
                               bounds);
        for (u32 slot = 0; slot < static_cast<u32>(population.value().cluster.size()); ++slot) {
            const FoliageInstance* instance = population.value().cluster.at(slot);
            if (instance == nullptr) {
                continue;
            }
            const world::WorldVec3d at = bounds.decode(*instance);
            const f32 yaw = static_cast<f32>(instance->yaw) * (math::kTwoPi / 65536.0F);
            const f32 scale = static_cast<f32>(instance->scale) * (1.0F / 65535.0F);
            if (Status added = builder.add(
                    population.value().cluster.species_at(instance->species_slot), at, yaw, scale,
                    instance->variation, instance->age, 0.0F, 0.0F, instance->flags);
                !added) {
                return make_unexpected(added.error());
            }
        }
        Expected<u32, Error> injected = apply_added(*exceptions, anchor, bounds, builder);
        if (!injected) {
            return make_unexpected(injected.error());
        }
        Expected<FoliageCluster, Error> rebuilt = builder.finish();
        if (!rebuilt) {
            return make_unexpected(rebuilt.error());
        }
        population.value().cluster = static_cast<FoliageCluster&&>(rebuilt.value());
        population.value().report = builder.report();
    }

    MaterialisedRegion result(static_cast<FoliageCluster&&>(population.value().cluster),
                              static_cast<Provenance&&>(population.value().provenance),
                              *allocator_);
    result.report = population.value().report;
    result.diagnostic = population.value().diagnostic;

    if (exceptions != nullptr) {
        Expected<ResolutionReport, Error> resolved =
            exceptions->resolve(context.seed, anchor, result.cluster, policy, result.resolutions);
        if (!resolved) {
            return make_unexpected(resolved.error());
        }
        result.exceptions = resolved.value();
        Expected<u32, Error> applied =
            exceptions->apply(anchor, result.resolutions.span(), result.cluster);
        if (!applied) {
            return make_unexpected(applied.error());
        }
    }
    return result;
}

Expected<u32, Error> FoliageSystem::publish_vegetation(environment::FieldStore& fields,
                                                       const ClusterStore& clusters,
                                                       const ClusterPolicy& policy,
                                                       environment::FieldResidency level) noexcept {
    if (!token_.valid()) {
        return fail(ErrorCode::PermissionDenied,
                    "foliage has not claimed the vegetation field: register_producer() first");
    }
    const environment::FieldDeclaration* declaration = fields.registry().declaration(vegetation_);
    if (declaration == nullptr) {
        return fail(ErrorCode::NotFound, "the vegetation field is not declared");
    }
    const environment::FieldLevel& resolution = declaration->levels[static_cast<u32>(level)];
    if (!resolution.declared()) {
        return fail(ErrorCode::InvalidArgument, "the vegetation field declares no such level");
    }
    Expected<environment::FieldWriter, Error> writer = fields.open_writer(token_);
    if (!writer) {
        return make_unexpected(writer.error());
    }

    const f32 cell = resolution.cell_metres;
    const f64 tile_metres = static_cast<f64>(cell) * static_cast<f64>(environment::kTileCells);
    u32 tiles = 0;
    for (const FoliageCluster& cluster : clusters.clusters()) {
        // How much vegetation this cluster actually carries, against what its policy targets. A
        // measurement of the world rather than a repetition of the rules: a cluster the rules
        // wanted full and the terrain refused reports what is there.
        const f32 carried = policy.target_instances == 0
                                ? 0.0F
                                : math::clamp(static_cast<f32>(cluster.size()) /
                                                  static_cast<f32>(policy.target_instances),
                                              0.0F, 1.0F);
        const auto tile_x = static_cast<i32>(std::floor(cluster.bounds().centre().x / tile_metres));
        const auto tile_z = static_cast<i32>(std::floor(cluster.bounds().centre().z / tile_metres));
        environment::TileAddress address;
        address.field = vegetation_;
        address.x = tile_x;
        address.z = tile_z;
        address.level = static_cast<u8>(level);
        address.layer = static_cast<u8>(environment::FieldLayer::Delta);
        if (Status staged = writer.value().stage(address); !staged) {
            return make_unexpected(staged.error());
        }
        if (Status filled = writer.value().fill(address, environment::FieldValue::scalar(carried));
            !filled) {
            return make_unexpected(filled.error());
        }
        ++tiles;
    }
    if (Status published = writer.value().publish(); !published) {
        return make_unexpected(published.error());
    }
    return tiles;
}

}  // namespace cy::foliage
