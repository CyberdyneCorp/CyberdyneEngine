#pragma once
// Rivers: a spline network with junctions, and the flow that geometry — not the spline — decides.
// M10 task 2.3.
//
// `water` — "Rivers": "Rivers SHALL be authored as spline networks supporting junctions and
// branches, with each section declaring width, depth, bed profile, water level, flow speed,
// turbulence, and material. The system SHALL generate from the network: the surface, a flow field,
// shoreline data, foam sources, and buoyancy and collision data. Flow SHALL be modified by geometry
// — accelerating in narrows, deflecting around obstacles, forming turbulence at bends and drops —
// rather than being uniform along the spline."
//
// ================================================================================================
// THE FLOW IS CONTINUITY, NOT AN AUTHORED NUMBER
// ================================================================================================
//
// A section declares a flow speed at its head. What every point downstream of that gets is
// **discharge divided by cross-section**: Q = width x depth x speed is conserved along a section,
// so a river that narrows to half its width at the same depth runs twice as fast without anyone
// authoring the number, and a tributary joining a trunk RAISES the trunk's discharge below the
// junction rather than overlaying a second surface on it.
//
// That is the specification's "accelerating in narrows" and its "A tributary joins" scenario as one
// mechanism rather than two: both are Q/A, and the junction is where two Q's are added.
//
// ================================================================================================
// A JUNCTION IS A CONTINUITY CONSTRAINT, AND THE ADJUSTMENT IS REPORTED
// ================================================================================================
//
// "THEN the junction SHALL produce a continuous surface and a combined flow field, not two
// overlapping surfaces." Two independently authored splines will not agree about the water level
// where they meet — one is always a few centimetres above the other, and that is the step a player
// sees. `build()` therefore pulls the tributary's mouth to the trunk's level at the junction,
// blending the correction back up the tributary over its last stretch, and REPORTS the correction
// in metres. A silent adjustment would hide an authoring error the size of a metre; a refusal would
// make a two-centimetre float difference an authoring failure.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/world/coordinates.h>

