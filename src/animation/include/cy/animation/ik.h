#pragma once
// The constraint framework: one ordering model, declared reads and writes, conflict detection, and
// the solvers the animation graph's `IK` node reaches. M8.b task 5.3.
//
// ================================================================================================
// ONE FRAMEWORK, AND WHAT IT COSTS TO BE ONE
// ================================================================================================
//
// `animation-and-skinning` — "Constraint and IK framework": "IK solvers and constraints SHALL be
// unified in one constraint framework with a single ordering model, conflict detection, and
// debugging view", and "Every constraint, solver, and modifier SHALL declare the joints it reads
// and writes, so ordering conflicts are detectable, and SHALL support a blend weight so it can be
// faded in and out."
//
// So a constraint is a DECLARATION plus a solve, never a solve alone: `ConstraintDecl` carries the
// masks and `detect_conflicts()` reads them. Two constraints writing one joint at the same order
// are reported at setup — "rather than resolved arbitrarily" — and the report names both.
//
// WHAT IS DELIVERED AND WHAT IS NOT, STATED RATHER THAN IMPLIED. Two-bone IK, look-at and copy are
// here, each weighted and each declaring its joints. FABRIK, CCD, spline IK, FULL-BODY IK, spring
// bones and foot placement are named by the specification and are NOT in this milestone: full-body
// IK is a multi-effector pose solve rather than a chain, and foot placement needs a raycast this
// module may not issue. A caller asking for one gets `NotImplemented` naming it, not a two-bone
// solve wearing its name.

#include <cy/core/base/expected.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>

#include <cy/animation/skeleton.h>

namespace cy::animation {

enum class ConstraintKind : u8 {
    /// A two-bone chain reaching for a target, with a pole vector deciding the plane.
    TwoBoneIk = 0,
    /// A joint aiming its forward axis at a target.
    LookAt,
    /// A joint copying another's rotation.
    CopyRotation,
};

/// A two-bone chain and where its target comes from.
struct IkChain {
    u16 root = kInvalidJoint;
    u16 mid = kInvalidJoint;
    u16 tip = kInvalidJoint;
    /// The first of three consecutive program parameters carrying the target in model space.
    u16 target_param = 0;
    /// The program parameter carrying the blend weight.
    u16 weight_param = 0;
    /// The plane the chain bends in.
    Vec3 pole{0.0F, 0.0F, 1.0F};
    [[nodiscard]] bool valid() const noexcept {
        return root != kInvalidJoint && mid != kInvalidJoint && tip != kInvalidJoint;
    }
};

/// What a constraint reads, what it writes, and where it sits in the order.
struct ConstraintDecl {
    Name name;
    ConstraintKind kind = ConstraintKind::TwoBoneIk;
    JointMask reads;
    JointMask writes;
    /// The evaluation order. Two constraints with the same order that write the same joint are a
    /// conflict; an author separates them by giving one a later order.
    u16 order = 0;
    /// Faded in and out rather than switched.
    f32 weight = 1.0F;
};

/// A pair that would fight, named so an author can fix it.
struct ConstraintConflict {
    Name first;
    Name second;
    /// The lowest joint both write, so the report points at one joint rather than at a mask.
    u16 joint = kInvalidJoint;
};

struct ConflictReport {
    u32 examined = 0;
    u32 conflicts = 0;
};

/// Report every pair of constraints that writes the same joint at the same order.
///
/// "WHEN two constraints write the same joint with no declared ordering THEN the conflict SHALL be
/// reported at setup rather than resolved arbitrarily."
[[nodiscard]] Status detect_conflicts(Span<const ConstraintDecl> constraints,
                                      Array<ConstraintConflict>& conflicts,
                                      ConflictReport& report) noexcept;

/// Solve a two-bone chain so the tip reaches `target`, in the skeleton's model space, writing the
/// result back into `local`.
///
/// `weight` fades the solve: zero leaves `local` untouched, one applies it fully, and a value
/// between interpolates the two joints it writes. Returns whether the target was reachable — an
/// unreachable target straightens the chain toward it rather than failing.
[[nodiscard]] Expected<bool, Error> solve_two_bone(const Skeleton& skeleton, const IkChain& chain,
                                                   Vec3 target, f32 weight,
                                                   Span<Transform> local) noexcept;

/// Rotate `joint` so its forward axis points at `target`, in model space, weighted.
[[nodiscard]] Status solve_look_at(const Skeleton& skeleton, u16 joint, Vec3 target, f32 weight,
                                   Span<Transform> local) noexcept;

/// The solvers this milestone does not implement, refused by name rather than approximated.
enum class UnimplementedSolver : u8 {
    Fabrik = 0,
    Ccd,
    SplineIk,
    FullBodyIk,
    SpringBones,
    FootPlacement,
};

[[nodiscard]] Status solve_unimplemented(UnimplementedSolver solver) noexcept;

}  // namespace cy::animation
