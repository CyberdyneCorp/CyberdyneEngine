#pragma once
// The camera server: definitions, rigs, stacks, the impulse bus, and evaluation. Tasks 4.3.1–4.3.3.
//
// `camera-system` reaches **Seed** at M4 — "interfaces, data model and invariants exist; dependents
// can be built against it". What that means concretely, and what it does not, is at the bottom of
// this comment.
//
// ================================================================================================
// THE ONE RULE THAT SHAPES EVERY SIGNATURE BELOW
// ================================================================================================
//
// A CAMERA IS NOT A SCENE OBJECT. `camera-system`: a camera "SHALL be addressable as a lightweight
// handle and data, and SHALL NOT require an ECS entity, a node, or a base class to exist", and
// "There SHALL NOT be a camera base class intended for subclassing to produce camera behaviours."
//
// So: no `Camera` class to inherit, no virtual `update()`, no entity in any parameter. A rig is a
// handle into a pool; a definition is a compiled program; an evaluated camera is a value. `src/
// servers/` is layer 2, so this file *cannot* name an entity even if somebody wanted to — the layer
// checker fails the build — and that is the requirement being kept by the build rather than by a
// reviewer.
//
// The consequence a caller feels: the camera server cannot resolve a target. Whoever owns the world
// resolves each binding into a `TargetSample` and passes the samples into `evaluate()`. That is not
// an inconvenience worked around; it is `camera-system`'s "rig nodes SHALL NOT retain raw pointers
// to component data across frames" made impossible to violate.
//
// ================================================================================================
// GAMEPLAY NEVER WRITES A CAMERA TRANSFORM
// ================================================================================================
//
// There is no `set_pose()`. There is `apply_intent()`, `emit_impulse()`, `set_target()` and
// `set_parameter()`. The single exception the requirement allows — "Direct pose control SHALL be
// available only to low-level debug and custom node code" — is `override_pose()`, which is named so
// that it reads as an override in a call site and which sets `EvaluatedCamera::pose_overridden` so
// that it reads as one in the inspector too.
//
// ================================================================================================
// SETTINGS ARE APPLIED IN ONE PLACE
// ================================================================================================
//
// `camera-system`: "Settings SHALL be applied in **one place** — as processors, modifiers, or rig
// parameters — and SHALL NOT be duplicated independently by input and camera implementations", with
// the scenario "**WHEN** a player changes look sensitivity **THEN** it SHALL apply once in the
// pipeline, and no camera code SHALL apply it a second time."
//
// The division this module keeps to: LOOK SENSITIVITY, INVERSION AND AIM ASSISTANCE BELONG TO THE
// INPUT PIPELINE and are already in `CameraIntent::look` by the time it arrives here — `set_
// settings()` carries them only so that a diagnostic can report what the pipeline was configured
// with. What the CAMERA applies, because there is nowhere else it could be applied, is the shake
// scale, the field-of-view override, the collision strength and motion reduction. Each is read
// exactly once, in `evaluate()`, and passed into `RigEvaluationInput` — a rig node that multiplied
// by a sensitivity again would be the defect the requirement names.
//
// ================================================================================================
// WHAT SEED MEANS HERE — WHAT IS REAL, AND WHAT IS AN INTERFACE WITH A SMALL IMPLEMENTATION
// ================================================================================================
//
// REAL AND EXERCISED: the four separated concepts and their handles; the rig graph, its compiler
// and its diagnostics; follow, orbit, offset, look-at, lens, noise and constraint evaluation;
// frame-rate independent smoothing; the lens model with both authoring forms and the blend rule;
// the camera stack with priorities, weights, per-channel blend policies and the contribution
// report; cuts and the history identity they invalidate; the listener anchor and the streaming
// source, derived; render view production into `cy::render::ViewDescription`; the impulse bus; the
// batched query protocol; the custom-node extension point.
//
// INTERFACE ONLY, DELIBERATELY, AND NAMED SO NOBODY MISTAKES IT FOR MORE: collision and occlusion
// *responses* apply results the caller supplies, because the physics server that would answer them
// arrives beside this module and the batching contract is the part that had to exist first (M6
// resolves them for real). Framing, camera volumes, the strategy camera, the director camera and
// screen/world projection are named by `camera-system` and are M8's — none of them is stubbed here,
// because a stub that returns a plausible value is worse than an absent function that a caller
// cannot call.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/handle_pool.h>
#include <cy/servers/camera/camera.h>
#include <cy/servers/camera/handles.h>
#include <cy/servers/camera/lens.h>
#include <cy/servers/camera/rig.h>
#include <cy/servers/camera/stack.h>
#include <cy/servers/camera/view.h>

