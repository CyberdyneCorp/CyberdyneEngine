#pragma once
// samples/08-vertical-slice — a playable game, assembled out of M8.b's systems. Section 12.
//
// ================================================================================================
// WHAT THIS FILE DECLARES, AND WHY IT IS ONE OBJECT RATHER THAN A FRAMEWORK
// ================================================================================================
//
// `Slice` is the game: a level, characters that animate and think, abilities with effects, a
// heads-up interface, sound and a 2D menu. It is one object with one `tick()` because that is what
// the milestone's exit criteria are about — the systems meeting in one loop — and a sample that
// invented a component framework to hold them would be demonstrating the framework.
//
// The systems it drives are the real ones. Nothing here reimplements a solver, a scheduler or a
// compiler; every graph is authored with `cy::graph::Graph` and compiled by the lowering its own
// specification names, and every per-tick call below is a batch call over packed state.
//
// ================================================================================================
// THE SEAMS M8.c ATTACHES TO, STATED HERE SO THEY ARE NOT REDISCOVERED
// ================================================================================================
//
//   * `CueLedger` — `Slice::cues()` is the activation pipeline's own cue stream, already carrying
//     (activation, cue tag, simulation point). A particle system consumes it; nothing else has to
//     change. `vfx-system` is M8.c.
//   * `Slice::director()` — the camera rig is evaluated through `cy::graph::camera`'s compiled
//     program every tick, with its inputs named. A sequence drives cameras by supplying those
//     inputs, which is `sequencing-and-cinematics`' "does not write camera transforms".
//   * `Presentation::frame()` — the render graph is declared and (with a device) executed here.
//     A particle pass is a `FrameSinks::passes[...]` callback and a new spatial index entry.
//
// ================================================================================================
// WHAT IS DELIBERATELY NOT HERE
// ================================================================================================
//
// No cinematic and no particles: M8.c's, and M8.c extends THIS slice. No physics solver: the level
// is static geometry and the characters are steered by the crowd, which is what `navigation`'s own
// "this is a stand-in for the character controller" note describes; `physics` closed at M8.a and
// adding a body per character would measure Jolt rather than this milestone.

#include <cy/animation/clip.h>
#include <cy/animation/evaluate.h>
#include <cy/animation/lod.h>
#include <cy/animation/skeleton.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/matrix.h>
#include <cy/core/math/shapes.h>
#include <cy/core/memory/array.h>
#include <cy/ecs/world.h>
#include <cy/gameplay/abilities/activation.h>
#include <cy/gameplay/rules.h>
#include <cy/gameplay/tags.h>
#include <cy/gameplay/teams.h>
#include <cy/gameplay/time.h>
#include <cy/graph/cybergraph.h>
#include <cy/graph/lower_behaviour.h>
#include <cy/graph/lower_camera.h>
#include <cy/graph/lower_pose.h>
#include <cy/graph/lower_script.h>
#include <cy/navigation/crowd.h>
#include <cy/navigation/navmesh.h>
#include <cy/rendering/scene/asset_binding.h>
#include <cy/rendering/scene/components.h>
#include <cy/rendering/scene/extract.h>
#include <cy/scene/tree.h>
#include <cy/servers/render/snapshot.h>

namespace cy::ai {
class AiRuntime;
class PerceptionScheduler;
}  // namespace cy::ai

namespace cy::sample::slice {

using cy::Aabb;
using cy::Allocator;
using cy::Array;
using cy::Error;
using cy::Expected;
using cy::f32;
using cy::f64;
using cy::Name;
using cy::Span;
using cy::Status;
using cy::Transform;
using cy::u32;
using cy::u64;
using cy::u8;
using cy::Vec3;

/// The meshes the level and its characters reference. A slot in the render server's table, and the
/// silhouette the artefact's picture draws — which is the whole of what M8.a could not do.
enum class MeshKind : u32 {
    Ground = 0,
    Wall,
    Crate,
    Pillar,
    Character,
    Count,
};

[[nodiscard]] const char* mesh_kind_name(MeshKind kind) noexcept;

/// The materials the level and its characters reference. A slot in the GPU material table, and
/// what the artefact's picture colours a shape by — which is the resolved handle rather than a
/// palette this sample keeps beside the geometry.
///
/// The two teams are two materials over one mesh, which is why this is not just `MeshKind` again:
/// what a thing IS and what it is PAINTED WITH are different references on the same component, and
/// `bind_render_assets` resolves them separately.
enum class MaterialKind : u32 {
    Ground = 0,
    Wall,
    Crate,
    Pillar,
    TeamBlue,
    TeamRed,
    Count,
};

[[nodiscard]] const char* material_kind_name(MaterialKind kind) noexcept;

/// What one run is asked to do.
struct Options {
    /// Characters in the level. The exit criterion's figure is 8000; the default is what a person
    /// running the sample by hand wants to watch.
    u32 agents = 256;
    u32 ticks = 240;
    /// Concurrent gameplay effects. The exit criterion's figure is 100.
    u32 effects = 100;
    /// Assemble the render frame. Off is the control that shows the simulation half alone.
    bool render = true;
    /// Introduce a per-entity virtual tick, so the "no graph is interpreted" audit has something to
    /// find. THE NEGATIVE CONTROL: the run must then report a gap and exit non-zero.
    bool interpret_control = false;
    u64 seed = 0x5EEDBEEFULL;
};

/// One character, as the slice stores it. Packed arrays rather than an object per character: every
/// system below takes a span.
struct Characters {
    explicit Characters(Allocator& allocator) noexcept;

