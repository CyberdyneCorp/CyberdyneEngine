#pragma once
// What navigation guarantees, declared from its worlds. openspec/changes/add-deterministic-math,
// design §9.1 and task 6.2.
//
// Each navigation world declares its arithmetic. A `Float` world runs the queries in query.h,
// flow_field.h and crowd.h, whose `f32` results are reproducible on one architecture only. A
// `Fixed` world runs them over the mesh converted at load (`cy::movement::FixedNavMesh`), in
// integer arithmetic, and reproduces across architectures. So navigation as a SUBSYSTEM declares
// `Lockstep` only when every authoritative world is `Fixed` and none takes a runtime rebuild as
// authoritative input (a Recast rebuild is float work on each peer), and `SamePlatform` otherwise —
// and the profile check then refuses a `CrossPlatform` or `Lockstep` session naming `navigation`.
//
// This header names no deterministic-math type: navigation does not link `cy::core-detmath`, so it
// does not define `CY_DETERMINISM_MATH` for its consumers.

#include <cy/core/base/types.h>
#include <cy/core/determinism/profile.h>

namespace cy::navigation {

/// The arithmetic a navigation world's queries use.
enum class NavArithmetic : u8 {
    /// `f32`: the M8.b queries. `SamePlatform`.
    Float = 0,
    /// `Fixed`, over a converted mesh. `Lockstep`.
    Fixed,
};

[[nodiscard]] const char* nav_arithmetic_name(NavArithmetic arithmetic) noexcept;

/// One navigation world, as the session declares it.
struct NavWorldDeclaration {
    /// A literal or storage outliving the configuration, for a diagnostic.
    const char* name = "";
    NavArithmetic arithmetic = NavArithmetic::Float;
    /// Whether the world's results feed authoritative simulation. A world that only steers
    /// presentation — ambient crowds — constrains nothing.
    bool authoritative = true;
    /// Whether the world takes runtime tile rebuilds as authoritative input. Refused for `Fixed`.
    bool runtime_rebuilds = false;
};

/// The name navigation declares itself under.
inline constexpr const char* kNavigationSubsystem = "navigation";

/// The first authoritative world that keeps navigation below `Lockstep`, or null when none does.
[[nodiscard]] const NavWorldDeclaration* first_float_world(
    Span<const NavWorldDeclaration> worlds) noexcept;

/// What navigation guarantees, given its worlds: `Lockstep` when every authoritative world is
/// `Fixed` with no runtime rebuilds, `SamePlatform` otherwise. Authoritative when any world is.
[[nodiscard]] determinism::SubsystemDeterminism navigation_determinism(
    Span<const NavWorldDeclaration> worlds) noexcept;

}  // namespace cy::navigation
