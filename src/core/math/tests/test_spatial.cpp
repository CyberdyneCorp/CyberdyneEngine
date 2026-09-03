// The spatial acceleration structures. Task 3.1.4.
//
// `core-math` — "Spatial acceleration structures" and "Frustum culling primitives". Two scenarios
// are named and both are asserted directly:
//
//   * "Small movement does not restructure" — moving an object inside its expanded AABB leaves the
//     tree untouched. `DynamicBvh::update` returns whether it restructured, which turns that from
//     something to be inferred into something to be checked.
//   * "Frustum query is conservative" — a query may return more than it should and must never
//     return less. Every query test below is written as a comparison against brute force *in that
//     direction*: the accelerated answer must be a superset of the exact one.
//
// An acceleration structure is only ever wrong by omission, and omission is invisible in a
// screenshot until the day it is not. Comparing against brute force over a few thousand random
// objects is the only test shape that catches it.

#include <cy/core/math/math.h>

#include <cy/test/test.h>

#include "approx.h"

#include <algorithm>
#include <vector>

namespace {

std::vector<cy::Aabb> random_boxes(cy::usize count, cy::f32 extent, cy::u64 seed) {
    cy::Random random(seed);
    std::vector<cy::Aabb> boxes;
    boxes.reserve(count);
    for (cy::usize i = 0; i < count; ++i) {
        const cy::Vec3 center{random.next_float_in(-extent, extent),
                              random.next_float_in(-extent, extent),
                              random.next_float_in(-extent, extent)};
        const cy::f32 size = random.next_float_in(0.2f, 3.0f);
        boxes.push_back(cy::Aabb::from_center_extents(center, cy::Vec3{size, size, size}));
    }
    return boxes;
}

}  // namespace

CY_TEST_CASE("DynamicBvh: a movement inside the margin does not restructure the tree") {
    // The scenario `core-math` names, asserted on the return value rather than inferred from the
    // tree's shape.
    cy::DynamicBvh tree(0.5f);
    const cy::Expected<cy::u32, cy::Error> proxy =
        tree.insert(cy::Aabb::from_center_extents(cy::Vec3{}, cy::Vec3{1.0f, 1.0f, 1.0f}), 42u);
    CY_REQUIRE(proxy.has_value());

    const cy::Aabb stored = tree.fat_bounds(*proxy);
    // The stored bounds are the fattened ones, which is what makes the next assertion possible.
    CY_CHECK(
        stored.contains(cy::Aabb::from_center_extents(cy::Vec3{}, cy::Vec3{1.0f, 1.0f, 1.0f})));
    CY_CHECK_CLOSE(stored.min.x, -1.5f, 1e-6f);

    // A step of 0.2 m stays inside a 0.5 m margin: nothing changes.
    const cy::Expected<bool, cy::Error> small = tree.update(
        *proxy,
        cy::Aabb::from_center_extents(cy::Vec3{0.2f, 0.0f, 0.0f}, cy::Vec3{1.0f, 1.0f, 1.0f}));
    CY_REQUIRE(small.has_value());
    CY_CHECK_FALSE(*small);
    CY_CHECK(tree.fat_bounds(*proxy) == stored);

    // A step of 5 m does not.
    const cy::Expected<bool, cy::Error> large = tree.update(
        *proxy,
        cy::Aabb::from_center_extents(cy::Vec3{5.0f, 0.0f, 0.0f}, cy::Vec3{1.0f, 1.0f, 1.0f}));
    CY_REQUIRE(large.has_value());
    CY_CHECK(*large);
    CY_CHECK_FALSE(tree.fat_bounds(*proxy) == stored);
    CY_CHECK_EQ(tree.size(), 1u);
}

