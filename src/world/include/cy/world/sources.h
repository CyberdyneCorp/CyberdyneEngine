#pragma once
// Streaming sources, their shapes, and prediction. Task 3.3.
//
// `world-partition-and-streaming` — "Streaming sources": streaming is driven by EXPLICIT sources,
// each with a location, a shape, a radius or extent, a priority, a prediction horizon and a channel
// mask. Players, cameras, editor viewports, network clients, teleport destinations, cinematic
// cameras, mission targets and AI groups are all sources, and any subsystem may register one.
// Multiple sources combine: a cell required by any source is required.
//
// THE CAMERA IS NOT THE ONLY SOURCE, and that is the whole design. A strategy camera far from the
// units it commands, a teleport destination that must be resident before arrival, a cinematic
// camera track that is known in advance — none of those is expressible by "stream what is near the
// player", and every engine that started there grew a pile of special cases instead of a source.
//
// --- WHY A SOURCE IS TRIVIALLY COPYABLE --------------------------------------------------------
//
// A path or spline source needs a list of points, and putting an `Array` inside `StreamingSource`
// would make the source non-copyable, make the registry's storage a list of pointers, and put a
// heap allocation on the path of "update the camera this frame". The points live in the registry's
// own pool and the source names a range of it. The struct stays a value: it is copied into a plan,
// sent to a diagnostic, and one day crosses the C ABI, none of which wants an owning container.

#include <cy/core/base/expected.h>
#include <cy/core/math/shapes.h>
#include <cy/core/memory/array.h>
#include <cy/world/cell.h>
#include <cy/world/partition.h>

namespace cy::world {

using SourceId = u32;
inline constexpr SourceId kInvalidSource = 0;

/// The ordering classes work is serviced in when it exceeds budget. `world-partition-and-
/// streaming`: "critical (teleport, gameplay-blocking), then gameplay-relevant, then visible, then
/// predicted, then background."
enum class RequestClass : u8 {
    Critical = 0,
    Gameplay,
    Visible,
    Predicted,
    Background,
};

[[nodiscard]] const char* request_class_name(RequestClass klass) noexcept;

/// Shapes beyond a radius, as the specification requires. `Spline` is `Path` with the points
/// already sampled — the engine does not carry two path evaluators, and a spline source hands its
/// sampled points to the registry.
enum class SourceShape : u8 {
    Sphere = 0,
    Box,
    Frustum,
    Cone,
    Path,
};

[[nodiscard]] const char* source_shape_name(SourceShape shape) noexcept;

/// What a source is. A value type: see the header comment.
struct StreamingSource {
    SourceShape shape = SourceShape::Sphere;
    /// Where the source is, in the persistent cell-relative form.
    WorldPosition position;
    /// `Sphere` and `Path`: the radius. `Cone`: the length.
    f32 radius = 256.0f;
    /// `Box`: half-extents around `position`.
    Vec3 extent{128.0f, 128.0f, 128.0f};
    /// `Cone`: the axis, normalised by the caller. `Frustum` ignores it.
    Vec3 direction{0.0f, 0.0f, 1.0f};
    /// `Cone`: the half-angle, in radians.
    f32 cone_half_angle = 0.7f;
    /// `Frustum`: the planes, and the bounds they are tested inside. A frustum is unbounded as a
    /// set of half-spaces — the far plane bounds it, but recovering a box from six planes is
    /// arithmetic nobody should have to do twice, so the caller that built the frustum passes the
    /// bounds it already knows.
    Frustum frustum;
    Aabb frustum_bounds;

    /// Metres per second. Prediction extrapolates along it.
    Vec3 velocity;
    /// Seconds ahead the planner requests content for. Zero disables prediction for this source.
    f32 prediction_horizon = 0.0f;

    /// Which channels this source requires. A spectator camera asks for geometry and textures and
    /// not for physics, navigation or AI.
    ChannelMask channels = ChannelMask::all();
    /// Whether the cells this source requires should be ACTIVATED, or only made resident. A
    /// prefetch source that only wants bytes in memory sets this false, and that is the separation
    /// of residency from activation expressed at the point a request is made.
    bool activates = true;
    /// 0..1. Feeds the central priority computation; it does not by itself order anything.
    f32 importance = 0.5f;
    RequestClass klass = RequestClass::Gameplay;
    /// Which hierarchy level this source drives. A source may require content at several levels;
    /// `level_span` says how many levels above `level` it also requires, which is how a distant
    /// view gets coarse cells and a nearby one gets fine cells from the same source.
    u8 level = 0;
    u8 level_span = 1;

    /// The registry's point pool range for a `Path` source. Not set by the caller.
    u32 path_first = 0;
    u32 path_count = 0;
};

/// One cell one source requires, and why. The planner merges these across sources.
struct CellRequirement {
    CellCoord coord;
    CellId cell;
    SourceId source = kInvalidSource;
    ChannelMask channels;
    RequestClass klass = RequestClass::Background;
    /// 0..1, where 1 is "under the source". Combined centrally from source importance, predicted
    /// visibility, estimated time until needed and gameplay importance.
    f32 priority = 0.0f;
    /// Nanoseconds from now until this content is needed. A background prefetch has a large one; a
    /// teleport has zero.
    Nanoseconds time_until_needed = 0;
    bool activate = false;
};

/// The sources, and the point pool their paths live in.
class SourceRegistry {
public:
    explicit SourceRegistry(Allocator& allocator) noexcept;

    /// Register a source. `path` is required for `SourceShape::Path` and ignored otherwise; a
    /// spline source samples itself and passes the samples.
    [[nodiscard]] Expected<SourceId, Error> add(const StreamingSource& source,
                                                Span<const WorldPosition> path = {}) noexcept;

    /// Replace a source's parameters, keeping its identifier. The camera's per-frame call.
    [[nodiscard]] Status update(SourceId id, const StreamingSource& source) noexcept;

    /// Move a source. The narrow form of `update()`, and the one a mover actually needs.
    [[nodiscard]] Status move_to(SourceId id, const WorldPosition& position,
                                 const Vec3& velocity) noexcept;

    [[nodiscard]] Status remove(SourceId id) noexcept;

    [[nodiscard]] const StreamingSource* find(SourceId id) const noexcept;
    [[nodiscard]] usize size() const noexcept { return sources_.size(); }

    /// Every source, with its identifier. Iterated in registration order, which is stable.
    struct Entry {
        SourceId id = kInvalidSource;
        StreamingSource source;
    };
    [[nodiscard]] Span<const Entry> entries() const noexcept { return sources_.span(); }

    [[nodiscard]] Span<const WorldPosition> path_of(const StreamingSource& source) const noexcept;

    /// The cells one source requires, appended to `out`. Deterministic in order and in content.
    [[nodiscard]] Status require(const Partitioner& partitioner, const Entry& entry,
                                 Array<CellRequirement>& out) const noexcept;

    /// The cells EVERY source requires, merged: a cell required by any source is required, its
    /// channels are the union, its class is the most urgent, and its priority is the highest.
    /// Sorted by cell identifier, so the result is one canonical set.
    [[nodiscard]] Status require_all(const Partitioner& partitioner,
                                     Array<CellRequirement>& out) const noexcept;

private:
    Array<Entry> sources_;
    Array<WorldPosition> points_;
    SourceId next_ = 1;
};

}  // namespace cy::world
