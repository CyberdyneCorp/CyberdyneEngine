#pragma once
// Environment queries: generate candidate points, score them by weighted tests, answer the best or
// a ranked set. M8.b task 6.4.
//
// ================================================================================================
// WHAT THIS IS FOR, IN ONE SENTENCE FROM THE SPECIFICATION
// ================================================================================================
//
// `ai-system`: "spatial reasoning queries that generate candidate points or actors, score them by
// weighted tests, and return the best or a ranked set" — and its worked example is finding cover:
// "generate candidate points, score them by distance, line of sight to the threat, and
// reachability, and return the best".
//
// The generators and the tests are the ones that requirement lists, and the two extension points it
// also requires — "a registration point for custom tests" and the same for generators — are
// `EnvironmentQuery::add_custom_test` and `generate_custom`, which take a function pointer and a
// user pointer rather than an interface, because a test is a scoring function and not an object.
//
// ================================================================================================
// THE BUDGET REDUCES CANDIDATES DETERMINISTICALLY, AND SAYS SO
// ================================================================================================
//
// `ai-system`: "queries SHALL declare a budget so cost is bounded", and "WHEN a query's candidate
// count exceeds its budget THEN candidates SHALL be REDUCED DETERMINISTICALLY before scoring, and
// the reduction REPORTED."
//
// The reduction is a stride, not a random sample and not a truncation: taking the first N of a grid
// would answer with one corner of it. `EnvironmentQueryReport::candidates_dropped` is the number,
// and `stride` is what produced it.
//
// ================================================================================================
// A TEST SCORES OR FILTERS, AND BOTH ARE ONE CALL
// ================================================================================================
//
// Every test answers a number. A test with `filter_below` set turns a number below that threshold
// into a rejection; a test without it only weights. That is `ai-system`'s "Each test SHALL support
// scoring (with a response curve) and filtering" in one mechanism rather than two, so a project
// cannot write a filter that disagrees with the score it is derived from.

#include <cy/core/base/expected.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/entity.h>
#include <cy/graph/lower_behaviour.h>

namespace cy::ai {

using ecs::Entity;
using graph::behaviour::ResponseCurve;

/// One candidate: a point, and the actor it came from when the generator produced actors.
struct QueryItem {
    Vec3 position;
    Entity actor;
    f32 score = 1.0F;
    bool rejected = false;
};

/// `ai-system`'s test list, by name: "distance, dot product and angle, line of sight, navigation
/// reachability and path cost, height difference, overlap, and a registration point for custom
/// tests".
enum class TestKind : u8 {
    Distance = 0,
    Dot,
    LineOfSight,
    Reachable,
    PathCost,
    HeightDifference,
    Overlap,
    Custom,
    Count,
};

[[nodiscard]] const char* test_kind_name(TestKind kind) noexcept;

/// What a test that needs the world asks the host. Line of sight, reachability and path cost are
/// the three that cannot be answered from a point alone, and they are the three the host owns —
/// the same boundary perception.h draws, and for the same reason.
class QueryHost {
public:
    QueryHost() = default;
    virtual ~QueryHost() = default;
    QueryHost(const QueryHost&) = delete;
    QueryHost& operator=(const QueryHost&) = delete;
    QueryHost(QueryHost&&) = delete;
    QueryHost& operator=(QueryHost&&) = delete;

