// SPDX-License-Identifier: MIT
#pragma once
// The GI debug views. `rendering-global-illumination` — "GI debug visualisation".
//
// ================================================================================================
// NINETEEN VIEWS OF THE ILLUMINATION STATE, EACH A PICTURE OF A DATUM THIS MODULE ALREADY HOLDS
// ================================================================================================
//
// The specification's list, one view each — the two lightmap views (texel density and charts) are
// drawn by the forward frame itself, which owns the lightmap UV, so they are not here:
//
// | View                    | Each pixel (or overlay)                                            |
// |-------------------------|--------------------------------------------------------------------|
// | GiProbes                | discs at the radiance cache's probes: green valid, red invalid     |
// | GiProbeIrradiance       | discs in the probe's mean irradiance                               |
// | GiProbeAge              | discs from green (just gathered) to red (at the age cap)           |
// | GiProbeError            | discs from green (settled) to red (moving)                         |
// | GiProbeVisibility       | the surface green where its probes see it, RED where a contributing |
// |                         | probe is a leak suspect; the suspect probes as red discs           |
// | GiRadianceClipmap       | the clipmap level covering the surface, dimmed where no probe is   |
// |                         | resident                                                           |
// | GiSurfaceCache          | the surface cache's radiance at the surface; magenta on a miss     |
// | GiDistanceField         | a horizontal slice of the field, banded by distance, in front of   |
// |                         | the geometry                                                       |
// | GiCards                 | the card each surface resolves to, one hue per card                |
// | GiReflectionProbes      | the influence volume's hue, mixed with the capture it returns      |
// | GiScreenTrace           | a screen ray's answer: green by confidence on a hit, red on a miss |
// | GiConfidence            | the resolved indirect diffuse's confidence                         |
// | GiTier                  | the tier that answered: screen blue, software green, hardware      |
// |                         | orange, sky white, nothing black                                   |
// | GiRayCount              | rays the pixel's reflection spent: red hardware, green software,   |
// |                         | blue screen                                                        |
// | GiDenoiserConfidence    | the history confidence the denoiser was handed                     |
// | GiDisocclusion          | magenta where the reprojection failed                              |
// | GiIndirectDiffuse       | the indirect diffuse alone                                         |
// | GiIndirectSpecular      | the indirect specular alone                                        |
// | GiEmissive              | the light the promoted emitters deliver, plus the surface's own     |
// |                         | emission, alone                                                    |
//
// Every view is drawn from the same functions the frame's illumination is computed by —
// `indirect_diffuse`, `indirect_specular`, `explain`, the tiered tracer, the caches' own lookups —
// so a view cannot show something the system does not do.
//
// A view's names are `render::DebugViewMode`'s, letter for letter (`gi_debug_view_from_name`), and
// that is the whole join: this module does not depend on the render server, and the frame asks for
// a view by the mode's name. The output is `Rgba8Unorm` with alpha zero where a view draws nothing
// (an overlay over the lit frame) and 255 where it draws.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/vec.h>
#include <cy/rendering/denoise/denoiser.h>
#include <cy/rendering/gi/irradiance_volume.h>
#include <cy/rendering/gi/tracing.h>

namespace cy::rendering::gi {

class IlluminationSystem;

enum class GiDebugView : u8 {
    Probes = 0,
    ProbeIrradiance,
    ProbeAge,
    ProbeError,
    ProbeVisibility,
    RadianceClipmap,
    SurfaceCache,
    DistanceField,
    Cards,
    ReflectionProbes,
    ScreenTrace,
    Confidence,
    Tier,
    RayCount,
    DenoiserConfidence,
    Disocclusion,
    IndirectDiffuse,
    IndirectSpecular,
    Emissive,
    Count,
};

inline constexpr u32 kGiDebugViewCount = static_cast<u32>(GiDebugView::Count);

/// The view's name: `render::DebugViewMode`'s spelling of the same view ("GiTier").
[[nodiscard]] const char* gi_debug_view_name(GiDebugView view) noexcept;

/// The view a `render::DebugViewMode` name asks for, or `GiDebugView::Count` for a mode that is not
/// one of these.
[[nodiscard]] GiDebugView gi_debug_view_from_name(const char* name) noexcept;

/// One pixel of the frame's surface buffer: where the view ray met the world.
struct GiViewPixel {
    Vec3 position{0.0F, 0.0F, 0.0F};
    Vec3 normal{0.0F, 1.0F, 0.0F};
    /// View-space depth in metres; zero or negative is sky, where a view draws nothing.
    f32 depth = 0.0F;
    f32 roughness = 0.5F;
    /// The surface's own emission, for the emissive view.
    Vec3 emission{0.0F, 0.0F, 0.0F};
};

struct GiViewCamera {
    u32 width = 0;
    u32 height = 0;
    Vec3 position{0.0F, 0.0F, 0.0F};
    /// World to clip, for the overlays drawn at points (probes).
    Mat4 world_to_clip = Mat4::identity();
};

/// What a view reads beyond the illumination system.
struct GiDebugInputs {
    /// `camera.width * camera.height` pixels, row-major from the top-left.
    Span<const GiViewPixel> pixels;
    /// The screen tier's buffers, for `GiScreenTrace`, `GiTier` and `GiRayCount`. Null: the views
    /// run without a screen tier, as a probe gather does.
    const ScreenView* screen = nullptr;
    /// The reprojection's answer handed to the denoiser, for `GiDenoiserConfidence` and
    /// `GiDisocclusion`. Empty: no history, which is a full disocclusion.
    denoise::HistoryGuidance history{};
    /// The height of the `GiDistanceField` slice, in metres.
    f32 field_slice_height = 0.0F;
    /// Radius of a probe disc, in metres.
    f32 probe_disc_metres = 0.12F;
    /// The age that reads red in `GiProbeAge`, in frames.
    u64 max_age_frames = 240;
    /// The frame the views are drawn in.
    u64 frame = 0;
    /// Linear radiance is mapped to a byte as `x / (1 + x)` after this multiplier.
    f32 exposure = 1.0F;
};

/// Draw one view into `out` (`width * height` `Rgba8Unorm` texels, red in the low byte).
///
/// Non-const because `GiTier`, `GiRayCount` and `GiIndirectSpecular` trace through the system's
/// tiered tracer, which counts what it traced; the views draw from a copy of nothing.
[[nodiscard]] Status render_gi_debug_view(IlluminationSystem& system, GiDebugView view,
                                          const GiViewCamera& camera, const GiDebugInputs& inputs,
                                          Span<u32> out) noexcept;

/// A byte from linear radiance: `x / (1 + x)` per channel, then quantised. What every radiance view
/// uses, so two views of one radiance agree.
[[nodiscard]] u32 pack_radiance(Vec3 radiance, f32 exposure) noexcept;

/// A byte from a colour already in [0, 1], opaque.
[[nodiscard]] u32 pack_colour(Vec3 colour) noexcept;

}  // namespace cy::rendering::gi
