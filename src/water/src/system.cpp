// The composition: one query over every backend, the seams, and the diagnostics. M10 task 2.3.

#include <cy/water/system.h>

#include <cy/core/math/scalar.h>

#include <cmath>

namespace cy::water {

WaterSystem::WaterSystem(Allocator& allocator, const world::PartitionConfig& partition) noexcept
    : allocator_(&allocator),
      partition_(&partition),
      registry_(allocator),
      rivers_(allocator),
      foam_(allocator),
      fields_(allocator),
      streaming_(allocator, registry_, partition),
      states_(allocator),
      dirty_(allocator) {}

WaterSystem::BodyState* WaterSystem::find_state(WaterBodyId body) noexcept {
    for (BodyState& state : states_) {
        if (state.id == body) {
            return &state;
        }
    }
    return nullptr;
}

const WaterSystem::BodyState* WaterSystem::find_state(WaterBodyId body) const noexcept {
    for (const BodyState& state : states_) {
        if (state.id == body) {
            return &state;
        }
    }
    return nullptr;
}

Status WaterSystem::ensure_state(WaterBodyId body) noexcept {
    if (find_state(body) != nullptr) {
        return ok();
    }
    BodyState state;
    state.id = body;
    return states_.push_back(state);
}

Status WaterSystem::set_ocean(WaterBodyId body, const OceanParams& params, u64 seed,
                              OceanReport& report) noexcept {
    const WaterBodyDesc* desc = registry_.describe(body);
    if (desc == nullptr) {
        return fail(ErrorCode::NotFound, "water: no such body is registered");
    }
    if (desc->backend != WaterBackend::Spectral) {
        return fail(ErrorCode::InvalidArgument,
                    "water: a spectral model belongs to a body whose backend is Spectral; a Flat "
                    "body has its mean level and a SplineFlow body has its network");
    }

    OceanParams resolved = params;
    resolved.sea_level = desc->mean_level;
    Expected<DisplacementModel, Error> model = build_ocean_model(resolved, seed, report);
    if (!model) {
        return make_unexpected(model.error());
    }
    // THE DISPLACEMENT CONTRACT, ENFORCED AT CONFIGURATION TIME. A model whose longest band is
    // visual, or whose visual bands are large enough to be felt, is refused here — before a boat
    // has floated on it and before a frame has drawn it.
    if (const DisplacementProblem problem = validate_model(*model);
        problem != DisplacementProblem::None) {
        return fail(ErrorCode::InvalidArgument, displacement_problem_name(problem));
    }

    if (Status ensured = ensure_state(body); !ensured) {
        return ensured;
    }
    BodyState* state = find_state(body);
    state->model = *model;
    state->params = resolved;
    state->has_model = true;
    return ok();
}

Status WaterSystem::set_ocean(WaterBodyId body, const OceanParams& params, u64 seed) noexcept {
    OceanReport discarded;
    return set_ocean(body, params, seed, discarded);
}

Status WaterSystem::drive_ocean_from_wind(WaterBodyId body, const Vec3& wind_mps) noexcept {
    BodyState* state = find_state(body);
    if (state == nullptr || !state->has_model) {
        return fail(ErrorCode::NotFound, "water: that body has no spectral model to drive");
    }
    OceanParams params = state->params;
    params.wind_speed_mps = std::sqrt((wind_mps.x * wind_mps.x) + (wind_mps.z * wind_mps.z));
    if (params.wind_speed_mps < 0.5F) {
        // A spectrum needs a wind. Below half a metre per second the sea is glass, and the
        // fetch-limited peak would run off to infinity; the floor keeps the model valid and the
        // amplitudes it produces are already below a centimetre.
        params.wind_speed_mps = 0.5F;
    } else {
        params.wind_direction_degrees =
            std::atan2(wind_mps.z, wind_mps.x) * (180.0F / 3.14159265358979F);
    }
    // The seed is the model's own, so a change of wind changes the sea's STATE and not its
    // identity: the same wave trains, re-weighted, rather than a different ocean.
    return set_ocean(body, params, state->model.seed);
}

const DisplacementModel* WaterSystem::model_of(WaterBodyId body) const noexcept {
    const BodyState* state = find_state(body);
    return (state == nullptr || !state->has_model) ? nullptr : &state->model;
}

const OceanParams* WaterSystem::ocean_params_of(WaterBodyId body) const noexcept {
    const BodyState* state = find_state(body);
    return (state == nullptr || !state->has_model) ? nullptr : &state->params;
}

void WaterSystem::set_bed_source(BedSource source, void* user) noexcept {
    bed_source_ = source;
    bed_user_ = user;
}

Vec3 WaterSystem::foam_velocity(void* user, const world::WorldVec3d& at) noexcept {
    const auto* system = static_cast<const WaterSystem*>(user);
    return system->query(at).velocity;
}

void WaterSystem::shoreline_water(void* user, const world::WorldVec3d& at,
                                  WaterSample& out) noexcept {
    out = static_cast<const WaterSystem*>(user)->query(at);
}

Status WaterSystem::tick(f32 seconds, const world::WorldVec3d& focus) noexcept {
    if (seconds < 0.0F) {
        return fail(ErrorCode::InvalidArgument, "water: a tick may not run backwards");
    }
    time_ += static_cast<f64>(seconds);
    if (foam_.resolution() == 0) {
        return ok();
    }
    if (Status recentred = foam_.recentre(focus); !recentred) {
        return recentred;
    }
    return foam_.advect(seconds, &WaterSystem::foam_velocity, this);
}

// --- Queries ---------------------------------------------------------------------------------

f64 WaterSystem::bed_height(const world::WorldVec3d& at, f64 fallback) const noexcept {
    if (bed_source_ == nullptr) {
        return fallback;
    }
    return bed_source_(bed_user_, at.x, at.z);
}

bool WaterSystem::provides_water(const WaterBodyRecord& record,
                                 const world::WorldVec3d& at) const noexcept {
    if (!record.desc.bounds.contains_horizontal(at)) {
        return false;
    }
    if (record.desc.backend != WaterBackend::SplineFlow) {
        return true;
    }
    // A river has water only inside its channel. This is what makes the overlap rule useful rather
    // than merely deterministic: a point on the bank between a river and the sea it flows into is
    // answered by the sea, because the river genuinely has no water there.
    const RiverSample river = rivers_.sample(at);
    return river.found && river.inside;
}

WaterSample WaterSystem::sample_body(const WaterBodyRecord& record, const world::WorldVec3d& at,
                                     bool resident) const noexcept {
    const WaterBodyDesc& desc = record.desc;
    WaterSample sample;
    sample.body = record.id;
    sample.density = desc.density;
    sample.surface_height = desc.mean_level;
    sample.resolution = resident ? WaterResolution::Simulated : WaterResolution::MeanLevel;

    f64 bed = bed_height(at, desc.bounds.min_y);

    if (resident) {
        switch (desc.backend) {
            case WaterBackend::Spectral: {
                const BodyState* state = find_state(record.id);
                if (state != nullptr && state->has_model) {
                    // THE AUTHORITATIVE BANDS, ALWAYS, FOR EVERY CALLER. The renderer's extra
                    // detail comes from `OceanSurface`, which is the only evaluation in this module
                    // that passes `BandSelection::All`.
                    const Displacement displacement = evaluate_displacement(
                        state->model, BandSelection::Authoritative, at.x, at.z, time_);
                    sample.surface_height = displacement.height;
                    sample.normal = displacement.normal;
                    sample.velocity = displacement.velocity;
                    sample.breaking = displacement.breaking;
                    ++state->queries;
                    state->cost += displacement.trains;
                    query_cost_ += displacement.trains;
                }
                break;
            }
            case WaterBackend::SplineFlow: {
                const RiverSample river = rivers_.sample(at);
                if (river.found) {
                    sample.surface_height = river.surface;
                    sample.velocity = river.velocity;
                    sample.breaking = river.turbulence;
                    // The river's own bed where terrain has nothing to say: a channel is cut into
                    // the ground and its profile is the network's, not the heightfield's.
                    bed = (bed_source_ == nullptr) ? river.bed : std::min(bed, river.bed);
                    if (!river.inside) {
                        // Outside the channel there is no water: the bed is raised to the surface
                        // so the column is empty rather than the position being declared submerged
                        // in a hillside the channel happens to pass through.
                        bed = river.surface;
                    }
                }
                break;
            }
            case WaterBackend::Flat:
            case WaterBackend::ShallowWater:
            case WaterBackend::Particle:
                break;
        }
    }

    const auto column = static_cast<f32>(sample.surface_height - bed);
    sample.column_thickness = (column > 0.0F) ? column : 0.0F;
    sample.depth_to_bed = static_cast<f32>(at.y - bed);
    const auto submersion = static_cast<f32>(sample.surface_height - at.y);
    // A position above the surface, or in a body whose column is empty here, is not in water. Both
    // halves matter: the second is what keeps a boat from floating on a dry riverbed.
    sample.submersion = (sample.column_thickness > 0.0F) ? submersion : 0.0F;
    return sample;
}

WaterSample WaterSystem::query(const world::WorldVec3d& at) const noexcept {
    ++queries_;
    const WaterBodyRecord* chosen = nullptr;
    const WaterBodyRecord* fallback = nullptr;

    // The declared resolution order, walked once. The first body that actually HAS water here
    // answers; if none does, the first body whose bounds contain the position answers with its mean
    // level, so a query inside a lake's bounds but above its water still names the lake.
    for (const WaterBodyRecord& record : registry_.bodies()) {
        if (!record.desc.bounds.contains_horizontal(at)) {
            continue;
        }
        const bool provides = provides_water(record, at);
        if (fallback == nullptr || record.desc.priority > fallback->desc.priority) {
            if (!provides) {
                fallback = &record;
            }
        }
        if (!provides) {
            continue;
        }
        if (chosen == nullptr || record.desc.priority > chosen->desc.priority) {
            chosen = &record;
        }
    }

    if (chosen == nullptr) {
        if (fallback == nullptr) {
            return WaterSample{};
        }
        ++mean_level_queries_;
        WaterSample sample = sample_body(*fallback, at, false);
        return sample;
    }

    // Residency. A system with no streamer attached answers `Simulated` everywhere: there is
    // nothing to say otherwise, which is the editor's case and the cook's.
    const bool resident = (streaming_.bound_cells() == 0)
                              ? true
                              : streaming_.payload_resident(chosen->id, at, WaterPayload::Query);
    if (!resident) {
        ++mean_level_queries_;
    }
    return sample_body(*chosen, at, resident);
}

Status WaterSystem::query_many(Span<const world::WorldVec3d> positions,
                               Span<WaterSample> out) const noexcept {
    if (positions.size() != out.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "water: a batched query needs one output per position");
    }
    for (usize index = 0; index < positions.size(); ++index) {
        out[index] = query(positions[index]);
    }
    return ok();
}

