#pragma once
// THE AIR IN THE BEAUTY SHOT — the effect `vfx-system` puts in the artefact's frame. M11.c
// task 6.3.
//
// ================================================================================================
// WHY THIS FILE EXISTS AT ALL
// ================================================================================================
//
// `m11c:vfx-in-the-shot` says it in one sentence: *"particles are in the artefact's frame. An
// art-directed shot with no particles in it does not exercise `vfx-system`."* Until this file the
// shot's own provenance said the opposite, under "what it is not": **"No particles."** So the
// colonnade is now lit through drifting embers — motes of ash rising off the courtyard and catching
// the low sun — simulated by `cy::vfx::SimulationWorld`, published through `publish_sprites` and
// drawn by `cy::rendering::particles::ParticleRenderer` inside the frame's own TRANSPARENT stage.
//
// ================================================================================================
// THE EFFECT IS AUTHORED IN C++, AND THAT IS A MEASUREMENT RATHER THAN A PREFERENCE
// ================================================================================================
//
// Everything else in this shot is content: the scene is `content/beauty/shot.cyshot`, the materials
// are `.cygraph` files the editor's canvas authored, the meshes are `.cyprim` sources, the textures
// are PNGs. The air is not, and the reason is that **there is no on-disk VFX asset format in this
// tree.** `VfxSystemAsset` is built by a caller and cooked by `compile_system`; nothing reads one
// from a file and nothing writes one, and `src/vfx/README.md`'s fourth recorded absence — the
// editor's VFX graph editor is not built — is the same fact from the authoring side.
//
// So this effect is authored exactly the way `src/vfx/tests/effects.cpp` authors the spark plume:
// `cy::graph::Graph`s built node by node, resolved against the shipping node registry, and compiled
// by the shipping compiler. What that proves is the runtime; what it does not prove is an authoring
// path, and `docs/design/beauty-shot.md` states the difference where the rest of the shot's
// honesty contract is.
//
// ================================================================================================
// IT IS SHARED WITH THE SUITE THAT PHOTOGRAPHS IT, ON PURPOSE
// ================================================================================================
//
// `src/vfx/tests/` compiles THIS FILE — the same arrangement in reverse as `samples/12-beauty`
// compiling `tests/render/golden.cpp`, and for the same reason: a suite that re-declared the
// artefact's effect would be a suite asserting its own copy. `render.vfx`'s
// `particles are in the assembled frame` case plays this system, from these emitters, through the
// artefact's own camera, and compares the result against a committed reference.

#include <cy/core/base/expected.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/graph/cybergraph.h>
#include <cy/vfx/asset.h>
#include <cy/vfx/compile.h>
#include <cy/vfx/interfaces.h>
#include <cy/vfx/ir.h>
#include <cy/vfx/renderers.h>
#include <cy/vfx/runtime.h>
#include <cy/vfx/world.h>