    /// A whole batch, for the same reason `VisibilityHost` takes one.
    virtual void trace_batch(Span<const Vec3> from, Span<const Vec3> to, Span<bool> results) = 0;
    /// The cost of walking from `from` to each of `to`, or a negative number where there is no
    /// route. Batched.
    virtual void path_cost_batch(Vec3 from, Span<const Vec3> to, Span<f32> costs) = 0;
};

using CustomTestFn = f32 (*)(const QueryItem& item, void* user) noexcept;

/// One weighted test.
struct QueryTest {
    TestKind kind = TestKind::Distance;
    ResponseCurve curve;
    f32 weight = 1.0F;
    /// A score below this rejects the candidate outright. `math::kInfinity` is "never filters".
    f32 filter_below = -math::kInfinity;
    /// The reference the test measures against: the threat for line of sight, the origin for
    /// distance, the facing for the dot product.
    Vec3 reference;
    Vec3 direction{0.0F, 0.0F, 1.0F};
    /// The distance at which `Distance` scores zero. Beyond it the score is clamped.
    f32 span = 20.0F;
    /// True when a HIGH raw value should score LOW — "as far from the threat as possible" rather
    /// than "as close as possible".
    bool invert = false;
    CustomTestFn custom = nullptr;
    void* user = nullptr;
};

struct EnvironmentQueryReport {
    u32 candidates_generated = 0;
    u32 candidates_dropped = 0;   ///< by the budget's stride
    u32 candidates_rejected = 0;  ///< by a test's filter
    u32 tests_run = 0;
    u32 traces_issued = 0;
    u32 path_costs_issued = 0;
    u32 stride = 1;
    bool budget_exceeded = false;
};

/// A query: generators, then tests, then a ranking.
class EnvironmentQuery {
public:
    EnvironmentQuery(Allocator& allocator, u32 budget) noexcept;

    EnvironmentQuery(const EnvironmentQuery&) = delete;
    EnvironmentQuery& operator=(const EnvironmentQuery&) = delete;

    [[nodiscard]] u32 budget() const noexcept { return budget_; }
    [[nodiscard]] Span<const QueryItem> items() const noexcept { return items_.span(); }

    // --- Generators ----------------------------------------------------------------------------
    //
    // `ai-system`: "points on a grid, points on a circle or donut, points on the navigation mesh,
    // points around an actor, and actors of a type in range". Each appends; a query may use more
    // than one, which is how "cover behind me or in the doorway" is expressed.

    [[nodiscard]] Status generate_grid(Vec3 centre, f32 extent, f32 spacing) noexcept;
    [[nodiscard]] Status generate_ring(Vec3 centre, f32 inner_radius, f32 outer_radius, u32 rings,
                                       u32 spokes) noexcept;
    [[nodiscard]] Status generate_around(Vec3 actor_position, f32 radius, u32 count) noexcept;
    [[nodiscard]] Status generate_actors(Span<const Entity> actors, Span<const Vec3> positions,
                                         Vec3 origin, f32 radius) noexcept;
    /// The registration point for a project's own generator: it hands over the points it made.
    [[nodiscard]] Status generate_custom(Span<const Vec3> points) noexcept;

    // --- Tests ---------------------------------------------------------------------------------

    [[nodiscard]] Status add_test(const QueryTest& test) noexcept;
    /// `ai-system`'s "registration point for custom tests".
    [[nodiscard]] Status add_custom_test(CustomTestFn function, void* user, f32 weight,
                                         const ResponseCurve& curve) noexcept;

    /// Reduce to the budget, run every test, and rank. `origin` is the asking agent's position,
    /// which the reachability and path-cost tests measure from.
    [[nodiscard]] Status run(Vec3 origin, QueryHost& host, EnvironmentQueryReport& report) noexcept;

    /// The best surviving candidate after `run`, or null when everything was rejected.
    [[nodiscard]] const QueryItem* best() const noexcept;
    /// The surviving candidates, best first. Valid until the next `run`.
    [[nodiscard]] Span<const u32> ranked() const noexcept { return ranked_.span(); }

    void clear() noexcept;

private:
    [[nodiscard]] Status apply_budget(EnvironmentQueryReport& report) noexcept;
    [[nodiscard]] Status run_test(const QueryTest& test, Vec3 origin, QueryHost& host,
                                  EnvironmentQueryReport& report) noexcept;
    /// The two halves of `run_test`: the batched world queries, and the per-candidate measurement.
    /// Split because the two together were well past the complexity this project holds systems code
    /// to, and because the eight test kinds read as a table on their own.
    [[nodiscard]] Status prepare_batch(const QueryTest& test, Vec3 origin, QueryHost& host,
                                       EnvironmentQueryReport& report) noexcept;
    [[nodiscard]] f32 measure(const QueryTest& test, const QueryItem& item,
                              usize batch_index) const noexcept;

    Array<QueryItem> items_;
    Array<QueryTest> tests_;
    Array<u32> ranked_;
    Array<Vec3> from_;
    Array<Vec3> to_;
    Array<bool> hits_;
    Array<f32> costs_;
    u32 budget_ = 64;
};

}  // namespace cy::ai
