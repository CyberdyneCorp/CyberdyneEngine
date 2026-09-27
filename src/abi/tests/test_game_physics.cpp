// SPDX-License-Identifier: MIT
// ABI 1.3's `physics_*` thunks against a fake backend. `add-swift-game-api`, task 2.1.
//
// What the thunks own is tested here: the phase list (`[N F U]`, so no phase refuses), UNAVAILABLE
// with no backend, the null and value checks, the default filter a null one becomes, a short
// `struct_size` filter, and the sizing pattern. What the physics server answers — the order, the
// ignore list, the step guard — is the adapter's, and integration.game_backend_physics tests it
// against the reference server.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/physics.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstddef>
#include <cstring>
#include <limits>

namespace {

using cy::u32;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

/// Records what it was asked and answers `total` hits (or entities) numbered from 1.
class FakePhysics final : public cy::abi::game::PhysicsQueryBackend {
public:
    mutable u32 calls = 0;
    mutable CyQueryFilter last_filter{};
    mutable CyRay last_ray{};
    mutable CyShape last_shape{};
    mutable float last_direction[3] = {};
    mutable float last_max_distance = 0.0F;
    u32 total = 3;
    bool has_hit = true;
    CyResult answer = CY_RESULT_OK;

    CyResult raycast(const CyRay& ray, const CyQueryFilter& filter, CyPhysicsHit& out_hit,
                     bool& out_has_hit) const noexcept override {
        record(filter);
        last_ray = ray;
        if (answer != CY_RESULT_OK) {
            return cy::abi::report(answer, "fake physics refused");
        }
        out_hit = hit(1);
        out_has_hit = has_hit;
        return CY_RESULT_OK;
    }

    CyResult raycast_all(const CyRay& ray, const CyQueryFilter& filter, cy::Span<CyPhysicsHit> out,
                         u32& out_total) const noexcept override {
        record(filter);
        last_ray = ray;
        for (u32 index = 0; index < total && index < out.size(); ++index) {
            out[index] = hit(index + 1);
        }
        out_total = total;
        return CY_RESULT_OK;
    }

    CyResult shape_cast(const CyShape& shape, const CyPose& /*start*/, const cy::f32* direction,
                        cy::f32 max_distance, const CyQueryFilter& filter, CyPhysicsHit& out_hit,
                        bool& out_has_hit) const noexcept override {
        record(filter);
        last_shape = shape;
        std::memcpy(last_direction, direction, sizeof(last_direction));
        last_max_distance = max_distance;
        out_hit = hit(1);
        out_has_hit = has_hit;
        return CY_RESULT_OK;
    }

    CyResult overlap(const CyShape& shape, const CyPose& /*pose*/, const CyQueryFilter& filter,
                     cy::Span<CyEntity> out, u32& out_total) const noexcept override {
        record(filter);
        last_shape = shape;
        for (u32 index = 0; index < total && index < out.size(); ++index) {
            out[index] = index + 1;
        }
        out_total = total;
        return CY_RESULT_OK;
    }

private:
    void record(const CyQueryFilter& filter) const noexcept {
        ++calls;
        last_filter = filter;
    }

    static CyPhysicsHit hit(u32 entity) noexcept {
        CyPhysicsHit value{};
        value.entity = entity;
        value.distance = static_cast<float>(entity);
        value.fraction = 0.5F;
        return value;
    }
};

struct Fixture {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    FakePhysics physics;

    Fixture() noexcept { host.game.physics = &physics; }
    [[nodiscard]] CyEngine engine() noexcept { return &host; }
};

CyRay down_ray() noexcept {
    CyRay ray{};
    ray.origin[1] = 10.0F;
    ray.direction[1] = -1.0F;
    ray.max_distance = 100.0F;
    return ray;
}

CyShape sphere(float radius) noexcept {
    CyShape shape{};
    shape.kind = CY_SHAPE_SPHERE;
    shape.radius = radius;
    return shape;
}

}  // namespace

CY_TEST_CASE("physics_raycast returns the backend's hit and says whether there was one") {
    Fixture fixture;
    const CyRay ray = down_ray();
    CyPhysicsHit hit{};
    bool has_hit = false;
    CY_CHECK_EQ(table().physics_raycast(fixture.engine(), &ray, nullptr, &hit, &has_hit),
                CY_RESULT_OK);
    CY_CHECK(has_hit);
    CY_CHECK_EQ(hit.entity, 1U);
    CY_CHECK_EQ(fixture.physics.last_ray.max_distance, 100.0F);

    // Nothing hit is an answer, not an error.
    fixture.physics.has_hit = false;
    CY_CHECK_EQ(table().physics_raycast(fixture.engine(), &ray, nullptr, &hit, &has_hit),
                CY_RESULT_OK);
    CY_CHECK_FALSE(has_hit);
}