#include <utility>

namespace cy::camera {

/// When a rig is evaluated. `camera-system` — "Evaluation timing": simulation, render, or hybrid,
/// with "**Hybrid SHALL be the recommended default** for ordinary gameplay cameras".
enum class EvaluationMode : u8 {
    /// On the fixed tick, reproducible. Must not depend on wall-clock time, which the interface
    /// enforces: `evaluate()` takes its delta as a parameter and this module reads no clock.
    Simulation = 0,
    /// Per frame, for visual smoothness. Never on a tick.
    Render,
    /// Target state from simulation, smoothing at render rate. The default.
    Hybrid,
    Count,
};

[[nodiscard]] const char* evaluation_mode_name(EvaluationMode mode) noexcept;

/// Player settings and the accessibility surface. See the header comment for the division of labour
/// between these and the input pipeline.
struct CameraSettings {
    /// Carried for reporting only: the INPUT pipeline applies these. See the header comment.
    f32 look_sensitivity = 1.0F;
    f32 aim_sensitivity = 1.0F;
    f32 gyroscopic_sensitivity = 1.0F;
    bool invert_y = false;
    bool edge_scrolling = true;

    /// Applied by this module, once, in `evaluate()`.
    f32 shake_scale = 1.0F;
    /// Zero means "no override". A non-zero value replaces the evaluated vertical field of view.
    f32 field_of_view_override_radians = 0.0F;
    f32 collision_strength = 1.0F;
    /// `camera-system`'s accessibility minimum. Motion reduction scales shake, head motion and
    /// camera acceleration together — "**WHEN** motion reduction is enabled **THEN** shake, head
    /// motion, and camera acceleration SHALL all respond" — so it is one flag and not three.
    bool reduce_motion = false;
    bool auto_centre = false;
};

/// What a rig is created with.
struct RigConfig {
    Name name;
    EvaluationMode mode = EvaluationMode::Hybrid;
    ListenerPolicy listener_policy = ListenerPolicy::AtCamera;
    /// Only read for `ListenerPolicy::Interpolated`.
    f32 listener_blend = 0.5F;
    /// This camera's importance, published on its streaming source and available to a view request.
    f32 importance = 1.0F;
    /// How far ahead the streaming source asks for content, in seconds of predicted motion.
    f32 prediction_seconds = 0.5F;
};

/// Sizes. Defaults are a game's; a test that took them would pay for capacity it does not use.
struct CameraServerConfig {
    u32 definition_capacity = 32;
    u32 rig_capacity = 32;
    u32 stack_capacity = 8;
    /// The impulse bus's ring. An impulse older than its duration is dropped at `advance()`.
    u32 impulse_capacity = 64;
    /// Collect a per-node trace on every evaluation. Off by default: the trace is what the
    /// inspector reads, and a game that is not inspecting should not pay for it.
    bool collect_trace = false;
};

/// Everything one evaluation needs from outside.
struct EvaluationContext {
    /// Seconds. The fixed tick's for a simulation evaluation, the frame's for a render one.
    f32 delta_seconds = 0.0F;
    u64 tick = 0;
    /// True when this is a fixed-tick evaluation. Checked against the rig's `EvaluationMode`: a
    /// `Simulation` rig evaluated off the tick, or a `Render` rig evaluated on it, is an error
    /// rather than a silently different camera.
    bool simulation = false;
    /// Bindings resolved by whoever owns the world, looked up by `stable_id`.
    Span<const TargetSample> targets;
    /// The controlled entity, for `ListenerPolicy::AtControlledEntity` and `Interpolated`.
    Transform controlled;
    Vec3 controlled_velocity{0.0F, 0.0F, 0.0F};
    /// The primary viewport's aspect, published on the streaming source.
    f32 aspect = 1.0F;
};

/// The camera server.
///
/// NOT THREAD-SAFE, and for the same reason `RenderServer` is not: it is mutated where intent is
/// applied and read where views are produced, which is one thread. Nothing here is safe from a
/// worker.
///
/// The four methods `runtime::Server` declares — `backend_name`, `initialize`, `shutdown`,
/// `is_null_backend` — are present with the same signatures, so the runtime's adapter is four
/// forwarding lines. This class does not derive from `runtime::Server`: that interface is at layer
/// 5 and this module is at layer 2, which is the same arrangement `RenderServer` documents.
class CameraServer {
public:
    explicit CameraServer(Allocator& allocator) noexcept;
    ~CameraServer();

