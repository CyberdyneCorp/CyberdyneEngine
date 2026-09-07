#pragma once
// The three seams the illumination subsystems reach each other through. Task 9.1.
//
// `rendering-global-illumination` — "Subsystems are separable": "WHEN the radiance cache is
// replaced or reworked THEN the GI scene, surface cache, tracers, and resolve SHALL be unaffected."
//
// That requirement is about the LINK GRAPH, not about intentions, so the surface cache does not
// name the radiance cache and the radiance cache does not name the tracers. Each names an abstract
// interface declared here, and the composition happens in system.h. Three interfaces is the whole
// of it, and each exists because exactly one dependency would otherwise be concrete:
//
//   `Occluder`       shadowing, for the direct lighting a cache page records. The distance field
//                    implements it; so does the ray tracing service. Neither is named by a cache.
//   `IndirectSource` the incoming indirect radiance at a point. The radiance cache implements it;
//                    a baked irradiance volume implements it too, which is what makes `Baked` a
//                    first-class mode rather than a branch.
//   `RadianceLookup` the radiance leaving a surface. The surface cache implements it, and the
//                    tracers resolve a hit through it — which is why "a secondary hit is a lookup"
//                    is structural: a tracer has no way to evaluate a material even if it wanted
//                    to.

#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>

namespace cy::rendering::gi {

/// A light, as illumination sees it. Deliberately not `render::Light`: this module needs position,
/// direction, colour and a radius, and taking the renderer's full light record would make the
/// illumination system depend on the shape of the lighting server's struct.
struct GiLight {
    Vec3 position{0.0F, 0.0F, 0.0F};
    /// The direction the light travels, for a directional light.
    Vec3 direction{0.0F, -1.0F, 0.0F};
    Vec3 colour{1.0F, 1.0F, 1.0F};
    /// Candela for a punctual light, lux for a directional one.
    f32 intensity = 1.0F;
    /// Metres. Zero means unbounded, which only a directional light should be.
    f32 range = 0.0F;
    bool directional = false;
    /// The identity an invalidation is attributed to when this light changes.
    u64 id = 0;
};

/// Whether the segment between two points is blocked.
class Occluder {
public:
    Occluder() = default;
    Occluder(const Occluder&) = delete;
    Occluder(Occluder&&) = delete;
    Occluder& operator=(const Occluder&) = delete;
    Occluder& operator=(Occluder&&) = delete;
    virtual ~Occluder() = default;

    [[nodiscard]] virtual bool occluded(Vec3 from, Vec3 to) const noexcept = 0;
};

/// The indirect radiance arriving at a point from a hemisphere around a normal.
class IndirectSource {
public:
    IndirectSource() = default;
    IndirectSource(const IndirectSource&) = delete;
    IndirectSource(IndirectSource&&) = delete;
    IndirectSource& operator=(const IndirectSource&) = delete;
    IndirectSource& operator=(IndirectSource&&) = delete;
    virtual ~IndirectSource() = default;

    [[nodiscard]] virtual Vec3 gather(Vec3 position, Vec3 normal) const noexcept = 0;
};

/// The radiance leaving a surface at a traced hit. See the header comment: a tracer holds one of
/// these and no material at all.
class RadianceLookup {
public:
    RadianceLookup() = default;
    RadianceLookup(const RadianceLookup&) = delete;
    RadianceLookup(RadianceLookup&&) = delete;
    RadianceLookup& operator=(const RadianceLookup&) = delete;
    RadianceLookup& operator=(RadianceLookup&&) = delete;
    virtual ~RadianceLookup() = default;

    /// `radiance` receives the outgoing radiance and `age_frames` how stale the entry is. Returns
    /// false when nothing in the cache covers the point, which the tracer turns into a low
    /// confidence rather than into black.
    [[nodiscard]] virtual bool radiance_at(Vec3 position, Vec3 normal, Vec3& radiance,
                                           u32& age_frames) const noexcept = 0;
};

/// What a world-space tracer answered with. The tiers differ in how they find this and not in what
/// it is, which is what lets the resolve, the probe gather and the bake share one code path.
struct SceneHit {
    bool hit = false;
    f32 t = 0.0F;
    Vec3 position{0.0F, 0.0F, 0.0F};
    Vec3 normal{0.0F, 1.0F, 0.0F};
    /// How much the tier trusts this answer, in [0, 1]. A software trace that grazed a surface and
    /// a hardware trace against a proxy are both less than one, for different reasons.
    f32 confidence = 1.0F;
    /// The world-space error the geometry declares. Non-zero only for a proxy.
    f32 declared_error_metres = 0.0F;
};

/// One world-space tracing tier, behind one interface.
///
/// `rendering-global-illumination` — "Consumers SHALL request radiance along a ray and SHALL NOT
/// branch on which tier answered, nor on device capability." The interface is how that is kept: a
/// consumer holds a `SceneTracer*` and cannot ask it what it is.
class SceneTracer {
public:
    SceneTracer() = default;
    SceneTracer(const SceneTracer&) = delete;
    SceneTracer(SceneTracer&&) = delete;
    SceneTracer& operator=(const SceneTracer&) = delete;
    SceneTracer& operator=(SceneTracer&&) = delete;
    virtual ~SceneTracer() = default;

    [[nodiscard]] virtual bool trace(Vec3 origin, Vec3 direction, f32 max_distance,
                                     SceneHit& hit) const noexcept = 0;
    /// Whether the segment between two points is blocked. Every tier can answer this and it is what
    /// makes a tracer usable as an `Occluder` for the direct term.
    [[nodiscard]] virtual bool occluded(Vec3 from, Vec3 to) const noexcept = 0;
};

/// The analytic sky a ray that escaped resolves to. Two colours and a horizon blend is enough for
/// GI's sky term; the full model is `atmosphere-sky-and-clouds`, which reaches Seed in this
/// milestone under someone else's hand — this is the seam it will replace, not a second sky.
struct SkyTerm {
    Vec3 zenith{0.35F, 0.48F, 0.72F};
    Vec3 horizon{0.62F, 0.68F, 0.78F};
    Vec3 ground{0.18F, 0.16F, 0.14F};
    f32 intensity = 1.0F;

    [[nodiscard]] Vec3 radiance(Vec3 direction) const noexcept;
};

/// The direct radiance one light delivers to a surface, before shadowing.
[[nodiscard]] Vec3 direct_radiance(const GiLight& light, Vec3 position, Vec3 normal) noexcept;

/// The same, shadowed through an occluder. `occluder` may be null, which means "unshadowed" —
/// and that is the honest degradation on a device with no tracer rather than a black frame.
[[nodiscard]] Vec3 shaded_direct(Span<const GiLight> lights, Vec3 position, Vec3 normal,
                                 const Occluder* occluder) noexcept;

}  // namespace cy::rendering::gi