namespace cy::sample::beauty {

/// Where one ember emitter sits, in WORLD metres — the same space `content/beauty/shot.cyshot`
/// places geometry in. `publish_sprites` rebases against the camera, so nothing here is
/// camera-relative and nothing here has to know where the camera is.
struct EmberEmitter {
    Vec3 position;
};

/// THE ARTEFACT'S CAMERA, duplicated from `content/beauty/shot.cyshot` so that a suite can
/// photograph the air from where the shot sees it without a working directory or a scene parser.
///
/// The duplication IS CHECKED: `Shot::read` refuses a scene file whose camera is not these numbers,
/// naming this header. Two copies that cannot drift are a constant; two that can are a defect
/// waiting for somebody to move the camera.
inline constexpr Vec3 kShotEye{1.8F, 1.62F, -11.5F};
inline constexpr Vec3 kShotTarget{-0.4F, 2.4F, 8.0F};
inline constexpr f32 kShotFovDegrees = 42.0F;
inline constexpr f32 kShotNearPlane = 0.08F;
/// The grade the resolve applies. Also `shot.cyshot`'s, also checked by `Shot::read`.
inline constexpr f32 kShotExposureStops = -11.45F;

/// Three emitters down the courtyard, between the camera and the monolith, placed where the low sun
/// rakes across the colonnade so the motes are lit rather than merely present. World metres.
inline constexpr EmberEmitter kEmberEmitters[] = {
    {Vec3{-1.15F, 0.25F, -2.20F}},
    {Vec3{1.05F, 0.25F, 3.60F}},
    {Vec3{-0.55F, 0.25F, 9.40F}},
};

inline constexpr u32 kEmberEmitterCount =
    static_cast<u32>(sizeof(kEmberEmitters) / sizeof(kEmberEmitters[0]));

/// Particles one emitter's block can hold. The population settles below this — see `kEmberWarmup`
/// — and a block at capacity would be a population whose size is the pool's rather than the
/// effect's, which is not a thing to photograph.
inline constexpr u32 kEmberCapacity = 512;

/// How long the field is simulated before the shutter opens, in seconds, and at what step.
///
/// A field photographed at t=0 is an empty frame; one photographed after a single step is a knot of
/// particles at the emitter. The motes live 4.5 to 7 seconds (see `embers.cpp`), so six seconds is
/// long enough that the oldest have died once and the population is in steady state, and short
/// enough that the capture does not spend a second of processor time on air.
inline constexpr f32 kEmberWarmup = 6.0F;
inline constexpr f32 kEmberStep = 1.0F / 60.0F;

/// WHAT A MOTE EMITS AND HOW OPAQUE IT IS — the two numbers that decide whether a field of sprites
/// reads as embers or as soot, and the first pair had both wrong.
///
/// THE BLEND IS `One, OneMinusSourceAlpha` OVER A PREMULTIPLIED FRAGMENT, so a texel under a mote
/// comes back as `radiance * a + background * (1 - a)` where `a` is the mote's opacity times the
/// sprite's own falloff. Two things follow, and they are the whole of this paragraph: a mote whose
/// RADIANCE is below what it is drawn over SUBTRACTS, and the opacity decides how much of the
/// background it takes away while it does.
///
/// MEASURED ON THE PUBLISHED CAPTURE rather than reasoned about, in three renders:
///
///   * **2 600 of radiance at 0.55-0.95 of opacity — the first version — photographed as SOOT.**
///     72 297 texels of the frame moved and only 2 118 of them got brighter, every one of those
///     over a shadow that was already black. A sky texel went from sRGB (95, 137, 179) to
///     (54, 81, 113): the motes read as dirt on the lens.
///   * **A PROBE SETTLED WHAT THE SKY ACTUALLY IS.** The same field rendered at a flat 20 000 of
///     radiance and an opacity of exactly 1 puts a known number in a texel: its core displays at
///     sRGB (255, 251, 197) where the sky beside it displays (98, 140, 179). Inverting that
///     against the chroma below puts this sky at roughly (1 800, 3 200, 5 000) of linear radiance
///     — which is why 2 600, and even 16 000 at that opacity, lost to it.
///   * **36 000 at 0.18-0.40 clears it in the channel that matters.** A fresh mote's core
///     contributes 6 480 to 14 400 of red against a sky of about 1 800, and 2 000 to 4 500 of blue
///     against a sky of about 5 000 — a warm core with an orange skirt over a blue sky, which is
///     what an ember is — while taking at most two fifths of the background away at the one texel
///     in the middle of a sprite that is three pixels across. Measured against the published still
///     at this pair: 15 590 texels brighter by more than ten and 10 763 darker by more than ten,
///     the brightest gain (+246, +202, +145) and the deepest loss (-40, -43, -53). At 0.30-0.60
///     the same field was brighter still and the losses ran to -84, which is a skirt a reader can
///     see; this is the pair where the subtractive half stops being visible.
///
/// The ratio between birth and death stays at twenty, which is what makes the far end of a field
/// dimmer than its near end without a per-particle branch. `declare_attributes` caps `emission` at
/// 40 000, which is the quantisation range the derived layout is built from rather than a ceiling
/// on taste; 36 000 leaves it a tenth of headroom.
inline constexpr f32 kEmberBirthRadiance = 36000.0F;
inline constexpr f32 kEmberDeathRadiance = 1800.0F;

/// The opacity a mote is drawn with: a floor, and the span a random draw adds to it — 0.18 to 0.40,
/// which is a spark rather than a puff.
inline constexpr f32 kEmberOpacityFloor = 0.18F;
inline constexpr f32 kEmberOpacitySpan = 0.22F;

/// The ring the renderer draws from. Larger than the settled population, so
/// `PublishReport::dropped` is zero and a non-zero one is a finding.
inline constexpr u32 kEmberRing = 4096;

/// Author the ember system. Every node is placed and wired here; nothing is read from a file,
/// because nothing in this tree writes one.
[[nodiscard]] Expected<vfx::VfxSystemAsset, Error> build_embers(Allocator& allocator) noexcept;

/// Author it and cook it with the shipping compiler and the shipping node library.
[[nodiscard]] Expected<vfx::CompiledSystem, Error> cook_embers(Allocator& allocator,
                                                               graph::DiagnosticSink& sink,
                                                               vfx::CompileReport& report) noexcept;

/// The cooked system, a world, the three instances, and the settled population.
///
/// Held by value by whoever draws it. The world keeps a POINTER to the compiled system, so the two
/// have to live together or the first step reads freed memory — which is why they are one object.
class EmberField {
public:
    explicit EmberField(Allocator& allocator) noexcept;

    EmberField(const EmberField&) = delete;
    EmberField& operator=(const EmberField&) = delete;

    /// Cook, initialise the world, and play one instance at each of `kEmberEmitters`.
    [[nodiscard]] Status build() noexcept;

    /// Step the world to `kEmberWarmup` seconds and publish the result against `camera_position`.
    /// Deterministic: `random_draw` is a hash of the particle index and the stream, so the same
    /// warm-up produces the same field on every run and on every machine.
    [[nodiscard]] Status settle(const Vec3& camera_position) noexcept;

    /// One more step and one more publication, for a caller drawing a sequence.
    [[nodiscard]] Status advance(const Vec3& camera_position, f32 dt = kEmberStep) noexcept;

    /// What `settle()` produced: one record per live mote, camera-relative, ready for
    /// `ParticleRenderer::upload`.
    [[nodiscard]] Span<const rendering::particles::ParticleInstance> records() const noexcept {
        return records_.span();
    }
    [[nodiscard]] const vfx::PublishReport& published() const noexcept { return published_; }
    [[nodiscard]] const vfx::StepReport& stepped() const noexcept { return stepped_; }
    [[nodiscard]] const vfx::CompileReport& cooked() const noexcept { return cook_; }
    [[nodiscard]] vfx::SimulationWorld& world() noexcept { return world_; }

private:
    [[nodiscard]] Status publish(const Vec3& camera_position) noexcept;

    Allocator* allocator_ = nullptr;
    graph::DiagnosticSink sink_;
    vfx::CompileReport cook_;
    Expected<vfx::CompiledSystem, Error> system_ = fail(ErrorCode::Unavailable, "not cooked");
    vfx::SimulationWorld world_;
    vfx::StepReport stepped_;
    vfx::PublishReport published_;
    Array<rendering::particles::ParticleInstance> records_;
    bool built_ = false;
};

}  // namespace cy::sample::beauty