    Array<cy::ecs::Entity> entities;
    Array<Vec3> positions;
    Array<Vec3> forward;
    Array<Vec3> goals;
    Array<cy::navigation::CrowdAgentId> crowd;
    Array<u32> animation_slot;
    Array<u8> team;

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(entities.size()); }
};

/// Everything one tick measured, so a figure a run prints is a module's own report.
struct TickCosts {
    f64 think_us = 0.0;
    f64 sense_us = 0.0;
    f64 navigate_us = 0.0;
    f64 animate_us = 0.0;
    /// The activation pipeline's own cost, which is not the effect advance's and must not be
    /// folded into it: the exit criterion names a hundred CONCURRENT EFFECTS, and that is
    /// `EffectSystem::advance` and nothing else.
    f64 abilities_us = 0.0;
    f64 effects_us = 0.0;
    f64 interface_us = 0.0;
    f64 frame_us = 0.0;

    [[nodiscard]] f64 total_us() const noexcept {
        return think_us + sense_us + navigate_us + animate_us + abilities_us + effects_us +
               interface_us + frame_us;
    }
};

/// The audit `docs/ROADMAP.md` asks for by name: "No graph is interpreted at runtime — an audit
/// finds no per-entity virtual tick in any graph consumer."
///
/// It is three separate checks and each can fail on its own:
///   1. every authored graph audits CLEAN AND COMPLETE (`AuditReport::complete` is the third
///      answer; a gate that read `missing` alone would go green on a plugin that failed to load);
///   2. no compilation happens inside the tick loop — the counter is incremented by the slice's own
///      compile calls and is read before and after the loop;
///   3. no per-entity state type of any consumer is polymorphic. A virtual `tick()` per entity
///      needs a vtable pointer in the per-entity record, so `std::is_polymorphic_v` over the state
///      types IS the check, and it is reported per type rather than as one boolean.
struct InterpretationAudit {
    u32 graphs_audited = 0;
    u32 graphs_incomplete = 0;
    u32 graphs_failed = 0;
    u32 compilations_before_loop = 0;
    u32 compilations_during_loop = 0;
    u32 state_types_checked = 0;
    u32 state_types_polymorphic = 0;
    /// The names of the polymorphic ones, so a failure says which.
    Array<Name> offenders;

    explicit InterpretationAudit(Allocator& allocator) noexcept : offenders(allocator) {}

    [[nodiscard]] bool passed() const noexcept {
        return graphs_audited > 0 && graphs_incomplete == 0 && graphs_failed == 0 &&
               compilations_during_loop == 0 && state_types_checked > 0 &&
               state_types_polymorphic == 0;
    }
};

/// What the whole run claims. `main.cpp` prints it as `key = value`; `slice.py` checks it.
struct Report {
    u32 agents = 0;
    u32 ticks = 0;
    u32 level_entities = 0;
    u32 level_triangles = 0;
    u32 navmesh_polys = 0;
    u32 navmesh_tiles = 0;
    bool navmesh_recast = false;

    // The compiled programs, each from an authored CyberGraph.
    u64 script_digest = 0;
    u64 ability_digest = 0;
    u64 pose_digest = 0;
    u64 behaviour_digest = 0;
    u64 rig_digest = 0;
    u64 rig_ir_digest = 0;
    u32 rig_query_calls = 0;
    u32 rig_queries = 0;

    // What the simulation did, summed over the run.
    u64 think_instructions = 0;
    u32 agents_thought = 0;
    u32 agents_starved = 0;
    /// The AI LOD distribution on the last tick, Full/Reduced/Minimal/Statistical. The evidence
    /// that "cost is bounded by configuration" is a configuration and not a wish.
    u32 agents_at_tier[4] = {};
    u32 perception_queries = 0;
    u32 crowd_adjusted = 0;
    u32 paths_found = 0;
    u32 clips_sampled = 0;
    u32 joints_sampled = 0;
    u32 poses_evaluated = 0;
    f32 root_motion_travelled = 0.0F;