namespace cy::water {

/// A spline control point of a river's centreline, with the section parameters at that point.
/// `water`: "each section declaring width, depth, bed profile, water level, flow speed, turbulence,
/// and material".
struct RiverControlPoint {
    /// The centreline position. `y` is the WATER LEVEL there, in absolute metres; the bed is
    /// `depth` below it under the deepest part of the channel.
    world::WorldVec3d position;
    f32 width = 8.0F;
    f32 depth = 1.5F;
    /// The speed at the section's head, m/s. Downstream of the head it is recomputed from
    /// continuity — see the header note — so this number is an input at the first point of a
    /// section and a starting condition everywhere else.
    f32 flow_speed = 1.5F;
    /// Baseline turbulence in [0, 1]. Bends and drops ADD to it; they do not replace it.
    f32 turbulence = 0.05F;
    /// How square the channel is, in [0, 1]: 0 is a parabolic bed (a natural channel), 1 is a flat
    /// bed with vertical banks (a canal). `water`'s "bed profile", as one number rather than a
    /// curve, because everything that consumes it wants a depth at a lateral offset.
    f32 bed_squareness = 0.0F;
    u64 material = 0;
};

/// The value of `RiverSectionDesc::joins_section` for a section that flows into open water — a sea,
/// a lake, or the edge of the world — rather than into another section.
inline constexpr u32 kNoSection = 0xFFFF'FFFFU;

/// One authored section of the network.
struct RiverSectionDesc {
    const char* name = "";
    /// The centreline's control points, head first. At least two.
    Span<const RiverControlPoint> points;
    /// The section this one flows into, or `kNoSection`.
    u32 joins_section = kNoSection;
    /// Where along the trunk it joins, as a fraction of the trunk's own length in [0, 1].
    f32 joins_at = 1.0F;
};

/// A rock, a pier, a wreck. `water`: flow is "deflecting around obstacles". Circular in plan and
/// vertical, because what the flow field needs from an obstacle is a footprint and a radius of
/// influence; a mesh here would make a flow field need a collision world.
struct RiverObstacle {
    world::WorldVec3d centre;
    /// The radius the obstacle occupies. Flow does not enter it.
    f32 radius = 1.0F;
    /// How far beyond the radius the deflection is felt.
    f32 influence = 3.0F;
};

/// One point of the generated centreline. What `build()` produces and what sampling walks.
struct RiverVertex {
    world::WorldVec3d position;
    /// Unit tangent, downstream, in the XZ plane.
    Vec3 tangent{1.0F, 0.0F, 0.0F};
    f32 width = 0.0F;
    f32 depth = 0.0F;
    /// Speed after continuity and the junction's added discharge.
    f32 speed = 0.0F;
    /// Turbulence after the bend and drop terms are added to the authored baseline.
    f32 turbulence = 0.0F;
    f32 bed_squareness = 0.0F;
    /// Signed curvature of the centreline here, 1/m. Its magnitude drives the bend's turbulence.
    f32 curvature = 0.0F;
    /// Downhill gradient of the water surface here, metres per metre. Drives the drop's turbulence
    /// and is what makes a rapid foam.
    f32 gradient = 0.0F;
    /// Distance from the section's head along the centreline, metres.
    f32 arc = 0.0F;
    u64 material = 0;
};

/// A built section: its vertices, its discharge and its place in the network.
struct RiverSection {
    const char* name = "";
    u32 first_vertex = 0;
    u32 vertex_count = 0;
    u32 joins_section = kNoSection;
    f32 joins_at = 1.0F;
    /// Volumetric discharge at the head, m^3/s. Q = width x depth x speed of the first point.
    f32 discharge = 0.0F;
    /// Discharge added by tributaries joining this section, m^3/s.
    f32 tributary_discharge = 0.0F;
    f32 length = 0.0F;
    /// Where this section meets the one it joins, as an arc length along THAT section. Derived from
    /// `joins_at` and the trunk's own length at build time, and kept because continuity needs to
    /// know which of the trunk's vertices are downstream of the confluence and which are not.
    f32 junction_arc = 0.0F;
};

/// One sample of the network at a position.
struct RiverSample {
    bool found = false;
    /// Whether the position is within the channel's own width.
    bool inside = false;
    u32 section = kNoSection;
    /// The water surface height at the nearest centreline point, absolute metres.
    f64 surface = 0.0;
    /// The bed height under the sampled position, absolute metres, from the bed profile.
    f64 bed = 0.0;
    /// Flow velocity, m/s, after continuity, geometry and obstacle deflection.
    Vec3 velocity{0.0F, 0.0F, 0.0F};
    /// Lateral distance from the centreline, metres.
    f32 distance_to_centre = 0.0F;
    /// Distance to the nearest bank, metres. Negative outside the channel, which is what makes it
    /// usable directly as a signed water-distance contribution.
    f32 distance_to_bank = 0.0F;
    f32 turbulence = 0.0F;
    f32 width = 0.0F;
    u64 material = 0;
};

/// Where foam is generated. `water` — "Foam": generated from "wave curvature and breaking,
/// shoreline interaction, RIVER TURBULENCE, WATERFALLS, and object interaction".
struct RiverFoamSource {
    world::WorldVec3d position;
    /// Generation rate in [0, 1] per second.
    f32 strength = 0.0F;
    /// The direction foam leaves the source in — the flow at that point.
    Vec3 drift{0.0F, 0.0F, 0.0F};
    u32 section = kNoSection;
};

/// What a build did, and what it had to correct.
struct RiverBuildReport {
    u32 sections = 0;
    u32 vertices = 0;
    u32 junctions = 0;
    /// The largest water-level correction a junction needed, in metres. See the header note: an
    /// authoring error the size of a metre should be visible in a report.
    f32 largest_junction_correction = 0.0F;
    /// The largest speed-up continuity produced within a section, as a ratio to its head speed.
    f32 largest_narrowing_ratio = 1.0F;
};

/// The network: authored sections, obstacles, and everything generated from them.
class RiverNetwork {
public:
    explicit RiverNetwork(Allocator& allocator) noexcept;

