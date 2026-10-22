#pragma once
// The GPU light record, and how one is built from a scene light. Task 4.4.1.
//
// ================================================================================================
// THIS STRUCT IS THE SHADER'S, AND THE ASSERTIONS AT THE BOTTOM ARE THE CONTRACT
// ================================================================================================
//
// `cy/light.slang` declares `Light` and this declares `GpuLight`; nothing at run time compares the
// two, so the offsets are asserted here and the build breaks the moment a field moves. That is the
// same arrangement `render::GpuInstance` uses, for the same reason: the alternative is a frame of
// lights whose colours are somebody else's ranges.
//
// ================================================================================================
// CAMERA-RELATIVE, AT THE MOMENT THE STRUCT IS WRITTEN — NOT WHEN PRECISION BREAKS
// ================================================================================================
//
// design.md §3, and `cy/light.slang` already says it from the other side: the position in this
// record is RELATIVE TO THE CAMERA and there is nowhere in it to put a world-space one. A light at
// (1000000, 0, 0) with the camera beside it reaches the shader as a small number, and the
// subtraction happens in `f64` on the CPU where both operands are exact — which is the half of
// camera-relative rendering that a shader cannot do for itself.
//
// The field is named `position_relative_to_camera` rather than `position` deliberately. A name that
// merely says "position" is one somebody fills in from a world transform, and the failure is
// invisible until content is far from the origin.
//
// ================================================================================================
// PHYSICAL UNITS ARRIVE ALREADY CONVERTED
// ================================================================================================
//
// `intensity` is the quantity the shader integrates — candela for a punctual light, lux for a
// directional one — never the authored lumens. units.h does the conversion once, on the CPU, per
// light per frame. Doing it in the shader would be four instructions per light per pixel to
// recompute a constant, and it would put the spot convention (units.h's one real choice) in a place
// where two shaders could disagree about it.

#include <cy/core/base/types.h>
#include <cy/core/math/transform.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/lighting/units.h>
#include <cy/servers/render/model.h>

#include <cstddef>
#include <type_traits>

namespace cy::rendering {

/// The shader-side light kinds. Plain integers rather than an enum class, because `cy/light.slang`
/// declares them as `uint` constants and the two must be the same numbers.
inline constexpr u32 kGpuLightDirectional = 0;
inline constexpr u32 kGpuLightPoint = 1;
inline constexpr u32 kGpuLightSpot = 2;
/// A spot whose cone falls off as the cosine rather than its square: a small Lambertian emitter
/// (`render::LightDescription::cosine_falloff`). `cy/light.slang`'s `kLightEmitter`.
inline constexpr u32 kGpuLightEmitter = 3;
/// The four area kinds (`render::is_area_light`). `cy/light.slang`'s `kLightRect` and on.
inline constexpr u32 kGpuLightRect = 4;
inline constexpr u32 kGpuLightDisc = 5;
inline constexpr u32 kGpuLightSphere = 6;
inline constexpr u32 kGpuLightTube = 7;
/// A STATIC light (`render::LightMobility::Static`): baked whole, so its record is a placeholder
/// that keeps its index and that no loop shades. `cy/light.slang`'s `kLightBaked`.
inline constexpr u32 kGpuLightBaked = 8;
/// `GpuLight::kind`'s low byte is the kind; the bits above it are flags no punctual light sets, so
/// every comparison a shader made before area lights existed still compares what it did.
inline constexpr u32 kGpuLightKindMask = 0xFFU;
/// A rect or a disc that emits from both faces.
inline constexpr u32 kGpuLightTwoSided = 0x100U;

/// Whether a record's kind is one of the four area kinds.
[[nodiscard]] constexpr bool gpu_light_is_area(u32 kind) noexcept {
    const u32 base = kind & kGpuLightKindMask;
    return base >= kGpuLightRect && base <= kGpuLightTube;
}

/// ONE LIGHT, IN THE LAYOUT THE SHADER SEES. 64 bytes, 16-byte aligned.
struct alignas(16) GpuLight {
    /// Metres, relative to the view's `camera_relative_origin`. See the header comment.
    f32 position_relative_to_camera[3] = {0.0F, 0.0F, 0.0F};
    /// Metres. Zero for a directional light, whose falloff is none.
    f32 range = 0.0F;

    /// Normalised, pointing the way the light travels. A spot's cone axis; a directional light's
    /// direction of sunlight.
    f32 direction[3] = {0.0F, -1.0F, 0.0F};
    /// Candela for a punctual light, lux for a directional one. Already converted: see units.h.
    f32 intensity = 0.0F;

    /// Linear, un-premultiplied, unit luminance. The temperature tint is already folded in.
    f32 color[3] = {1.0F, 1.0F, 1.0F};
    u32 kind = kGpuLightPoint;

