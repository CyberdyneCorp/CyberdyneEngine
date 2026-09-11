// The particles and the cut, inside the game. M8.c tasks 5.1, 5.2 and 5.3. See spectacle.h.

#include "spectacle.h"

#include "internals.h"

#include <cy/core/math/transform.h>
#include <cy/graph/cybergraph.h>
#include <cy/sequencing/camera/bridge.h>
#include <cy/sequencing/compile.h>
#include <cy/sequencing/player.h>
#include <cy/servers/camera/server.h>
#include <cy/vfx/compile.h>

#include "effects.h"

#include <cmath>
#include <utility>

namespace cy::sample::slice {
namespace {

using namespace cy::sequencing;  // NOLINT(google-build-using-namespace) — one file, one vocabulary

/// The cinematic's two camera bindings, one per shot. A shot list names a camera per shot, and that
/// is also what keeps each shot's contribution in the stack its own: the bridge holds one entry per
/// binding, so the outgoing shot blends out while the incoming one blends in.
constexpr u32 kCameraWide = 11;
constexpr u32 kCameraTight = 12;
constexpr u32 kSubjectBinding = 13;
constexpr u64 kWideRig = 9001;
constexpr u64 kTightRig = 9002;
constexpr u64 kSubjectIdentity = 1;

/// The cut runs at the simulation's own rate, so one advance is one tick and the frame the picture
/// is taken on is a tick number rather than a conversion.
constexpr i64 kCutFrames = 90;
constexpr i64 kCutAt = 36;
/// Half a second of blend at 60 Hz. The midpoint — frame 51 — is where both shots contribute.
constexpr f32 kBlendSeconds = 0.5F;

/// The effect's pool. Eight megabytes is what `src/vfx/tests/` uses for the same plume.
constexpr u64 kPoolBytes = 8ULL * 1024ULL * 1024ULL;
/// THE THREE NUMBERS THAT BOUND THE COST, AND THEY WERE MEASURED RATHER THAN GUESSED. The fight
/// commits eight activations a tick and every one of them emits a cue, so an unbounded slice plays
/// an effect a tick forever: the first version of this file cooked the plume at two emitters of 256
/// particles with a ceiling of 192 instances, reached 96,000 live particles at 120 ticks and cost
/// **20.4 ms a tick** — five times the whole spectacle budget and most of a frame at 60 Hz. That is
/// what "cost is bounded by configuration" means when nothing configures it.
///
/// So: one emitter of 48 particles, at most 12 SPAWNING effects at once, and a ceiling of 160
/// instances with the difference left for the ones that are finishing. A thirteenth cue does not
/// evict anything: it STOPS the oldest gracefully, which is `vfx-system`'s own "Graceful stop —
/// ceases spawning and lets existing particles live out their lifetimes", and is what a game does
/// with a transient hit effect anyway.
///
/// MEASURED, at 120 ticks in a development build on the reference machine: 1.40 ms a tick at 64
/// agents and 1.68 ms at 256 — the cue rate rises with the population and THE COST DOES NOT,
/// because the configuration above is what bounds it and not the number of things asking. What
/// rises instead is `effects_refused`, 32 against 651, which is the world declining to exceed its
/// own instance ceiling and SAYING SO. That number is the artefact's evidence for "cost is bounded
/// by configuration"; a run in which it stayed at zero while the cost climbed would be the
/// opposite claim.
constexpr u32 kEmitterCapacity = 48;
constexpr u32 kSpawningEffects = 12;
constexpr u32 kMaxInstances = 160;
/// The VFX frame allocation the slice declares. `BudgetController` reduces effects to hold it —
/// which is `vfx-system`'s "cost bounded by configuration" exercised by the game rather than by a
/// suite beside it.
constexpr f32 kVfxAllocationMs = 1.5F;

[[nodiscard]] cy::camera::RigNodeDesc node(const char* id, const char* input,
                                           cy::camera::RigNodeKind kind) noexcept {
    cy::camera::RigNodeDesc desc;
    desc.id = Name::intern(id);
    desc.input = (input == nullptr) ? Name{} : Name::intern(input);
    desc.kind = kind;
    return desc;
}

/// A shot: a rig that frames its target from a declared offset through a declared lens. A shot
/// changes lens by selecting a DIFFERENT RIG — `cy::camera` has no per-rig lens setter — so a wide
/// shot and a long-lens shot are two rigs and the stack cross-fades between them.
[[nodiscard]] cy::camera::RigDefinition shot_rig(Allocator& allocator, const char* name,
                                                 Vec3 offset, f32 fov_radians) noexcept {
    cy::camera::RigDefinition definition(allocator);
    definition.name = Name::intern(name);
    (void)definition.nodes.push_back(node("target", nullptr, cy::camera::RigNodeKind::Target));
    cy::camera::RigNodeDesc follow = node("follow", "target", cy::camera::RigNodeKind::Follow);
    follow.follow.space = cy::camera::FollowSpace::World;
    follow.follow.offset = offset;
    follow.follow.position_half_life = 0.0F;
    (void)definition.nodes.push_back(follow);
    cy::camera::RigNodeDesc look = node("look", "follow", cy::camera::RigNodeKind::LookAt);
    look.look_at.rotation_half_life = 0.0F;
    (void)definition.nodes.push_back(look);
    cy::camera::RigNodeDesc lens = node("lens", "look", cy::camera::RigNodeKind::Lens);
    lens.lens.near_value = fov_radians;
    lens.lens.far_value = fov_radians;
    lens.lens.half_life = 0.0F;
    (void)definition.nodes.push_back(lens);
    (void)definition.nodes.push_back(node("output", "lens", cy::camera::RigNodeKind::Output));
    return definition;
}

/// The cinematic the slice plays over its own fight: a wide shot, a long-lens shot over it with a
/// half-second blend, the cut announced ahead of itself so the camera's streaming source can
/// prefetch, and a key-light track beside them.
///
/// THERE IS NO TRANSFORM CHANNEL ANYWHERE IN IT, and there cannot be: the only property track below
/// addresses `SubsystemId::Light`. That is `sequencing-and-cinematics`' "a sequence does not write
/// camera transforms" stated where a reader will look for it, and `SpectacleReport`'s
/// `cut_camera_property_writes` is the number that keeps it true.
[[nodiscard]] SequenceSource authored_cut(Allocator& allocator) noexcept {
    SequenceSource source(allocator);
    source.name = Name::intern("slice.cut");
    source.stable_id = 1;
    source.rate = Rate{60, 1};
    source.domain = ClockDomain::Presentation;
    source.duration = SequenceTime::from_frame(kCutFrames);
    source.accessibility.camera_motion_intensity = 0.4F;

    BindingDeclaration wide_binding;
    wide_binding.stable_id = kCameraWide;
    wide_binding.name = Name::intern("camera.wide");
    wide_binding.kind = BindingKind::Camera;
    (void)source.bindings.push_back(wide_binding);
    BindingDeclaration tight_binding;
    tight_binding.stable_id = kCameraTight;
    tight_binding.name = Name::intern("camera.tight");
    tight_binding.kind = BindingKind::Camera;
    (void)source.bindings.push_back(tight_binding);
    BindingDeclaration subject;
    subject.stable_id = kSubjectBinding;
    subject.name = Name::intern("squad");
    subject.kind = BindingKind::Entity;
    (void)source.bindings.push_back(subject);

    Track wide_track(allocator);
    wide_track.name = Name::intern("shot A — wide");
    wide_track.kind = TrackKind::Camera;
    wide_track.stable_id = 300;
    wide_track.binding = kCameraWide;
    Section wide(allocator);
    wide.name = Name::intern("wide");
    wide.start = SequenceTime::from_frame(0);
    wide.end = SequenceTime::from_frame(kCutAt);
    wide.priority = 100;
    wide.blend_in_seconds = 0.0F;
    wide.blend_out_seconds = kBlendSeconds;
    wide.blend_curve = 0;
    wide.framing_binding = kSubjectBinding;
    wide.stable_id = 3000;
    (void)wide_track.sections.push_back(std::move(wide));
    (void)source.tracks.push_back(std::move(wide_track));

    Track tight_track(allocator);
    tight_track.name = Name::intern("shot B — long lens");
    tight_track.kind = TrackKind::Camera;
    tight_track.stable_id = 301;
    tight_track.binding = kCameraTight;
    Section tight(allocator);
    tight.name = Name::intern("tight");
    tight.start = SequenceTime::from_frame(kCutAt);
    tight.end = SequenceTime::from_frame(kCutFrames);
    tight.priority = 100;
    tight.blend_in_seconds = kBlendSeconds;
    tight.blend_out_seconds = 0.25F;
    tight.blend_curve = 0;
    tight.framing_binding = kSubjectBinding;
    tight.stable_id = 3001;
    (void)tight_track.sections.push_back(std::move(tight));
    (void)source.tracks.push_back(std::move(tight_track));

    Track cut(allocator);
    cut.name = Name::intern("cut to the long lens");
    cut.kind = TrackKind::CameraCut;
    cut.stable_id = 400;
    cut.binding = kCameraTight;
    Section moment(allocator);
    moment.name = Name::intern("cut");
    moment.start = SequenceTime::from_frame(kCutAt);
    moment.end = SequenceTime::from_frame(kCutAt + 1);
    // ANNOUNCED HALF A SECOND AHEAD. The bridge turns this into `CameraServer::cut(..., true,
    // lead)`, which the camera's own `StreamingSource` carries to residency as `cut_pending`.
    moment.pre_roll = SequenceTime::from_frame(30);
    moment.stable_id = 4000;
    (void)cut.sections.push_back(std::move(moment));
    (void)source.tracks.push_back(std::move(cut));

    Track light(allocator);
    light.name = Name::intern("key light");
    light.kind = TrackKind::Property;
    light.subsystem = SubsystemId::Light;
    light.stable_id = 100;
    light.binding = kSubjectBinding;
    Section lit(allocator);
    lit.name = Name::intern("rise");
    lit.start = SequenceTime::from_frame(0);
    lit.end = SequenceTime::from_frame(kCutFrames);
    lit.completion = CompletionPolicy::HoldFinal;
    lit.stable_id = 1000;
    Channel intensity(allocator);
    intensity.property = Name::intern("intensity");
    intensity.type = ChannelType::Scalar;
    intensity.stable_id = 1;
    Key first;
    first.time = SequenceTime::from_frame(0);
    first.value[0] = 0.25F;
    (void)intensity.keys.push_back(first);
    Key last;
    last.time = SequenceTime::from_frame(kCutFrames);
    last.value[0] = 4.0F;
    (void)intensity.keys.push_back(last);
    (void)lit.channels.push_back(std::move(intensity));
    (void)light.sections.push_back(std::move(lit));
    (void)source.tracks.push_back(std::move(light));

    MarkerDeclaration marker;
    marker.time = SequenceTime::from_frame(kCutAt);
    marker.name = Name::intern("cut");
    (void)source.markers.push_back(marker);
    return source;
}

}  // namespace

// --- The state -----------------------------------------------------------------------------------

struct Spectacle::State {
    explicit State(Allocator& allocator) noexcept
        : sink(allocator),
          cook(allocator),
          world(allocator),
          records(allocator),
          adapters(allocator),
          program(allocator),
          compiled(allocator),
          player(allocator, adapters, PlaybackScope::World, 0),
          batches(allocator),
          arbitration(allocator),
          server(allocator),
          bridge(allocator, server),
          contributions(allocator) {}

