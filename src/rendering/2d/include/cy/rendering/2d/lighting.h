#ifndef CY_RENDERING_2D_LIGHTING_H
#define CY_RENDERING_2D_LIGHTING_H
// 2D lights, occluders and their shadows, the screen-space signed distance field, and `Camera2D`.
// M8.b task 9.5.
//
// `rendering-2d` asks for point and directional 2D lights with cookies, energy, range and falloff;
// normal-map response; per-light layer masks; blend modes; per-layer opting out of lighting;
// polygon and polyline occluders with a culling mode; shadows rendered per light with filtering;
// and a screen-space signed distance field the VFX system can sample.
//
// --- WHAT IS HERE, AND WHAT IS THE PASS'S
// ---------------------------------------------------------
//
// The geometry and the decisions: which lights touch a layer, which occluders a light must
// consider, the shadow volume an occluder casts away from a light, and the distance field an
// occluder set rasterises to. What is NOT here is the shading itself — that is a material and a
// pass, and a module that named a device could not have this file's tests.
//
// --- AN UNLIT LAYER COSTS NOTHING, AND THAT IS MEASURED ------------------------------------------
//
// "WHEN a layer is marked unlit THEN 2D lights SHALL not affect it and no lighting cost SHALL be
// incurred for it." `gather_lights` skips an unlit layer before it examines a single light, and
// `LightingReport::lights_considered` is what a test reads to see that it did.
//
// --- THE ONE-SIDED OCCLUDER IS THE SUBTLE ONE ----------------------------------------------------
//
// "WHEN an occluder uses one-sided culling THEN light SHALL pass from one side and be blocked from
// the other, letting a light inside a room escape through the wall's inner face." So an edge casts
// a shadow only when the light is on its front side, and which side that is comes from the winding
// — which is why `OccluderCulling` names a winding rather than a boolean.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/rendering/2d/sprite.h>

namespace cy::rendering2d {

/// How a light contributes.
enum class LightBlend : u8 { Add = 0, Subtract, Multiply };

/// What a light is.
enum class Light2DKind : u8 {
    /// Radiates from a point, attenuated by distance.
    Point = 0,
    /// Comes from a direction, unattenuated. A sun.
    Directional = 1,
};

struct Light2D {
    Name name;
    Light2DKind kind = Light2DKind::Point;
    Vec2 position;
    /// `Directional`: the direction light travels. Ignored for a point light.
    Vec2 direction{0.0F, 1.0F};
    f32 range = 200.0F;
    /// Multiplies the cookie and the colour. A light at zero energy is off and is skipped.
    f32 energy = 1.0F;
    /// The exponent of the distance falloff. One is linear, two is inverse-square.
    f32 falloff = 1.0F;
    u32 colour = 0xFFFFFFFFU;
    LightBlend blend = LightBlend::Add;
    /// Which layers this light reaches. A bit per layer index, so a light can be confined to the
    /// gameplay layers without touching the interface.
    u32 layer_mask = 0xFFFFFFFFU;
    /// A texture that shapes the light. Carried, sampled by the pass.
    u16 cookie = 0;
    /// Whether this light casts shadows at all. A fill light usually does not, and that is the
    /// cheapest possible saving.
    bool casts_shadows = true;
    /// Shadow softness in world units, for the PCF filter.
    f32 softness = 0.0F;
};

/// Which side of an occluder blocks light.
enum class OccluderCulling : u8 {
    /// Both faces block. A pillar.
    Both = 0,
    /// Only the front face, with the polygon wound clockwise.
    Clockwise = 1,
    /// Only the front face, with the polygon wound counter-clockwise.
    CounterClockwise = 2,
};

/// A polygon or polyline that blocks light.
struct Occluder2D {
    /// The points, in world units. Not owned.
    Span<const Vec2> points;
    /// A polyline is open; a polygon is closed and its last point joins its first.
    bool closed = true;
    OccluderCulling culling = OccluderCulling::Both;
    /// Which layers this occluder blocks.
    u32 layer_mask = 0xFFFFFFFFU;
};

/// One quad of a shadow volume: an occluder edge extruded away from the light.
struct ShadowQuad {
    Vec2 near_a;
    Vec2 near_b;
    Vec2 far_a;
    Vec2 far_b;
};

struct LightingReport {
    u32 layers = 0;
    u32 lights_considered = 0;
    u32 lights_active = 0;
    u32 occluders_considered = 0;
    u32 shadow_quads = 0;
    /// Layers skipped because they are unlit. The measurement behind "no lighting cost SHALL be
    /// incurred for it".
    u32 unlit_layers = 0;
};

/// The lights that affect a layer, appended to `out`.
///
/// An unlit layer is skipped before any light is examined, which is why `report` counts what was
/// CONSIDERED rather than only what was returned.
[[nodiscard]] Status gather_lights(Span<const Light2D> lights, const Layer2D& layer,
                                   Array<u32>& out, LightingReport& report) noexcept;

/// The shadow volume an occluder casts away from a light, appended to `out`.
///
/// `extrusion` is how far the volume extends — far enough to leave the view, and a number rather
/// than infinity because a finite quad is what a rasteriser wants.
[[nodiscard]] Status build_shadow(const Light2D& light, const Occluder2D& occluder, f32 extrusion,
                                  Array<ShadowQuad>& out, LightingReport& report) noexcept;

/// Whether an edge faces a light, under a culling mode. Exposed because it is the whole content of
/// the one-sided-occluder scenario and a caller debugging a shadow wants to ask it directly.
[[nodiscard]] bool edge_casts(const Light2D& light, Vec2 a, Vec2 b,
                              OccluderCulling culling) noexcept;

// --- The screen-space signed distance field
// --------------------------------------------------------

/// How the field is sized. "sized by a configurable oversize and scale factor."
struct SdfSettings {
    /// The field's resolution relative to the viewport. A half-scale field is a quarter of the
    /// texels and is usually enough for collision.
    f32 scale = 0.5F;
    /// How far beyond the viewport the field extends, as a fraction. "WHEN the SDF is oversized
    /// beyond the viewport THEN occluders just off screen SHALL still contribute" — which matters
    /// because a particle can be pushed by geometry it cannot see.
    f32 oversize = 0.25F;
    /// Distances beyond this are clamped, so the field can be stored in eight bits if a caller
    /// wants.
    f32 maximum_distance = 64.0F;
};

/// A rasterised distance field: signed distances in world units, negative inside an occluder.
class SignedDistanceField {
public:
    explicit SignedDistanceField(Allocator& allocator) noexcept : texels_(allocator) {}

