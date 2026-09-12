// Water bodies: one abstraction, several backends, and an overlap resolved by a declared order.
// M10 task 2.3, and `water`'s "Water bodies" and "Simulation backends" requirements.

#include <cy/test/test.h>

#include <cy/water/system.h>

#include "fixtures.h"

namespace test = cy::water::test;
using cy::water::BackendStatus;
using cy::water::BodyProblem;
using cy::water::WaterBackend;
using cy::water::WaterBodyDesc;
using cy::water::WaterBodyId;
using cy::water::WaterBodyType;
using cy::water::WaterRegistry;
using cy::water::WaterSystem;

namespace {

[[nodiscard]] bool contains(const char* text, const char* needle) noexcept {
    if (text == nullptr || needle == nullptr) {
        return false;
    }
    for (const char* start = text; *start != '\0'; ++start) {
        const char* a = start;
        const char* b = needle;
        while (*b != '\0' && *a == *b) {
            ++a;
            ++b;
        }
        if (*b == '\0') {
            return true;
        }
    }
    return false;
}

}  // namespace

CY_TEST_CASE("an ocean, a lake and a river are one abstraction with different backends") {
    WaterRegistry registry(test::allocator());
    CY_REQUIRE(registry.add(test::ocean_desc()).has_value());
    CY_REQUIRE(registry.add(test::lake_desc()).has_value());
    CY_REQUIRE(registry.add(test::river_desc()).has_value());
    CY_REQUIRE(registry.add(test::pool_desc()).has_value());
    CY_CHECK_EQ(registry.size(), 4u);

    // Four types over three backends, and every one of them is a `WaterBodyRecord` — there is no
    // second type for a river and no separate query for a pool.
    cy::u32 spectral = 0;
    cy::u32 spline = 0;
    cy::u32 flat = 0;
    for (const cy::water::WaterBodyRecord& record : registry.bodies()) {
        spectral += (record.desc.backend == WaterBackend::Spectral) ? 1u : 0u;
        spline += (record.desc.backend == WaterBackend::SplineFlow) ? 1u : 0u;
        flat += (record.desc.backend == WaterBackend::Flat) ? 1u : 0u;
    }
    CY_CHECK_EQ(spectral, 2u);
    CY_CHECK_EQ(spline, 1u);
    CY_CHECK_EQ(flat, 1u);
}

CY_TEST_CASE("a lake may use a smaller spectral model than the ocean, and no consumer changes") {
    WaterSystem system(test::allocator(), test::partition());
    const auto ocean = *system.registry().add(test::ocean_desc());
    const auto lake = *system.registry().add(test::lake_desc());

    cy::water::OceanParams sea = test::rough_sea();
    CY_REQUIRE(system.set_ocean(ocean, sea, 1234).has_value());

    // The lake's own spectrum: a two-kilometre fetch and a gentler wind. A DIFFERENT NUMBER IN A
    // DESCRIPTION, and the same `set_ocean` call.
    cy::water::OceanParams still = sea;
    still.wind_speed_mps = 4.0F;
    still.fetch_km = 2.0F;
    still.longest_wavelength = 24.0F;
    CY_REQUIRE(system.set_ocean(lake, still, 1234).has_value());

    const cy::water::AmplitudeSplit sea_split = cy::water::amplitude_split(*system.model_of(ocean));
    const cy::water::AmplitudeSplit lake_split = cy::water::amplitude_split(*system.model_of(lake));
    CY_CHECK_GT(sea_split.authoritative_metres, lake_split.authoritative_metres);

    // And the consumer is the same call on both.
    const cy::water::WaterSample at_sea = system.query(cy::world::WorldVec3d{0.0, 0.0, 0.0});
    const cy::water::WaterSample at_lake = system.query(cy::world::WorldVec3d{700.0, 12.0, 700.0});
    CY_CHECK(at_sea.body == ocean);
    CY_CHECK(at_lake.body == lake);
}

CY_TEST_CASE("a planned backend and a deferred one are refused, and the refusal says which") {
    WaterRegistry registry(test::allocator());

    WaterBodyDesc shallow = test::pool_desc();
    shallow.name = "test.flood";
    shallow.backend = WaterBackend::ShallowWater;
    const auto planned = registry.add(shallow);
    CY_REQUIRE_FALSE(planned.has_value());
    CY_CHECK(planned.error().code == cy::ErrorCode::NotImplemented);
    CY_CHECK_EQ(static_cast<int>(registry.last_problem()),
                static_cast<int>(BodyProblem::BackendPlanned));
    CY_CHECK(contains(planned.error().message, "ShallowWater"));
    CY_CHECK(contains(planned.error().message, "planned"));

    WaterBodyDesc particles = test::pool_desc();
    particles.name = "test.splash";
    particles.backend = WaterBackend::Particle;
    const auto deferred = registry.add(particles);
    CY_REQUIRE_FALSE(deferred.has_value());
    CY_CHECK_EQ(static_cast<int>(registry.last_problem()),
                static_cast<int>(BodyProblem::BackendDeferred));
    // "its seam SHALL be the same seam `vfx-system` reserves for fluids; the two SHALL NOT become
    // separate fluid systems" — a refusal that did not say where the seam is would invite one.
    CY_CHECK(contains(deferred.error().message, "vfx-system"));

    CY_CHECK_EQ(static_cast<int>(cy::water::backend_status(WaterBackend::Flat)),
                static_cast<int>(BackendStatus::Required));
    CY_CHECK_EQ(static_cast<int>(cy::water::backend_status(WaterBackend::Spectral)),
                static_cast<int>(BackendStatus::Required));
    CY_CHECK_EQ(static_cast<int>(cy::water::backend_status(WaterBackend::SplineFlow)),
                static_cast<int>(BackendStatus::Required));
}