    u32 activations_committed = 0;
    u32 activations_refused = 0;
    u32 effects_peak = 0;
    u32 effects_expired = 0;
    u32 cues_emitted = 0;
    u32 cues_suppressed = 0;

    // The interface, the menu and the sound.
    u32 ui_elements = 0;
    u32 ui_primitives = 0;
    u32 ui_batches = 0;
    bool ui_accessible = false;
    u32 ui_findings = 0;
    u32 menu_draws = 0;
    u32 menu_batches = 0;
    u32 audio_sources = 0;
    u32 audio_full_acoustic = 0;
    u32 audio_virtual = 0;
    u32 music_transitions = 0;

    // The assembled frame.
    bool frame_assembled = false;
    u32 frame_draws = 0;
    u32 frame_visible = 0;
    u32 frame_lights = 0;
    u32 frame_passes = 0;
    u32 frame_material_slots = 0;
    u32 frame_draws_without_material = 0;
    /// Distinct MESH handles over the frame's own draw list, read back out of the scene index.
    /// It must equal `mesh_assets`: a resolver that answered one handle for every reference would
    /// leave `meshes_unresolved` at zero and this at one, which is M8.a's box wearing a handle.
    u32 frame_distinct_meshes = 0;
    u32 frame_clusters = 0;
    u32 frame_shadow_pages = 0;
    u32 meshes_bound = 0;
    u32 meshes_unresolved = 0;
    u32 materials_bound = 0;
    /// Distinct mesh assets the level interned. Two wall shapes are two meshes — see internals.h.
    u32 mesh_assets = 0;

    // The measured half. Medians over `Options::repeats` runs of the loop.
    f64 simulation_us_median = 0.0;
    f64 simulation_us_worst = 0.0;
    /// Per phase, so "cost is bounded by configuration" can say WHICH cost. A total alone cannot
    /// tell a quadratic neighbour search from an expensive animation evaluation.
    f64 think_us_median = 0.0;
    f64 sense_us_median = 0.0;
    f64 navigate_us_median = 0.0;
    f64 animate_us_median = 0.0;
    f64 abilities_us_median = 0.0;
    f64 per_agent_us_median = 0.0;
    f64 effects_us_median = 0.0;
    f64 interface_us_median = 0.0;
    f64 frame_us_median = 0.0;

    /// The determinism digest: everything the simulation decided, folded in tick order.
    u64 state_digest = 0;
    /// The same digest recomputed by a second, independently built slice in this process.
    u64 replay_digest = 0;

    bool audit_passed = false;
    u32 audit_graphs = 0;
    u32 audit_polymorphic = 0;
    u32 audit_compilations_during_loop = 0;
};

/// THE BUDGETS THIS SLICE DECLARES, in one place, printed by the program and checked by the driver.
///
/// `docs/ROADMAP.md`'s M8.b row: "Cost is bounded by configuration: 8,000 agents and 100 concurrent
/// effects hold their budgets", and "the interface holds its frame budget". A budget nobody wrote
/// down cannot be held, so these are the numbers, with the reasoning that fixed each.
///
/// THEY ARE SHAPE BOUNDS, NOT MACHINE NUMBERS. Every one is set well above what this machine
/// measures, because a bound tight enough to be a measurement of the host fails on a loaded build
/// agent and teaches everyone to ignore it. What actually catches a regression is
/// `kLinearityFactor` beside them: the per-agent cost at four times the population, which a
/// quadratic cannot hold and a slow machine does not affect.
struct Budgets {
    /// One simulation tick at the scale figure — think, sense, navigate, animate, act. Measured
    /// 10.2 ms at 8,000 agents in a development build on the reference machine.
    static constexpr f64 kSimulationUs = 25000.0;
    /// `EffectSystem::advance` over the hundred concurrent effects the criterion names, which is
    /// not the activation pipeline's cost and must not be folded into it. Measured 0.4 µs.
    static constexpr f64 kEffectsUs = 25.0;
    /// One frame of interface: measure, arrange, flatten, budget, audit. Measured 1.9 µs.
    static constexpr f64 kInterfaceUs = 2000.0;
    /// THE ONE THAT CATCHES A QUADRATIC. Four times the agents may cost at most this much more
    /// per agent. A quadratic gives four; a linear one gives one. Measured 1.15.
    static constexpr f64 kLinearityFactor = 2.0;
    /// The population the scale act runs, and the one the criterion names.
    static constexpr u32 kScaleAgents = 8000;
    /// The population it is compared against, a quarter of it.
    static constexpr u32 kBaselineAgents = 2000;
};

