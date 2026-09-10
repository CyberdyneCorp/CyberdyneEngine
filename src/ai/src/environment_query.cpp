// Environment queries. See cy/ai/environment_query.h for the argument.

#include <cy/ai/environment_query.h>

#include <cmath>
#include <numbers>

namespace cy::ai {
namespace {

using graph::behaviour::apply_curve;

/// Map a raw measurement into 0..1 before the response curve sees it. Every test produces a number
/// in its own units; the curve is defined on the unit interval, so the normalisation is one step
/// and one place rather than a convention each test remembers.
[[nodiscard]] f32 normalise(f32 value, f32 span, bool invert) noexcept {
    const f32 clamped = (span <= 0.0F) ? 0.0F : (value / span);
    const f32 bounded = std::fmin(std::fmax(clamped, 0.0F), 1.0F);
    return invert ? bounded : (1.0F - bounded);
}

}  // namespace

const char* test_kind_name(TestKind kind) noexcept {
    switch (kind) {
        case TestKind::Distance:
            return "Distance";
        case TestKind::Dot:
            return "Dot";
        case TestKind::LineOfSight:
            return "LineOfSight";
        case TestKind::Reachable:
            return "Reachable";
        case TestKind::PathCost:
            return "PathCost";
        case TestKind::HeightDifference:
            return "HeightDifference";
        case TestKind::Overlap:
            return "Overlap";
        case TestKind::Custom:
            return "Custom";
        case TestKind::Count:
            break;
    }
    return "unknown";
}

EnvironmentQuery::EnvironmentQuery(Allocator& allocator, u32 budget) noexcept
    : items_(allocator),
      tests_(allocator),
      ranked_(allocator),
      from_(allocator),
      to_(allocator),
      hits_(allocator),
      costs_(allocator),
      budget_(budget == 0 ? 1 : budget) {}

void EnvironmentQuery::clear() noexcept {
    items_.clear();
    tests_.clear();
    ranked_.clear();
}

Status EnvironmentQuery::generate_grid(Vec3 centre, f32 extent, f32 spacing) noexcept {
    if (spacing <= 0.0F || extent <= 0.0F) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a grid needs a positive extent and spacing"});
    }
    const i32 steps = static_cast<i32>(extent / spacing);
    for (i32 z = -steps; z <= steps; ++z) {
        for (i32 x = -steps; x <= steps; ++x) {
            QueryItem item;
            item.position = Vec3{centre.x + (static_cast<f32>(x) * spacing), centre.y,
                                 centre.z + (static_cast<f32>(z) * spacing)};
            if (Status pushed = items_.push_back(item); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

Status EnvironmentQuery::generate_ring(Vec3 centre, f32 inner_radius, f32 outer_radius, u32 rings,
                                       u32 spokes) noexcept {
    if (rings == 0 || spokes == 0 || outer_radius < inner_radius) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a ring needs at least one ring and one spoke"});
    }
    for (u32 ring = 0; ring < rings; ++ring) {
        const f32 t = (rings == 1) ? 0.0F : (static_cast<f32>(ring) / static_cast<f32>(rings - 1));
        const f32 radius = inner_radius + ((outer_radius - inner_radius) * t);
        for (u32 spoke = 0; spoke < spokes; ++spoke) {
            const f32 angle = (2.0F * std::numbers::pi_v<f32> * static_cast<f32>(spoke)) /
                              static_cast<f32>(spokes);
            QueryItem item;
            item.position = Vec3{centre.x + (std::cos(angle) * radius), centre.y,
                                 centre.z + (std::sin(angle) * radius)};
            if (Status pushed = items_.push_back(item); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

Status EnvironmentQuery::generate_around(Vec3 actor_position, f32 radius, u32 count) noexcept {
    return generate_ring(actor_position, radius, radius, 1, count);
}

Status EnvironmentQuery::generate_actors(Span<const Entity> actors, Span<const Vec3> positions,
                                         Vec3 origin, f32 radius) noexcept {
    if (actors.size() != positions.size()) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "the actor and position columns differ in length"});
    }
    for (usize index = 0; index < actors.size(); ++index) {
        const Vec3 offset = positions[index] - origin;
        if (((offset.x * offset.x) + (offset.z * offset.z)) > radius * radius) {
            continue;
        }
        QueryItem item;
        item.position = positions[index];
        item.actor = actors[index];
        if (Status pushed = items_.push_back(item); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status EnvironmentQuery::generate_custom(Span<const Vec3> points) noexcept {
    for (const Vec3 point : points) {
        QueryItem item;
        item.position = point;
        if (Status pushed = items_.push_back(item); !pushed) {
            return pushed;
        }
    }
    return ok();
}

Status EnvironmentQuery::add_test(const QueryTest& test) noexcept {
    return tests_.push_back(test);
}

Status EnvironmentQuery::add_custom_test(CustomTestFn function, void* user, f32 weight,
                                         const ResponseCurve& curve) noexcept {
    if (function == nullptr) {
        return make_unexpected(Error{ErrorCode::InvalidArgument, "a custom test needs a function"});
    }
    QueryTest test;
    test.kind = TestKind::Custom;
    test.custom = function;
    test.user = user;
    test.weight = weight;
    test.curve = curve;
    return tests_.push_back(test);
}

Status EnvironmentQuery::apply_budget(EnvironmentQueryReport& report) noexcept {
    report.candidates_generated = static_cast<u32>(items_.size());
    report.stride = 1;
    if (items_.size() <= budget_) {
        return ok();
    }
    // A STRIDE, not a truncation. Taking the first `budget_` of a grid answers with one corner of
    // it; taking every n-th keeps the shape of the candidate set. `ai-system` requires the
    // reduction to be deterministic and reported, and a stride is both.
    report.budget_exceeded = true;
    const u32 stride = static_cast<u32>((items_.size() + budget_ - 1) / budget_);
    report.stride = stride;
    usize keep = 0;
    for (usize index = 0; index < items_.size(); index += stride) {
        items_[keep] = items_[index];
        ++keep;
    }
    report.candidates_dropped = static_cast<u32>(items_.size() - keep);
    while (items_.size() > keep) {
        items_.pop_back();
    }
    return ok();
}

/// The three tests that need the world, batched once each over every surviving candidate.
///
/// A per-candidate call would be the per-agent raycasting perception.h exists to remove, one level
/// down — so the batch is built here, before anything is scored, and the scoring loop reads it.
Status EnvironmentQuery::prepare_batch(const QueryTest& test, Vec3 origin, QueryHost& host,
                                       EnvironmentQueryReport& report) noexcept {
    if (test.kind == TestKind::LineOfSight) {
        from_.clear();
        to_.clear();
        for (const QueryItem& item : items_.span()) {
            if (item.rejected) {
                continue;
            }
            if (!from_.push_back(item.position) || !to_.push_back(test.reference)) {
                return make_unexpected(
                    Error{ErrorCode::OutOfMemory, "the query could not hold its traces"});
            }
        }
        if (Status sized = hits_.resize(from_.size()); !sized) {
            return sized;
        }
        for (bool& hit : hits_.span()) {
            hit = false;
        }
        if (!from_.empty()) {
            host.trace_batch(from_.span(), to_.span(), hits_.span());
        }
        report.traces_issued += static_cast<u32>(from_.size());
        return ok();
    }
    if (test.kind != TestKind::Reachable && test.kind != TestKind::PathCost) {
        return ok();
    }
    to_.clear();
    for (const QueryItem& item : items_.span()) {
        if (!item.rejected && !to_.push_back(item.position)) {
            return make_unexpected(
                Error{ErrorCode::OutOfMemory, "the query could not hold its path costs"});
        }
    }
    if (Status sized = costs_.resize(to_.size()); !sized) {
        return sized;
    }
    for (f32& cost : costs_.span()) {
        cost = -1.0F;
    }
    if (!to_.empty()) {
        host.path_cost_batch(origin, to_.span(), costs_.span());
    }
    report.path_costs_issued += static_cast<u32>(to_.size());
    return ok();
}

/// One test's raw measurement of one candidate, in 0..1, before its response curve.
///
/// `batch_index` is the candidate's position among the SURVIVING ones, which is what the batched
/// results above are indexed by. A switch and nothing else: separated from `run_test` so that the
/// eight cases read as a table rather than as the middle of a loop.
f32 EnvironmentQuery::measure(const QueryTest& test, const QueryItem& item,
                              usize batch_index) const noexcept {
    switch (test.kind) {
        case TestKind::Distance: {
            const Vec3 offset = item.position - test.reference;
            return normalise(std::sqrt((offset.x * offset.x) + (offset.z * offset.z)), test.span,
                             test.invert);
        }
        case TestKind::Dot: {
            const Vec3 offset = item.position - test.reference;
            const f32 length_xz = std::sqrt((offset.x * offset.x) + (offset.z * offset.z));
            const f32 facing = std::sqrt((test.direction.x * test.direction.x) +
                                         (test.direction.z * test.direction.z));
            const f32 cosine =
                (length_xz < 1e-5F || facing < 1e-5F)
                    ? 1.0F
                    : (((offset.x * test.direction.x) + (offset.z * test.direction.z)) /
                       (length_xz * facing));
            return test.invert ? ((1.0F - cosine) * 0.5F) : ((cosine + 1.0F) * 0.5F);
        }
        case TestKind::LineOfSight:
            return (hits_[batch_index] != test.invert) ? 1.0F : 0.0F;
        case TestKind::Reachable:
            return ((costs_[batch_index] >= 0.0F) != test.invert) ? 1.0F : 0.0F;
        case TestKind::PathCost: {
            const f32 cost = costs_[batch_index];
            return (cost < 0.0F) ? 0.0F : normalise(cost, test.span, test.invert);
        }
        case TestKind::HeightDifference:
            return normalise(std::fabs(item.position.y - test.reference.y), test.span, test.invert);
        case TestKind::Overlap: {
            const Aabb box = Aabb::from_center_extents(test.reference, test.direction);
            return (box.contains(item.position) != test.invert) ? 1.0F : 0.0F;
        }
        case TestKind::Custom:
            return (test.custom != nullptr) ? test.custom(item, test.user) : 0.0F;
        case TestKind::Count:
            break;
    }
    return 0.0F;
}

Status EnvironmentQuery::run_test(const QueryTest& test, Vec3 origin, QueryHost& host,
                                  EnvironmentQueryReport& report) noexcept {
    if (Status prepared = prepare_batch(test, origin, host, report); !prepared) {
        return prepared;
    }

    usize batch_index = 0;
    for (QueryItem& item : items_.span()) {
        if (item.rejected) {
            continue;
        }
        ++report.tests_run;
        const f32 scored = apply_curve(test.curve, measure(test, item, batch_index));
        ++batch_index;
        if (scored < test.filter_below) {
            item.rejected = true;
            ++report.candidates_rejected;
            continue;
        }
        // Multiplicative combination: `ai-system` asks for both, and multiplication is the default
        // because a candidate that fails one consideration outright should not be rescued by
        // scoring well on another. A test that wants to ADD sets `weight` and a curve that floors
        // at one.
        item.score *= 1.0F + (test.weight * (scored - 1.0F));
    }
    return ok();
}

Status EnvironmentQuery::run(Vec3 origin, QueryHost& host,
                             EnvironmentQueryReport& report) noexcept {
    report = EnvironmentQueryReport{};
    ranked_.clear();
    for (QueryItem& item : items_.span()) {
        item.score = 1.0F;
        item.rejected = false;
    }
    if (Status budgeted = apply_budget(report); !budgeted) {
        return budgeted;
    }
    for (const QueryTest& test : tests_.span()) {
        if (Status ran = run_test(test, origin, host, report); !ran) {
            return ran;
        }
    }

    for (u32 index = 0; index < items_.size(); ++index) {
        if (!items_[index].rejected) {
            if (Status pushed = ranked_.push_back(index); !pushed) {
                return pushed;
            }
        }
    }
    // Highest score first, and the tie-break is the candidate's own index — which is the
    // generator's order, which is a pure function of the query. Two runs rank identically.
    for (usize index = 1; index < ranked_.size(); ++index) {
        const u32 candidate = ranked_[index];
        usize position = index;
        while (position > 0 && items_[ranked_[position - 1]].score < items_[candidate].score) {
            ranked_[position] = ranked_[position - 1];
            --position;
        }
        ranked_[position] = candidate;
    }
    return ok();
}

const QueryItem* EnvironmentQuery::best() const noexcept {
    return ranked_.empty() ? nullptr : &items_[ranked_[0]];
}

}  // namespace cy::ai
