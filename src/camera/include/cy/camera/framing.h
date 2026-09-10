#ifndef CY_CAMERA_FRAMING_H
#define CY_CAMERA_FRAMING_H
// Framing, composition and the constraints a rail camera is made of. M8.b task 7.3.
//
// --- WHAT THIS MODULE IS, AND WHY IT IS NOT src/servers/camera/ ----------------------------------
//
// `camera-system` reached **Seed** at M4 in `src/servers/camera/`, at layer 2, and that module's
// README lists exactly what it left: "Absent, not stubbed: framing and composition constraints,
// camera volumes, the strategy camera, the director camera, aim assistance and screen/world
// projection. All are `camera-system`'s and all are M8's."
//
// This module is those six, and it is at layer 4 rather than 2 because every one of them needs
// something layer 2 cannot name:
//
//   framing            a target resolved from the world, and cell-relative positions
//   volumes            the world's spatial index
//   the strategy camera  terrain height, which is `terrain`'s query and not a physics cast
//   the director       world-space candidate viewpoints
//   aim assistance     gameplay spatial queries
//   screen projection  `cy::render::ViewDescription`, which the server produces but does not
//   consume
//
// It EXTENDS `namespace cy::camera` rather than opening a second one, and it reuses that module's
// `TargetBinding`, `TargetSample`, `Lens` and `EvaluatedCamera` rather than declaring its own. Two
// definitions of a camera's target would be the conflation the specification's first requirement
// spends a page forbidding.
//
// --- THE TARGET IS STILL NOT THE CONTROLLED ENTITY -----------------------------------------------
//
// A binding names what is framed; a `TargetResolver` — implemented by whoever owns the world —
// turns it into a sample once per frame. Nothing here reads a component, and no type here has a
// pointer field, so "rig nodes SHALL NOT retain raw pointers to component data across frames" is
// unrepresentable rather than merely unwritten.
//
// --- ONE FRAMING IMPLEMENTATION, FIVE CASES ------------------------------------------------------
//
// "Framing SHALL be usable for strategy selections, boss encounters, dialogue, two-player fighting
// cameras, and editor previews, from one implementation." `solve_framing()` is that implementation.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/array.h>
#include <cy/servers/camera/camera.h>
#include <cy/world/coordinates.h>

namespace cy::camera {

/// How a binding becomes a value. Implemented by the host over its own entity storage.
class TargetResolver {
public:
    TargetResolver() = default;
    virtual ~TargetResolver() = default;
    TargetResolver(const TargetResolver&) = delete;
    TargetResolver& operator=(const TargetResolver&) = delete;
    TargetResolver(TargetResolver&&) = delete;
    TargetResolver& operator=(TargetResolver&&) = delete;

    /// Answer one binding. A binding the host cannot resolve — a streamed-out entity — leaves
    /// `valid` false, and the framing solver drops it for that frame rather than framing the
    /// origin.
    virtual void resolve(const TargetBinding& binding, TargetSample& out) noexcept = 0;
};

/// Resolve a set of bindings into samples, in binding order.
///
/// `Position` and `Bounds` bindings are answered from the binding itself and never reach the
/// resolver: a callback that can only repeat what its argument already says is a callback whose
/// absence is simpler than its presence.
[[nodiscard]] Status resolve_targets(Span<const TargetBinding> bindings, TargetResolver* resolver,
                                     Array<TargetSample>& out) noexcept;

/// A world position rebased into a camera's own cell, in f64 before any float sees it.
///
/// `camera-system`: "Camera position SHALL use the world's cell-relative representation ... so
/// cameras remain exact at planetary distances." The camera server works in one frame's floats,
/// which is correct and is only correct because this conversion happens above it.
[[nodiscard]] Vec3 rebase(const world::PartitionConfig& partition,
                          const world::WorldPosition& position, world::CellCoord frame) noexcept;

/// Screen-space composition, in view fractions where (0, 0) is the centre.
struct Composition {
    /// Where the framed subject sits. (0, -0.15) gives headroom by placing it below centre.
    Vec2 screen_offset;
    /// Movement inside this half-extent produces NO camera movement.
    Vec2 dead_zone{0.05F, 0.05F};
    /// Keep the horizon level: the solved orientation has zero roll.
    bool level_horizon = true;
    /// Padding around the framed bounds, as a fraction of the view.
    f32 padding = 0.1F;
};

/// What the framing solver was asked to satisfy.
struct FramingRequest {
    Span<const TargetBinding> bindings;
    Span<const TargetSample> samples;
    Composition composition;
    /// The lens the framing must fit inside. Its field of view decides the distance.
    Lens lens;
    f32 aspect = 16.0F / 9.0F;
    /// Where the camera is now, in the frame the samples are in. Framing keeps its direction rather
    /// than inventing one, and the dead zone is measured against it.
    Transform current;
    f32 minimum_distance = 1.0F;
    f32 maximum_distance = 200.0F;
};

/// What it produced.
struct FramingSolution {
    /// The point to look at. The weighted centroid of the resolved targets.
    Vec3 anchor;
    /// How far back the camera must sit for the whole set to be framed.
    f32 distance = 0.0F;
    /// The pose framing asks for. Roll is zero when `level_horizon` is set.
    Transform pose;
    /// True when the subject was inside the dead zone: `pose` is then the one that went in,
    /// unchanged, so a caller that ignores the flag still does not jitter.
    bool inside_dead_zone = false;
    /// How many bindings contributed. A sample the resolver could not answer is not one.
    u32 contributors = 0;
    /// True when the minimum screen sizes could not all be honoured because framing every target
    /// mattered more. Reported rather than silently resolved: a designer whose minimum was ignored
    /// should be able to see that it was.
    bool screen_size_yielded = false;
};

/// Solve framing for a weighted target set.
[[nodiscard]] Status solve_framing(const FramingRequest& request, FramingSolution& out) noexcept;

// --- Composition constraints
// -----------------------------------------------------------------------
//
// The camera server's `Constraint` rig node already clamps DISTANCE and a REGION, which is the
// bounded map and the arena. What is absent there is the rest of the list — "side-scrolling, rail,
// arena, and bounded map ... as compositions rather than as separate camera implementations" — so
// this vocabulary adds the plane, the orbit limits and the rail, and deliberately does not restate
// the two that already exist.

enum class ConstraintKind : u8 {
    /// Hold one axis fixed. A side-scroller holds Z.
    Plane = 0,
    /// Clamp yaw and pitch about the anchor.
    Orbit,
    /// Project the position onto a polyline. A rail camera — "a follow node constrained to a
    /// spline, not a separate camera type".
    Spline,
    Count,
};

struct Constraint {
    ConstraintKind kind = ConstraintKind::Plane;
    /// `Plane`: the unit normal and the offset along it.
    Vec3 plane_normal{0.0F, 0.0F, 1.0F};
    f32 plane_offset = 0.0F;
    /// `Orbit`: yaw limits in radians.
    Vec2 yaw_range{-3.15F, 3.15F};
    /// `Orbit`: pitch limits in radians.
    Vec2 pitch_range{-1.4F, 1.4F};
    /// `Spline`: the polyline. Not owned; it outlives the call and nothing keeps it.
    Span<const Vec3> spline;
};

/// Apply constraints in order to a desired position, about `anchor`.
[[nodiscard]] Vec3 apply_constraints(Span<const Constraint> constraints, Vec3 desired,
                                     Vec3 anchor) noexcept;

}  // namespace cy::camera

#endif  // CY_CAMERA_FRAMING_H
