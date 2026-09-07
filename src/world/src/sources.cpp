#include <cy/world/sources.h>

#include <algorithm>
#include <cmath>

namespace cy::world {
namespace {

/// How many points a prediction horizon is sampled at. Four is enough for a straight extrapolation
/// to cross more than one cell without leaving a hole between samples, and small enough that a
/// hundred sources cost nothing.
constexpr u32 kPredictionSamples = 4;

/// A place the source will be, and when. Prediction turns one source into several of these.
struct Sample {
    WorldVec3d point;
    /// Nanoseconds from now until the source is there. Zero for where it is.
    Nanoseconds when = 0;
};

[[nodiscard]] f64 axis_distance(f64 value, f64 low, f64 high) noexcept {
    if (value < low) {
        return low - value;
    }
    if (value > high) {
        return value - high;
    }
    return 0.0;
}

/// The distance from an absolute point to a cell's box, in f64. Exact at any distance from the
/// world origin, which is the reason the planner does not work in f32 absolute coordinates.
[[nodiscard]] f64 distance_to_cell(const PartitionConfig& config, CellCoord coord,
                                   const WorldVec3d& point) noexcept {
    const WorldVec3d low = simulation_origin(config, coord);
    const f64 size = config.cell_size(coord.level);
    const f64 dx = axis_distance(point.x, low.x, low.x + size);
    const f64 dy = axis_distance(point.y, low.y, low.y + size);
    const f64 dz = axis_distance(point.z, low.z, low.z + size);
    return std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
}

[[nodiscard]] WorldVec3d cell_centre(const PartitionConfig& config, CellCoord coord) noexcept {
    const WorldVec3d low = simulation_origin(config, coord);
    const f64 half = config.cell_size(coord.level) * 0.5;
    return WorldVec3d{low.x + half, low.y + half, low.z + half};
}

[[nodiscard]] f32 clamp01(f32 value) noexcept {
    return std::clamp(value, 0.0f, 1.0f);
}

/// The largest half-extent of a box source, which is the reach its enumeration needs.
[[nodiscard]] f64 box_reach(const Vec3& extent) noexcept {
    return static_cast<f64>(std::max({extent.x, extent.y, extent.z}));
}

/// The reach a source's shape needs enumerating to, in metres.
[[nodiscard]] f64 shape_reach(const StreamingSource& source) noexcept {
    switch (source.shape) {
        case SourceShape::Box:
            return box_reach(source.extent);
        case SourceShape::Frustum: {
            const Vec3 half = source.frustum_bounds.is_empty()
                                  ? Vec3{source.radius, source.radius, source.radius}
                                  : source.frustum_bounds.half_extents();
            return box_reach(half);
        }
        case SourceShape::Sphere:
        case SourceShape::Cone:
        case SourceShape::Path:
            break;
    }
    return static_cast<f64>(source.radius);
}

/// Whether a cell is inside the source's shape, given a sample point it is being measured from.
/// `distance` is the f64 distance from that point to the cell's box, already computed.
[[nodiscard]] bool shape_accepts(const PartitionConfig& config, const StreamingSource& source,
                                 const Sample& sample, CellCoord coord, f64 distance) noexcept {
    switch (source.shape) {
        case SourceShape::Sphere:
        case SourceShape::Path:
            return distance <= static_cast<f64>(source.radius);

        case SourceShape::Box: {
            const WorldVec3d low = simulation_origin(config, coord);
            const f64 size = config.cell_size(coord.level);
            return axis_distance(sample.point.x, low.x, low.x + size) <=
                       static_cast<f64>(source.extent.x) &&
                   axis_distance(sample.point.y, low.y, low.y + size) <=
                       static_cast<f64>(source.extent.y) &&
                   axis_distance(sample.point.z, low.z, low.z + size) <=
                       static_cast<f64>(source.extent.z);
        }

        case SourceShape::Cone: {
            if (distance > static_cast<f64>(source.radius)) {
                return false;
            }
            // Measured to the cell's CENTRE with the cell's half-diagonal as slack, so a cell the
            // cone clips a corner of is included. A conservative cone is a cell that streams and is
            // not seen; a tight one is a hole in the world.
            const WorldVec3d centre = cell_centre(config, coord);
            const f64 dx = centre.x - sample.point.x;
            const f64 dy = centre.y - sample.point.y;
            const f64 dz = centre.z - sample.point.z;
            const f64 length = std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
            const f64 half_diagonal = config.cell_size(coord.level) * 0.8660254;  // sqrt(3)/2
            if (length <= half_diagonal) {
                return true;
            }
            const f64 along = ((dx * static_cast<f64>(source.direction.x)) +
                               (dy * static_cast<f64>(source.direction.y)) +
                               (dz * static_cast<f64>(source.direction.z))) /
                              length;
            const f64 slack = std::asin(std::min(1.0, half_diagonal / length));
            return std::acos(std::max(-1.0, std::min(1.0, along))) <=
                   static_cast<f64>(source.cone_half_angle) + slack;
        }

        case SourceShape::Frustum:
            // The frustum's planes are in absolute f32 space, which is what the renderer that built
            // them works in. The cell's f32 bounds are exact enough for a visibility test.
            return source.frustum.intersects(cell_bounds(config, coord));
    }
    return false;
}

/// The central priority computation. `world-partition-and-streaming`: "Streaming priority SHALL
/// combine source importance, predicted visibility, estimated time until needed and gameplay
/// importance, and SHALL be computed CENTRALLY rather than by each source."
[[nodiscard]] f32 combine_priority(const StreamingSource& source, f64 distance,
                                   Nanoseconds when) noexcept {
    const f64 reach = std::max(1.0, shape_reach(source));
    const f32 proximity = clamp01(1.0f - static_cast<f32>(distance / reach));
    // Content needed in a second outranks content needed in ten. The reciprocal, rather than a
    // linear falloff, so that "now" is sharply separated from "soon" and "soon" from "eventually".
    const f32 seconds = static_cast<f32>(static_cast<f64>(when) / 1e9);
    const f32 urgency = 1.0f / (1.0f + seconds);
    // A frustum source is a statement about predicted VISIBILITY, so what it can see outranks what
    // is merely near it — the specification's "Frustum matters more than position" scenario.
    const f32 visibility = (source.shape == SourceShape::Frustum) ? 1.15f : 1.0f;
    return clamp01(clamp01(source.importance) * visibility *
                   ((0.55f * proximity) + (0.45f * urgency)));
}

/// The samples one source is evaluated at: where it is, and where prediction says it will be.
[[nodiscard]] Status build_samples(const PartitionConfig& config, const StreamingSource& source,
                                   Span<const WorldPosition> path, Array<Sample>& out) noexcept {
    if (Status pushed = out.push_back(Sample{to_absolute(config, source.position), 0}); !pushed) {
        return pushed;
    }

    if (source.shape == SourceShape::Path) {
        // A path source requests content along its route ahead of arrival. The time until a point
        // is needed is its arc length from the start divided by the source's speed; with no speed
        // the whole route is wanted now, which is what a cinematic camera track means.
        const f64 speed =
            std::sqrt((static_cast<f64>(source.velocity.x) * static_cast<f64>(source.velocity.x)) +
                      (static_cast<f64>(source.velocity.y) * static_cast<f64>(source.velocity.y)) +
                      (static_cast<f64>(source.velocity.z) * static_cast<f64>(source.velocity.z)));
        WorldVec3d previous = to_absolute(config, source.position);
        f64 travelled = 0.0;
        for (const WorldPosition& step : path) {
            const WorldVec3d point = to_absolute(config, step);
            const f64 dx = point.x - previous.x;
            const f64 dy = point.y - previous.y;
            const f64 dz = point.z - previous.z;
            travelled += std::sqrt((dx * dx) + (dy * dy) + (dz * dz));
            previous = point;
            const Nanoseconds when =
                (speed > 0.01) ? static_cast<Nanoseconds>((travelled / speed) * 1e9) : 0;
            if (Status pushed = out.push_back(Sample{point, when}); !pushed) {
                return pushed;
            }
        }
        return ok();
    }

    if (source.prediction_horizon <= 0.0f) {
        return ok();
    }
    // Straight extrapolation from velocity. `world-partition-and-streaming` also names navigation
    // and spline paths and declared future positions; both of those are expressed as a `Path`
    // source by whoever knows the route, which is why there is one mechanism here and not three.
    for (u32 step = 1; step <= kPredictionSamples; ++step) {
        const f64 seconds = static_cast<f64>(source.prediction_horizon) *
                            (static_cast<f64>(step) / static_cast<f64>(kPredictionSamples));
        const WorldVec3d base = to_absolute(config, source.position);
        const Sample sample{WorldVec3d{base.x + (static_cast<f64>(source.velocity.x) * seconds),
                                       base.y + (static_cast<f64>(source.velocity.y) * seconds),
                                       base.z + (static_cast<f64>(source.velocity.z) * seconds)},
                            static_cast<Nanoseconds>(seconds * 1e9)};
        if (Status pushed = out.push_back(sample); !pushed) {
            return pushed;
        }
    }
    return ok();
}

/// Fold `candidate` into `into`. A cell required by any source is required; its channels are the
/// union, its class the most urgent, its priority the highest and its deadline the earliest.
void absorb(CellRequirement& into, const CellRequirement& candidate) noexcept {
    into.channels = into.channels | candidate.channels;
    into.activate = into.activate || candidate.activate;
    if (static_cast<u8>(candidate.klass) < static_cast<u8>(into.klass)) {
        into.klass = candidate.klass;
    }
    if (candidate.priority > into.priority) {
        into.priority = candidate.priority;
        into.source = candidate.source;
    }
    into.time_until_needed = std::min(into.time_until_needed, candidate.time_until_needed);
}

/// Sort by cell identifier and fold duplicates together, in place.
///
/// SORT AND MERGE RATHER THAN A LINEAR PROBE PER CANDIDATE. The obvious implementation — scan the
/// output for the cell before pushing — is quadratic, and this loop runs over every cell every
/// source requires at every level at every prediction sample. On the M6 traversal route that was
/// tens of millions of comparisons a frame, which is the frame budget the milestone exists to hold.
void canonicalize(Array<CellRequirement>& out) noexcept {
    if (out.size() < 2) {
        return;
    }
    std::sort(out.data(), out.data() + out.size(),
              [](const CellRequirement& a, const CellRequirement& b) { return a.cell < b.cell; });
    usize kept = 0;
    for (usize index = 1; index < out.size(); ++index) {
        if (out[index].cell == out[kept].cell) {
            absorb(out[kept], out[index]);
        } else {
            ++kept;
            out[kept] = out[index];
        }
    }
    while (out.size() > kept + 1) {
        out.pop_back();
    }
}

}  // namespace

const char* request_class_name(RequestClass klass) noexcept {
    switch (klass) {
        case RequestClass::Critical:
            return "critical";
        case RequestClass::Gameplay:
            return "gameplay";
        case RequestClass::Visible:
            return "visible";
        case RequestClass::Predicted:
            return "predicted";
        case RequestClass::Background:
            return "background";
    }
    return "unknown";
}

const char* source_shape_name(SourceShape shape) noexcept {
    switch (shape) {
        case SourceShape::Sphere:
            return "sphere";
        case SourceShape::Box:
            return "box";
        case SourceShape::Frustum:
            return "frustum";
        case SourceShape::Cone:
            return "cone";
        case SourceShape::Path:
            return "path";
    }
    return "unknown";
}

SourceRegistry::SourceRegistry(Allocator& allocator) noexcept
    : sources_(allocator), points_(allocator) {}

Expected<SourceId, Error> SourceRegistry::add(const StreamingSource& source,
                                              Span<const WorldPosition> path) noexcept {
    Entry entry;
    entry.id = next_;
    entry.source = source;
    entry.source.path_first = static_cast<u32>(points_.size());
    entry.source.path_count = 0;
    if (source.shape == SourceShape::Path) {
        if (Status appended = points_.append(path); !appended) {
            return make_unexpected(appended.error());
        }
        entry.source.path_count = static_cast<u32>(path.size());
    }
    if (Status pushed = sources_.push_back(entry); !pushed) {
        return make_unexpected(pushed.error());
    }
    ++next_;
    return entry.id;
}

Status SourceRegistry::update(SourceId id, const StreamingSource& source) noexcept {
    for (Entry& entry : sources_.span()) {
        if (entry.id != id) {
            continue;
        }
        const u32 first = entry.source.path_first;
        const u32 count = entry.source.path_count;
        entry.source = source;
        // The point pool is not reallocated by an update: a source that changes shape away from
        // `Path` keeps its points until it is removed, which costs a few dozen bytes and avoids a
        // compaction on the per-frame path.
        entry.source.path_first = first;
        entry.source.path_count = count;
        return ok();
    }
    return fail(ErrorCode::NotFound, "no streaming source with that identifier");
}

Status SourceRegistry::move_to(SourceId id, const WorldPosition& position,
                               const Vec3& velocity) noexcept {
    for (Entry& entry : sources_.span()) {
        if (entry.id == id) {
            entry.source.position = position;
            entry.source.velocity = velocity;
            return ok();
        }
    }
    return fail(ErrorCode::NotFound, "no streaming source with that identifier");
}

Status SourceRegistry::remove(SourceId id) noexcept {
    for (usize index = 0; index < sources_.size(); ++index) {
        if (sources_[index].id != id) {
            continue;
        }
        // Order-preserving removal: `entries()` is iterated in registration order and a swap-remove
        // would make two runs of the same scenario evaluate sources in different orders.
        for (usize shift = index + 1; shift < sources_.size(); ++shift) {
            sources_[shift - 1] = sources_[shift];
        }
        sources_.pop_back();
        return ok();
    }
    return fail(ErrorCode::NotFound, "no streaming source with that identifier");
}

const StreamingSource* SourceRegistry::find(SourceId id) const noexcept {
    for (const Entry& entry : sources_.span()) {
        if (entry.id == id) {
            return &entry.source;
        }
    }
    return nullptr;
}

Span<const WorldPosition> SourceRegistry::path_of(const StreamingSource& source) const noexcept {
    if (source.path_count == 0 || source.path_first >= points_.size()) {
        return {};
    }
    return points_.span().subspan(source.path_first, source.path_count);
}

namespace {

/// Every cell at `level` this sample requires, appended to `out`.
///
/// The neighbourhood is enumerated by INTEGER COORDINATE around the sample's own cell rather than
/// by intersecting an absolute f32 box: the distance test is then done in f64 relative to the
/// sample, and is exact a thousand kilometres from the origin as well as at it.
[[nodiscard]] Status require_around(const Partitioner& partitioner, SourceId id,
                                    const StreamingSource& source, const Sample& sample, u8 level,
                                    Array<CellRequirement>& out) noexcept {
    const PartitionConfig& config = partitioner.config();
    const auto span =
        static_cast<i64>(std::floor(shape_reach(source) / config.cell_size(level))) + 1;
    const CellCoord centre = partitioner.coord_of(sample.point, level);

    for (i64 dz = -span; dz <= span; ++dz) {
        for (i64 dy = -span; dy <= span; ++dy) {
            for (i64 dx = -span; dx <= span; ++dx) {
                const CellCoord coord{static_cast<i32>(centre.x + dx),
                                      static_cast<i32>(centre.y + dy),
                                      static_cast<i32>(centre.z + dz), level};
                const f64 distance = distance_to_cell(config, coord, sample.point);
                if (!shape_accepts(config, source, sample, coord, distance)) {
                    continue;
                }
                CellRequirement requirement;
                requirement.coord = coord;
                requirement.cell = partitioner.id_of(coord);
                requirement.source = id;
                requirement.channels = source.channels;
                requirement.klass = source.klass;
                requirement.activate = source.activates;
                requirement.time_until_needed = sample.when;
                requirement.priority = combine_priority(source, distance, sample.when);
                if (Status pushed = out.push_back(requirement); !pushed) {
                    return pushed;
                }
            }
        }
    }
    return ok();
}

}  // namespace

Status SourceRegistry::require(const Partitioner& partitioner, const Entry& entry,
                               Array<CellRequirement>& out) const noexcept {
    const StreamingSource& source = entry.source;

    Array<Sample> samples(out.allocator());
    if (Status built = build_samples(partitioner.config(), source, path_of(source), samples);
        !built) {
        return built;
    }

    // A source may drive several levels: a distant view wants coarse cells and a nearby one wants
    // fine cells, and both come from the one source rather than from two.
    const u8 last_level = static_cast<u8>(
        std::min<u32>(partitioner.level_count(), u32{source.level} + u32{source.level_span}));
    for (u8 level = source.level; level < last_level; ++level) {
        for (const Sample& sample : samples.span()) {
            if (Status required = require_around(partitioner, entry.id, source, sample, level, out);
                !required) {
                return required;
            }
        }
    }
    canonicalize(out);
    return ok();
}

Status SourceRegistry::require_all(const Partitioner& partitioner,
                                   Array<CellRequirement>& out) const noexcept {
    for (const Entry& entry : sources_.span()) {
        if (Status required = require(partitioner, entry, out); !required) {
            return required;
        }
    }
    // One canonical set, in one canonical order: everything downstream — the plan, the report, the
    // trace — reads this array in order, and two machines must read the same one.
    canonicalize(out);
    return ok();
}

}  // namespace cy::world
