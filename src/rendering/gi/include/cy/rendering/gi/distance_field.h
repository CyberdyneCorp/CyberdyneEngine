#pragma once
// The sparse signed distance field: camera-centred clipmaps, sphere tracing, sky visibility.
// Task 9.1.
//
// `rendering-global-illumination` — "Distance field representation".
//
// ================================================================================================
// THREE PROPERTIES, AND EACH ONE IS A DIFFERENT DECISION
// ================================================================================================
//
// **Clipmaps.** Levels of increasing extent and decreasing resolution, centred on the camera. Level
// 0 covers metres at fine spacing and the last covers kilometres coarsely, which is what makes a
// large world affordable: the number of voxels is constant and their meaning is not.
//
// **Scrolling, not rebuilding.** A level is a set of BRICKS keyed by their world brick coordinate,
// held in a hash. Moving the camera changes which keys the level needs; the ones it already has are
// kept untouched and only the newly exposed keys are solved. That is "clipmaps SHALL scroll with
// the camera, re-solving only newly exposed regions" implemented as the data structure rather than
// as a pass, and `ScrollReport::bricks_solved` is what makes it checkable — a rebuild would report
// every brick every frame.
//
// A toroidal index would be the conventional implementation and would be faster. It would also be
// the same number of re-solved bricks, and a keyed hash says what it is doing on the page.
//
// **Sparse.** A brick every one of whose voxels is further from a surface than the brick's own
// diagonal holds no information a constant could not: it is freed and remembered as empty, and
// `distance()` answers from the level's far value. An open sky above a landscape costs nothing.
//
// ================================================================================================
// PER-ASSET FIELDS COMPOSITE; A MOVING OBJECT IS A TRANSFORM
// ================================================================================================
//
// `AssetDistanceField` is what a cook produces once per asset: a dense grid in the asset's own
// space. The world field is the minimum over the placed instances of each asset's field, evaluated
// through the instance's inverse transform. So a door that opens is `move()` — an invalidation of
// the bricks it left and the bricks it entered — and never a regeneration of anything.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/hash_map.h>

namespace cy::rendering::gi {

/// Voxels per brick edge. Four is the usual balance: a brick is 64 voxels, small enough that an
/// empty one is cheap to discover and large enough that the hash is not the cost.
inline constexpr u32 kBrickEdge = 4;
inline constexpr u32 kBrickVoxels = kBrickEdge * kBrickEdge * kBrickEdge;

struct ClipmapSettings {
    /// Clipmap levels. Each doubles the previous level's extent at the same voxel count, so
    /// resolution halves and coverage grows geometrically: four levels from an 8 m level 0 reach
    /// 64 m, and eight reach a kilometre.
    u32 levels = 4;
    /// Voxels per level edge. A multiple of `kBrickEdge`.
    u32 resolution = 32;
    /// The world extent of level 0, in metres.
    f32 base_extent_metres = 8.0F;
};

/// What a cook produces for one asset: a dense signed distance grid in the asset's own space.
struct AssetDistanceField {
    /// Voxels per edge. The grid is `dimension^3`.
    u32 dimension = 0;
    /// The asset's local bounds the grid spans.
    Aabb bounds{};
    /// Distances in metres, negative inside. Indexed `((z * dimension) + y) * dimension + x`.
    Span<const f32> distances;
};

/// A box of a given half-extent, as a field. The fixture every test and every simple placement
/// needs, and the one shape whose exact distance is worth having in closed form.
[[nodiscard]] f32 box_distance(Vec3 point, Vec3 half_extents) noexcept;

struct SphereTraceHit {
    bool hit = false;
    f32 t = 0.0F;
    Vec3 position{0.0F, 0.0F, 0.0F};
    Vec3 normal{0.0F, 1.0F, 0.0F};
    /// The closest the ray came to a surface along its whole march, in metres — what a sphere
    /// trace produces for free. A small value means the ray grazed something, and a grazing ray's
    /// hit (or miss) is the least certain thing the software tier produces, which is exactly what a
    /// confidence wants to be built on.
    ///
    /// It is an ABSOLUTE distance rather than a ratio against the distance travelled. A ratio was
    /// the first form and it was wrong: the field answers a conservative constant in empty space,
    /// so d/t falls as a clean ray travels and a long clean miss reported less certainty than a
    /// short grazing one.
    f32 closest_approach_metres = 0.0F;
    /// The trace ran out of steps before deciding. Not a hit and not a clean miss.
    bool exhausted = false;
};

struct ScrollReport {
    u32 bricks_solved = 0;
    u32 bricks_reused = 0;
    u32 bricks_freed = 0;
    u32 bricks_empty = 0;
};

struct FieldDiagnostics {
    u32 levels = 0;
    u32 allocated_bricks = 0;
    /// Bricks known to contain no surface. They hold no storage, which is the sparsity.
    u32 empty_bricks = 0;
    u64 bytes = 0;
    ScrollReport last_scroll{};
};

/// The world's sparse signed distance field.
class DistanceField {
public:
    DistanceField() noexcept;

