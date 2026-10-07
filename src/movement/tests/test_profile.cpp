// SPDX-License-Identifier: MIT
// THE PROFILE CHECK FOR A LOCKSTEP SESSION BUILT FROM THESE SUBSYSTEMS. Design §12.1, tasks 6.2,
// 7.1 and 7.3.
//
// With the deterministic math module linked — this suite links it through `cy::movement`, and
// `from_build()` is the real one — `require()` reaches the subsystem loop, and a `Lockstep` session
// is accepted exactly when every authoritative subsystem declares it, and refused naming the first
// one that does not, in name order.

#include <cy/core/determinism/profile.h>
#include <cy/gameplay/abilities/determinism.h>
#include <cy/gameplay/command.h>
#include <cy/gameplay/control.h>
#include <cy/movement/determinism.h>
#include <cy/navigation/determinism.h>

#include <string>

#include "movement_fixture.h"

namespace {

using cy::determinism::BuildConfiguration;
using cy::determinism::DeterminismConfiguration;
using cy::determinism::DeterminismProfile;
using cy::determinism::ProfileRefusal;
using cy::navigation::NavArithmetic;
using cy::navigation::NavWorldDeclaration;
using namespace cy::movement_test;

/// The parts of a lockstep RTS session: the mover, one navigation world, and the command stream.
struct Session {
    Session() noexcept
        : control(allocator()), commands(allocator(), control), registry(allocator()) {}

    [[nodiscard]] bool declare(NavArithmetic arithmetic, DeterminismProfile stream) noexcept {
        if (!commands.set_determinism_profile(stream)) {
            return false;
        }
        const NavWorldDeclaration world{"battlefield", arithmetic, true, false};
        return registry.declare(cy::movement::movement_determinism()).has_value() &&
               registry
                   .declare(cy::navigation::navigation_determinism(
                       cy::Span<const NavWorldDeclaration>(&world, 1)))
                   .has_value() &&
               registry.declare(commands.determinism_declaration()).has_value();
    }

    cy::gameplay::ControlRegistry control;
    cy::gameplay::CommandStream commands;
    DeterminismConfiguration registry;
};

}  // namespace

CY_TEST_CASE("movement profile: this build has deterministic math, because it links the mover") {
    const BuildConfiguration build = BuildConfiguration::from_build();
    CY_CHECK(build.deterministic_math_available);
    CY_CHECK(build.contraction_off);
}

CY_TEST_CASE("movement profile: Lockstep is accepted when every authoritative subsystem meets it") {
    Session session;
    CY_REQUIRE(session.declare(NavArithmetic::Fixed, DeterminismProfile::Lockstep));
    const auto verdict =
        session.registry.require(DeterminismProfile::Lockstep, BuildConfiguration::from_build());
    CY_REQUIRE(verdict.has_value());
    CY_CHECK_EQ(verdict->authoritative_subsystems, 3U);
    CY_CHECK_EQ(session.registry.strongest_supported(), DeterminismProfile::Lockstep);
    // And CrossPlatform, which Lockstep implies.
    CY_CHECK(session.registry
                 .require(DeterminismProfile::CrossPlatform, BuildConfiguration::from_build())
                 .has_value());
}

CY_TEST_CASE("movement profile: a Float navigation world is refused by name") {
    Session session;
    CY_REQUIRE(session.declare(NavArithmetic::Float, DeterminismProfile::Lockstep));
    const auto verdict =
        session.registry.require(DeterminismProfile::Lockstep, BuildConfiguration::from_build());
    CY_REQUIRE_FALSE(verdict.has_value());
    CY_CHECK_EQ(verdict.error().tag, ProfileRefusal::SubsystemGuarantee);
    CY_CHECK_EQ(std::string(verdict.error().subsystem), std::string("navigation"));
    // SamePlatform is what is left, and it is still offered.
    CY_CHECK_EQ(session.registry.strongest_supported(), DeterminismProfile::SamePlatform);
}

CY_TEST_CASE("movement profile: a runtime-rebuilt Fixed world keeps navigation SamePlatform") {
    const NavWorldDeclaration worlds[] = {
        {"ambient", NavArithmetic::Float, false, true},
        {"battlefield", NavArithmetic::Fixed, true, true},
    };
    const auto declared = cy::navigation::navigation_determinism(worlds);
    CY_CHECK_EQ(declared.guarantees, DeterminismProfile::SamePlatform);
    CY_CHECK_EQ(std::string(cy::navigation::first_float_world(worlds)->name),
                std::string("battlefield"));
    // The presentation-only Float world alone constrains nothing.
    const auto ambient =
        cy::navigation::navigation_determinism(cy::Span<const NavWorldDeclaration>(worlds, 1));
    CY_CHECK_EQ(ambient.guarantees, DeterminismProfile::Lockstep);
    CY_CHECK_FALSE(ambient.authoritative);
}

CY_TEST_CASE("movement profile: an unchecked command stream is refused by name") {
    Session session;
    CY_REQUIRE(session.declare(NavArithmetic::Fixed, DeterminismProfile::SamePlatform));
    const auto verdict =
        session.registry.require(DeterminismProfile::Lockstep, BuildConfiguration::from_build());
    CY_REQUIRE_FALSE(verdict.has_value());
    CY_CHECK_EQ(std::string(verdict.error().subsystem), std::string("gameplay-commands"));
}

CY_TEST_CASE("movement profile: authoritative abilities refuse the session, named first") {
    Session session;
    CY_REQUIRE(session.declare(NavArithmetic::Fixed, DeterminismProfile::Lockstep));
    CY_REQUIRE(
        session.registry.declare(cy::gameplay::abilities::abilities_determinism()).has_value());
    const auto verdict =
        session.registry.require(DeterminismProfile::Lockstep, BuildConfiguration::from_build());
    CY_REQUIRE_FALSE(verdict.has_value());
    CY_CHECK_EQ(std::string(verdict.error().subsystem), std::string("abilities"));
}