CY_TEST_CASE("a null filter reaches the backend as layer 0, every layer, nothing ignored") {
    Fixture fixture;
    const CyRay ray = down_ray();
    CyPhysicsHit hit{};
    bool has_hit = false;
    CY_REQUIRE(table().physics_raycast(fixture.engine(), &ray, nullptr, &hit, &has_hit) ==
               CY_RESULT_OK);
    const CyQueryFilter& seen = fixture.physics.last_filter;
    CY_CHECK_EQ(seen.struct_size, sizeof(CyQueryFilter));
    CY_CHECK_EQ(seen.layer, 0U);
    CY_CHECK_EQ(seen.mask, 0xFFFFFFFFU);
    CY_CHECK_EQ(seen.flags, 0U);
    CY_CHECK(seen.ignore == nullptr);
    CY_CHECK_EQ(seen.ignore_count, 0U);
}

CY_TEST_CASE("a filter from an older, shorter caller reaches the backend whole") {
    Fixture fixture;
    const CyRay ray = down_ray();
    CyQueryFilter filter{};
    filter.layer = 3;
    filter.mask = 0x0FU;
    filter.flags = CY_QUERY_INCLUDE_TRIGGERS;
    const CyEntity ignored = 42;
    filter.ignore = &ignored;
    filter.ignore_count = 1;
    // A caller that knew only up to `flags`: the ignore list it could not have written reads as
    // empty, never as whatever followed its struct in memory.
    filter.struct_size = static_cast<u32>(offsetof(CyQueryFilter, ignore));
    CyPhysicsHit hit{};
    bool has_hit = false;
    CY_REQUIRE(table().physics_raycast(fixture.engine(), &ray, &filter, &hit, &has_hit) ==
               CY_RESULT_OK);
    const CyQueryFilter& seen = fixture.physics.last_filter;
    CY_CHECK_EQ(seen.struct_size, sizeof(CyQueryFilter));
    CY_CHECK_EQ(seen.layer, 3U);
    CY_CHECK_EQ(seen.mask, 0x0FU);
    CY_CHECK_EQ(seen.flags, CY_QUERY_INCLUDE_TRIGGERS);
    CY_CHECK(seen.ignore == nullptr);
    CY_CHECK_EQ(seen.ignore_count, 0U);

    // And a whole one is passed through, ignore list included.
    filter.struct_size = 0;
    CY_REQUIRE(table().physics_raycast(fixture.engine(), &ray, &filter, &hit, &has_hit) ==
               CY_RESULT_OK);
    CY_CHECK(fixture.physics.last_filter.ignore == &ignored);
    CY_CHECK_EQ(fixture.physics.last_filter.ignore_count, 1U);
}

