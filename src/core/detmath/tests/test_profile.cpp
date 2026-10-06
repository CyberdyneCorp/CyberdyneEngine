// THE SEAM BETWEEN THIS MODULE AND THE PROFILE CHECK. Task 5.1, design §12.1.
//
// `cy::core-detmath` exports `CY_DETERMINISM_MATH=1` as PUBLIC, so a translation unit that links it
// — this one — sees `BuildConfiguration::from_build().deterministic_math_available`, and
// `DeterminismConfiguration::require()` stops refusing `CrossPlatform` and `Lockstep` as a whole.
// It goes on to the subsystem loop, which refuses the first authoritative subsystem that declares
// less.
//
// src/core/determinism/tests/test_profile.cpp holds the other side: that suite does not link this
// module, so its `from_build()` still reports no deterministic math, and its rewritten cases assert
// the three outcomes with explicit configurations. Delete the PUBLIC definition from
// src/core/detmath/CMakeLists.txt and the first case here goes red; make the module's definition
// leak into a target that does not link it and that suite's seam case goes red.

#include <cy/core/determinism/profile.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstring>
#include <initializer_list>

namespace {

using cy::determinism::BuildConfiguration;
using cy::determinism::DeterminismConfiguration;
using cy::determinism::DeterminismProfile;
using cy::determinism::ProfileRefusal;

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

}  // namespace

CY_TEST_CASE("detmath: linking the module is what makes from_build() report deterministic math") {
    const BuildConfiguration build = BuildConfiguration::from_build();
    CY_CHECK(build.deterministic_math_available);
    // The suite is covered by a determinism profile, so the contraction flag is a checked fact here
    // too, and the case below cannot pass by the build half failing for another reason.
    CY_CHECK(build.contraction_off);
    CY_CHECK_FALSE(build.fast_math);
}

CY_TEST_CASE("detmath: with the module linked, CrossPlatform and Lockstep reach the subsystems") {
    // Accepted when every authoritative subsystem declares the profile...
    DeterminismConfiguration converted(allocator());
    CY_REQUIRE(converted.declare({"movement", DeterminismProfile::Lockstep, true}).has_value());
    CY_REQUIRE(converted.declare({"navigation", DeterminismProfile::Lockstep, true}).has_value());
    CY_REQUIRE(converted.declare({"physics", DeterminismProfile::SamePlatform, false}).has_value());
    for (const DeterminismProfile profile :
         {DeterminismProfile::CrossPlatform, DeterminismProfile::Lockstep}) {
        const auto accepted = converted.require(profile, BuildConfiguration::from_build());
        CY_REQUIRE(accepted.has_value());
        CY_CHECK_EQ(accepted->authoritative_subsystems, 2U);
    }

    // ...and refused, naming the subsystem, when one that is authoritative declares less. Not
    // `DeterministicMathMissing`: the refusal now says which subsystem is still float-based.
    DeterminismConfiguration jolt(allocator());
    CY_REQUIRE(jolt.declare({"movement", DeterminismProfile::Lockstep, true}).has_value());
    CY_REQUIRE(jolt.declare({"physics", DeterminismProfile::SamePlatform, true}).has_value());
    const auto refused =
        jolt.require(DeterminismProfile::Lockstep, BuildConfiguration::from_build());
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().tag == ProfileRefusal::SubsystemGuarantee);
    CY_CHECK(std::strcmp(refused.error().subsystem, "physics") == 0);
    CY_CHECK(std::strcmp(refused.error().guarantee, "cross-platform reproducibility") == 0);
}
