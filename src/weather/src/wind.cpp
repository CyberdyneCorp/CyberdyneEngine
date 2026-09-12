// The wind field: one composition, two determinism classes. M10 task 3.1.

#include <cy/weather/wind.h>

#include <cy/core/determinism/random.h>
#include <cy/weather/storm.h>

#include <algorithm>
#include <cmath>

namespace cy::weather {

namespace {

/// The gust's stream. Authoritative: a gust is felt by a projectile and hashed with the rest of the
/// state, so it is drawn from `determinism::RandomStream` like every other authoritative draw in
/// the engine — design.md §3, "M10 adds no second mechanism".
constexpr const char* kGustStream = "weather.wind.gust";

[[nodiscard]] f32 smoothstep(f32 t) noexcept {
    const f32 clamped = std::clamp(t, 0.0F, 1.0F);
    return clamped * clamped * (3.0F - (2.0F * clamped));
}

[[nodiscard]] f32 falloff_of(WindFalloff falloff, f32 normalised) noexcept {
    if (normalised >= 1.0F) {
        return 0.0F;
    }
    switch (falloff) {
        case WindFalloff::Constant:
            return 1.0F;
        case WindFalloff::Linear:
            return 1.0F - normalised;
        case WindFalloff::Smooth:
            return smoothstep(1.0F - normalised);
        case WindFalloff::InverseSquare: {
            const f32 denominator = 1.0F + (8.0F * normalised * normalised);
            return 1.0F / denominator;
        }
    }
    return 0.0F;
}

[[nodiscard]] Vec3 normalise(const Vec3& v) noexcept {
    const f32 length = std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
    if (length < 1e-6F) {
        return Vec3{1.0F, 0.0F, 0.0F};
    }
    return Vec3{v.x / length, v.y / length, v.z / length};
}

/// The nearest point on a segment, and how far along it. Used by `Spline`, where the wind follows
/// the polyline's local direction rather than pointing at its end.
[[nodiscard]] world::WorldVec3d closest_on_segment(const world::WorldVec3d& a,
                                                   const world::WorldVec3d& b,
                                                   const world::WorldVec3d& p,
                                                   Vec3& tangent_out) noexcept {
    const f64 dx = b.x - a.x;
    const f64 dy = b.y - a.y;
    const f64 dz = b.z - a.z;
    const f64 length_squared = (dx * dx) + (dy * dy) + (dz * dz);
    if (length_squared < 1e-9) {
        tangent_out = Vec3{1.0F, 0.0F, 0.0F};
        return a;
    }
    f64 t = (((p.x - a.x) * dx) + ((p.y - a.y) * dy) + ((p.z - a.z) * dz)) / length_squared;
    t = std::clamp(t, 0.0, 1.0);
    tangent_out = normalise(Vec3{static_cast<f32>(dx), static_cast<f32>(dy), static_cast<f32>(dz)});
    return world::WorldVec3d{a.x + (dx * t), a.y + (dy * t), a.z + (dz * t)};
}

/// A hash-based value noise in three dimensions, for the PRESENTATION-ONLY turbulence residual.
///
/// Deliberately NOT `determinism::RandomStream`: this value is never hashed, never replicated and
/// never read by an authoritative system, and routing it through the authoritative generator would
/// suggest to a reader that it is part of the simulation's contract. It is a pure function anyway —
/// a renderer that reprojects a frame wants the same eddy in the same place — but its determinism
/// is a convenience rather than a guarantee.
[[nodiscard]] f32 value_noise(f32 x, f32 y, f32 z) noexcept {
    const f32 s = std::sin((x * 12.9898F) + (y * 78.233F) + (z * 37.719F)) * 43758.5453F;
    return (2.0F * (s - std::floor(s))) - 1.0F;
}

}  // namespace

const char* wind_volume_kind_name(WindVolumeKind kind) noexcept {
    switch (kind) {
        case WindVolumeKind::Directional:
            return "directional";
        case WindVolumeKind::Vortex:
            return "vortex";
        case WindVolumeKind::RadialBlast:
            return "radial-blast";
        case WindVolumeKind::Updraft:
            return "updraft";
        case WindVolumeKind::Downdraft:
            return "downdraft";
        case WindVolumeKind::Spline:
            return "spline";
    }
    return "unknown";
}

const char* wind_source_kind_name(WindSourceKind kind) noexcept {
    switch (kind) {
        case WindSourceKind::Prevailing:
            return "prevailing";
        case WindSourceKind::Regional:
            return "regional";
        case WindSourceKind::Storm:
            return "storm";
        case WindSourceKind::Terrain:
            return "terrain";
        case WindSourceKind::Volume:
            return "volume";
        case WindSourceKind::Transient:
            return "transient";
        case WindSourceKind::Gust:
            return "gust";
        case WindSourceKind::Turbulence:
            return "turbulence";
        default:
            return "unknown";
    }
}

f32 WindSample::speed() const noexcept {
    const Vec3 v = authoritative();
    return std::sqrt((v.x * v.x) + (v.y * v.y) + (v.z * v.z));
}

bool WindVolume::is_valid() const noexcept {
    if (radius_metres <= 0.0F) {
        return false;
    }
    if (kind == WindVolumeKind::Spline) {
        return point_count >= 2 && point_count <= kMaxWindSplinePoints;
    }
    return true;
}

Vec3 wind_volume_velocity(const WindVolume& volume, const world::WorldVec3d& at,
                          f32* influence_out) noexcept {
    if (influence_out != nullptr) {
        *influence_out = 0.0F;
    }
    if (!volume.is_valid()) {
        return Vec3{};
    }

    world::WorldVec3d reference = volume.position;
    Vec3 tangent = normalise(volume.direction);
    if (volume.kind == WindVolumeKind::Spline) {
        // The nearest point on the polyline, and its local direction. A canyon's wind follows the
        // canyon; a volume that pointed at the spline's end would blow through its walls.
        f64 best = 1e300;
        for (u32 index = 0; index + 1 < volume.point_count; ++index) {
            Vec3 segment_tangent;
            const world::WorldVec3d candidate = closest_on_segment(
                volume.points[index], volume.points[index + 1], at, segment_tangent);
            const f64 dx = at.x - candidate.x;
            const f64 dy = at.y - candidate.y;
            const f64 dz = at.z - candidate.z;
            const f64 distance_squared = (dx * dx) + (dy * dy) + (dz * dz);
            if (distance_squared < best) {
                best = distance_squared;
                reference = candidate;
                tangent = segment_tangent;
            }
        }
    }

    const f64 dx = at.x - reference.x;
    const f64 dy = at.y - reference.y;
    const f64 dz = at.z - reference.z;
    const f64 distance = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
    const auto normalised = static_cast<f32>(distance / static_cast<f64>(volume.radius_metres));
    f32 influence = falloff_of(volume.falloff, normalised);
    if (influence <= 0.0F) {
        return Vec3{};
    }

    Vec3 velocity;
    switch (volume.kind) {
        case WindVolumeKind::Directional:
        case WindVolumeKind::Spline:
            velocity = Vec3{tangent.x * volume.strength, tangent.y * volume.strength,
                            tangent.z * volume.strength};
            break;
        case WindVolumeKind::Updraft:
            velocity = Vec3{0.0F, volume.strength, 0.0F};
            break;
        case WindVolumeKind::Downdraft:
            velocity = Vec3{0.0F, -volume.strength, 0.0F};
            break;
        case WindVolumeKind::RadialBlast: {
            // The shell: strongest at `shell_speed * age`, which the caller expresses by moving the
            // volume's radius. Here the blast is outward with a hollow centre, so a character at
            // the epicentre is not pushed in a random direction by floating-point noise.
            const Vec3 outward = (distance < 1e-3)
                                     ? Vec3{0.0F, 1.0F, 0.0F}
                                     : normalise(Vec3{static_cast<f32>(dx), static_cast<f32>(dy),
                                                      static_cast<f32>(dz)});
            const f32 shell = smoothstep(normalised * 2.0F) * influence;
            velocity =
                Vec3{outward.x * volume.strength * shell, outward.y * volume.strength * shell,
                     outward.z * volume.strength * shell};
            influence = shell;
            break;
        }
        case WindVolumeKind::Vortex: {
            const Vec3 axis = normalise(volume.direction);
            const Vec3 radial =
                Vec3{static_cast<f32>(dx), static_cast<f32>(dy), static_cast<f32>(dz)};
            // Tangential = axis x radial. The rotation, the inward pull and the lift are the three
            // components a tornado actually has, and the volume declares their proportions.
            const Vec3 tangential{(axis.y * radial.z) - (axis.z * radial.y),
                                  (axis.z * radial.x) - (axis.x * radial.z),
                                  (axis.x * radial.y) - (axis.y * radial.x)};
            const Vec3 around = normalise(tangential);
            const Vec3 inward =
                (distance < 1e-3) ? Vec3{} : normalise(Vec3{-radial.x, -radial.y, -radial.z});
            const f32 rotation = 1.0F - volume.inward;
            velocity =
                Vec3{((around.x * rotation) + (inward.x * volume.inward) + (axis.x * volume.lift)) *
                         volume.strength,
                     ((around.y * rotation) + (inward.y * volume.inward) + (axis.y * volume.lift)) *
                         volume.strength,
                     ((around.z * rotation) + (inward.z * volume.inward) + (axis.z * volume.lift)) *
                         volume.strength};
            break;
        }
    }
    if (influence_out != nullptr) {
        *influence_out = influence;
    }
    return Vec3{velocity.x * influence, velocity.y * influence, velocity.z * influence};
}

WindComposer::WindComposer(Allocator& allocator) noexcept
    : allocator_(&allocator), volumes_(allocator), transients_(allocator) {}

Status WindComposer::add_volume(const WindVolume& volume) noexcept {
    if (!volume.is_valid()) {
        return fail(ErrorCode::InvalidArgument,
                    "weather: a wind volume needs a positive radius, and a spline needs points");
    }
    return volumes_.push_back(volume);
}

void WindComposer::clear_volumes() noexcept {
    volumes_.clear();
}

void WindComposer::set_transient_budget(u32 budget) noexcept {
    transient_budget_ = budget;
    // A budget reduced below what is already registered drops the excess immediately and by the
    // same rule, rather than waiting for the next registration to notice.
    if (Status enforced = enforce_budget(); !enforced) {
        return;
    }
}

Status WindComposer::enforce_budget() noexcept {
    if (transients_.size() <= transient_budget_) {
        return ok();
    }
    // THE TOTAL ORDER THE DROP USES: priority descending, then identity ascending. Never
    // registration order — see wind.h's header note, and `test_wind.cpp`, which registers the same
    // set forwards and backwards and requires the kept sets to be identical.
    std::ranges::sort(transients_, [](const TransientWindSource& a, const TransientWindSource& b) {
        if (a.volume.priority != b.volume.priority) {
            return a.volume.priority > b.volume.priority;
        }
        return a.id.value < b.id.value;
    });
    while (transients_.size() > transient_budget_) {
        last_dropped_ = transients_[transients_.size() - 1].id;
        ++dropped_;
        if (Status shrunk = transients_.resize(transients_.size() - 1); !shrunk) {
            return shrunk;
        }
    }
    return ok();
}

Status WindComposer::add_transient(const TransientWindSource& source) noexcept {
    if (!source.id.is_valid()) {
        return fail(ErrorCode::InvalidArgument,
                    "weather: a transient wind source needs a non-zero identity");
    }
    if (!source.volume.is_valid()) {
        return fail(ErrorCode::InvalidArgument,
                    "weather: a transient wind source needs a valid volume");
    }
    for (TransientWindSource& existing : transients_.span()) {
        if (existing.id == source.id) {
            existing = source;
            return ok();
        }
    }
    if (Status pushed = transients_.push_back(source); !pushed) {
        return pushed;
    }
    return enforce_budget();
}

Status WindComposer::remove_transient(WindSourceId id) noexcept {
    for (usize index = 0; index < transients_.size(); ++index) {
        if (transients_[index].id == id) {
            for (usize shift = index + 1; shift < transients_.size(); ++shift) {
                transients_[shift - 1] = transients_[shift];
            }
            return transients_.resize(transients_.size() - 1);
        }
    }
    return fail(ErrorCode::NotFound, "weather: no transient wind source with that identity");
}

Status WindComposer::advance(f32 seconds) noexcept {
    usize live = 0;
    for (auto source : transients_) {
        source.age_seconds += seconds;
        if (source.lifetime_seconds > 0.0F && source.age_seconds >= source.lifetime_seconds) {
            continue;
        }
        transients_[live] = source;
        ++live;
    }
    return transients_.resize(live);
}

Vec3 WindComposer::terrain_influence(const world::WorldVec3d& at,
                                     const Vec2& horizontal) const noexcept {
    if (!terrain_.installed()) {
        return Vec3{};
    }
    const f32 speed = std::sqrt((horizontal.x * horizontal.x) + (horizontal.y * horizontal.y));
    if (speed < 0.01F) {
        return Vec3{};
    }
    // The slope ALONG the wind and the slope ACROSS it, over a short baseline. The two carry the
    // specification's three effects between them: along-slope is blocking on the lee and
    // acceleration over the ridge (one coefficient with the sign of the slope), and across-slope is
    // channelling — a wind in a valley turns to run along it.
    constexpr f64 kBaseline = 40.0;
    const f32 ux = horizontal.x / speed;
    const f32 uz = horizontal.y / speed;
    const f64 step_x = static_cast<f64>(ux) * kBaseline;
    const f64 step_z = static_cast<f64>(uz) * kBaseline;
    const f64 ahead = terrain_.elevation(at.x + step_x, at.z + step_z);
    const f64 behind = terrain_.elevation(at.x - step_x, at.z - step_z);
    const f64 left = terrain_.elevation(at.x - step_z, at.z + step_x);
    const f64 right = terrain_.elevation(at.x + step_z, at.z - step_x);
    const auto along = static_cast<f32>((ahead - behind) / (2.0 * kBaseline));
    const auto across = static_cast<f32>((left - right) / (2.0 * kBaseline));

    // Uphill: the air is squeezed over the ridge and speeds up, and some of it goes vertical.
    // Downhill: the lee is sheltered, and the along term is negative, so the same expression
    // produces the blocking.
    constexpr f32 kRidgeGain = 0.8F;
    constexpr f32 kChannelGain = 1.6F;
    const f32 accelerate = kRidgeGain * along * speed;
    return Vec3{(ux * accelerate) - (uz * across * kChannelGain * speed), along * speed * 0.6F,
                (uz * accelerate) + (ux * across * kChannelGain * speed)};
}

Vec3 WindComposer::gust_at(const world::WorldVec3d& at, determinism::SimulationPoint when,
                           f64 seconds, f32 mean_speed) const noexcept {
    if (gust_.period_seconds <= 0.0F || gust_.strength <= 0.0F) {
        return Vec3{};
    }
    // A gust is a draw per (cycle, gust cell), interpolated across the cycle. Two properties fall
    // out of that and both matter: it is CONTINUOUS in time, so a tree does not snap between
    // cycles; and it is a PURE FUNCTION of (seed, cycle, cell), so a rollback that rewinds three
    // ticks and a peer that joined late compute the same gust without any state travelling.
    const f64 phase = seconds / static_cast<f64>(gust_.period_seconds);
    const auto cycle = static_cast<u64>(std::floor(phase));
    const auto blend = static_cast<f32>(phase - static_cast<f64>(cycle));

    const auto correlation = static_cast<f64>(gust_.correlation_metres);
    const auto cell_x = static_cast<i64>(std::floor(at.x / correlation));
    const auto cell_z = static_cast<i64>(std::floor(at.z / correlation));
    const u64 cell = (static_cast<u64>(static_cast<u32>(cell_x)) << 32U) |
                     static_cast<u64>(static_cast<u32>(cell_z));

    const determinism::RandomStream stream(seed_, determinism::stream_id(kGustStream),
                                           determinism::StreamPurpose::Authoritative);
    const auto draw = [&](u64 index) {
        return Vec3{(2.0F * stream.unit_float(when, cell, index * 3U)) - 1.0F,
                    (2.0F * stream.unit_float(when, cell, (index * 3U) + 1U)) - 1.0F,
                    (2.0F * stream.unit_float(when, cell, (index * 3U) + 2U)) - 1.0F};
    };
    const Vec3 from = draw(cycle);
    const Vec3 to = draw(cycle + 1);
    const f32 t = smoothstep(blend);
    const f32 amplitude = gust_.strength * mean_speed;
    return Vec3{(from.x + ((to.x - from.x) * t)) * amplitude,
                (from.y + ((to.y - from.y) * t)) * amplitude * gust_.vertical_share,
                (from.z + ((to.z - from.z) * t)) * amplitude};
}

Vec3 WindComposer::turbulence_at(const world::WorldVec3d& at, f64 seconds,
                                 f32 mean_speed) const noexcept {
    if (turbulence_.scale_metres <= 0.0F || turbulence_.strength <= 0.0F) {
        return Vec3{};
    }
    const auto scale = static_cast<f64>(turbulence_.scale_metres);
    const auto x = static_cast<f32>(at.x / scale);
    const auto y = static_cast<f32>(at.y / scale);
    const auto z = static_cast<f32>(at.z / scale);
    const auto t = static_cast<f32>(seconds * static_cast<f64>(turbulence_.rate));
    const f32 amplitude = turbulence_.strength * mean_speed;
    return Vec3{value_noise(x + t, y, z) * amplitude, value_noise(x, y + t, z) * amplitude * 0.5F,
                value_noise(x, y, z + t) * amplitude};
}

void WindComposer::Emitter::add(WindSourceKind kind, const Vec3& velocity, u64 source) noexcept {
    if (sample != nullptr) {
        Vec3* target = &sample->base;
        if (kind == WindSourceKind::Gust) {
            target = &sample->gust;
        } else if (kind == WindSourceKind::Turbulence) {
            target = &sample->turbulence;
        }
        target->x += velocity.x;
        target->y += velocity.y;
        target->z += velocity.z;
    }
    if (written < out.size()) {
        out[written] = WindContribution{kind, velocity, source};
        ++written;
    }
}

void WindComposer::compose(const world::WorldVec3d& at, determinism::SimulationClass reader,
                           determinism::SimulationPoint when, f64 seconds,
                           Emitter& emit) const noexcept {
    // PREVAILING, from the climate the CELL was seeded with — never from the climate map. See
    // climate.h's header note.
    Vec2 regional_wind{0.0F, 0.0F};
    if (cells_ != nullptr) {
        const ClimateSample climate = cells_->climate_at(at.x, at.z);
        emit.add(WindSourceKind::Prevailing,
                 Vec3{climate.prevailing_wind.x, 0.0F, climate.prevailing_wind.y}, 0);

        // REGIONAL: the cell's own wind, over and above the prevailing it relaxed from.
        const WeatherCellSample cell = cells_->sample(at, WeatherScale::Local);
        regional_wind = cell.state.wind;
        emit.add(WindSourceKind::Regional,
                 Vec3{cell.state.wind.x - climate.prevailing_wind.x, cell.state.vertical_wind,
                      cell.state.wind.y - climate.prevailing_wind.y},
                 0);
    }

    // STORMS, each reported separately so the inspector can name the one that is blowing.
    if (storms_ != nullptr) {
        for (const Storm& storm : storms_->storms()) {
            const StormContribution contribution = storm_contribution_at(storm, at.x, at.z);
            if (contribution.influence <= 0.0F) {
                continue;
            }
            emit.add(WindSourceKind::Storm, Vec3{contribution.wind.x, 0.0F, contribution.wind.y},
                     storm.id.value);
            regional_wind.x += contribution.wind.x;
            regional_wind.y += contribution.wind.y;
        }
    }

    // TERRAIN: blocking, channelling and ridge acceleration, from the wind that has been composed
    // so far — which is why it comes after the storms and before the volumes.
    const Vec3 terrain = terrain_influence(at, regional_wind);
    if (terrain.x != 0.0F || terrain.y != 0.0F || terrain.z != 0.0F) {
        emit.add(WindSourceKind::Terrain, terrain, 0);
    }

    for (usize index = 0; index < volumes_.size(); ++index) {
        f32 influence = 0.0F;
        const Vec3 velocity = wind_volume_velocity(volumes_[index], at, &influence);
        if (influence > 0.0F) {
            emit.add(WindSourceKind::Volume, velocity, index + 1);
        }
    }

    for (const TransientWindSource& source : transients_.span()) {
        f32 influence = 0.0F;
        Vec3 velocity = wind_volume_velocity(source.volume, at, &influence);
        if (influence <= 0.0F) {
            continue;
        }
        if (source.fade) {
            const f32 remaining = source.remaining();
            velocity = Vec3{velocity.x * remaining, velocity.y * remaining, velocity.z * remaining};
        }
        emit.add(WindSourceKind::Transient, velocity, source.id.value);
    }

    // THE GUST is authoritative and is added for every reader. THE TURBULENCE is presentation and
    // is added only for a reader `determinism::may_read()` allows it to — the engine's own firewall
    // predicate, at the one place the two classes meet.
    const f32 mean_speed = (emit.sample == nullptr)
                               ? 0.0F
                               : std::sqrt((emit.sample->base.x * emit.sample->base.x) +
                                           (emit.sample->base.z * emit.sample->base.z));
    emit.add(WindSourceKind::Gust, gust_at(at, when, seconds, mean_speed), 0);
    if (determinism::may_read(reader, determinism::SimulationClass::Presentation)) {
        emit.add(WindSourceKind::Turbulence, turbulence_at(at, seconds, mean_speed), 0);
    }
}

WindSample WindComposer::sample(const world::WorldVec3d& at, determinism::SimulationClass reader,
                                determinism::SimulationPoint when, f64 seconds) const noexcept {
    WindSample out;
    Emitter emit;
    emit.sample = &out;
    compose(at, reader, when, seconds, emit);
    return out;
}

Status WindComposer::sample_many(Span<const world::WorldVec3d> positions,
                                 determinism::SimulationClass reader,
                                 determinism::SimulationPoint when, f64 seconds,
                                 Span<WindSample> out) const noexcept {
    if (out.size() < positions.size()) {
        return fail(ErrorCode::BufferTooSmall,
                    "weather: the wind batch output is shorter than its input");
    }
    // No allocation, by construction: the loop writes into the caller's span and the composition
    // holds nothing per position. "Sampling SHALL be batchable and SHALL NOT allocate."
    for (usize index = 0; index < positions.size(); ++index) {
        out[index] = sample(positions[index], reader, when, seconds);
    }
    return ok();
}

usize WindComposer::explain(const world::WorldVec3d& at, determinism::SimulationClass reader,
                            determinism::SimulationPoint when, f64 seconds,
                            Span<WindContribution> out) const noexcept {
    WindSample sample_out;
    Emitter emit;
    // BOTH filled: the contributions AND the sample, from ONE walk. That is what makes the
    // inspector's sum equal the sampled wind exactly rather than approximately — see
    // diagnostics.h's header note.
    emit.sample = &sample_out;
    emit.out = out;
    compose(at, reader, when, seconds, emit);
    return emit.written;
}

}  // namespace cy::weather