CY_TEST_CASE("a malformed filter is refused before the backend sees it") {
    Fixture fixture;
    const CyRay ray = down_ray();
    CyPhysicsHit hit{};
    bool has_hit = false;

    CyQueryFilter filter{};
    filter.struct_size = 2;
    CY_CHECK_EQ(table().physics_raycast(fixture.engine(), &ray, &filter, &hit, &has_hit),
                CY_RESULT_INVALID_ARGUMENT);

    filter = CyQueryFilter{};
    filter.layer = 32;
    CY_CHECK_EQ(table().physics_raycast(fixture.engine(), &ray, &filter, &hit, &has_hit),
                CY_RESULT_INVALID_ARGUMENT);

    filter = CyQueryFilter{};
    filter.ignore_count = 2;
    CY_CHECK_EQ(table().physics_raycast(fixture.engine(), &ray, &filter, &hit, &has_hit),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(fixture.physics.calls, 0U);
}

CY_TEST_CASE("a malformed ray is refused and the outputs are left untouched") {
    Fixture fixture;
    CyPhysicsHit hit{};
    hit.entity = 99;
    bool has_hit = true;

    CyRay ray = down_ray();
    ray.max_distance = -1.0F;
    CY_CHECK_EQ(table().physics_raycast(fixture.engine(), &ray, nullptr, &hit, &has_hit),
                CY_RESULT_INVALID_ARGUMENT);
    ray = down_ray();
    ray.origin[0] = std::numeric_limits<float>::quiet_NaN();
    CY_CHECK_EQ(table().physics_raycast(fixture.engine(), &ray, nullptr, &hit, &has_hit),
                CY_RESULT_INVALID_ARGUMENT);
    ray = down_ray();
    ray.direction[1] = 0.0F;
    CY_CHECK_EQ(table().physics_raycast(fixture.engine(), &ray, nullptr, &hit, &has_hit),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().physics_raycast(fixture.engine(), nullptr, nullptr, &hit, &has_hit),
                CY_RESULT_INVALID_ARGUMENT);
    ray = down_ray();
    CY_CHECK_EQ(table().physics_raycast(fixture.engine(), &ray, nullptr, nullptr, &has_hit),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(hit.entity, 99U);
    CY_CHECK(has_hit);
    CY_CHECK_EQ(fixture.physics.calls, 0U);
}

CY_TEST_CASE("every physics query answers in every phase") {
    Fixture fixture;
    const CyRay ray = down_ray();
    CyPhysicsHit hit{};
    bool has_hit = false;
    for (const CyPhase phase : {CY_PHASE_NONE, CY_PHASE_FIXED_UPDATE, CY_PHASE_FRAME_UPDATE}) {
        const cy::abi::game::PhaseScope scope(fixture.host.game.clock, phase);
        CY_CHECK_EQ(table().physics_raycast(fixture.engine(), &ray, nullptr, &hit, &has_hit),
                    CY_RESULT_OK);
        u32 count = 0;
        CY_CHECK_EQ(
            table().physics_raycast_all(fixture.engine(), &ray, nullptr, nullptr, 0, &count),
            CY_RESULT_OK);
    }
}

CY_TEST_CASE("with no physics backend bound every query is UNAVAILABLE, naming the service") {
    cy::abi::Host host(cy::system_allocator(cy::MemoryDomain::Scripting));
    const CyRay ray = down_ray();
    const CyShape shape = sphere(1.0F);
    const CyPose pose{};
    const float direction[3] = {1.0F, 0.0F, 0.0F};
    CyPhysicsHit hit{};
    bool has_hit = false;
    u32 count = 0;
    CY_CHECK_EQ(table().physics_raycast(&host, &ray, nullptr, &hit, &has_hit),
                CY_RESULT_UNAVAILABLE);
    CY_CHECK(std::strstr(cy::abi::last_error_message(), "physics") != nullptr);
    CY_CHECK_EQ(table().physics_raycast_all(&host, &ray, nullptr, nullptr, 0, &count),
                CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(
        table().physics_shape_cast(&host, &shape, &pose, direction, 1.0F, nullptr, &hit, &has_hit),
        CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().physics_overlap(&host, &shape, &pose, nullptr, nullptr, 0, &count),
                CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(table().physics_raycast(nullptr, &ray, nullptr, &hit, &has_hit),
                CY_RESULT_INVALID_ARGUMENT);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("physics_raycast_all follows the sizing pattern") {
    Fixture fixture;
    const CyRay ray = down_ray();
    fixture.physics.total = 3;

    // A null buffer asks for the total.
    u32 count = 0;
    CY_CHECK_EQ(table().physics_raycast_all(fixture.engine(), &ray, nullptr, nullptr, 0, &count),
                CY_RESULT_OK);
    CY_CHECK_EQ(count, 3U);

    // A short buffer is filled with the nearest and says so.
    CyPhysicsHit two[2] = {};
    CY_CHECK_EQ(table().physics_raycast_all(fixture.engine(), &ray, nullptr, two, 2, &count),
                CY_RESULT_BUFFER_TOO_SMALL);
    CY_CHECK_EQ(count, 3U);
    CY_CHECK_EQ(two[0].entity, 1U);
    CY_CHECK_EQ(two[1].entity, 2U);

    // A big enough one is OK, with the total, which is also the number written.
    CyPhysicsHit four[4] = {};
    CY_CHECK_EQ(table().physics_raycast_all(fixture.engine(), &ray, nullptr, four, 4, &count),
                CY_RESULT_OK);
    CY_CHECK_EQ(count, 3U);
    CY_CHECK_EQ(four[2].entity, 3U);
    CY_CHECK_EQ(four[3].entity, 0U);

    CY_CHECK_EQ(table().physics_raycast_all(fixture.engine(), &ray, nullptr, four, 4, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("physics_overlap follows the sizing pattern") {
    Fixture fixture;
    const CyShape shape = sphere(2.0F);
    const CyPose pose{};
    fixture.physics.total = 5;
    u32 count = 0;
    CY_CHECK_EQ(
        table().physics_overlap(fixture.engine(), &shape, &pose, nullptr, nullptr, 0, &count),
        CY_RESULT_OK);
    CY_CHECK_EQ(count, 5U);
    CyEntity three[3] = {};
    CY_CHECK_EQ(table().physics_overlap(fixture.engine(), &shape, &pose, nullptr, three, 3, &count),
                CY_RESULT_BUFFER_TOO_SMALL);
    CY_CHECK_EQ(count, 5U);
    CY_CHECK_EQ(three[2], 3U);
    CY_CHECK_EQ(fixture.physics.last_shape.radius, 2.0F);
}

CY_TEST_CASE("physics_shape_cast passes the sweep through and refuses a bad shape") {
    Fixture fixture;
    CyPose start{};
    const float direction[3] = {0.0F, 0.0F, 1.0F};
    CyPhysicsHit hit{};
    bool has_hit = false;

    CyShape capsule{};
    capsule.kind = CY_SHAPE_CAPSULE;
    capsule.radius = 0.5F;
    capsule.half_height = 1.0F;
    CY_CHECK_EQ(table().physics_shape_cast(fixture.engine(), &capsule, &start, direction, 7.0F,
                                           nullptr, &hit, &has_hit),
                CY_RESULT_OK);
    CY_CHECK(has_hit);
    CY_CHECK_EQ(fixture.physics.last_shape.kind, static_cast<u32>(CY_SHAPE_CAPSULE));
    CY_CHECK_EQ(fixture.physics.last_direction[2], 1.0F);
    CY_CHECK_EQ(fixture.physics.last_max_distance, 7.0F);
    const u32 calls = fixture.physics.calls;

    CyShape bad = sphere(0.0F);
    CY_CHECK_EQ(table().physics_shape_cast(fixture.engine(), &bad, &start, direction, 1.0F, nullptr,
                                           &hit, &has_hit),
                CY_RESULT_INVALID_ARGUMENT);
    bad = CyShape{};
    bad.kind = CY_SHAPE_BOX;
    bad.half_extents[0] = 1.0F;
    bad.half_extents[1] = 1.0F;
    CY_CHECK_EQ(table().physics_shape_cast(fixture.engine(), &bad, &start, direction, 1.0F, nullptr,
                                           &hit, &has_hit),
                CY_RESULT_INVALID_ARGUMENT);
    bad.kind = 7;
    CY_CHECK_EQ(table().physics_shape_cast(fixture.engine(), &bad, &start, direction, 1.0F, nullptr,
                                           &hit, &has_hit),
                CY_RESULT_INVALID_ARGUMENT);

    const float zero[3] = {};
    const CyShape good = sphere(1.0F);
    CY_CHECK_EQ(table().physics_shape_cast(fixture.engine(), &good, &start, zero, 1.0F, nullptr,
                                           &hit, &has_hit),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(table().physics_shape_cast(fixture.engine(), &good, &start, direction, -2.0F,
                                           nullptr, &hit, &has_hit),
                CY_RESULT_INVALID_ARGUMENT);
    start.rotation[3] = std::numeric_limits<float>::infinity();
    CY_CHECK_EQ(table().physics_shape_cast(fixture.engine(), &good, &start, direction, 1.0F,
                                           nullptr, &hit, &has_hit),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(fixture.physics.calls, calls);
}

CY_TEST_CASE("a backend refusal is returned as is and leaves the outputs untouched") {
    Fixture fixture;
    fixture.physics.answer = CY_RESULT_UNAVAILABLE;
    const CyRay ray = down_ray();
    CyPhysicsHit hit{};
    hit.entity = 5;
    bool has_hit = false;
    CY_CHECK_EQ(table().physics_raycast(fixture.engine(), &ray, nullptr, &hit, &has_hit),
                CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(hit.entity, 5U);
    CY_CHECK_EQ(cy::abi::last_error_code(), CY_RESULT_UNAVAILABLE);

    // And a success after it clears the last error.
    fixture.physics.answer = CY_RESULT_OK;
    CY_CHECK_EQ(table().physics_raycast(fixture.engine(), &ray, nullptr, &hit, &has_hit),
                CY_RESULT_OK);
    CY_CHECK_EQ(cy::abi::last_error_code(), CY_RESULT_OK);
}
