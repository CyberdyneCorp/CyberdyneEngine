#ifndef CY_CAMERA_PROJECTION_H
#define CY_CAMERA_PROJECTION_H
// Screen and world projection. M8.b task 7.3.
//
// `camera-system`: "The camera SHALL expose projection utilities usable by gameplay, interface, and
// tools: screen point to world ray, world position to screen point with depth and a behind-camera
// indication, and a screen rectangle to a frustum for box selection. These SHALL be derived from
// the evaluated camera and view, and SHALL NOT require renderer internals. Results SHALL account
// for the viewport rectangle, so split-screen and editor viewports are correct without special
// cases."
//
// NO RENDERER INTERNALS, and the signatures are what enforce it: every function takes an
// `EvaluatedCamera` and a `cy::render::ViewDescription` — the semantic projection and the viewport
// — and none of them takes a matrix, a swapchain, a depth convention or a jitter offset. The view's
// own matrices are `cy::render::View::refresh()`'s, one layer down, and a caller that had to reach
// for one would be reaching past this interface.
//
// THE VIEWPORT IS SUBTRACTED IN ONE PLACE. A split-screen player's viewport starts at x = 960 and a
// utility that assumed the back buffer's origin is wrong for exactly one of the two players, which
// is the bug this requirement exists to prevent. Every function here goes through the same
// normalisation, and the suite drives the right-hand half of a window to prove it.

#include <cy/core/base/types.h>
#include <cy/core/math/shapes.h>
#include <cy/core/math/vec.h>
#include <cy/servers/camera/camera.h>
#include <cy/servers/render/model.h>

namespace cy::camera {

/// Where a world point lands on screen.
struct ScreenPoint {
    /// In the view's own pixels, in the same space `screen_point_to_ray` takes.
    Vec2 position;
    /// Distance along the view direction. Negative when the point is behind the camera.
    f32 depth = 0.0F;
    /// A projected point behind the camera lands on screen MIRRORED, and a caller that draws a
    /// marker there draws it in the wrong half of the frame. The flag is the difference between a
    /// caller that knows and one that has a bug once a quarter.
    bool behind = false;
    /// Whether the point is inside the viewport rectangle.
    bool on_screen = false;
};

/// A screen point to a world ray.
[[nodiscard]] Ray screen_point_to_ray(const EvaluatedCamera& camera,
                                      const render::ViewDescription& view, Vec2 screen) noexcept;

/// A world point to a screen point, with depth and the behind-camera indication.
[[nodiscard]] ScreenPoint world_to_screen(const EvaluatedCamera& camera,
                                          const render::ViewDescription& view, Vec3 world) noexcept;

/// A dragged screen rectangle to a selection frustum. The corners may be given in any order.
[[nodiscard]] Frustum screen_rect_to_frustum(const EvaluatedCamera& camera,
                                             const render::ViewDescription& view, Vec2 corner_a,
                                             Vec2 corner_b) noexcept;

}  // namespace cy::camera

#endif  // CY_CAMERA_PROJECTION_H
