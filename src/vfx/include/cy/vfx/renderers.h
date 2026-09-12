#pragma once
// THE SIX RENDERER KINDS BEYOND `Sprite` AND `Mesh`. M10 task 5.3.
//
// ================================================================================================
// WHAT WAS MISSING, AND WHAT THIS FILE IS AND IS NOT
// ================================================================================================
//
// `src/vfx/README.md`, under "Deliberate limits, recorded rather than hidden", before this file
// existed:
//
//   > `Ribbon`, `Beam`, `Trail`, `Decal`, `Light` and `Volume` renderers are NOT IMPLEMENTED.
//   > `Sprite` is `src/rendering/particles/` and `Mesh` is `publish_mesh_instances`; the other six
//   > are compositing work in the renderer's layer, and this module has no way to fake them.
//
// Half of that sentence was right and half of it was an excuse. The half that was right: this
// module cannot draw a ribbon, and nothing here creates a pipeline, declares a pass or holds a
// device handle. The half that was an excuse: every one of the six needs GEOMETRY OR INSTANCES
// DERIVED FROM PARTICLE STATE before anything can composite them, and deriving those is exactly
// what a simulation is for. A ribbon's strip is a function of a chain's positions and widths; a
// light instance is a function of a particle's position, colour and emission. None of that is the
// renderer's to invent.
//
// So this file is the SEAM, the same shape `publish_sprites` and `publish_mesh_instances` already
// are: typed rows a host appends to whatever buffer its renderer reads. What is still not here is
// the compositing, and `src/vfx/README.md` says so where the evidence is.
//
// ================================================================================================
// A RENDERER IS DECLARED BY THE EMITTER, NOT INFERRED FROM ITS ATTRIBUTES
// ================================================================================================
//
// `publish_mesh_instances` decides whether an emitter is a mesh emitter by asking whether its
// layout has a `mesh` attribute — "the layout is the compiler's answer to what this emitter has, so
// asking it is asking the graphs". That worked for two kinds and does not scale to eight: a `Trail`
// and a `Ribbon` read the same attributes and differ only in what they mean, and `Decal` and
// `Volume` would be told apart by which optional attribute an author happened to write.
//
// `vfx-system` settles it: "The system SHALL provide these particle renderers, EACH CONSUMING
// ATTRIBUTES PRODUCED BY THE RENDER STAGE" — the renderer is a property of the emitter and the
// attributes are its input. `RendererDecl` is that property, and `publish_*` refuses an emitter
// whose declared kind is not the one it was asked for rather than guessing from a layout.
//
// ================================================================================================
// MOTION VECTORS ARE DERIVED FROM THE PREVIOUS SIMULATION STEP, AND ARE SUPPRESSED
// ================================================================================================
//
// "WHEN temporal anti-aliasing or motion blur is enabled THEN particle renderers SHALL output
// motion vectors DERIVED FROM THE PREVIOUS SIMULATION STEP, so particles do not smear or ghost",
// and — one requirement earlier — "WHEN an effect's particles change discontinuously (spawn, kill,
// teleport) THEN interpolation SHALL be SUPPRESSED for those particles rather than smearing them."
//
// The two are one mechanism here. `PublicationHistory` remembers what each slot published last
// frame; a row's `previous_position` is that, and `motion_valid` is false when the slot did not
// publish last frame — which is precisely spawn, kill and re-use of a freed slot. A publication
// that always produced a motion vector would smear every newly spawned particle from wherever the
// slot's previous occupant died, which is the exact artefact the requirement names.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/vfx/runtime.h>
#include <cy/vfx/world.h>

