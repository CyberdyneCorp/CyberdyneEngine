#pragma once
// Physics debug visualisation, as a sink the server writes primitives into. Task 4.2.4.
//
// `physics` — "Physics debugging": debug visualisation of "collider shapes, contact points and
// normals, constraint anchors and limits, body sleep state, velocities, centres of mass,
// broad-phase bounds, and query shapes and results".
//
// A SINK, NOT A BUFFER, AND NOT `cy::render::DebugDraw`. The render server is layer 2 alongside
// this one, so `cy_physics` could name its debug-draw store — and must not. A server that pushed
// into another server would make physics undrawable without a renderer, which is what a headless
// determinism test is. So physics emits into an interface the caller implements, and the caller at
// layer 4 is the one that happens to forward into the render server's debug store.
//
// `physics` — "Colliders do not match visuals": "shapes SHALL be drawn at their simulated
// transforms so mismatches with the visual mesh are immediately visible". That is why every method
// here takes a world-space transform rather than a pre-transformed vertex list: the sink draws what
// the solver has, not what the caller thought it had.

#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/transform.h>
#include <cy/servers/physics/handles.h>

namespace cy::physics {

/// Which families of primitive a `debug_draw` call should emit. A bit set, because a session that
/// wants contact normals almost never wants every collider outline at the same time.
enum class DebugDrawFlags : u32 {
    None = 0,
    Colliders = 1U << 0U,
    Contacts = 1U << 1U,
    Constraints = 1U << 2U,
    SleepState = 1U << 3U,
    Velocities = 1U << 4U,
    CentersOfMass = 1U << 5U,
    BroadPhaseBounds = 1U << 6U,
    All = 0x7FU,
};

[[nodiscard]] constexpr DebugDrawFlags operator|(DebugDrawFlags a, DebugDrawFlags b) noexcept {
    return static_cast<DebugDrawFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}

[[nodiscard]] constexpr bool has_flag(DebugDrawFlags set, DebugDrawFlags one) noexcept {
    return (static_cast<u32>(set) & static_cast<u32>(one)) != 0U;
}

/// The colours physics uses, as a small enumeration rather than an RGBA. A sink maps them to its
/// own palette; the point is that "asleep" is one colour everywhere rather than a literal repeated
/// at nine call sites.
enum class DebugColor : u8 {
    Static = 0,
    Kinematic,
    DynamicAwake,
    DynamicAsleep,
    Trigger,
    Contact,
    Normal,
    Constraint,
    Velocity,
    Bounds,
};

/// What physics can draw. Implemented by the caller; every method has a default that discards, so a
/// sink that only wants contact points overrides one.
class DebugDrawSink {
public:
    DebugDrawSink() = default;
    virtual ~DebugDrawSink() = default;

    DebugDrawSink(const DebugDrawSink&) = delete;
    DebugDrawSink& operator=(const DebugDrawSink&) = delete;
    DebugDrawSink(DebugDrawSink&&) = delete;
    DebugDrawSink& operator=(DebugDrawSink&&) = delete;

    virtual void line(Vec3 from, Vec3 to, DebugColor color) noexcept {
        (void)from, (void)to, (void)color;
    }
    virtual void box(const Aabb& box, const Transform& transform, DebugColor color) noexcept;
    virtual void sphere(Vec3 center, f32 radius, DebugColor color) noexcept;
    virtual void capsule(const Transform& transform, f32 radius, f32 half_height,
                         DebugColor color) noexcept;
    /// A contact: a point, its normal, and how deep the overlap is.
    virtual void contact(Vec3 position, Vec3 normal, f32 penetration) noexcept;
};

}  // namespace cy::physics