    CameraServer(const CameraServer&) = delete;
    CameraServer& operator=(const CameraServer&) = delete;

    [[nodiscard]] const char* backend_name() const noexcept { return backend_name_; }
    [[nodiscard]] Status initialize() noexcept;
    void shutdown() noexcept;
    /// Always false, and a member rather than a constant so that the four methods keep
    /// `runtime::Server`'s exact signatures — the adapter is four forwarding lines and no
    /// decisions, which is the arrangement `RenderServer` documents. There is no third-party camera
    /// library to fall back from, and a camera system that produced no views would be a renderer
    /// with nothing to draw rather than a degraded mode.
    [[nodiscard]] bool is_null_backend() const noexcept { return null_backend_; }

    [[nodiscard]] Status configure(const CameraServerConfig& config) noexcept;
    [[nodiscard]] const CameraServerConfig& configuration() const noexcept { return config_; }

    // --- Definitions -------------------------------------------------------------------------

    /// Compile a definition and keep the program. The definition itself is not retained: what a rig
    /// executes is the program, and keeping the graph beside it would be a second description.
    [[nodiscard]] Expected<DefinitionHandle, Error> create_definition(
        const RigDefinition& definition) noexcept;
    void destroy_definition(DefinitionHandle handle) noexcept;
    [[nodiscard]] const RigProgram* program(DefinitionHandle handle) const noexcept;

    /// The registry a project registers custom node kinds into, before compiling a definition that
    /// names one.
    [[nodiscard]] RigNodeRegistry& node_registry() noexcept { return registry_; }
    [[nodiscard]] const RigNodeRegistry& node_registry() const noexcept { return registry_; }

    // --- Rigs --------------------------------------------------------------------------------

    [[nodiscard]] Expected<RigHandle, Error> create_rig(DefinitionHandle definition,
                                                        const RigConfig& config) noexcept;
    void destroy_rig(RigHandle handle) noexcept;
    [[nodiscard]] bool alive(RigHandle handle) const noexcept;
    [[nodiscard]] u32 live_rigs() const noexcept { return rigs_.size(); }

    /// Bind what the camera frames. Refused on a rig whose program has no `Target` node: a binding
    /// nothing reads is a configuration mistake that would otherwise be invisible.
    [[nodiscard]] Status set_target(RigHandle handle, const TargetBinding& binding) noexcept;
    [[nodiscard]] const TargetBinding* target(RigHandle handle) const noexcept;

    /// Accumulate intent for the next evaluation. Several sources may call this in one frame.
    [[nodiscard]] Status apply_intent(RigHandle handle, const CameraIntent& intent) noexcept;

    /// Cut. Resets the rig's smoothing, bumps its cut epoch — which invalidates the temporal
    /// history of every view derived from it — and publishes the reason.
    [[nodiscard]] Status cut(RigHandle handle, CutReason reason, bool anticipated = false,
                             f32 lead_seconds = 0.0F) noexcept;

    /// Direct pose control. Debug and custom node code only — see the header comment.
    [[nodiscard]] Status override_pose(RigHandle handle, const Transform& pose) noexcept;
    [[nodiscard]] Status clear_pose_override(RigHandle handle) noexcept;

    /// Evaluate one rig. The evaluated camera is owned by the server and is valid until the next
    /// evaluation of the same rig.
    [[nodiscard]] Expected<const EvaluatedCamera*, Error> evaluate(
        RigHandle handle, const EvaluationContext& context) noexcept;
    [[nodiscard]] const EvaluatedCamera* evaluated(RigHandle handle) const noexcept;

    /// The per-node trace of the last evaluation, when `CameraServerConfig::collect_trace` is on.
    [[nodiscard]] Span<const RigOpTrace> trace(RigHandle handle) const noexcept;

    // --- The impulse bus ---------------------------------------------------------------------

    /// Emit a disturbance. Every camera responds according to its own distance and occlusion, which
    /// is why the impulse carries a world position rather than a per-camera amplitude.
    [[nodiscard]] Status emit_impulse(const CameraImpulse& impulse) noexcept;
    /// Age the bus. Impulses whose duration has elapsed are dropped.
    void advance_impulses(f32 delta_seconds) noexcept;
    [[nodiscard]] usize live_impulses() const noexcept { return impulses_.size(); }

    // --- Stacks ------------------------------------------------------------------------------

    [[nodiscard]] Expected<StackHandle, Error> create_stack() noexcept;
    void destroy_stack(StackHandle handle) noexcept;
    [[nodiscard]] CameraStack* stack(StackHandle handle) noexcept;
    [[nodiscard]] const CameraStack* stack(StackHandle handle) const noexcept;

