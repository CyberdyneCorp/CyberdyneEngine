#pragma once
// Determinism profiles, and the refusal that happens at configuration. M9 tasks 2.1 and 2.2.
//
// `simulation-and-determinism` — "Determinism profiles": a session declares a profile and the
// engine "SHALL enforce and verify only what that profile requires", and the profile "SHALL be
// validated at configuration time against the subsystems in use: a session declaring
// `CrossPlatform` while using a subsystem that guarantees only same-platform determinism SHALL be
// rejected with a diagnostic naming the subsystem."
//
// ================================================================================================
// A PROFILE IS A BUILD-CONFIGURATION CONTRACT, NOT ONLY A SOURCE CONTRACT
// ================================================================================================
//
// This is M9's spike finding and it is the reason this header has a `BuildConfiguration` in it at
// all. `design.md` §1.2, measured rather than argued:
//
//   * The engine's own primitives agree bit-for-bit between clang 18 and GCC 13 at every one of the
//     four build profiles' optimisation levels — in 18 of the spike's 20 configurations.
//   * The two that disagree are `-march=native` with floating-point contraction left at the
//     compiler's default. 13 of 16 workloads move, the two compilers disagree with *each other* in
//     8 of them, and 231 of 267 values move in `state_hash` — which is the number a lockstep
//     session compares.
//   * They agree today only because the engine targets baseline x86-64, which has no FMA, so the
//     default contraction setting has nothing to fuse. `src/core/math/tests/CMakeLists.txt` already
//     anticipates `-march=x86-64-v3` in as many words.
//
// So two builds of **identical source**, differing only in `-march` and a contraction flag, produce
// different state hashes. A refusal that looked only at which subsystems were linked would pass
// both of them. `require()` therefore takes a `BuildConfiguration`, and the refusal names the flag
// as readily as it names a subsystem.
//
// The build half is checked twice, at two different moments, and both are needed:
//
//   CMake configure time   `cy_declare_determinism_profile()` (cmake/determinism_profile.cmake)
//                          refuses to configure a module that declares a profile without
//                          `-ffp-contract=off` on its compile options. That is the earliest
//                          possible moment and it is the one the exit criterion asks for.
//   Session configuration  `require()` below, from `BuildConfiguration::from_build()`, so a module
//                          compiled by something other than that CMake function — a downstream
//                          project, a hand-written translation unit — is still refused before a
//                          tick runs rather than after a desync.
//
// ================================================================================================
// WHAT THIS HEADER DELIBERATELY DOES NOT CLAIM
// ================================================================================================
//
// `CrossPlatform` and `Lockstep` require deterministic math types, and this machine has **one
// architecture and one operating system**. Nothing in this tree has compared a state hash between
// two architectures, so nothing here reports that it holds. A subsystem may *declare* that it meets
// `CrossPlatform`; `require()` believes the declaration and checks the build flags around it, and
// that is the whole of what it does. The measurement is CI's, and `docs/roadmap/status.yaml` may
// not record the claim until a run has compared two architectures — `design.md` §1.4.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

namespace cy::determinism {

/// The five profiles, weakest first. `simulation-and-determinism`'s table, verbatim in meaning.
enum class DeterminismProfile : u8 {
    /// No reproducibility guarantee.
    None = 0,
    /// Enough authoritative information is recorded that the session can be reconstructed.
    ReplayStable,
    /// The same binary, architecture and inputs reproduce identical authoritative state.
    SamePlatform,
    /// Different platforms converge to identical authoritative state.
    CrossPlatform,
    /// Peers reproduce identical state from commands alone.
    Lockstep,
};

inline constexpr u32 kDeterminismProfileCount = 5;

/// The enumerator's own spelling, for a diagnostic. Never null.
const char* determinism_profile_name(DeterminismProfile profile) noexcept;

/// What a profile obliges, as separate facts rather than as a rank.
///
/// A rank alone would make `Lockstep` "`CrossPlatform` and a bit more", which is how the two get
/// conflated: lockstep's distinguishing obligation is that state is reconstructible **from commands
/// alone** — no state replication at all — and that is orthogonal to how the arithmetic is done.
/// Keeping the obligations separate is also what lets the refusal name the *guarantee* rather than
/// only the profile, which is what the requirement asks for.
struct Guarantees {
    /// Commands are recorded. Everything above `None`.
    bool records_commands = false;
    /// External results are recorded, so replay consumes rather than re-invokes.
    bool records_external_results = false;
    /// The same binary on the same architecture reproduces authoritative state bit-exactly.
    bool same_binary_reproducible = false;
    /// Different architectures converge. Requires deterministic math types.
    bool cross_platform_reproducible = false;
    /// State is reconstructible from commands alone.
    bool commands_alone = false;