CY_TEST_CASE("DynamicBvh: an AABB query returns every box that intersects and nothing is omitted") {
    const std::vector<cy::Aabb> boxes = random_boxes(2000, 100.0f, 11u);
    cy::DynamicBvh tree(0.1f);
    for (cy::usize i = 0; i < boxes.size(); ++i) {
        CY_REQUIRE(tree.insert(boxes[i], static_cast<cy::u64>(i)).has_value());
    }
    CY_CHECK_EQ(tree.size(), boxes.size());

    const cy::Aabb query =
        cy::Aabb::from_center_extents(cy::Vec3{10.0f, -5.0f, 20.0f}, cy::Vec3{15.0f, 15.0f, 15.0f});

    std::vector<bool> reported(boxes.size(), false);
    tree.query_aabb(query, [&](cy::u32, cy::u64 user_data) { reported[user_data] = true; });

    // The direction that matters: everything the exact test finds must have been reported. Extra
    // reports are allowed — the tree stores fattened bounds, so it is conservative by construction.
    cy::usize expected = 0;
    for (cy::usize i = 0; i < boxes.size(); ++i) {
        if (boxes[i].intersects(query)) {
            ++expected;
            CY_REQUIRE(reported[i]);
        }
    }
    CY_CHECK(expected > 0);
}

CY_TEST_CASE("DynamicBvh: removal leaves the remaining objects findable") {
    const std::vector<cy::Aabb> boxes = random_boxes(500, 50.0f, 77u);
    cy::DynamicBvh tree(0.1f);
    std::vector<cy::u32> proxies;
    proxies.reserve(boxes.size());
    for (cy::usize i = 0; i < boxes.size(); ++i) {
        const cy::Expected<cy::u32, cy::Error> proxy =
            tree.insert(boxes[i], static_cast<cy::u64>(i));
        CY_REQUIRE(proxy.has_value());
        proxies.push_back(*proxy);
    }

    // Remove every other one.
    for (cy::usize i = 0; i < proxies.size(); i += 2) {
        CY_REQUIRE(tree.remove(proxies[i]).has_value());
    }
    CY_CHECK_EQ(tree.size(), boxes.size() / 2);
    // A proxy that is gone is gone: removing it again reports rather than corrupting the tree.
    CY_CHECK_FALSE(tree.remove(proxies[0]).has_value());

    // Everything that remains is still reachable by a query over the whole world.
    const cy::Aabb everything =
        cy::Aabb::from_center_extents(cy::Vec3{}, cy::Vec3{1000.0f, 1000.0f, 1000.0f});
    cy::usize found = 0;
    tree.query_aabb(everything, [&](cy::u32, cy::u64 user_data) {
        CY_REQUIRE(user_data % 2 == 1);
        ++found;
    });
    CY_CHECK_EQ(found, boxes.size() / 2);

    tree.clear();
    CY_CHECK(tree.empty());
    CY_CHECK_EQ(tree.height(), 0);
}

CY_TEST_CASE("DynamicBvh: balancing keeps the height near the logarithmic minimum") {
    // Objects inserted along a line, which is the worst realistic insertion order: a tree that did
    // not rebalance would degenerate into a list and every query would be linear.
    cy::DynamicBvh tree(0.1f);
    constexpr cy::usize kCount = 1024;
    for (cy::usize i = 0; i < kCount; ++i) {
        const cy::Vec3 center{static_cast<cy::f32>(i), 0.0f, 0.0f};
        CY_REQUIRE(tree.insert(cy::Aabb::from_center_extents(center, cy::Vec3{0.4f, 0.4f, 0.4f}),
                               static_cast<cy::u64>(i))
                       .has_value());
    }
    // A perfectly balanced tree of 1024 leaves has height 10. Three times that is a generous bound
    // that a degenerate list (height 1023) fails by two orders of magnitude.
    CY_CHECK(tree.height() < 30);
    CY_CHECK(tree.surface_area_ratio() > 0.0f);
}