    graph::DiagnosticSink sink;
    vfx::CompileReport cook;
    Expected<vfx::CompiledSystem, Error> system =
        cy::fail(cy::ErrorCode::Unavailable, "not cooked");
    vfx::SimulationWorld world;
    vfx::StepReport steps;
    vfx::PublishReport published;
    Array<cy::rendering::particles::ParticleInstance> records;

    AdapterRegistry adapters;
    Program program;
    CompileReport compiled;
    SequencePlayer player;
    DispatchBatches batches;
    ArbitrationReport arbitration;
    InstanceId instance;

    cy::camera::CameraServer server;
    CameraStackBridge bridge;
    CameraBridgeReport bridge_report;
    Array<cy::camera::StackContribution> contributions;
    cy::camera::StackHandle stack;
    cy::camera::RigHandle wide;
    cy::camera::RigHandle tight;

    /// The effects that are still spawning, oldest first. See `kSpawningEffects`.
    vfx::EffectHandle live[kSpawningEffects] = {};
    u32 next_slot = 0;

    u32 cut_start = 0;
    u32 ring = 0;
    bool playing = false;
    bool finished = false;
};

Spectacle::Spectacle(Allocator& allocator) noexcept : allocator_(&allocator) {}

Spectacle::~Spectacle() {
    delete state_;
}

// --- Building ------------------------------------------------------------------------------------

Status Spectacle::build(u32 cut_start_tick, u32 particle_ring) noexcept {
    state_ = new (std::nothrow) State(*allocator_);
    if (state_ == nullptr) {
        return cy::fail(cy::ErrorCode::OutOfMemory, "the spectacle did not allocate");
    }
    State& state = *state_;
    state.cut_start = cut_start_tick;
    state.ring = particle_ring;

    // THE EFFECT IS THE ONE THE VFX SUITES COOK, SIMULATE AND DRAW, compiled here from its own
    // source rather than copied. `src/vfx/tests/effects.cpp` authors the spark plume with
    // CyberGraph and cooks it through `compile_system`; three suites already assert what it
    // compiles to, what it simulates to and what it draws as. A second plume written out in this
    // directory would be a fourth description of one effect and would drift from the other three
    // inside a milestone, which is the argument `tests/render/README.md` makes about golden scenes.
    // What this artefact adds is the only thing those three cannot say: that it survives being
    // played from a gameplay cue inside a running game, at the population a fight produces.
    vfx::CompileOptions options;
    auto compiled =
        cy::vfx_test::cook_plume(*allocator_, state.sink, state.cook, options, 1, kEmitterCapacity);
    if (!compiled) {
        return Status{cy::make_unexpected(compiled.error())};
    }
    state.system = Expected<vfx::CompiledSystem, Error>(std::move(compiled.value()));

    vfx::WorldDescription description;
    description.pool_bytes = kPoolBytes;
    description.max_instances = kMaxInstances;
    if (Status made = state.world.initialize(description); !made) {
        return made;
    }
    state.world.budget().set_allocation(kVfxAllocationMs);

    // --- The cinematic.
    AdapterProperties light;
    light.name = Name::intern("light");
    light.subsystem = SubsystemId::Light;
    light.supports_capture_restore = false;
    const Expected<u32, Error> light_id = state.adapters.register_adapter(light);
    if (!light_id) {
        return Status{cy::make_unexpected(light_id.error())};
    }
    if (!state.adapters.declare_property(*light_id, Name::intern("intensity"),
                                         ChannelType::Scalar)) {
        return cy::fail(cy::ErrorCode::Internal, "the light adapter refused its property");
    }
    AdapterProperties camera;
    camera.name = Name::intern("camera");
    camera.subsystem = SubsystemId::Camera;
    if (!state.adapters.register_adapter(camera)) {
        return cy::fail(cy::ErrorCode::Internal, "the camera adapter was not registered");
    }

    {
        SequenceCompiler compiler(*allocator_, state.adapters);
        const SequenceSource source = authored_cut(*allocator_);
        if (Status built =
                compiler.compile(source, CompileOptions{}, state.program, state.compiled);
            !built) {
            return built;
        }
    }
    report_.cut_segments = state.compiled.segments;
    report_.cut_channels = state.compiled.channels;

    if (!state.server.configure(cy::camera::CameraServerConfig{}) || !state.server.initialize()) {
        return cy::fail(cy::ErrorCode::Internal, "the camera server did not start");
    }
    const Expected<cy::camera::StackHandle, Error> stack = state.server.create_stack();
    if (!stack) {
        return Status{cy::make_unexpected(stack.error())};
    }
    state.stack = *stack;
    if (Status ready = state.bridge.initialize(state.stack); !ready) {
        return ready;
    }

    struct RigSetup {
        u64 identity = 0;
        const char* name = "";
        Vec3 offset;
        f32 fov = 1.0F;
    };
    // Two shots over the arena: a wide establishing angle above and behind the squad, and a long
    // lens close in on it. The two lenses are far apart on purpose — a blend between two similar
    // ones photographs as no blend at all, and task 5.5 asks for a picture in which it is VISIBLE.
    const RigSetup setups[2] = {{kWideRig, "wide", Vec3{9.0F, 7.0F, 18.0F}, 1.15F},
                                {kTightRig, "tight", Vec3{-2.5F, 2.0F, 5.5F}, 0.42F}};
    report_.cut_wide_fov = setups[0].fov;
    report_.cut_tight_fov = setups[1].fov;
    for (const RigSetup& setup : setups) {
        const Expected<cy::camera::DefinitionHandle, Error> definition =
            state.server.create_definition(
                shot_rig(*allocator_, setup.name, setup.offset, setup.fov));
        if (!definition) {
            return Status{cy::make_unexpected(definition.error())};
        }
        cy::camera::RigConfig config;
        config.name = Name::intern(setup.name);
        const Expected<cy::camera::RigHandle, Error> rig =
            state.server.create_rig(*definition, config);
        if (!rig) {
            return Status{cy::make_unexpected(rig.error())};
        }
        if (Status bound = state.bridge.bind_rig(setup.identity, *rig); !bound) {
            return bound;
        }
        (setup.identity == kWideRig ? state.wide : state.tight) = *rig;
    }

    const BindingResolution bindings[3] = {
        BindingResolution{kCameraWide, kWideRig, true},
        BindingResolution{kCameraTight, kTightRig, true},
        BindingResolution{kSubjectBinding, kSubjectIdentity, true}};
    PlayRequest request;
    request.bindings = bindings;
    const Expected<InstanceId, Error> instance = state.player.create(state.program, request);
    if (!instance) {
        return Status{cy::make_unexpected(instance.error())};
    }
    state.instance = *instance;
    // PREPARED, NOT PLAYED. `prepare()` is the no-hitch guarantee: everything the first frame needs
    // is resolved before the cinematic starts, which is what a preload plan is for.
    (void)state.player.prepare(state.instance);
    return cy::ok();
}

// --- The cue -------------------------------------------------------------------------------------

Status Spectacle::on_cue(const Vec3& at) noexcept {
    if (state_ == nullptr || !state_->system.has_value()) {
        return cy::ok();
    }
    State& state = *state_;
    // THE OLDEST STILL-SPAWNING EFFECT STOPS GRACEFULLY BEFORE THE NEW ONE STARTS. Not evicted —
    // `stop(handle, /*allow_completion=*/true)` ceases spawning and lets the particles it already
    // has live out their lifetimes, so nothing pops. This is the whole of what keeps the population
    // bounded while every cue still gets an effect; see `kSpawningEffects` for the measurement.
    if (state.live[state.next_slot] != vfx::kInvalidEffect) {
        (void)state.world.stop(state.live[state.next_slot], true);
        state.live[state.next_slot] = vfx::kInvalidEffect;
    }

    vfx::EffectSpawn spawn;
    spawn.position = at;
    spawn.scale = 1.0F;
    // FIRE AND FORGET: the instance releases itself when its last particle dies, so a stopped
    // effect needs nothing further from this file.
    spawn.release_on_completion = true;
    const Expected<vfx::EffectHandle, Error> played = state.world.play(*state.system, spawn);
    if (!played) {
        // A world at its instance ceiling REFUSES rather than evicting a live effect, and that is a
        // number rather than a failure. It should be rare with the ring above; a run where it is
        // not is a run whose effects outlive their stop, which is worth seeing.
        ++report_.effects_refused;
        return cy::ok();
    }
    state.live[state.next_slot] = *played;
    state.next_slot = (state.next_slot + 1U) % kSpawningEffects;
    ++report_.effects_played;
    return cy::ok();
}

// --- One frame -----------------------------------------------------------------------------------

Status Spectacle::step(u32 tick, f32 dt, const Vec3& subject, const CameraPose& gameplay,
                       CameraPose& out) noexcept {
    out = gameplay;
    if (state_ == nullptr) {
        return cy::ok();
    }
    State& state = *state_;

    const f64 particles_started = cpu_micros();
    if (Status stepped = state.world.step(dt, state.steps); !stepped) {
        return stepped;
    }
    particles_us_ = cpu_micros() - particles_started;
    // THE BUDGET CONTROLLER IS FED THE MEASUREMENT, so the levers it hands the next step are a
    // function of what this one cost. Without this the "importance classes bound the cost" claim
    // would be about a dial nobody turned.
    state.world.budget().measure(static_cast<f32>(particles_us_ / 1000.0));
    state.world.budget().update(dt);

    const vfx::StepReport& steps = state.steps;
    report_.vfx_live_particles = steps.live_particles;
    report_.vfx_peak_particles = steps.live_particles > report_.vfx_peak_particles
                                     ? steps.live_particles
                                     : report_.vfx_peak_particles;
    for (u32 which = 0; which < 4U; ++which) {
        report_.vfx_by_importance[which] = steps.particles_by_importance[which];
    }
    report_.vfx_spawned += steps.spawned;
    report_.vfx_killed += steps.killed;
    report_.vfx_substeps += steps.substeps;
    report_.vfx_dispatches_unmerged += steps.dispatches_unmerged;
    report_.vfx_dispatches_merged += steps.dispatches_merged;
    report_.vfx_cpu_emitters += steps.cpu_emitters;
    report_.vfx_gpu_emitters += steps.gpu_emitters;
    report_.vfx_cpu_fallbacks += steps.cpu_fallbacks;
    report_.vfx_pool_used_bytes = state.world.pool().report().used_bytes;
    report_.vfx_pool_total_bytes = state.world.pool().report().total_bytes;

    if (state.finished || tick < state.cut_start) {
        return cy::ok();
    }
    const f64 cinematic_started = cpu_micros();
    if (!state.playing) {
        if (Status started = state.player.play(state.instance); !started) {
            return started;
        }
        state.playing = true;
        report_.cut_ran = true;
    }

    state.batches.clear();
    // The presentation clock's own tick, in nanoseconds. `sequencing-and-cinematics` requires exact
    // time; `kDeltaTime` is 1/60 and 16666667 ns is the integer the rate table cancels against.
    if (Status advanced = state.player.advance(16666667, state.batches); !advanced) {
        return advanced;
    }
    state.batches.sort();
    if (Status arbitrated = arbitrate(state.batches, state.arbitration); !arbitrated) {
        return arbitrated;
    }
    for (const ResolvedValue& value : state.arbitration.values.span()) {
        if (value.subsystem == SubsystemId::Camera) {
            ++report_.cut_camera_property_writes;
        }
    }
    if (Status applied = state.bridge.apply(state.arbitration.cameras.span(), state.bridge_report);
        !applied) {
        return applied;
    }
    report_.cut_pushed = state.bridge_report.pushed;
    report_.cut_released = state.bridge_report.released;
    report_.cut_cuts = state.bridge_report.cuts;
    report_.cut_anticipated_cuts = state.bridge_report.anticipated_cuts;
    report_.cut_unresolved_rigs = state.bridge_report.unresolved_rigs;

    cy::camera::TargetSample sample;
    sample.stable_id = kSubjectIdentity;
    sample.transform = cy::Transform::from_translation(subject);
    sample.valid = true;
    cy::camera::EvaluationContext context;
    context.delta_seconds = dt;
    context.targets = Span<const cy::camera::TargetSample>(&sample, 1);
    context.aspect = 16.0F / 9.0F;

    cy::camera::EvaluatedCamera resolved;
    if (!state.server.evaluate_stack(state.stack, context, resolved, &state.contributions)) {
        // AN EMPTY STACK IS THE END OF THE CINEMATIC, NOT A FAILURE. `evaluate_stack` advances the
        // blends and then refuses to blend nothing, so the frame on which the last contribution
        // finishes blending out comes back `NotFound` — the camera module's own contract, recorded
        // in src/sequencing/README.md as a sharp edge every consumer meets on the last frame.
        const cy::camera::CameraStack* remaining = state.server.stack(state.stack);
        if (remaining != nullptr && remaining->empty()) {
            state.finished = true;
            cinematic_us_ = cpu_micros() - cinematic_started;
            return cy::ok();
        }
        return cy::fail(cy::ErrorCode::Internal, "the camera stack did not evaluate");
    }
    report_.cut_pose_overrides += resolved.pose_overridden ? 1U : 0U;

    // MATCHED BY RIG, NOT BY POSITION. Entries leave the stack when their blend out finishes, so
    // index 0 is the wide shot before the cut and the tight one after it; reading by index would
    // report the tight shot's weight as the wide shot's for the whole second half.
    f32 wide = 0.0F;
    f32 tight = 0.0F;
    for (const cy::camera::StackContribution& contribution : state.contributions.span()) {
        if (contribution.rig == state.wide) {
            wide = contribution.weight;
        } else if (contribution.rig == state.tight) {
            tight = contribution.weight;
        }
    }
    report_.cut_wide_weight = wide;
    report_.cut_tight_weight = tight;
    report_.cut_fov = resolved.lens.vertical_fov_radians();
    ++report_.cut_frames;
    if (wide > 0.01F && tight > 0.01F) {
        ++report_.cut_blend_frames;
        // The MOST BALANCED frame of the blend, kept rather than the last one: the last frame of an
        // overlap is nearly all of one shot, and a picture taken there shows no blend at all.
        const f32 balance = wide < tight ? wide : tight;
        const f32 best = report_.cut_blend_wide < report_.cut_blend_tight ? report_.cut_blend_wide
                                                                          : report_.cut_blend_tight;
        if (balance > best) {
            report_.cut_blend_wide = wide;
            report_.cut_blend_tight = tight;
            report_.cut_blend_fov = resolved.lens.vertical_fov_radians();
        }
    }

    // THE VIEW COMES OUT OF THE STACK, and this is the whole of task 5.2. Nothing above assigned a
    // camera transform: the sequence selected rigs, the bridge pushed them, and the camera server
    // blended the poses its own rigs produced.
    const cy::Transform& pose = resolved.pose;
    const Vec3 forward = pose.rotation * Vec3{0.0F, 0.0F, -1.0F};
    out.eye = pose.translation;
    out.target = Vec3{pose.translation.x + forward.x, pose.translation.y + forward.y,
                      pose.translation.z + forward.z};
    out.vertical_fov = resolved.lens.vertical_fov_radians();
    out.from_cut = true;
    cinematic_us_ = cpu_micros() - cinematic_started;
    return cy::ok();
}

Status Spectacle::publish(const Vec3& camera) noexcept {
    if (state_ == nullptr) {
        return cy::ok();
    }
    if (Status published = vfx::publish_sprites(state_->world, camera, state_->ring,
                                                state_->records, state_->published);
        !published) {
        return published;
    }
    report_.vfx_published = state_->published.particles;
    report_.vfx_dropped = state_->published.dropped;
    return cy::ok();
}

Span<const cy::rendering::particles::ParticleInstance> Spectacle::particles() const noexcept {
    if (state_ == nullptr) {
        return {};
    }
    return state_->records.span();
}

}  // namespace cy::sample::slice