/// A shape the artefact's picture draws: one visible instance, projected by the engine's own
/// matrices, with the mesh it actually resolved to.
struct ShotShape {
    /// The eight projected corners of the instance's world bounds, in pixels, plus its depth.
    f32 corners[8][2] = {};
    f32 depth = 0.0F;
    f32 shade = 1.0F;
    MeshKind kind = MeshKind::Ground;
    /// The material slot the frame resolved this draw to — read back out of the scene index, not
    /// out of a table beside the geometry.
    u32 material = 0;
};

/// The presentation half: the interface, the menu, the sound and the frame.
class Presentation;

/// The game.
class Slice {
public:
    explicit Slice(Allocator& allocator) noexcept;
    ~Slice();

    Slice(const Slice&) = delete;
    Slice& operator=(const Slice&) = delete;

    /// Build the level, compile every graph, and place the characters. Nothing after this
    /// allocates a program.
    [[nodiscard]] Status build(const Options& options) noexcept;

    /// Advance one simulation tick: think, sense, navigate, animate, act, present.
    [[nodiscard]] Status tick(TickCosts& costs) noexcept;

    /// Run `options.ticks` ticks `options.repeats` times, filling the report's measured half.
    [[nodiscard]] Status run() noexcept;

    /// Run the audit. Called after the loop, so `compilations_during_loop` means something.
    [[nodiscard]] Status audit(InterpretationAudit& out) const noexcept;

    [[nodiscard]] const Report& report() const noexcept { return report_; }
    [[nodiscard]] Report& report() noexcept { return report_; }
    [[nodiscard]] u64 digest() const noexcept { return digest_; }
    [[nodiscard]] const Options& options() const noexcept { return options_; }
    [[nodiscard]] Allocator& allocator() const noexcept { return *allocator_; }

    /// The shapes the picture draws, filled by the last `tick()` that assembled a frame.
    [[nodiscard]] Span<const ShotShape> shot_shapes() const noexcept;
    /// The heads-up interface's own flattened primitives, and the menu's own batched instances.
    [[nodiscard]] Status shot_interface(Array<f32>& rects, Array<u32>& colours) const noexcept;
    [[nodiscard]] Status shot_menu(Array<f32>& rects, Array<u32>& colours) const noexcept;

private:
    struct Level;
    struct Brain;
    struct Kit;

    [[nodiscard]] Status build_level() noexcept;
    [[nodiscard]] Status add_prop(Vec3 at, MeshKind kind, MaterialKind paint, Vec3 half) noexcept;
    /// Project what the frame drew, for the picture. Filled by the last `tick()` that assembled.
    [[nodiscard]] Status build_shot() noexcept;
    /// The silhouette a draw is drawn with, derived from the MESH HANDLE THE FRAME PUBLISHED
    /// rather than from this sample's record of what it authored. See shot.cpp.
    [[nodiscard]] MeshKind kind_of(cy::render::MeshHandle mesh) const noexcept;
    [[nodiscard]] Status build_navmesh() noexcept;
    [[nodiscard]] Status build_programs() noexcept;
    [[nodiscard]] Status build_characters() noexcept;
    [[nodiscard]] Status build_abilities() noexcept;

    [[nodiscard]] Status think(TickCosts& costs) noexcept;
    [[nodiscard]] Status navigate(TickCosts& costs) noexcept;
    [[nodiscard]] Status animate(TickCosts& costs) noexcept;
    [[nodiscard]] Status act(TickCosts& costs) noexcept;
    [[nodiscard]] Status publish() noexcept;

    void fold(u64 value) noexcept;
    void fold_tick() noexcept;

    Allocator* allocator_ = nullptr;
    Options options_;
    Report report_;
    u64 digest_ = 0;
    u64 tick_ = 0;
    u32 compilations_ = 0;
    bool in_loop_ = false;
    u32 compilations_in_loop_ = 0;

    Level* level_ = nullptr;
    Brain* brain_ = nullptr;
    Kit* kit_ = nullptr;
    Presentation* presentation_ = nullptr;
    Characters characters_;
    Array<ShotShape> shot_;
};

/// Draw the picture: the level and its characters as the frame's own draw list projected them, the
/// heads-up interface as its own flattened primitives, and the menu as its own batched instances.
///
/// Written as a plain text description the driver rasterises, for the reason README.md gives at
/// length: the assembly hands a pass's record callback to its CALLER, this sample supplies none,
/// and a second renderer beside `samples/03-first-light`'s would be a second renderer.
[[nodiscard]] Status write_shot_data(const Slice& slice, const char* path) noexcept;

}  // namespace cy::sample::slice