    /// The smooth cone attenuation, precomputed: `saturate(cos(angle) * scale + bias)`. Two
    /// multiply-adds in the shader instead of two inverse cosines.
    ///
    /// AN AREA LIGHT HAS NO CONE, and the pair is its two half-extents in metres instead: a rect's
    /// half width and half height, a disc's or a sphere's radius twice, a tube's half length and
    /// its radius. `direction` is a rect's or a disc's emitting normal and a tube's axis.
    f32 spot_scale = 1.0F;
    f32 spot_bias = 0.0F;
    /// The atlas tile this light's shadow was rendered into, or `kNoShadowSlot`. Filled in by the
    /// shadow pass, after assignment, which is why it is not an argument to `build_gpu_light`.
    ///
    /// AN AREA LIGHT TAKES NO ATLAS TILE — its shadow is sampled over its surface by the stochastic
    /// path — and the word is its TANGENT instead: a rect's width axis, octahedral, two 16-bit
    /// signed halves (`encode_area_tangent`). `cy/light.slang` calls the word `shapeWord`.
    u32 shadow_slot = 0xFFFFFFFFU;
    /// Which layers this light illuminates. `rendering-lighting-and-shadows`' light channels: "a
    /// compact bitfield test during light assignment, not a per-object light loop", so this word is
    /// read by the assignment compute pass and never by the shading loop.
    u32 layer_mask = 0xFFFFFFFFU;
};

inline constexpr u32 kNoShadowSlot = 0xFFFFFFFFU;

/// What a light is, as far as the renderer is concerned, before it becomes a record.
///
/// Restates `render::LightDescription`'s fields that survive the conversion plus the two that do
/// not exist there — the authored unit and the colour temperature — because those are properties of
/// the AUTHORING and the server holds the evaluated light.
struct LightBuildParameters {
    /// The unit `render::LightDescription::intensity` is expressed in. Defaulted per kind by
    /// `default_unit_for`, which is what a caller with no opinion passes.
    PhotometricUnit unit = PhotometricUnit::Candela;
    /// Kelvin. Zero means "no temperature tint", which is what a zeroed struct gives.
    f32 temperature_kelvin = 0.0F;
    /// The camera-relative origin the position is measured from:
    /// `render::View::camera_relative_origin`.
    Vec3 camera_origin{0.0F, 0.0F, 0.0F};
};

/// Build one record. The only function that writes a `GpuLight`, so the camera-relative
/// subtraction, the unit conversion and the cone precomputation each happen in exactly one place.
[[nodiscard]] GpuLight build_gpu_light(const render::LightDescription& light,
                                       const LightBuildParameters& parameters) noexcept;

/// The record a static light keeps its place in the frame's list with: no intensity, no channels,
/// kind `kGpuLightBaked`.
[[nodiscard]] GpuLight baked_light_placeholder() noexcept;

/// The importance a light is ranked by when there are more of them than a budget allows.
///
/// `rendering-lighting-and-shadows`: "Lights exceeding budgets SHALL be dropped deterministically
/// by importance (screen coverage × intensity)". Both factors are properties of the light and the
/// view and neither is an index, so two runs of one frame drop the same lights — which is the
/// "deterministic dropping" scenario, and the reason this returns a number rather than sorting.
[[nodiscard]] f32 light_importance(const render::LightDescription& light, Vec3 camera_position,
                                   f32 shading_intensity) noexcept;

/// The world-space bounding sphere a light affects: what cluster assignment and the volume index
/// test against. A directional light has no bound and returns a radius of `math::kInfinity`.
///
/// An area light's is its reach about its centre: the authored `range`, or, with none authored,
/// `area_light_reach` — the shader windows its contribution to zero there, as a punctual light's.
[[nodiscard]] f32 light_bounding_radius(const render::LightDescription& light) noexcept;

/// The illuminance, in lux, below which an area light with no authored range stops: where the
/// cluster lists end and the shader's window reaches zero.
inline constexpr f32 kAreaLightReachIlluminance = 0.01F;

/// How far an area light of this luminance and size reaches, in metres: the distance at which its
/// far-field illuminance, `L * A / d^2` on a receiver facing it, falls to
/// `kAreaLightReachIlluminance`, and never less than the emitter's own extent.
[[nodiscard]] f32 area_light_reach(const render::LightDescription& light,
                                   f32 shading_luminance) noexcept;

/// A unit vector as `GpuLight::shadow_slot` carries an area light's tangent: octahedral, x in the
/// low 16 bits and y in the high, each a signed normalised integer. `cy/light.slang`'s
/// `decodeAreaTangent` is the inverse.
[[nodiscard]] u32 encode_area_tangent(Vec3 tangent) noexcept;
[[nodiscard]] Vec3 decode_area_tangent(u32 word) noexcept;

// --- The contract with cy/light.slang -----------------------------------------------------------
//
// `Light` in cy/light.slang is float3 + float + float3 + float + float3 + uint + float2 + uint +
// uint, and every offset below matches it. The shader's last two words are `shapeWord` and
// `layerMask`: the shading loop reads the first for an area light's tangent, and the device's
// cluster assignment reads the second, which is the light channels' one AND.

static_assert(sizeof(GpuLight) == 64, "GpuLight is cy/light.slang's Light: 64 bytes");
static_assert(alignof(GpuLight) == 16, "GpuLight must be std430/std140-friendly");
static_assert(offsetof(GpuLight, position_relative_to_camera) == 0);
static_assert(offsetof(GpuLight, range) == 12);
static_assert(offsetof(GpuLight, direction) == 16);
static_assert(offsetof(GpuLight, intensity) == 28);
static_assert(offsetof(GpuLight, color) == 32);
static_assert(offsetof(GpuLight, kind) == 44);
static_assert(offsetof(GpuLight, spot_scale) == 48);
static_assert(offsetof(GpuLight, spot_bias) == 52);
static_assert(offsetof(GpuLight, shadow_slot) == 56);
static_assert(offsetof(GpuLight, layer_mask) == 60);
static_assert(std::is_trivially_copyable_v<GpuLight>);

}  // namespace cy::rendering