    friend constexpr bool operator==(const Guarantees&, const Guarantees&) noexcept = default;
};

[[nodiscard]] constexpr Guarantees guarantees_of(DeterminismProfile profile) noexcept {
    switch (profile) {
        case DeterminismProfile::None:
            return {};
        case DeterminismProfile::ReplayStable:
            return {true, true, false, false, false};
        case DeterminismProfile::SamePlatform:
            return {true, true, true, false, false};
        case DeterminismProfile::CrossPlatform:
            return {true, true, true, true, false};
        case DeterminismProfile::Lockstep:
            return {true, true, true, true, true};
    }
    return {};
}

/// May a subsystem guaranteeing `provided` be used in a session requiring `required`?
///
/// Every obligation the session carries must be met by the subsystem. Written as an implication per
/// field rather than as `provided >= required` so that a future profile which is not a superset of
/// the one below it does not silently become one.
[[nodiscard]] constexpr bool satisfies(DeterminismProfile provided,
                                       DeterminismProfile required) noexcept {
    const Guarantees have = guarantees_of(provided);
    const Guarantees want = guarantees_of(required);
    return (!want.records_commands || have.records_commands) &&
           (!want.records_external_results || have.records_external_results) &&
           (!want.same_binary_reproducible || have.same_binary_reproducible) &&
           (!want.cross_platform_reproducible || have.cross_platform_reproducible) &&
           (!want.commands_alone || have.commands_alone);
}

/// The name of the first obligation `required` carries that `provided` does not. Null when there is
/// none — so a caller can write the diagnostic without restating the table.
[[nodiscard]] const char* unmet_guarantee(DeterminismProfile provided,
                                          DeterminismProfile required) noexcept;

// --- The build half -----------------------------------------------------------------------------

/// The floating-point settings the translation unit asking the question was compiled with.
///
/// Not a global: each field is read from a macro that `cy_declare_determinism_profile()` sets
/// **PRIVATE** on the target it covers, so a module that never declared a profile reports the
/// truth — that nothing checked its flags — rather than inheriting a neighbour's answer.
struct BuildConfiguration {
    /// `-ffp-contract=off` was on the compile line. The spike's load-bearing flag.
    bool contraction_off = false;
    /// `-ffast-math` or an equivalent. Detected from `__FAST_MATH__`, which both compilers define.
    bool fast_math = false;
    /// The target has a fused multiply-add the compiler may contract into. `__FP_FAST_FMA` on both
    /// compilers. False on baseline x86-64, which is why the engine agrees with itself today.
    bool target_has_fma = false;
    /// Deterministic math types are available to authoritative code. There is no such module in
    /// this tree yet; `cross_platform_reproducible` may not be claimed without one.
    bool deterministic_math_available = false;