namespace cy::vfx {

// --- What an emitter declares --------------------------------------------------------------------

/// `vfx-system`'s renderer table, in its order. `Count` is the size of a per-kind array and never a
/// kind.
enum class RendererKind : u8 {
    /// Camera-facing or axis-aligned quads. `publish_sprites`, and `src/rendering/particles/`.
    Sprite = 0,
    /// Instances in the renderer's GPU scene. `publish_mesh_instances`.
    Mesh,
    /// Connected strips through particle chains, with width and twist over length.
    Ribbon,
    /// Point-to-point strips with sag and noise.
    Beam,
    /// Per-particle history ribbons.
    Trail,
    /// Projected decals.
    Decal,
    /// Light instances contributing to scene lighting.
    Light,
    /// Volumetric primitives compositing into the volumetric pipeline.
    Volume,
    Count,
};

[[nodiscard]] const char* renderer_kind_name(RendererKind kind) noexcept;

inline constexpr u32 kRendererKindCount = static_cast<u32>(RendererKind::Count);

/// `vfx-system`: "Transparent particles SHALL support sorting modes: none, by distance to camera,
/// by age, by a custom key, and per-emitter draw order."
enum class SortingMode : u8 {
    None = 0,
    Distance,
    Age,
    /// A `sort_key` attribute the render stage writes.
    CustomKey,
    /// The emitter's own `draw_order`, with no per-particle ordering at all.
    EmitterOrder,
};

[[nodiscard]] const char* sorting_mode_name(SortingMode mode) noexcept;

/// "lighting participation (unlit, vertex, per-pixel)".
enum class LightingParticipation : u8 { Unlit = 0, Vertex, PerPixel };

/// What an emitter declares about how it is drawn. Every member is a bullet of `vfx-system`'s
/// "Renderers SHALL support" list, and there are no others — a field here that the requirement does
/// not name would be a field the renderer's layer has no reason to read.
struct RendererDecl {
    RendererKind kind = RendererKind::Sprite;
    /// The material this emitter's rows carry. Opaque to this module: a material identifier is
    /// `material-compiler`'s and resolving one here would put a second resolver in the engine.
    u32 material = 0;
    SortingMode sorting = SortingMode::Distance;
    LightingParticipation lighting = LightingParticipation::Unlit;
    bool casts_shadows = false;
    bool receives_shadows = false;
    /// Whether the rows carry the previous frame's position. Off costs nothing and produces
    /// `motion_valid` false on every row, which is what a frame with no temporal pass wants.
    bool motion_vectors = true;
    /// Per-renderer LOD, added to the budget controller's own `lod_bias`.
    u8 lod_bias = 0;
    /// `SortingMode::EmitterOrder`'s key, and the tie-break for every other mode.
    u16 draw_order = 0;

    // --- Kind-specific, and each one is read by exactly one publication -----------------------

