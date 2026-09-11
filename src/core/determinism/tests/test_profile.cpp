// M9 TASKS 2.1 AND 2.2 — determinism profiles, and the refusal that happens at configuration.
//
// The exit criterion is the strong one: "A session declaring a determinism profile a subsystem
// cannot meet is **rejected at configuration**, not discovered later." These cases are the runtime
// half. The build half is CMake's — `cy_declare_determinism_profile()` fails the configure — and
// `determinism: from_build() reports what this translation unit was compiled with` below is the
// seam between the two.

#include <cy/core/determinism/profile.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstring>
#include <limits>

namespace {

using namespace cy;
using namespace cy::determinism;

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::World);
}

/// A build with nothing wrong with it: contraction off, no fast-math, deterministic math present.
/// Every case that is *not* about the build starts from this so a failure names the subsystem.
[[nodiscard]] BuildConfiguration clean_build() noexcept {
    BuildConfiguration build;
    build.contraction_off = true;
    build.fast_math = false;
    build.target_has_fma = true;
    build.deterministic_math_available = true;
    return build;
}

}  // namespace

CY_TEST_CASE("determinism: a profile's guarantees are separate facts, not a rank") {
    CY_CHECK(guarantees_of(DeterminismProfile::None) == Guarantees{});
    CY_CHECK(guarantees_of(DeterminismProfile::ReplayStable).records_commands);
    CY_CHECK_FALSE(guarantees_of(DeterminismProfile::ReplayStable).same_binary_reproducible);
    CY_CHECK(guarantees_of(DeterminismProfile::SamePlatform).same_binary_reproducible);
    CY_CHECK_FALSE(guarantees_of(DeterminismProfile::SamePlatform).cross_platform_reproducible);
    CY_CHECK(guarantees_of(DeterminismProfile::CrossPlatform).cross_platform_reproducible);
    CY_CHECK_FALSE(guarantees_of(DeterminismProfile::CrossPlatform).commands_alone);
    CY_CHECK(guarantees_of(DeterminismProfile::Lockstep).commands_alone);

    // `simulation-and-determinism`'s "A profile is a decision": a project selecting `ReplayStable`
    // pays for command and external-result recording and is NOT required to meet lockstep ordering.
    CY_CHECK(satisfies(DeterminismProfile::ReplayStable, DeterminismProfile::ReplayStable));
    CY_CHECK_FALSE(satisfies(DeterminismProfile::ReplayStable, DeterminismProfile::Lockstep));
    CY_CHECK(satisfies(DeterminismProfile::Lockstep, DeterminismProfile::ReplayStable));

    CY_CHECK(unmet_guarantee(DeterminismProfile::SamePlatform, DeterminismProfile::SamePlatform) ==
             nullptr);
    const char* unmet =
        unmet_guarantee(DeterminismProfile::SamePlatform, DeterminismProfile::CrossPlatform);
    CY_REQUIRE(unmet != nullptr);
    CY_CHECK(std::strcmp(unmet, "cross-platform reproducibility") == 0);
}

CY_TEST_CASE(
    "determinism: a session requiring more than a subsystem guarantees is refused by name") {
    // The requirement's own scenario: "WHEN a session declares CrossPlatform while relying on a
    // subsystem that does not guarantee it THEN configuration validation SHALL fail naming that
    // subsystem."
    DeterminismConfiguration configuration(allocator());
    CY_REQUIRE(
        configuration.declare({"navigation", DeterminismProfile::Lockstep, true}).has_value());
    CY_REQUIRE(
        configuration.declare({"physics", DeterminismProfile::SamePlatform, true}).has_value());
    CY_REQUIRE(
        configuration.declare({"animation", DeterminismProfile::Lockstep, true}).has_value());

    const auto refused = configuration.require(DeterminismProfile::CrossPlatform, clean_build());
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().tag == ProfileRefusal::SubsystemGuarantee);
    CY_CHECK(std::strcmp(refused.error().subsystem, "physics") == 0);
    CY_CHECK(std::strcmp(refused.error().guarantee, "cross-platform reproducibility") == 0);
    CY_CHECK(refused.error().required == DeterminismProfile::CrossPlatform);
    CY_CHECK(refused.error().provided == DeterminismProfile::SamePlatform);

    // ...and the same registry accepts the profile it can actually meet, with the margin reported.
    const auto accepted = configuration.require(DeterminismProfile::SamePlatform, clean_build());
    CY_REQUIRE(accepted.has_value());
    CY_CHECK_EQ(accepted->authoritative_subsystems, 3U);
    CY_CHECK_EQ(accepted->at_the_limit, 1U);  // physics, which cannot take one more step
    CY_CHECK(configuration.strongest_supported() == DeterminismProfile::SamePlatform);
}