    /// What *this* translation unit was compiled with.
    [[nodiscard]] static constexpr BuildConfiguration from_build() noexcept {
        BuildConfiguration configuration;
#if defined(CY_DETERMINISM_CONTRACTION_OFF) && CY_DETERMINISM_CONTRACTION_OFF
        configuration.contraction_off = true;
#endif
#if defined(__FAST_MATH__)
        configuration.fast_math = true;
#endif
#if defined(__FP_FAST_FMA)
        configuration.target_has_fma = true;
#endif
#if defined(CY_DETERMINISM_MATH) && CY_DETERMINISM_MATH
        configuration.deterministic_math_available = true;
#endif
        return configuration;
    }
};

// --- The refusal --------------------------------------------------------------------------------

/// Why a session was refused. Structured, because "the profile could not be met" is unactionable
/// and each of these has a different fix.
enum class ProfileRefusal : u8 {
    None = 0,
    /// A subsystem in use guarantees less than the session requires.
    SubsystemGuarantee,
    /// Floating-point contraction was left at the compiler's default on a target that has an FMA.
    /// The spike's two disagreeing configurations, as a refusal.
    FloatContraction,
    /// Fast-math transformations are enabled. `simulation-and-determinism` disallows them on
    /// authoritative paths under every deterministic profile.
    FastMath,
    /// The profile requires deterministic math types and the build has none.
    DeterministicMathMissing,
    /// No subsystem declared itself. A session that required a profile of an empty registry would
    /// otherwise be accepted, which is a check that cannot fail.
    NoSubsystemsDeclared,
    Count,
};

const char* profile_refusal_name(ProfileRefusal refusal) noexcept;

/// One refusal, with everything the diagnostic needs and nothing it has to look up.
struct ProfileRejection {
    ProfileRefusal tag = ProfileRefusal::None;
    /// The subsystem named by the diagnostic, or "" for a refusal about the build rather than about
    /// a subsystem.
    const char* subsystem = "";
    /// The obligation that was not met, spelled — "cross-platform reproducibility",
    /// "-ffp-contract=off". Never null.
    const char* guarantee = "";
    DeterminismProfile required = DeterminismProfile::None;
    DeterminismProfile provided = DeterminismProfile::None;
};

/// A subsystem's own declaration of what it can hold to.
struct SubsystemDeterminism {
    /// A literal, or storage outliving the configuration. Ordered by, so it must be stable.
    const char* name = "";
    DeterminismProfile guarantees = DeterminismProfile::None;
    /// False for a subsystem that takes no part in authoritative simulation — a renderer, an audio
    /// mixer. It constrains nothing, and it is counted so a report can say how much was waved past.
    bool authoritative = true;
};

/// What one configuration run examined. Reported whether or not anything was refused, because a
/// check that looked at nothing and a check that found nothing read identically otherwise — the
/// defect class this project has paid for in four separate gates.
struct ConfigurationReport {
    DeterminismProfile profile = DeterminismProfile::None;
    u32 subsystems_examined = 0;
    /// Those taking part in authoritative simulation. Only these constrain the profile.
    u32 authoritative_subsystems = 0;
    /// Those that would have refused a profile one step stronger. The margin, as a number.
    u32 at_the_limit = 0;
};

/// The subsystems in use, and the one place a session's profile is accepted or refused.
///
/// `finalize()` sorts by name before anything reads the list, so the subsystem a refusal names is
/// the same one on every run whatever order the declarations arrived in —
/// `simulation-and-determinism`'s "Registration and initialisation order".
class DeterminismConfiguration {
public:
    explicit DeterminismConfiguration(Allocator& allocator) noexcept : subsystems_(allocator) {}

    DeterminismConfiguration(const DeterminismConfiguration&) = delete;
    DeterminismConfiguration& operator=(const DeterminismConfiguration&) = delete;

    /// Refuses a duplicate name and a declaration made after `finalize()`.
    [[nodiscard]] Status declare(const SubsystemDeterminism& subsystem) noexcept;

    void finalize() noexcept;
    [[nodiscard]] bool finalized() const noexcept { return finalized_; }

    /// **The refusal.** Called when a session is configured, before a tick runs.
    ///
    /// Returns the report on acceptance and the rejection on refusal, so a caller cannot reach the
    /// report without having handled the refusal — which a `Status` plus an out-parameter would
    /// have let it do.
    ///
    /// Finalises the registry if it has not been, because a configuration validated against an
    /// unsorted list would name a different subsystem on a different run.
    [[nodiscard]] Expected<ConfigurationReport, ProfileRejection> require(
        DeterminismProfile profile, const BuildConfiguration& build) noexcept;

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(subsystems_.size()); }
    [[nodiscard]] const SubsystemDeterminism& at(u32 index) const noexcept {
        return subsystems_[index];
    }

    /// The strongest profile every declared authoritative subsystem could meet. What a tool offers
    /// as the answer to "then what can I have?".
    [[nodiscard]] DeterminismProfile strongest_supported() const noexcept;

private:
    Array<SubsystemDeterminism> subsystems_;
    bool finalized_ = false;
};

// --- Non-finite detection -----------------------------------------------------------------------

/// `simulation-and-determinism`: "Development builds under deterministic profiles SHALL detect
/// writes of non-finite values to authoritative fields and report them, since a propagated
/// non-finite value destroys reproducibility."
///
/// A counter rather than an abort: a non-finite value is a defect, and the run that produced it is
/// the run whose divergence window is worth keeping. Aborting throws that away.
class NonFiniteGuard {
public:
    /// Returns the value unchanged, counting it when it is not finite. `field` is a literal naming
    /// what was written, kept so the report says which field rather than how many.
    [[nodiscard]] f32 check(f32 value, const char* field) noexcept;
    [[nodiscard]] f64 check(f64 value, const char* field) noexcept;

    [[nodiscard]] u32 writes_checked() const noexcept { return checked_; }
    [[nodiscard]] u32 non_finite_writes() const noexcept { return non_finite_; }
    /// The first field that was written non-finite, or "" for none. The first is what a bisect
    /// wants; the last is whatever happened to be on the stack.
    [[nodiscard]] const char* first_field() const noexcept { return first_field_; }

    void clear() noexcept;

private:
    u32 checked_ = 0;
    u32 non_finite_ = 0;
    const char* first_field_ = "";
};

}  // namespace cy::determinism