    /// `Ribbon` and `Trail`: the strip's width at the head, scaled by the particle's `size`.
    f32 width = 1.0F;
    /// `Ribbon`: radians of twist accumulated along the whole chain. "with width and twist over
    /// length".
    f32 twist = 0.0F;
    /// `Trail`: how many past positions a particle's ribbon carries. Bounded by
    /// `kMaxTrailHistory`, because a history the author can grow without limit is a per-particle
    /// allocation on the frame path.
    u8 trail_history = 4;
    /// `Beam`: how far the midpoint sags towards -Y, in metres. "with sag and noise".
    f32 beam_sag = 0.0F;
    /// `Beam`: the amplitude of the lateral noise along the beam, in metres.
    f32 beam_noise = 0.0F;
    /// `Beam` and `Ribbon`: segments the strip is divided into.
    u8 segments = 8;
    /// `Light`: the hard per-frame count budget. `vfx-system`: particle lights "SHALL participate
    /// in clustered light assignment subject to a HARD PER-FRAME COUNT BUDGET, degraded by the
    /// budget controller like any other VFX cost."
    u16 max_lights = 32;
    /// `Decal`: how far the projection reaches along the particle's -Y, in metres.
    f32 decal_depth = 1.0F;
    /// `Volume`: the primitive's extinction coefficient, per metre.
    f32 volume_extinction = 1.0F;
};

/// The longest history a `Trail` may declare. Fixed because the publication holds it in a ring the
/// caller owns, and an unbounded one would be an allocation a frame.
inline constexpr u32 kMaxTrailHistory = 8;

// --- The rows each kind publishes ------------------------------------------------------------

/// What every published row carries, whatever its kind.
///
/// CAMERA-RELATIVE, like `ParticleInstance` and `MeshParticleInstance` and for the same reason:
/// there is no world-space position in any of these structures and nowhere to put one, so the
/// rebase cannot be got wrong in one publication and right in another.
struct RenderRowCommon {
    f32 position[3] = {0.0F, 0.0F, 0.0F};
    /// Where this row was last frame. Meaningful only when `motion_valid`; see the note at the top
    /// of this file about why a row that always carried one would smear.
    f32 previous_position[3] = {0.0F, 0.0F, 0.0F};
    /// Linear, un-premultiplied radiance — `emission` already folded in, as `publish_sprites` does.
    f32 color[4] = {1.0F, 1.0F, 1.0F, 1.0F};
    u32 material = 0;
    /// The slot this row came from, so a caller can correlate a row with a particle without a
    /// second walk.
    u32 particle = 0;
    /// False on the frame a particle spawned, on the frame its slot was re-used, and after a
    /// teleport: interpolation is suppressed rather than smeared.
    bool motion_valid = false;
};

/// One vertex of a `Ribbon` or `Trail` strip, in strip order.
struct RibbonVertex {
    RenderRowCommon row;
    /// Half-width, in metres.
    f32 width = 0.0F;
    /// Radians about the strip's tangent at this vertex.
    f32 twist = 0.0F;
    /// Where along the strip this vertex is, in [0, 1]. What a material's gradient reads.
    f32 along = 0.0F;
    /// Which strip this vertex belongs to. A strip is a maximal run of equal values, and a CHAIN
    /// BROKEN BY A KILL PRODUCES TWO — see `publish_ribbons`.
    u32 strip = 0;
};

/// One vertex of a `Beam`.
struct BeamVertex {
    RenderRowCommon row;
    f32 width = 0.0F;
    f32 along = 0.0F;
    u32 beam = 0;
};

/// One projected decal.
struct DecalInstance {
    RenderRowCommon row;
    /// The projector's axis, unit length. Derived from the particle's velocity where it has one and
    /// from world -Y where it does not, because a decal with no direction is a decal projected the
    /// way the ground faces.
    f32 axis[3] = {0.0F, -1.0F, 0.0F};
    /// Half-extent across the projection, in metres.
    f32 radius = 0.0F;
    /// How far the projection reaches along `axis`, in metres.
    f32 depth = 0.0F;
    /// Opacity, already faded by the particle's own colour alpha.
    f32 opacity = 1.0F;
};

/// One light emitted by a particle.
struct LightInstance {
    RenderRowCommon row;
    /// Linear radiant intensity — the particle's `color` scaled by its `emission`, which is why
    /// `RenderRowCommon::color` already carries the product.
    f32 intensity[3] = {0.0F, 0.0F, 0.0F};
    /// Metres. Derived from the intensity and a cut-off, so a dim particle's light does not claim a
    /// cluster it contributes nothing to.
    f32 radius = 0.0F;
    /// What the budget controller and the hard count budget ranked this light by: larger survives.
    f32 rank = 0.0F;
};

/// One volumetric primitive.
struct VolumeInstance {
    RenderRowCommon row;
    /// Metres.
    f32 radius = 0.0F;
    /// Per metre.
    f32 extinction = 0.0F;
    /// Linear, and separate from `row.color` because a volume scatters and emits independently.
    f32 scattering[3] = {0.0F, 0.0F, 0.0F};
};

// --- The history a motion vector needs ----------------------------------------------------------

/// What the last publication put where, so this one can produce a motion vector and know when not
/// to.
///
/// OWNED BY THE CALLER and not by the world, for the reason `runtime.h`'s seam note gives: a
/// simulation that held the renderer's per-frame state would be a renderer. A host keeps one of
/// these beside its publication buffers and passes it in; two cameras publishing the same world
/// keep two, which is correct and would not be if the world held one.
class PublicationHistory {
public:
    explicit PublicationHistory(Allocator& allocator) noexcept : entries_(allocator) {}

    PublicationHistory(const PublicationHistory&) = delete;
    PublicationHistory& operator=(const PublicationHistory&) = delete;
    PublicationHistory(PublicationHistory&&) noexcept = default;
    PublicationHistory& operator=(PublicationHistory&&) noexcept = default;

    /// Size it for one world's blocks. Called once; a publication that resized here would allocate
    /// on the frame path.
    [[nodiscard]] Status resize(u32 slots, u32 history) noexcept;

    /// Forget everything. What a teleport, a load or a camera cut does — and the reason it exists
    /// is that all three are discontinuities, and a motion vector across one is the smear the
    /// requirement names.
    void reset() noexcept;

    [[nodiscard]] u32 history_depth() const noexcept { return history_; }
    [[nodiscard]] u32 slots() const noexcept { return slots_; }

    // --- What a publication calls. Public because the publications are free functions, the same
    // arrangement `SimulationWorld` makes for its executor.

    /// The position `slot` published `age` frames ago, and whether it published at all.
    [[nodiscard]] bool sample(u32 slot, u32 age, f32 out[3]) const noexcept;
    /// Record this frame's position for `slot`, shifting its history.
    void record(u32 slot, const f32 position[3]) noexcept;
    /// Mark every slot absent, before a publication records the ones that are present. A slot that
    /// is not recorded this frame has no motion vector next frame, which is how a kill and a
    /// re-use both become a suppression without either being detected as such.
    void begin_frame() noexcept;

private:
    struct Entry {
        f32 positions[kMaxTrailHistory][3] = {};
        /// One bit a history step: whether that step was recorded.
        u8 present = 0;
    };