Status WaterSystem::point_report(const world::WorldVec3d& at, WaterPointReport& report,
                                 BandContribution* bands) const noexcept {
    report = WaterPointReport{};
    report.sample = query(at);
    if (!report.sample.body.is_valid()) {
        return fail(ErrorCode::NotFound, "water: no body owns that position");
    }
    const WaterBodyRecord* record = registry_.find(report.sample.body);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "water: the owning body is no longer registered");
    }
    report.body_name = record->desc.name;
    report.type = record->desc.type;
    report.backend = record->desc.backend;

    Array<WaterBodyId> overlapping(*allocator_);
    if (Status listed = registry_.bodies_at(at, overlapping); !listed) {
        return listed;
    }
    report.overlapping_bodies = static_cast<u32>(overlapping.size());

    if (const DisplacementModel* model = model_of(record->id); model != nullptr) {
        const AmplitudeSplit split = amplitude_split(*model);
        report.authoritative_metres = split.authoritative_metres;
        report.visual_metres = split.visual_metres;
        if (bands != nullptr) {
            report.band_count = band_contributions(*model, at.x, at.z, time_, bands);
        }
    }
    return ok();
}

Expected<BuoyancyResult, Error> WaterSystem::buoyancy(const BuoyancyState& state,
                                                      Span<const BuoyancySample> samples,
                                                      const BuoyancyParams& params) noexcept {
    Array<world::WorldVec3d> positions(*allocator_);
    if (Status sized = positions.resize(samples.size()); !sized) {
        return make_unexpected(sized.error());
    }
    if (Status placed = hull_positions(state, samples, positions.span()); !placed) {
        return make_unexpected(placed.error());
    }
    Array<WaterSample> water(*allocator_);
    if (Status sized = water.resize(samples.size()); !sized) {
        return make_unexpected(sized.error());
    }
    if (Status queried = query_many(positions.span(), water.span()); !queried) {
        return make_unexpected(queried.error());
    }
    buoyancy_samples_ += samples.size();
    ++buoyancy_solves_;
    return compute_buoyancy(state, samples, water.span(), params);
}