CY_TEST_CASE("DynamicBvh: a frustum query omits nothing that the exact test accepts") {
    const std::vector<cy::Aabb> boxes = random_boxes(2000, 120.0f, 909u);
    cy::DynamicBvh tree(0.1f);
    for (cy::usize i = 0; i < boxes.size(); ++i) {
        CY_REQUIRE(tree.insert(boxes[i], static_cast<cy::u64>(i)).has_value());
    }

    const cy::Mat4 view = cy::look_at(cy::Vec3{0.0f, 40.0f, 120.0f}, cy::Vec3{});
    const cy::Mat4 projection =
        cy::perspective_reversed_z(cy::math::radians(50.0f), 16.0f / 9.0f, 1.0f, 400.0f);
    const cy::Frustum frustum = cy::Frustum::from_view_projection(projection * view);

    std::vector<bool> reported(boxes.size(), false);
    tree.query_frustum(frustum, [&](cy::u32, cy::u64 user_data) { reported[user_data] = true; });

    // `core-math` — "Frustum query is conservative": it MAY return nodes outside the frustum and
    // SHALL NOT omit any that intersect it.
    cy::usize visible = 0;
    for (cy::usize i = 0; i < boxes.size(); ++i) {
        if (frustum.intersects(boxes[i])) {
            ++visible;
            CY_REQUIRE(reported[i]);
        }
    }
    CY_CHECK(visible > 0);
    CY_CHECK(visible < boxes.size());
}

CY_TEST_CASE("DynamicBvh: a ray query omits nothing the brute-force sweep finds") {
    const std::vector<cy::Aabb> boxes = random_boxes(1500, 60.0f, 313u);
    cy::DynamicBvh tree(0.0f);  // no margin, so the tree's bounds are the real ones
    for (cy::usize i = 0; i < boxes.size(); ++i) {
        CY_REQUIRE(tree.insert(boxes[i], static_cast<cy::u64>(i)).has_value());
    }

    const cy::Ray ray{cy::Vec3{-100.0f, 0.0f, 0.0f}, normalize(cy::Vec3{1.0f, 0.1f, 0.05f})};
    constexpr cy::f32 kMaxDistance = 300.0f;

    std::vector<bool> reported(boxes.size(), false);
    tree.query_ray(ray, kMaxDistance,
                   [&](cy::u32, cy::u64 user_data) { reported[user_data] = true; });

    cy::usize hits = 0;
    for (cy::usize i = 0; i < boxes.size(); ++i) {
        cy::f32 enter = 0.0f;
        cy::f32 exit = 0.0f;
        if (cy::geom::ray_aabb(ray, boxes[i], kMaxDistance, enter, exit)) {
            ++hits;
            CY_REQUIRE(reported[i]);
        }
    }
    CY_CHECK(hits > 0);
}

CY_TEST_CASE("Bvh<T>: a static build answers the same queries brute force does") {
    const std::vector<cy::Aabb> boxes = random_boxes(3000, 80.0f, 5150u);
    std::vector<cy::u32> payloads(boxes.size());
    for (cy::usize i = 0; i < payloads.size(); ++i) {
        payloads[i] = static_cast<cy::u32>(i);
    }

    cy::Bvh<cy::u32> bvh;
    CY_REQUIRE(bvh.build(boxes.data(), payloads.data(), boxes.size(), 4).has_value());
    CY_CHECK_EQ(bvh.size(), boxes.size());
    CY_CHECK(bvh.node_count() > 1);

    // The root bounds contain everything, which is the cheapest possible check that the build did
    // not lose a primitive somewhere in the partition.
    cy::Aabb all = cy::Aabb::empty();
    for (const cy::Aabb& box : boxes) {
        all.grow(box);
    }
    CY_CHECK(bvh.bounds().expanded(1e-3f).contains(all));

    const cy::Aabb query =
        cy::Aabb::from_center_extents(cy::Vec3{-20.0f, 10.0f, 5.0f}, cy::Vec3{12.0f, 12.0f, 12.0f});
    std::vector<bool> reported(boxes.size(), false);
    bvh.query_aabb(query, [&](cy::u32 payload, const cy::Aabb&) { reported[payload] = true; });

    cy::usize expected = 0;
    for (cy::usize i = 0; i < boxes.size(); ++i) {
        if (boxes[i].intersects(query)) {
            ++expected;
            CY_REQUIRE(reported[i]);
        }
    }
    CY_CHECK(expected > 0);

    // A degenerate build — every primitive at the same place — must terminate rather than recurse
    // forever looking for a split plane. This is the case the median fallback exists for.
    const std::vector<cy::Aabb> coincident(64, cy::Aabb::from_point(cy::Vec3{1.0f, 1.0f, 1.0f}));
    std::vector<cy::u32> coincident_payloads(coincident.size(), 0u);
    cy::Bvh<cy::u32> degenerate;
    CY_CHECK(degenerate.build(coincident.data(), coincident_payloads.data(), coincident.size(), 4)
                 .has_value());
    CY_CHECK_EQ(degenerate.size(), coincident.size());

    cy::Bvh<cy::u32> empty;
    CY_CHECK(empty.build(nullptr, nullptr, 0).has_value());
    CY_CHECK(empty.empty());
}