    Array<Entry> entries_;
    u32 slots_ = 0;
    u32 history_ = 1;
};

// --- The publications ----------------------------------------------------------------------------
//
// Every one takes the emitter's `RendererDecl` explicitly rather than reading it off the compiled
// system, because the DECLARATION is authoring data that a cook carries and a host may override per
// instance — a distant effect that dropped from `PerPixel` to `Unlit` is the budget controller's
// `feature_level` lever arriving here, and a publication that read a fixed declaration could not
// express it.

/// What one publication did, beyond `PublishReport`'s counts.
struct RenderPublishReport {
    PublishReport base;
    /// Strips, beams, decals, lights or volumes produced — as against `base.particles`, which is
    /// vertices for the strip kinds.
    u32 primitives = 0;
    /// Chains that ended because the next particle in the chain was dead. `vfx-system`: "WHEN
    /// particles in a ribbon chain are killed mid-chain THEN the ribbon SHALL TERMINATE CLEANLY
    /// rather than connecting across the gap." A number, so that the termination is measurable
    /// rather than a claim about a picture.
    u32 chain_breaks = 0;
    /// Rows whose motion vector was suppressed because the slot did not publish last frame.
    u32 motion_suppressed = 0;
    /// Lights the hard per-frame budget refused. Counted, never silently dropped.
    u32 lights_over_budget = 0;
};

/// `Ribbon`: connected strips through particle chains.
///
/// A CHAIN IS A RUN OF CONSECUTIVE LIVE SLOTS, which is the ordering the simulation already has:
/// the pool allocates ascending and the compaction lists ascending, so a chain's particles are
/// adjacent slots in spawn order. A dead slot ends the run and the next live one begins a new
/// strip, which is the "terminate cleanly rather than connecting across the gap" requirement made
/// structural rather than checked.
[[nodiscard]] Status publish_ribbons(const SimulationWorld& world, const RendererDecl& decl,
                                     const Vec3& camera_position, u32 capacity,
                                     PublicationHistory& history, Array<RibbonVertex>& out,
                                     RenderPublishReport& report) noexcept;

/// `Trail`: per-particle history ribbons. One strip a particle, through the positions
/// `PublicationHistory` remembers — so a trail is exactly as long as the history the caller sized
/// and never longer, and a particle whose slot was re-used starts a new one.
[[nodiscard]] Status publish_trails(const SimulationWorld& world, const RendererDecl& decl,
                                    const Vec3& camera_position, u32 capacity,
                                    PublicationHistory& history, Array<RibbonVertex>& out,
                                    RenderPublishReport& report) noexcept;

/// `Beam`: point-to-point strips with sag and noise. The far end is the particle's `beam_end`
/// attribute where it has one, and the emitter's origin where it does not — so a beam authored
/// without one is a beam back to where it was spawned rather than a diagnostic.
[[nodiscard]] Status publish_beams(const SimulationWorld& world, const RendererDecl& decl,
                                   const Vec3& camera_position, u32 capacity,
                                   PublicationHistory& history, Array<BeamVertex>& out,
                                   RenderPublishReport& report) noexcept;

/// `Decal`: projected decals, one a particle.
[[nodiscard]] Status publish_decals(const SimulationWorld& world, const RendererDecl& decl,
                                    const Vec3& camera_position, u32 capacity,
                                    PublicationHistory& history, Array<DecalInstance>& out,
                                    RenderPublishReport& report) noexcept;

/// `Light`: light instances, subject to the HARD per-frame count budget.
///
/// `levers` is the budget controller's, and it is not decoration: `vfx-system` requires particle
/// lights to be "degraded by the budget controller like any other VFX cost", and the degradation is
/// `count_cap_scale` applied to `RendererDecl::max_lights`. What survives the budget is ranked by
/// radiance and distance rather than by slot, so the lights a frame keeps are the ones a viewer
/// would notice losing.
[[nodiscard]] Status publish_lights(const SimulationWorld& world, const RendererDecl& decl,
                                    const BudgetLevers& levers, const Vec3& camera_position,
                                    u32 capacity, PublicationHistory& history,
                                    Array<LightInstance>& out,
                                    RenderPublishReport& report) noexcept;

/// `Volume`: volumetric primitives.
[[nodiscard]] Status publish_volumes(const SimulationWorld& world, const RendererDecl& decl,
                                     const Vec3& camera_position, u32 capacity,
                                     PublicationHistory& history, Array<VolumeInstance>& out,
                                     RenderPublishReport& report) noexcept;

}  // namespace cy::vfx
