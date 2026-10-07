// SPDX-License-Identifier: MIT
// openspec/changes/add-deterministic-math, task 7.3: the ability system declares `SamePlatform`
// explicitly, and a `Lockstep` session that uses it authoritatively is refused naming it.
//
// The build half of `require()` is stated by hand here — contraction off and deterministic math
// present — because the question is the subsystem loop, and this suite does not link the
// deterministic math module (`unit.detmath` and `unit.movement` hold the real `from_build()` side).

#include <cy/core/determinism/profile.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/abilities/determinism.h>
#include <cy/test/test.h>

#include <string>

namespace {

using cy::determinism::BuildConfiguration;
using cy::determinism::DeterminismConfiguration;
using cy::determinism::DeterminismProfile;
using cy::determinism::ProfileRefusal;
using cy::determinism::SubsystemDeterminism;

[[nodiscard]] BuildConfiguration deterministic_build() noexcept {
    BuildConfiguration build;
    build.contraction_off = true;
    build.deterministic_math_available = true;
    return build;
}

}  // namespace

CY_TEST_CASE("abilities: a Lockstep session using abilities authoritatively is refused by name") {
    DeterminismConfiguration configuration(cy::system_allocator(cy::MemoryDomain::World));
    CY_REQUIRE(
        configuration.declare(SubsystemDeterminism{"movement", DeterminismProfile::Lockstep, true})
            .has_value());
    CY_REQUIRE(configuration.declare(cy::gameplay::abilities::abilities_determinism()).has_value());

    const auto verdict = configuration.require(DeterminismProfile::Lockstep, deterministic_build());
    CY_REQUIRE_FALSE(verdict.has_value());
    CY_CHECK_EQ(verdict.error().tag, ProfileRefusal::SubsystemGuarantee);
    CY_CHECK_EQ(std::string(verdict.error().subsystem), std::string("abilities"));
    CY_CHECK_EQ(verdict.error().provided, DeterminismProfile::SamePlatform);
}

CY_TEST_CASE("abilities: presentation-only abilities do not constrain a Lockstep session") {
    DeterminismConfiguration configuration(cy::system_allocator(cy::MemoryDomain::World));
    CY_REQUIRE(
        configuration.declare(SubsystemDeterminism{"movement", DeterminismProfile::Lockstep, true})
            .has_value());
    CY_REQUIRE(
        configuration.declare(cy::gameplay::abilities::abilities_determinism(false)).has_value());
    CY_CHECK(
        configuration.require(DeterminismProfile::Lockstep, deterministic_build()).has_value());
    // And the same registry still runs SamePlatform with abilities authoritative.
    DeterminismConfiguration same(cy::system_allocator(cy::MemoryDomain::World));
    CY_REQUIRE(same.declare(cy::gameplay::abilities::abilities_determinism()).has_value());
    CY_CHECK(same.require(DeterminismProfile::SamePlatform, deterministic_build()).has_value());
}