CY_TEST_CASE("determinism: the subsystem a refusal names does not depend on declaration order") {
    // Two registries with the same three subsystems declared in opposite orders. A refusal that
    // reported whichever it happened to reach first would name different subsystems, and a bug
    // report quoting one would send the reader to the wrong module.
    const char* names[] = {"zulu", "alpha", "mike"};
    const char* first_named[2] = {"", ""};
    for (u32 run = 0; run < 2; ++run) {
        DeterminismConfiguration configuration(allocator());
        for (u32 index = 0; index < 3; ++index) {
            const u32 slot = run == 0 ? index : 2 - index;
            CY_REQUIRE(configuration.declare({names[slot], DeterminismProfile::ReplayStable, true})
                           .has_value());
        }
        const auto refused = configuration.require(DeterminismProfile::SamePlatform, clean_build());
        CY_REQUIRE_FALSE(refused.has_value());
        first_named[run] = refused.error().subsystem;
    }
    CY_CHECK(std::strcmp(first_named[0], first_named[1]) == 0);
    CY_CHECK(std::strcmp(first_named[0], "alpha") == 0);
}

CY_TEST_CASE("determinism: an empty registry cannot satisfy a profile") {
    // A check that passes when it has been told nothing is the shape of every criterion this
    // project's gates have had to remove.
    DeterminismConfiguration configuration(allocator());
    const auto refused = configuration.require(DeterminismProfile::Lockstep, clean_build());
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().tag == ProfileRefusal::NoSubsystemsDeclared);

    // `None` is the one profile an empty registry may have, because it promises nothing.
    CY_CHECK(configuration.require(DeterminismProfile::None, clean_build()).has_value());
}

CY_TEST_CASE("determinism: the build's own flags are part of the contract") {
    DeterminismConfiguration configuration(allocator());
    CY_REQUIRE(configuration.declare({"physics", DeterminismProfile::Lockstep, true}).has_value());

    // M9's spike, as a refusal. Contraction at the compiler's default on a target that HAS a fused
    // multiply-add: 13 of 16 workloads moved, and 231 of 267 values in the state_hash workload.
    BuildConfiguration contracted = clean_build();
    contracted.contraction_off = false;
    const auto refused = configuration.require(DeterminismProfile::SamePlatform, contracted);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK(refused.error().tag == ProfileRefusal::FloatContraction);

    // ...and the same flags on a target with NO fused multiply-add are accepted, because there is
    // nothing for the default setting to fuse. That is why the engine agrees with itself today, and
    // it is a fact about the target rather than about the source.
    BuildConfiguration baseline = contracted;
    baseline.target_has_fma = false;
    CY_CHECK(configuration.require(DeterminismProfile::SamePlatform, baseline).has_value());

    // Fast-math is refused before any subsystem is blamed: it would fail for every subsystem in the
    // list, and naming the alphabetically first one would send the reader to the wrong file.
    BuildConfiguration fast = clean_build();
    fast.fast_math = true;
    const auto fast_refused = configuration.require(DeterminismProfile::ReplayStable, fast);
    CY_REQUIRE_FALSE(fast_refused.has_value());
    CY_CHECK(fast_refused.error().tag == ProfileRefusal::FastMath);
    CY_CHECK(std::strcmp(fast_refused.error().subsystem, "") == 0);
}