CY_TEST_CASE("Bvh<T>: an early-out query stops when the callback returns false") {
    const std::vector<cy::Aabb> boxes = random_boxes(200, 10.0f, 2u);
    std::vector<cy::u32> payloads(boxes.size());
    for (cy::usize i = 0; i < payloads.size(); ++i) {
        payloads[i] = static_cast<cy::u32>(i);
    }
    cy::Bvh<cy::u32> bvh;
    CY_REQUIRE(bvh.build(boxes.data(), payloads.data(), boxes.size()).has_value());

    cy::usize visits = 0;
    bvh.query_aabb(cy::Aabb::from_center_extents(cy::Vec3{}, cy::Vec3{100.0f, 100.0f, 100.0f}),
                   [&](cy::u32, const cy::Aabb&) {
                       ++visits;
                       return false;  // stop after the first
                   });
    CY_CHECK_EQ(visits, 1u);
}

CY_TEST_CASE("SpatialHash: a query reports each entry once and omits nothing") {
    const std::vector<cy::Aabb> boxes = random_boxes(1500, 40.0f, 4004u);
    cy::SpatialHash grid(4.0f);
    for (cy::usize i = 0; i < boxes.size(); ++i) {
        CY_REQUIRE(grid.insert(static_cast<cy::u32>(i), boxes[i]).has_value());
    }
    CY_CHECK_EQ(grid.size(), boxes.size());
    CY_CHECK(grid.cell_count() > 0);

    const cy::Aabb query =
        cy::Aabb::from_center_extents(cy::Vec3{5.0f, 5.0f, 5.0f}, cy::Vec3{9.0f, 9.0f, 9.0f});

    // An entry spanning several cells appears in several cell lists; the per-query stamp is what
    // stops it being reported once per cell. Counting is the assertion.
    std::vector<cy::u32> counts(boxes.size(), 0u);
    grid.query_aabb(query, [&](cy::u32 id, const cy::Aabb&) { counts[id] += 1u; });

    cy::usize expected = 0;
    for (cy::usize i = 0; i < boxes.size(); ++i) {
        const bool overlaps = boxes[i].intersects(query);
        CY_REQUIRE(counts[i] <= 1u);
        if (overlaps) {
            ++expected;
            CY_REQUIRE_EQ(counts[i], 1u);
        }
    }
    CY_CHECK(expected > 0);

    // Removal and re-insertion under the same id.
    CY_REQUIRE(grid.remove(0u).has_value());
    CY_CHECK_FALSE(grid.contains(0u));
    CY_CHECK_FALSE(grid.remove(0u).has_value());
    CY_CHECK_FALSE(grid.update(0u, boxes[0]).has_value());
    CY_REQUIRE(grid.insert(0u, boxes[0]).has_value());
    CY_CHECK(grid.contains(0u));
    CY_CHECK_EQ(grid.size(), boxes.size());
}