CharacterWaterState WaterSystem::character_state_at(
    const world::WorldVec3d& at, const SwimThresholds& thresholds) const noexcept {
    const WaterSample sample = query(at);
    // Derived from the DEPTH AT THE CHARACTER'S POSITION, which is the column between the bed and
    // the surface — not from how far the character's origin happens to be under the surface, which
    // would make a character on a submerged ledge swim.
    return character_state(sample.column_thickness, thresholds);
}

// --- Shoreline, navigation and level changes ---------------------------------------------------

ShorelineInputs WaterSystem::shoreline_inputs() const noexcept {
    ShorelineInputs inputs;
    inputs.water_at = &WaterSystem::shoreline_water;
    inputs.water_user = const_cast<WaterSystem*>(this);
    inputs.bed_at = bed_source_;
    inputs.bed_user = bed_user_;
    // The run-up is the authoritative amplitude of the roughest body: waves wash further up a
    // beach when the sea is rough, and the amplitude split is where that number already lives.
    f32 run_up = 0.0F;
    for (const BodyState& state : states_) {
        if (!state.has_model) {
            continue;
        }
        const AmplitudeSplit split = amplitude_split(state.model);
        if (split.authoritative_metres > run_up) {
            run_up = split.authoritative_metres;
        }
    }
    inputs.run_up_metres = (run_up > 0.0F) ? (run_up * 2.0F) : 1.0F;
    return inputs;
}