    [[nodiscard]] u32 width() const noexcept { return width_; }
    [[nodiscard]] u32 height() const noexcept { return height_; }
    [[nodiscard]] Span<const f32> texels() const noexcept { return texels_.span(); }
    /// The world rectangle the field covers, which is the viewport grown by the oversize.
    [[nodiscard]] Rect2D bounds() const noexcept { return bounds_; }

    /// Sample the field at a world point, bilinearly. THE FUNCTION THE VFX SYSTEM CALLS: "The SDF
    /// SHALL be sampleable from 2D materials, and SHALL be exposed to the VFX system as a data
    /// interface so 2D effects can collide against occluder geometry."
    [[nodiscard]] f32 sample(Vec2 world) const noexcept;

    friend Status rasterise_sdf(Span<const Occluder2D>, const Rect2D&, const SdfSettings&,
                                SignedDistanceField&) noexcept;

private:
    Array<f32> texels_;
    Rect2D bounds_;
    u32 width_ = 0;
    u32 height_ = 0;
};

/// Rasterise occluders into a signed distance field over `viewport`.
[[nodiscard]] Status rasterise_sdf(Span<const Occluder2D> occluders, const Rect2D& viewport,
                                   const SdfSettings& settings, SignedDistanceField& out) noexcept;

// --- Camera2D
// ---------------------------------------------------------------------------------------

/// What happens to the visible area when the window's aspect changes.
enum class CanvasStrategy : u8 {
    /// The viewport keeps its aspect and the rest of the window is black.
    FixedWithLetterbox = 0,
    /// The canvas is scaled to fill, which stretches when the aspect differs.
    ScaledCanvas = 1,
    /// More of the world becomes visible. "WHEN the window aspect changes with the 'expand'
    /// strategy THEN more of the world SHALL become visible rather than the image stretching."
    ExpandCanvas = 2,
};

struct Camera2D {
    Vec2 position;
    /// The offset from the position to the view's centre — what a camera "looks at" versus where it
    /// "is", which is what a screen-shake and a look-ahead move independently.
    Vec2 offset;
    f32 zoom = 1.0F;
    f32 rotation = 0.0F;
    /// The rectangle the camera's position is confined to. A zero-size limit means unconfined.
    Rect2D limits;
    bool limited = false;
    /// Seconds. The half-life of the camera's approach to its target; zero is instant.
    f32 smoothing_half_life = 0.0F;
    /// "pixel-perfect modes that snap to integer pixel boundaries."
    bool pixel_perfect = false;
    /// The size of one world unit in pixels, which pixel-perfect snapping rounds against.
    f32 pixels_per_unit = 1.0F;
    CanvasStrategy strategy = CanvasStrategy::ExpandCanvas;
    /// The canvas the camera was authored against.
    Vec2 reference{640.0F, 360.0F};
};

/// Advance a camera toward a target, honouring smoothing, limits and pixel snapping.
///
/// The smoothing is a HALF-LIFE, frame-rate independent for the same reason `camera-system`'s is:
/// a per-frame lerp behaves differently at 30 and 144 frames a second and a designer cannot tell
/// from the number which they tuned.
void advance_camera(Camera2D& camera, Vec2 target, f32 dt) noexcept;

/// The world rectangle a camera sees, given the output size and the strategy.
[[nodiscard]] Rect2D visible_bounds(const Camera2D& camera, Vec2 output) noexcept;

/// A world point in the camera's view, in output pixels.
[[nodiscard]] Vec2 world_to_view(const Camera2D& camera, Vec2 output, Vec2 world) noexcept;

}  // namespace cy::rendering2d

#endif  // CY_RENDERING_2D_LIGHTING_H