    /// Evaluate every rig in a stack, advance the blends, and blend the results.
    ///
    /// `report`, when given, receives one contribution per entry — which is how "the camera is not
    /// where I expect" is answered without a debugger.
    [[nodiscard]] Status evaluate_stack(StackHandle handle, const EvaluationContext& context,
                                        EvaluatedCamera& out,
                                        Array<StackContribution>* report) noexcept;

    // --- The batched query protocol ------------------------------------------------------------

    /// Open a frame's query batch. Clears the queries every camera published last frame.
    ///
    /// Explicit rather than inferred from a tick number: several cameras contribute to one batch —
    /// that is what makes it a batch — so the server cannot tell from one `evaluate()` call whether
    /// a frame has started. A caller that never calls it gets a batch that grows, which is visible
    /// in `queries().size()` rather than silent.
    void begin_frame() noexcept;

    /// The collision and occlusion queries this frame's evaluations published. Resolved by whoever
    /// owns a physics server and handed back through `submit_query_results()`. Batched by
    /// construction: a rig node appends here and never casts.
    [[nodiscard]] Span<const CameraQuery> queries() const noexcept { return queries_.span(); }
    [[nodiscard]] Status submit_query_results(Span<const CameraQueryResult> results) noexcept;

    // --- Settings ------------------------------------------------------------------------------

    [[nodiscard]] const CameraSettings& settings() const noexcept { return settings_; }
    void set_settings(const CameraSettings& settings) noexcept { settings_ = settings; }

private:
    /// One rig instance. Not exposed: a caller holds a handle, and everything it may ask is a
    /// method above. Exposing the record would be the first step back toward a camera object.
    struct Rig {
        DefinitionHandle definition;
        RigConfig config;
        TargetBinding binding;
        RigState state;
        CameraIntent intent;
        EvaluatedCamera evaluated;
        Array<RigOpTrace> trace;

        u32 cut_epoch = 0;
        CameraCut pending_cut;
        bool cut_requested = false;
        bool pose_override_active = false;
        Transform pose_override;
        /// Mixed into every view's history identity. Derived from the handle at creation, so two
        /// rigs never collide and one rig's identity survives its target changing.
        u64 history_seed = 0;

        Rig(Allocator& allocator, DefinitionHandle def, const RigConfig& cfg) noexcept
            : definition(def), config(cfg), trace(allocator) {}
    };

    struct DefinitionRecord {
        Name name;
        RigProgram program;

        DefinitionRecord(Name definition_name, RigProgram&& compiled) noexcept
            : name(definition_name), program(std::move(compiled)) {}
    };

    /// Everything `evaluate()` does after the rig and program have been resolved. Split out so that
    /// `evaluate()` is a lookup and a dispatch rather than one function doing both.
    [[nodiscard]] Status evaluate_resolved(Rig& rig, const RigProgram& compiled, RigHandle handle,
                                           const EvaluationContext& context) noexcept;
    /// The impulses that reach one camera, attenuated by distance and occlusion, written into
    /// `impulse_scratch_`. Returns the span.
    [[nodiscard]] Span<const CameraImpulse> impulses_for(Vec3 camera_position) noexcept;
    /// The sample matching a binding, or an invalid one.
    [[nodiscard]] static TargetSample sample_for(const TargetBinding& binding,
                                                 Span<const TargetSample> samples) noexcept;

    Allocator* allocator_;
    CameraServerConfig config_;
    const char* backend_name_ = "engine";
    bool null_backend_ = false;
    bool initialized_ = false;

    RigNodeRegistry registry_;
    HandlePool<DefinitionRecord, CameraDefinitionTag> definitions_;
    HandlePool<Rig, CameraRigTag> rigs_;
    HandlePool<CameraStack, CameraStackTag> stacks_;

    CameraSettings settings_;

    /// The impulse bus. A flat array rather than a queue: it is walked in full by every camera
    /// every frame, and its lifetime is bounded by each impulse's own duration.
    struct LiveImpulse {
        CameraImpulse impulse;
        f32 elapsed = 0.0F;
    };
    Array<LiveImpulse> impulses_;
    /// Reused across evaluations so a frame of shake allocates nothing.
    Array<CameraImpulse> impulse_scratch_;

    Array<CameraQuery> queries_;
    Array<CameraQueryResult> query_results_;
    /// Evaluated cameras for one stack resolution, in entry order. Reused, for the same reason.
    Array<EvaluatedCamera> stack_scratch_;
};

}  // namespace cy::camera