f32 WaterSystem::directional_cost(const world::WorldVec3d& from, const world::WorldVec3d& to,
                                  const WaterNavigationCosts& costs) const noexcept {
    const WaterSample start = query(from);
    const WaterSample end = query(to);
    if (start.column_thickness <= 0.0F && end.column_thickness <= 0.0F) {
        return 1.0F;
    }
    const Vec3 travel{static_cast<f32>(to.x - from.x), 0.0F, static_cast<f32>(to.z - from.z)};
    const Vec3 direction = normalized_or(travel, Vec3{1.0F, 0.0F, 0.0F});

    const bool swimming = start.submersion > 0.0F || end.submersion > 0.0F;
    const f32 base = swimming ? costs.swim_multiplier : costs.surface_multiplier;

    // The flow's component ALONG the direction of travel. Positive downstream, negative upstream,
    // and the same field a particle and the AI read — "downstream travel SHALL cost less than
    // upstream, THROUGH THE FLOW FIELD'S DIRECTIONAL COST".
    const Vec3 flow = (start.velocity + end.velocity) * 0.5F;
    const f32 along = dot(Vec3{flow.x, 0.0F, flow.z}, direction);
    const f32 assist = costs.flow_influence * (along / std::max(costs.reference_flow_mps, 0.01F));
    const f32 cost = base * (1.0F - assist);
    return (cost < costs.minimum_multiplier) ? costs.minimum_multiplier : cost;
}

