#ifndef CY_CAMERA_STRATEGY_H
#define CY_CAMERA_STRATEGY_H
// The strategy camera, as a composition. M8.b task 7.3.
//
// `camera-system`: "The engine SHALL provide a strategy camera as a first-class composition: pan,
// orbit, zoom, edge scrolling, terrain following, map bounds, and collision." Four of those seven
// are rig nodes the camera server already has; the three that are not are here, and each is here
// because of a sentence in the same requirement:
//
//   ZOOM IS ONE PARAMETER. "Zoom SHALL drive a single normalised parameter mapped through curves to
//   height, distance, tilt, and lens, so that zooming out raises and tilts the camera coherently
//   rather than through independent controls." `evaluate_zoom()` is the only function in this
//   module that produces a height, and it takes one number. There is nowhere to set the height
//   independently, which is the requirement made structural.
//
//   TERRAIN FOLLOWING IS A TERRAIN QUERY. "Terrain following SHALL query terrain height directly
//   rather than casting physics rays every frame", and "Per-frame physics ray casts where a direct
//   terrain or spatial query exists" is on the forbidden list. `TerrainHeightService` is a SEPARATE
//   interface from the camera server's collision batch, so a host cannot implement terrain
//   following with a cast by accident: the two do not share a call. It is batched for the same
//   reason the casts are.
//
//   EDGE SCROLLING IS AN ACTION, NOT MOUSE HANDLING. "Edge scrolling SHALL be derived from a
//   pointer position exposed as an action, and SHALL NOT be implemented in platform mouse
//   handling." `edge_scroll()` takes a pointer position in view pixels and the viewport, and
//   produces a pan intent. Nothing here knows what a mouse is.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/servers/camera/camera.h>
#include <cy/servers/render/model.h>

namespace cy::camera {

/// The four curves one zoom parameter drives.
struct ZoomCurve {
    f32 near_height = 8.0F;
    f32 far_height = 60.0F;
    f32 near_distance = 12.0F;
    f32 far_distance = 90.0F;
    /// Radians below the horizon. Zooming out tilts down.
    f32 near_tilt = 0.6F;
    f32 far_tilt = 1.1F;
    /// Vertical field of view, radians. Zooming out narrows it, which is what keeps the sense of
    /// scale as the camera climbs.
    f32 near_fov = 0.9F;
    f32 far_fov = 0.6F;
};

struct ZoomState {
    f32 height = 0.0F;
    f32 distance = 0.0F;
    f32 tilt_radians = 0.0F;
    f32 fov_radians = 0.0F;
};

/// The ONE mapping. Every strategy-camera quantity comes out of this call.
[[nodiscard]] ZoomState evaluate_zoom(const ZoomCurve& curve, f32 normalised_zoom) noexcept;

/// Ground height, batched. `terrain`'s query, not a physics cast.
class TerrainHeightService {
public:
    TerrainHeightService() = default;
    virtual ~TerrainHeightService() = default;
    TerrainHeightService(const TerrainHeightService&) = delete;
    TerrainHeightService& operator=(const TerrainHeightService&) = delete;
    TerrainHeightService(TerrainHeightService&&) = delete;
    TerrainHeightService& operator=(TerrainHeightService&&) = delete;

    /// Answer a batch of ground samples: `points` are (x, z), `heights` receives y.
    virtual void sample_heights(Span<const Vec2> points, Span<f32> heights) noexcept = 0;
};

/// Map bounds: what the anchor is confined to.
struct MapBounds {
    /// The rectangle, in world x and z. `Aabb` rather than a second rectangle type; the y extent is
    /// ignored, because a map bound is a footprint and a camera's height comes from the zoom curve.
    Aabb region = Aabb::from_min_max(Vec3{-1e6F, -1e6F, -1e6F}, Vec3{1e6F, 1e6F, 1e6F});
    /// An optional convex polygon, in world x and z. When it has three or more points it wins over
    /// the rectangle — "Map bounds SHALL constrain the camera anchor to a rectangle, polygon, or
    /// world region".
    Span<const Vec2> polygon;
};

/// One strategy camera's state, advanced per frame.
struct StrategyState {
    /// The anchor on the ground the camera orbits and pans over.
    Vec3 anchor{0.0F, 0.0F, 0.0F};
    /// The normalised zoom, in [0, 1].
    f32 zoom = 0.3F;
    /// Yaw about the anchor, radians.
    f32 yaw = 0.0F;
    /// Set when the last `advance()` clamped the anchor against the map bounds, so an interface can
    /// show the player they have reached the edge rather than leaving them pushing at nothing.
    bool at_bounds = false;
};

/// What one frame asks of the strategy camera.
struct StrategyInput {
    /// The pan, in world units per second, in the camera's own ground plane. Produced by
    /// `edge_scroll()` or by a stick.
    Vec2 pan;
    /// The zoom delta this frame, in normalised units.
    f32 zoom_delta = 0.0F;
    /// The yaw delta, radians.
    f32 yaw_delta = 0.0F;
    f32 dt = 1.0F / 60.0F;
};

/// Advance one strategy camera and produce its pose.
///
/// `terrain` may be null, in which case the anchor keeps its own height — an editor preview over a
/// world with no terrain, and the case that would otherwise need a special path.
[[nodiscard]] Status advance_strategy(StrategyState& state, const StrategyInput& input,
                                      const ZoomCurve& curve, const MapBounds& bounds,
                                      TerrainHeightService* terrain, Transform& pose,
                                      Lens& lens) noexcept;

/// A pan request from a pointer near the edge of a viewport.
///
/// `dead_border` is the fraction of the viewport within which scrolling starts. The result is in
/// the range [-1, 1] per axis and is scaled by the caller's own speed, so a project's feel is a
/// multiplication rather than a fork of this function.
[[nodiscard]] Vec2 edge_scroll(const render::ViewportRect& viewport, Vec2 pointer,
                               f32 dead_border = 0.05F) noexcept;

}  // namespace cy::camera

#endif  // CY_CAMERA_STRATEGY_H
