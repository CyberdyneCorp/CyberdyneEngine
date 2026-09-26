// SPDX-License-Identifier: MIT
#pragma once
// Screen-space contact shadows: the settings, the constants the dispatch reads, and the CPU
// reference the suites compare against.
//
// `virtual-shadows` — "Contact and traced refinement": "Where shadow texel footprints are too
// coarse for contact detail, the system SHALL support refinement: a short screen-space trace ...
// applied selectively by importance and budget. Refinement SHALL be an addition to the paged
// result, not a replacement for it".
//
// ================================================================================================
// WHAT THE TRACE IS
// ================================================================================================
//
// From each pixel's reconstructed position, lifted off its surface along the prepass normal by one
// pixel's footprint, march `steps` taps toward the directional light over `length` metres. Each tap
// is projected back into the depth buffer; a tap that lies BEHIND the surface the buffer holds
// there, by less than `thickness` metres, is inside an occluder, and the pixel is in contact
// shadow. An occluder found early is full shadow and one found near the end of the trace fades out,
// so the term has no hard edge at `length`.
//
// WHY LIFT THE START. A tap near the start lands on the receiver's own surface a few pixels away,
// and the depth buffer holds that surface at pixel CENTRES: half a pixel of rounding at a grazing
// view is more depth than the tap has climbed. One footprint along the normal puts every tap above
// its own surface by more than that rounding, so an open plane traces to exactly one.
//
// WHAT IT CANNOT SEE, stated rather than discovered: anything off screen or behind the nearest
// surface. That is the half the shadow map keeps, and why the frame takes the darker of the two
// rather than either one.
//
// ================================================================================================
// IMPORTANCE AND BUDGET
// ================================================================================================
//
// Per pixel, not a global toggle: a pixel farther than `max_distance` metres from the camera is not
// traced — contact detail is sub-pixel there — and neither is one facing away from the light, which
// the shading's own N.L already darkens. `ShadowLever::Refinement`'s value (1, 0.5, 0 by default)
// scales that distance through `apply_refinement`, so the budget removes refinement from the far
// receivers first and at zero traces nothing, while the paged result underneath is untouched —
// "refinement SHALL be reduced before paged shadow quality is".

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>

namespace cy::rendering::contact_shadows {

struct ContactShadowSettings {
    /// How far toward the light the trace reaches, in metres. Contact scale, not shadow scale.
    f32 length = 0.35F;
    u32 steps = 16;
    /// How far behind a depth sample a tap may be and still count as inside it, in metres. A tap
    /// deeper than this is behind a surface that is IN FRONT of the occluder, not inside it.
    f32 thickness = 0.2F;
    /// Pixels farther than this, in metres of view depth, are not traced.
    f32 max_distance = 40.0F;
    /// The start's lift along the normal, in pixel footprints. See the header comment.
    f32 normal_offset_pixels = 1.0F;
};

/// `ShadowLever::Refinement`'s value applied: `max_distance` scaled by it, clamped to [0, 1]. At
/// zero no pixel is traced.
[[nodiscard]] ContactShadowSettings apply_refinement(const ContactShadowSettings& settings,
                                                     f32 refinement) noexcept;

/// What one frame's view contributes.
struct ContactShadowView {
    /// The UNJITTERED projection the depth was written with, reversed-Z.
    Mat4 projection = Mat4::identity();
    /// Camera-relative space to view space. Its rotation takes the prepass normals and the light
    /// direction into the view space the trace runs in.
    Mat4 relative_to_view = Mat4::identity();
    /// Camera-relative, normalised, pointing FROM the surface TO the light.
    Vec3 to_light{0.0F, 1.0F, 0.0F};
    u32 width = 0;
    u32 height = 0;
    /// The sub-pixel jitter the depth was rasterised with, in pixels, +y up — `GtaoView::jitter`'s
    /// convention.
    Vec2 jitter{0.0F, 0.0F};
};

/// `CyContactConstants` in contact_shadows.slang, word for word.
struct alignas(16) ContactShadowConstants {
    f32 view_rows[3][4] = {};
    /// x: P00, y: P11, z: m22, w: m32.
    f32 projection[4] = {};
    /// xy: the extent in pixels, zw: its reciprocal.
    f32 extent[4] = {};
    /// xyz: view-space direction to the light. w: trace length in metres.
    f32 light[4] = {};
    /// x: steps, y: thickness, z: the largest traced view depth, w: normal offset in footprints.
    f32 control[4] = {};
    /// xy: the jitter undone, as `GtaoConstants::jitter`. zw: unused.
    f32 jitter[4] = {};
};
static_assert(sizeof(ContactShadowConstants) == 128, "CyContactConstants is eight float4");

/// The dispatch's constants. Refuses an empty extent, a zero step count and a non-positive length.
[[nodiscard]] Expected<ContactShadowConstants, Error> make_contact_constants(
    const ContactShadowSettings& settings, const ContactShadowView& view) noexcept;

/// The inputs the trace reads, as the device reads them: `width * height`, row-major from the
/// top-left.
struct ContactShadowInputs {
    u32 width = 0;
    u32 height = 0;
    /// Reversed-Z depth in [0, 1]; 0 is the cleared far plane.
    Span<const f32> depth;
    /// The prepass target's `.xy`: the octahedral normal, camera-relative.
    Span<const Vec2> normals;
};

/// contact_shadows.slang's `cyContactShadows` at one pixel, on the host: the visibility, 1 lit and
/// 0 in contact shadow.
[[nodiscard]] f32 contact_shadow_reference_at(const ContactShadowInputs& inputs,
                                              const ContactShadowConstants& constants, u32 x,
                                              u32 y) noexcept;

/// `cy/packing.slang`'s octahedral decode, transcribed, and its encode, for a test that has to
/// write what the prepass would.
[[nodiscard]] Vec3 decode_octahedral(Vec2 encoded) noexcept;
[[nodiscard]] Vec2 encode_octahedral(Vec3 normal) noexcept;

/// The whole image. `out` is `width * height`.
[[nodiscard]] Status contact_shadow_reference(const ContactShadowInputs& inputs,
                                              const ContactShadowConstants& constants,
                                              Span<f32> out) noexcept;

}  // namespace cy::rendering::contact_shadows