CY_TEST_CASE("a description that cannot describe a body is refused, by reason") {
    WaterRegistry registry(test::allocator());

    WaterBodyDesc nameless = test::pool_desc();
    nameless.name = "";
    CY_CHECK_FALSE(registry.add(nameless).has_value());
    CY_CHECK_EQ(static_cast<int>(registry.last_problem()), static_cast<int>(BodyProblem::NoName));

    WaterBodyDesc empty = test::pool_desc();
    empty.bounds.max_x = empty.bounds.min_x;
    CY_CHECK_FALSE(registry.add(empty).has_value());
    CY_CHECK_EQ(static_cast<int>(registry.last_problem()),
                static_cast<int>(BodyProblem::EmptyBounds));

    WaterBodyDesc floating = test::pool_desc();
    floating.mean_level = floating.bounds.max_y + 5.0;
    CY_CHECK_FALSE(registry.add(floating).has_value());
    CY_CHECK_EQ(static_cast<int>(registry.last_problem()),
                static_cast<int>(BodyProblem::LevelOutsideBounds));

    CY_REQUIRE(registry.add(test::pool_desc()).has_value());
    CY_CHECK_FALSE(registry.add(test::pool_desc()).has_value());
    CY_CHECK_EQ(static_cast<int>(registry.last_problem()),
                static_cast<int>(BodyProblem::DuplicateName));
}

CY_TEST_CASE("where a river meets the sea the overlap resolves to one consistent answer") {
    WaterRegistry registry(test::allocator());
    const auto ocean = *registry.add(test::ocean_desc());
    const auto river = *registry.add(test::river_desc());

    const cy::world::WorldVec3d mouth{100.0, 1.0, 100.0};
    // ONE answer, and it is the higher declared priority.
    CY_CHECK(registry.body_at(mouth) == river);

    // And the overlap itself is visible: both bodies contain the position, in the declared order.
    cy::Array<WaterBodyId> overlapping(test::allocator());
    CY_REQUIRE(registry.bodies_at(mouth, overlapping).has_value());
    CY_REQUIRE_EQ(overlapping.size(), 2u);
    CY_CHECK(overlapping[0] == river);
    CY_CHECK(overlapping[1] == ocean);

    // A position the river's bounds do not reach is the sea's, with no ambiguity to resolve.
    cy::Array<WaterBodyId> offshore(test::allocator());
    CY_REQUIRE(
        registry.bodies_at(cy::world::WorldVec3d{-2000.0, 0.0, -2000.0}, offshore).has_value());
    CY_REQUIRE_EQ(offshore.size(), 1u);
    CY_CHECK(offshore[0] == ocean);
}

CY_TEST_CASE("the resolution order does not depend on registration order") {
    WaterRegistry first(test::allocator());
    CY_REQUIRE(first.add(test::ocean_desc()).has_value());
    const auto river_a = *first.add(test::river_desc());

    WaterRegistry second(test::allocator());
    const auto river_b = *second.add(test::river_desc());
    CY_REQUIRE(second.add(test::ocean_desc()).has_value());

    const cy::world::WorldVec3d mouth{100.0, 1.0, 100.0};
    CY_CHECK(first.body_at(mouth) == river_a);
    CY_CHECK(second.body_at(mouth) == river_b);

    // And both list the same two bodies in the same order.
    cy::Array<WaterBodyId> a(test::allocator());
    cy::Array<WaterBodyId> b(test::allocator());
    CY_REQUIRE(first.bodies_at(mouth, a).has_value());
    CY_REQUIRE(second.bodies_at(mouth, b).has_value());
    CY_REQUIRE_EQ(a.size(), 2u);
    CY_REQUIRE_EQ(b.size(), 2u);
    CY_CHECK(a[0] == b[0]);
    CY_CHECK(a[1] == b[1]);
}

CY_TEST_CASE("two bodies of equal priority resolve by declaration order, stably") {
    WaterRegistry registry(test::allocator());
    WaterBodyDesc first = test::pool_desc();
    first.name = "test.pool.a";
    WaterBodyDesc second = test::pool_desc();
    second.name = "test.pool.b";
    const auto a = *registry.add(first);
    CY_REQUIRE(registry.add(second).has_value());

    const cy::world::WorldVec3d inside{2005.0, 30.0, 2004.0};
    // The tie-break exists and is the declaration order. What matters is not which rule it is but
    // that the answer is ONE body and the same one every time.
    CY_CHECK(registry.body_at(inside) == a);
    CY_CHECK(registry.body_at(inside) == a);
}
