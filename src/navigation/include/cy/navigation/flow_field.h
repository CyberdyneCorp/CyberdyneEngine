#pragma once
// Flow fields: one computed field guiding many agents to a shared destination, instead of one path
// query each. M8.b task 6.2.
//
// `navigation` states the shape and the reason: "a GRID over a navigable region where each cell
// stores a direction toward a shared destination, so that many agents heading to the same place are
// guided by one computed field rather than by one path each", with "20,000 agents ordered to the
// same destination" guided by one field.
//
// ================================================================================================
// WHY A GRID RATHER THAN A FIELD OVER THE POLYGON GRAPH
// ================================================================================================
//
// A Dijkstra over the polygon graph is cheaper to build and it is NOT what the specification asks
// for, for a reason its own scenarios make plain: a polygon can be tens of metres across, so a
// per-polygon direction sends every agent inside it the same way regardless of where in it they
// stand, and "local avoidance still applies" then has to fight the field rather than refine it. The
// grid's resolution is the field's resolution, and it is a parameter.
//
// ================================================================================================
// INCREMENTAL REGENERATION, AND WHY THE AFFECTED SET IS WHAT IT IS
// ================================================================================================
//
// "the affected region of the field SHALL be regenerated INCREMENTALLY rather than the whole field
// recomputed." `regenerate` recomputes exactly two things:
//
//   * every cell whose integrated cost could have RISEN. A cell's shortest route rises only if that
//     route passed through a cell whose own cost changed, and any such cell's integration is at
//     least the smallest integration in the dirty region — so that value is the threshold, and
//     resetting every cell at or above it is a correct superset with no search to find it.
//   * every cell whose integrated cost could have FALLEN. Relaxation from the frontier — the
//     unreset cells that touch a reset one — reaches them, because a fall propagates outward from
//     the change like any other Dijkstra improvement.
//
// `FlowFieldUpdate::cells_visited` is what makes "incrementally rather than the whole field" a
// measurement rather than a claim, and the test asserts it against the cell count.

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/navigation/navmesh.h>

namespace cy::navigation {

struct FlowFieldParams {
    /// The bounded region of interest. `navigation` requires one: a field over an open world is an
    /// unbounded allocation.
    Aabb region;
    f32 cell_size = 1.0f;
    /// How far above and below a cell's centre the mesh is sampled for the polygon under it.
    f32 sample_height = 2.0f;
    AreaMask areas = kAllAreas;
    NavAreaCosts costs = NavAreaCosts::uniform();
};

/// What a build or a regeneration cost.
struct FlowFieldUpdate {
    u32 cells = 0;          ///< the whole field
    u32 cells_reset = 0;    ///< cleared because their value could have risen
    u32 cells_visited = 0;  ///< popped from the priority queue
    u32 unreachable = 0;
    bool full_rebuild = false;
};

/// A direction per cell toward the nearest destination, and the integrated cost that produced it.
///
/// Deterministic: `navigation` requires it outright ("Flow-field generation SHALL be
/// deterministic"), so the queue's tie-break is the cell index and nothing in the build reads a
/// clock, an address or an iteration order.
class FlowField {
public:
    FlowField(Allocator& allocator, const FlowFieldParams& params) noexcept;

    FlowField(const FlowField&) = delete;
    FlowField& operator=(const FlowField&) = delete;
    FlowField(FlowField&&) noexcept = default;
    FlowField& operator=(FlowField&&) noexcept = default;

    [[nodiscard]] const FlowFieldParams& params() const noexcept { return params_; }
    [[nodiscard]] u32 width() const noexcept { return width_; }
    [[nodiscard]] u32 depth() const noexcept { return depth_; }
    [[nodiscard]] u32 cell_count() const noexcept { return width_ * depth_; }
    /// The mesh version the field was built against. A field older than the mesh is stale.
    [[nodiscard]] u32 mesh_version() const noexcept { return mesh_version_; }