CY_TEST_CASE("SpatialHash: negative coordinates land in the cell below the origin") {
    const cy::SpatialHash grid(2.0f);
    CY_CHECK(grid.cell_of(cy::Vec3{0.0f, 0.0f, 0.0f}) == cy::IVec3{0, 0, 0});
    CY_CHECK(grid.cell_of(cy::Vec3{1.9f, 1.9f, 1.9f}) == cy::IVec3{0, 0, 0});
    CY_CHECK(grid.cell_of(cy::Vec3{-0.1f, -2.1f, -4.0f}) == cy::IVec3{-1, -2, -2});
}

CY_TEST_CASE("Octree: an entry lives in one node and every query finds it") {
    cy::Octree tree;
    const cy::Aabb world = cy::Aabb::from_center_extents(cy::Vec3{}, cy::Vec3{64.0f, 64.0f, 64.0f});
    CY_REQUIRE(tree.reset(world, 5, 4).has_value());

    const std::vector<cy::Aabb> boxes = random_boxes(1200, 50.0f, 8080u);
    for (cy::usize i = 0; i < boxes.size(); ++i) {
        CY_REQUIRE(tree.insert(static_cast<cy::u32>(i), boxes[i]).has_value());
    }
    CY_CHECK_EQ(tree.size(), boxes.size());
    CY_CHECK(tree.node_count() > 1);

    const cy::Aabb query =
        cy::Aabb::from_center_extents(cy::Vec3{-15.0f, 8.0f, 3.0f}, cy::Vec3{10.0f, 10.0f, 10.0f});
    std::vector<cy::u32> counts(boxes.size(), 0u);
    tree.query_aabb(query, [&](cy::u32 id, const cy::Aabb&) { counts[id] += 1u; });

    cy::usize expected = 0;
    for (cy::usize i = 0; i < boxes.size(); ++i) {
        CY_REQUIRE(counts[i] <= 1u);  // one node per entry, so at most one report
        if (boxes[i].intersects(query)) {
            ++expected;
            CY_REQUIRE_EQ(counts[i], 1u);
        }
    }
    CY_CHECK(expected > 0);

    // Outside the root volume is reported, not clamped: an object that has left the world is a
    // fact the caller needs.
    const cy::Expected<void, cy::Error> outside =
        tree.insert(9999u, cy::Aabb::from_point(cy::Vec3{1000.0f, 0.0f, 0.0f}));
    CY_CHECK_FALSE(outside.has_value());
    CY_CHECK(outside.error().code == cy::ErrorCode::OutOfRange);

    CY_REQUIRE(tree.remove(0u).has_value());
    CY_CHECK_EQ(tree.size(), boxes.size() - 1);
    CY_CHECK_FALSE(tree.remove(0u).has_value());
}

CY_TEST_CASE("Octree: a frustum query omits nothing the exact test accepts") {
    cy::Octree tree;
    CY_REQUIRE(
        tree.reset(cy::Aabb::from_center_extents(cy::Vec3{}, cy::Vec3{128.0f, 128.0f, 128.0f}), 5,
                   8)
            .has_value());

    const std::vector<cy::Aabb> boxes = random_boxes(1000, 100.0f, 6161u);
    for (cy::usize i = 0; i < boxes.size(); ++i) {
        CY_REQUIRE(tree.insert(static_cast<cy::u32>(i), boxes[i]).has_value());
    }

    const cy::Mat4 view = cy::look_at(cy::Vec3{0.0f, 30.0f, 90.0f}, cy::Vec3{});
    const cy::Mat4 projection =
        cy::perspective_reversed_z(cy::math::radians(60.0f), 1.0f, 1.0f, 300.0f);
    const cy::Frustum frustum = cy::Frustum::from_view_projection(projection * view);

    std::vector<bool> reported(boxes.size(), false);
    tree.query_frustum(frustum, [&](cy::u32 id, const cy::Aabb&) { reported[id] = true; });

    cy::usize visible = 0;
    for (cy::usize i = 0; i < boxes.size(); ++i) {
        if (frustum.intersects(boxes[i])) {
            ++visible;
            CY_REQUIRE(reported[i]);
        }
    }
    CY_CHECK(visible > 0);
}