CY_TEST_CASE("determinism: CrossPlatform is refused because this tree has no deterministic math") {
    // THE HONEST ANSWER, AND IT IS A TEST RATHER THAN A COMMENT. `simulation-and-determinism`:
    // "The engine SHALL NOT claim that arbitrary floating-point code produces identical results
    // across architectures, compilers, or vector widths", and `CrossPlatform` "SHALL require
    // deterministic math types ... provided as an optional module". There is no such module here,
    // so every CrossPlatform and Lockstep session is refused, and this case is what stops that
    // becoming a silent pass the day someone adds a flag.
    DeterminismConfiguration configuration(allocator());
    CY_REQUIRE(configuration.declare({"physics", DeterminismProfile::Lockstep, true}).has_value());

    BuildConfiguration without = clean_build();
    without.deterministic_math_available = false;
    for (const DeterminismProfile profile :
         {DeterminismProfile::CrossPlatform, DeterminismProfile::Lockstep}) {
        const auto refused = configuration.require(profile, without);
        CY_REQUIRE_FALSE(refused.has_value());
        CY_CHECK(refused.error().tag == ProfileRefusal::DeterministicMathMissing);
    }
    // SamePlatform is unaffected: it is a claim about one binary on one machine.
    CY_CHECK(configuration.require(DeterminismProfile::SamePlatform, without).has_value());
}

CY_TEST_CASE("determinism: a presentation-only subsystem constrains nothing") {
    DeterminismConfiguration configuration(allocator());
    CY_REQUIRE(configuration.declare({"physics", DeterminismProfile::Lockstep, true}).has_value());
    // A renderer guarantees nothing and takes no part in authoritative simulation. Refusing a
    // lockstep session because the renderer is not deterministic would be refusing every session.
    CY_REQUIRE(configuration.declare({"renderer", DeterminismProfile::None, false}).has_value());

    const auto accepted = configuration.require(DeterminismProfile::Lockstep, clean_build());
    CY_REQUIRE(accepted.has_value());
    CY_CHECK_EQ(accepted->subsystems_examined, 2U);
    CY_CHECK_EQ(accepted->authoritative_subsystems, 1U);
}

CY_TEST_CASE("determinism: declarations are refused after the configuration is finalised") {
    DeterminismConfiguration configuration(allocator());
    CY_REQUIRE(configuration.declare({"physics", DeterminismProfile::Lockstep, true}).has_value());
    CY_CHECK_FALSE(configuration.declare({"physics", DeterminismProfile::None, true}).has_value());
    configuration.finalize();
    CY_CHECK(configuration.finalized());
    CY_CHECK_FALSE(configuration.declare({"audio", DeterminismProfile::None, false}).has_value());
    CY_CHECK_FALSE(configuration.declare({"", DeterminismProfile::None, true}).has_value());
}

CY_TEST_CASE("determinism: from_build() reports what this translation unit was compiled with") {
    // THE SEAM BETWEEN THE TWO HALVES OF TASK 2.2. This suite's target carries
    // CY_DETERMINISM_CONTRACTION_OFF because `cy_declare_determinism_profile()` set it after
    // checking the target's compile options. Deleting `-ffp-contract=off` from
    // src/core/determinism/tests/CMakeLists.txt fails the CONFIGURE; deleting the define without
    // the flag would turn this case red. Neither can happen quietly.
    const BuildConfiguration build = BuildConfiguration::from_build();
    CY_CHECK(build.contraction_off);
    CY_CHECK_FALSE(build.fast_math);
    // `target_has_fma` is a fact about the target and is NOT asserted either way: the engine builds
    // for baseline x86-64 today, where it is false, and `-march=x86-64-v3` — which
    // src/core/math/tests/CMakeLists.txt already anticipates — makes it true. Asserting it would
    // make this case a test of the baseline rather than of the mechanism.
    CY_CHECK_FALSE(build.deterministic_math_available);
}

CY_TEST_CASE("determinism: a non-finite write to an authoritative field is detected and named") {
    NonFiniteGuard guard;
    CY_CHECK_EQ(guard.check(1.5F, "velocity.x"), 1.5F);
    CY_CHECK_EQ(guard.non_finite_writes(), 0U);

    const f32 zero = 0.0F;
    CY_CHECK(std::isnan(guard.check(zero / zero, "velocity.y")));
    CY_CHECK(std::isinf(guard.check(std::numeric_limits<f64>::infinity(), "mass")));
    CY_CHECK_EQ(guard.writes_checked(), 3U);
    CY_CHECK_EQ(guard.non_finite_writes(), 2U);
    // The FIRST field, because that is what a bisect wants; the last is whatever happened to be on
    // the stack when someone looked.
    CY_CHECK(std::strcmp(guard.first_field(), "velocity.y") == 0);

    guard.clear();
    CY_CHECK_EQ(guard.non_finite_writes(), 0U);
    CY_CHECK(std::strcmp(guard.first_field(), "") == 0);
}