    /// Build from scratch against `destinations`. More than one is allowed and is what a field
    /// toward "any exit" is.
    [[nodiscard]] Expected<FlowFieldUpdate, Error> build(const NavMesh& mesh,
                                                         Span<const Vec3> destinations) noexcept;

    /// Recompute only what `dirty` could have changed. See the header for what that set is.
    [[nodiscard]] Expected<FlowFieldUpdate, Error> regenerate(const NavMesh& mesh,
                                                              const Aabb& dirty) noexcept;

    /// The unit direction to follow from `position`, or the zero vector where the field has none.
    [[nodiscard]] Vec3 direction_at(Vec3 position) const noexcept;
    /// The integrated cost to the nearest destination, or infinity.
    [[nodiscard]] f32 cost_at(Vec3 position) const noexcept;
    [[nodiscard]] bool reachable_at(Vec3 position) const noexcept;

    [[nodiscard]] i32 cell_x(f32 x) const noexcept;
    [[nodiscard]] i32 cell_z(f32 z) const noexcept;
    [[nodiscard]] Vec3 cell_centre(u32 index) const noexcept;

    // --- Reference counting -------------------------------------------------------------------
    //
    // `navigation`: "reference counting so a field is released when no agent uses it", and its
    // scenario "the last agent following a field stops using it ... the field SHALL be released".
    // The count lives here and the release is `FlowFieldCache`'s, because a field does not know
    // who owns its storage.

    void acquire() noexcept { ++users_; }
    void release() noexcept {
        if (users_ != 0) {
            --users_;
        }
    }
    [[nodiscard]] u32 users() const noexcept { return users_; }

private:
    [[nodiscard]] Status allocate() noexcept;
    void sample_costs(const NavMesh& mesh, u32 from_x, u32 from_z, u32 to_x, u32 to_z) noexcept;
    [[nodiscard]] Expected<FlowFieldUpdate, Error> integrate(Array<u32>& seeds, bool full) noexcept;
    void build_directions(u32 from_x, u32 from_z, u32 to_x, u32 to_z) noexcept;

    FlowFieldParams params_;
    u32 width_ = 0;
    u32 depth_ = 0;
    u32 mesh_version_ = 0;
    u32 users_ = 0;
    Array<f32> cell_cost_;    ///< traversal cost multiplier, or infinity where not navigable
    Array<f32> integration_;  ///< cost to the nearest destination
    Array<Vec2> direction_;   ///< XZ, unit length, zero where there is nowhere to go
    Array<Vec3> destinations_;
};

/// Fields, keyed by their destination, so that twenty thousand agents ordered to one place share
/// one. A field whose last user releases it is destroyed on the next `collect()`.
class FlowFieldCache {
public:
    FlowFieldCache(Allocator& allocator, const FlowFieldParams& params) noexcept;
    ~FlowFieldCache();

    FlowFieldCache(const FlowFieldCache&) = delete;
    FlowFieldCache& operator=(const FlowFieldCache&) = delete;

    /// The field for `destination`, built if it does not exist. The caller holds a reference and
    /// must `release` it.
    [[nodiscard]] Expected<FlowField*, Error> acquire(const NavMesh& mesh,
                                                      Vec3 destination) noexcept;
    void release(FlowField* field) noexcept;
    /// Destroy every field with no users. Returns how many went.
    [[nodiscard]] u32 collect() noexcept;
    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(fields_.size()); }
    /// Regenerate every live field over `dirty`. What a tile rebuild or an obstacle calls.
    [[nodiscard]] u32 regenerate(const NavMesh& mesh, const Aabb& dirty) noexcept;

private:
    /// Heap-allocated one at a time, because callers hold `FlowField*` across acquisitions and an
    /// array that grew would move every field out from under them.
    struct Entry {
        Vec3 destination;
        FlowField field;

        Entry(Allocator& allocator, const FlowFieldParams& params, Vec3 target) noexcept
            : destination(target), field(allocator, params) {}
    };

    void destroy(Entry* entry) noexcept;

    Allocator* allocator_;
    FlowFieldParams params_;
    Array<Entry*> fields_;
};

}  // namespace cy::navigation