    [[nodiscard]] Status configure(const ClipmapSettings& settings) noexcept;
    [[nodiscard]] const ClipmapSettings& settings() const noexcept { return settings_; }

    // --- Placed geometry ------------------------------------------------------------------------

    /// Place an asset's field in the world. The field's samples are copied; the transform is not
    /// required to be rigid, but a non-uniform scale makes the composited distance conservative
    /// rather than exact, which is what a sphere trace needs it to be.
    [[nodiscard]] Status place(u64 id, const AssetDistanceField& field,
                               const Mat4& transform) noexcept;

    /// Move a placed instance. Invalidates the bricks it left and the bricks it entered, and
    /// nothing else — which is the door-opens scenario.
    [[nodiscard]] Status move(u64 id, const Mat4& transform) noexcept;
    void remove(u64 id) noexcept;
    [[nodiscard]] u32 placement_count() const noexcept;

    // --- The clipmaps ---------------------------------------------------------------------------

    /// Centre the clipmaps on `camera`, solving only newly exposed bricks.
    ScrollReport scroll_to(Vec3 camera) noexcept;

    /// Mark every brick overlapping `region` for re-solving at the next `scroll_to`.
    u32 invalidate(const Aabb& region) noexcept;

    // --- Queries --------------------------------------------------------------------------------

    /// Distance to the nearest surface, in metres. Positive outside. A point outside every level's
    /// coverage answers with the coarsest level's far value, which keeps a sphere trace marching
    /// rather than stopping.
    [[nodiscard]] f32 distance(Vec3 point) const noexcept;

    /// The finest level's voxel size, in metres. The scale everything about this field's accuracy
    /// is expressed in, and what a caller starting a ray on a surface must clear.
    [[nodiscard]] f32 voxel_size() const noexcept;

    /// Sphere trace, the software tier's intersection.
    ///
    /// `t_min` is where the march starts, and a caller whose ray ORIGINATES ON A SURFACE must set
    /// it. The field's hit test is "within half a voxel of something", so a ray leaving a wall at a
    /// grazing angle is still within half a voxel of that wall after its first step and reports a
    /// hit on the surface it started from. That defect cost every wall-lit surface in a room its
    /// direct lighting on the software tier, and the symptom — a room lit correctly by the hardware
    /// tier and black by the software one — looked like a tier disagreement rather than a bias.
    [[nodiscard]] SphereTraceHit sphere_trace(const Ray& ray, f32 max_distance,
                                              f32 t_min = 0.0F) const noexcept;

    /// The fraction of a cosine-weighted hemisphere around `normal` that reaches the sky, in
    /// [0, 1]. `rays` cone-traces are issued; the deterministic hemisphere sequence makes the
    /// answer reproducible, which a golden image needs.
    [[nodiscard]] f32 sky_visibility(Vec3 position, Vec3 normal, u32 rays) const noexcept;

    [[nodiscard]] const FieldDiagnostics& diagnostics() const noexcept { return diagnostics_; }

private:
    struct Placement {
        u64 id = 0;
        Mat4 transform = Mat4::identity();
        Mat4 inverse = Mat4::identity();
        u32 dimension = 0;
        Aabb local_bounds{};
        Aabb world_bounds{};
        Array<f32> distances;
        /// The smallest distance stored anywhere on the grid's boundary shell. See
        /// `sample_placements` for what it is for and why it is a valid bound.
        f32 boundary_min = 0.0F;
        bool live = false;
    };

    struct Brick {
        /// `kEmptySlot` for a brick known to hold no surface: it carries no storage.
        u32 slot = 0;
        bool empty = false;
        bool stale = false;
    };

    struct Level {
        f32 voxel_size = 0.0F;
        f32 brick_size = 0.0F;
        /// The far value a query outside an allocated brick answers with.
        f32 far_distance = 0.0F;
        i32 origin_brick[3] = {0, 0, 0};
        HashMap<u64, Brick> bricks;
        bool centred = false;

        Level() noexcept = default;
    };

    [[nodiscard]] f32 sample_placements(Vec3 point) const noexcept;
    [[nodiscard]] u32 find_placement(u64 id) const noexcept;
    [[nodiscard]] Status solve_brick(Level& level, i32 bx, i32 by, i32 bz, Brick& brick) noexcept;
    void retire_departed(Level& level, i32 half, ScrollReport& report) noexcept;
    void visit_brick(Level& level, i32 bx, i32 by, i32 bz, ScrollReport& report) noexcept;
    void free_brick(Brick& brick) noexcept;
    [[nodiscard]] f32 sample_level(const Level& level, Vec3 point) const noexcept;
    void account() noexcept;

    ClipmapSettings settings_{};
    Array<Level> levels_;
    Array<Placement> placements_;
    /// Brick storage, `kBrickVoxels` floats each, with a free list. One pool for every level: a
    /// brick is the same size everywhere and a per-level pool would fragment four ways.
    Array<f32> brick_pool_;
    Array<u32> free_bricks_;
    Array<Aabb> pending_invalidations_;
    FieldDiagnostics diagnostics_{};

    static constexpr u32 kEmptySlot = ~0U;
};

}  // namespace cy::rendering::gi