    RiverNetwork(const RiverNetwork&) = delete;
    RiverNetwork& operator=(const RiverNetwork&) = delete;

    /// Author one section. Returns its index, which is what another section's `joins_section`
    /// names. Refuses fewer than two points, a non-positive width or depth, and a junction naming
    /// a section that has not been added — a forward reference would make the network's shape
    /// depend on declaration order.
    [[nodiscard]] Expected<u32, Error> add_section(const RiverSectionDesc& desc) noexcept;

    [[nodiscard]] Status add_obstacle(const RiverObstacle& obstacle) noexcept;

    /// How finely the centreline is resampled, in metres. Smaller is smoother and costs linearly;
    /// the default is one vertex every two metres, which resolves a bend a boat can feel.
    [[nodiscard]] Status set_resample_metres(f32 metres) noexcept;

    /// Generate the centrelines, resolve the junctions, propagate discharge and compute the
    /// geometry-driven flow. Idempotent: building twice produces the same vertices.
    [[nodiscard]] Status build(RiverBuildReport& report) noexcept;

    [[nodiscard]] bool built() const noexcept { return built_; }
    [[nodiscard]] Span<const RiverSection> sections() const noexcept { return sections_.span(); }
    [[nodiscard]] Span<const RiverVertex> vertices() const noexcept { return vertices_.span(); }

    /// The network at a position. Never blocks, never allocates; the whole of the flow field, the
    /// surface and the bed as one answer.
    [[nodiscard]] RiverSample sample(const world::WorldVec3d& at) const noexcept;

    /// Foam sources from bends, drops and junctions. Recomputed from the built vertices, so a
    /// rebuild moves them rather than leaving a source where the river no longer is.
    [[nodiscard]] Status foam_sources(Array<RiverFoamSource>& out) const noexcept;

    /// The network's horizontal extent, in absolute metres, including the channel's half width. A
    /// streamer uses it to decide which cells a river touches.
    [[nodiscard]] Status bounds(f64& min_x, f64& min_z, f64& max_x, f64& max_z) const noexcept;

private:
    struct Authored {
        const char* name = "";
        Array<RiverControlPoint> points;
        u32 joins_section = kNoSection;
        f32 joins_at = 1.0F;

        explicit Authored(Allocator& allocator) noexcept : points(allocator) {}
    };

    /// The four steps of a build, named. A build written as one function was unreadable and each of
    /// these is a seam a reader already has a name for.
    [[nodiscard]] Status resample_sections(RiverBuildReport& report) noexcept;
    [[nodiscard]] Status resolve_junctions(RiverBuildReport& report) noexcept;
    void propagate_discharge() noexcept;
    void apply_continuity(RiverBuildReport& report) noexcept;

    /// The nearest point on one section to a position: which vertex pair, how far along, and the
    /// lateral distance. The whole of the projection, used by sampling and by the junction solve.
    struct Projection {
        u32 vertex = 0;
        f32 blend = 0.0F;
        f32 lateral = 0.0F;
        bool valid = false;
    };
    [[nodiscard]] Projection project(const RiverSection& section,
                                     const world::WorldVec3d& at) const noexcept;
    /// The interpolated vertex at a projection.
    [[nodiscard]] RiverVertex interpolate(const RiverSection& section,
                                          const Projection& projection) const noexcept;
    /// Flow after obstacles have deflected it.
    [[nodiscard]] Vec3 deflect(const world::WorldVec3d& at, Vec3 velocity) const noexcept;

    Allocator* allocator_;
    Array<Authored> authored_;
    Array<RiverObstacle> obstacles_;
    Array<RiverSection> sections_;
    Array<RiverVertex> vertices_;
    f32 resample_metres_ = 2.0F;
    bool built_ = false;
};

}  // namespace cy::water