Status WaterSystem::set_mean_level(WaterBodyId body, f64 level) noexcept {
    const WaterBodyRecord* record = registry_.find(body);
    if (record == nullptr) {
        return fail(ErrorCode::NotFound, "water: no such body is registered");
    }
    const f64 previous = record->desc.mean_level;
    if (level == previous) {
        return ok();
    }

    WaterBodyDesc desc = record->desc;
    desc.mean_level = level;
    if (level < desc.bounds.min_y) {
        desc.bounds.min_y = level;
    }
    if (level > desc.bounds.max_y) {
        desc.bounds.max_y = level;
    }
    // The registry owns the description, so the change goes through it rather than through a
    // pointer into its storage: `WaterRegistry::bodies()` hands out a const span for exactly that
    // reason, and a water system that wrote through it would be a second owner of the same record.
    if (Status updated = registry_.set_mean_level(body, desc.mean_level, desc.bounds); !updated) {
        return updated;
    }

    if (BodyState* state = find_state(body); state != nullptr && state->has_model) {
        state->model.mean_level = level + static_cast<f64>(state->params.tide_metres);
        state->params.sea_level = level;
    }

    // "Water level changes and flooding SHALL emit navigation dirty regions like terrain change
    // does." The rectangle is the body's own extent: a level change moves the waterline everywhere
    // the body reaches, and a smaller rectangle would be a guess about where the shore is.
    WaterDirtyRegion region;
    region.body = body;
    region.min_x = desc.bounds.min_x;
    region.min_z = desc.bounds.min_z;
    region.max_x = desc.bounds.max_x;
    region.max_z = desc.bounds.max_z;
    region.level_delta = static_cast<f32>(level - previous);
    return dirty_.push_back(region);
}

Status WaterSystem::drain_navigation_dirty(Array<WaterDirtyRegion>& out) noexcept {
    for (const WaterDirtyRegion& region : dirty_) {
        if (Status pushed = out.push_back(region); !pushed) {
            return pushed;
        }
    }
    dirty_.clear();
    return ok();
}

// --- Diagnostics -------------------------------------------------------------------------------

WaterDiagnostics WaterSystem::diagnostics() const noexcept {
    WaterDiagnostics report;
    report.bodies = static_cast<u32>(registry_.size());
    report.resident_segments = static_cast<u32>(streaming_.resident_segments());
    report.segment_bytes = streaming_.bytes();
    report.foam_bytes = foam_.bytes();
    report.queries = queries_;
    report.query_cost = query_cost_;
    report.mean_level_queries = mean_level_queries_;
    report.buoyancy_samples = buoyancy_samples_;
    report.buoyancy_solves = buoyancy_solves_;
    return report;
}

Status WaterSystem::body_diagnostics(Array<WaterBodyDiagnostics>& out) const noexcept {
    for (const WaterBodyRecord& record : registry_.bodies()) {
        WaterBodyDiagnostics entry;
        entry.id = record.id;
        entry.name = record.desc.name;
        entry.type = record.desc.type;
        entry.backend = record.desc.backend;
        for (const WaterSegment& segment : streaming_.segments()) {
            if (segment.key.body == record.id) {
                ++entry.resident_segments;
            }
        }
        if (const BodyState* state = find_state(record.id); state != nullptr) {
            entry.queries = state->queries;
            entry.simulation_cost = state->cost;
            if (state->has_model) {
                entry.split = amplitude_split(state->model);
            }
        }
        if (Status pushed = out.push_back(entry); !pushed) {
            return pushed;
        }
    }
    return ok();
}

void WaterSystem::reset_counters() noexcept {
    queries_ = 0;
    query_cost_ = 0;
    mean_level_queries_ = 0;
    buoyancy_samples_ = 0;
    buoyancy_solves_ = 0;
    for (const BodyState& state : states_) {
        state.queries = 0;
        state.cost = 0;
    }
}

}  // namespace cy::water
