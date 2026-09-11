#pragma once
// The authored form of a sequence: tracks, sections, channels, bindings, parameters and policies.
// M8.c task 3.3.
//
// ================================================================================================
// THIS IS THE THING THAT IS COMPILED. NOTHING HERE IS EVALUATED.
// ================================================================================================
//
// `sequencing-and-cinematics` — "Compiled programs": a sequence "SHALL be authored as tracks,
// sections, and channels, and **compiled** into a program", and "Editor timeline object graphs
// SHALL NOT be traversed at runtime". So this header is the compiler's INPUT and the editor's
// document, and the runtime never sees it: `program.h` is what plays. The two are separate types
// rather than one type with a "compiled" flag for the reason every other compiled layer in this
// engine gives — a shared type is a type the runtime can be handed the uncompiled form of.
//
// AND IT IS NOT A NODE GRAPH. M8.b's spike measured that a timeline cannot lower through the shared
// pure-expression core: the core is a hash-consed DAG that re-sorts commutative operands and
// deletes values nothing reads, and an authored track order is exactly what must not be re-sorted.
// A sequence therefore adopts CyberGraph for nothing at all, which is the spike's answer and not a
// preference — see openspec/changes/implement-m8c-spectacle/design.md §1.
//
// ================================================================================================
// WHAT THE ENUMERATIONS ARE FOR
// ================================================================================================
//
// Six of them — the clock domain, the authority class, the completion policy, the skip policy, the
// network policy and the side-effect policy — exist so the COMPILER can refuse a combination the
// specification forbids. They are not documentation: `compile.h`'s validation reads every one of
// them, and a sequence that pairs a presentation clock with an authoritative track fails to compile
// naming the track. A policy nothing validates would be a comment with a type.

#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/sequencing/time.h>

namespace cy::sequencing {

/// The subsystems evaluation dispatches to. `sequencing-and-cinematics` — "Batched subsystem
/// dispatch" names exactly these nine.
enum class SubsystemId : u8 {
    Camera = 0,
    Animation,
    Audio,
    Effects,
    Material,
    Light,
    Environment,
    Interface,
    Gameplay,
    Count,
};

[[nodiscard]] const char* subsystem_name(SubsystemId subsystem) noexcept;

/// Track kinds. The specification's "at minimum" list, complete.
enum class TrackKind : u8 {
    Property = 0,
    Transform,
    Animation,
    Camera,
    CameraCut,
    Audio,
    Effects,
    Material,
    Light,
    Environment,
    GameplayEvent,
    GameplayCommand,
    Interface,
    WorldLayer,
    TimeScale,
    NestedSequence,
    Marker,
    Count,
};

[[nodiscard]] const char* track_kind_name(TrackKind kind) noexcept;

/// Which subsystem a track kind dispatches to. One transcription, read by the compiler.
[[nodiscard]] SubsystemId subsystem_for(TrackKind kind) noexcept;

/// `sequencing-and-cinematics` — "Track authority classification": "Every track SHALL declare an
/// **authority class**".
enum class AuthorityClass : u8 {
    PresentationOnly = 0,
    LocalGameplay,
    AuthoritativeGameplay,
    DeterministicSimulation,
    Count,
};

[[nodiscard]] const char* authority_class_name(AuthorityClass authority) noexcept;

/// Channel types. Interpolation follows the type — a rotation interpolates as an orientation, a
/// boolean and an enumeration do not interpolate at all — which is the requirement's own scenario.
///
/// `Transform` is declared because the specification names it, and the compiler REFUSES it with a
/// diagnostic naming `add_transform_channels()`: a transform is authored as a vector, a rotation
/// and a vector channel, because those three are what interpolate correctly and a ten-float key
/// would make every scalar channel pay for the one case that needs it. See README.md, "deviations".
enum class ChannelType : u8 {
    Scalar = 0,
    Vector,
    Rotation,
    Color,
    Boolean,
    Enumeration,
    Transform,
    Count,
};

[[nodiscard]] const char* channel_type_name(ChannelType type) noexcept;

/// How many of a key's four components a type reads.
[[nodiscard]] u8 channel_component_count(ChannelType type) noexcept;

enum class Interpolation : u8 {
    /// Holds the previous key's value until the next. Correct for booleans and enumerations, and
    /// the only interpolation they may declare.
    Constant = 0,
    Linear,
    /// Catmull-Rom through the neighbouring keys. No authored tangents: a tangent is an editing
    /// affordance and this milestone has no timeline editor to author one in.
    Smooth,
    Count,
};

[[nodiscard]] const char* interpolation_name(Interpolation interpolation) noexcept;

/// A value on a channel, whatever the channel's type. Four floats and an integer, because that is
/// what covers every type above and because a variant would put a branch in the evaluation loop.
struct ChannelValue {
    ChannelType type = ChannelType::Scalar;
    f32 components[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    u32 integer = 0;

    [[nodiscard]] static ChannelValue scalar(f32 value) noexcept;
    [[nodiscard]] static ChannelValue vector(f32 x, f32 y, f32 z) noexcept;
    [[nodiscard]] static ChannelValue rotation(f32 x, f32 y, f32 z, f32 w) noexcept;
    [[nodiscard]] static ChannelValue color(f32 r, f32 g, f32 b, f32 a) noexcept;
    [[nodiscard]] static ChannelValue boolean(bool value) noexcept;
    [[nodiscard]] static ChannelValue enumeration(u32 value) noexcept;

    /// Exact equality of the components a type reads. Used by the compiler's redundant-key removal
    /// and by capture and restore, never by evaluation.
    [[nodiscard]] bool equals(const ChannelValue& other) const noexcept;
};

/// One authored key.
struct Key {
    SequenceTime time;
    f32 value[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    u32 integer = 0;
    Interpolation interpolation = Interpolation::Linear;
    /// Preserved across benign edits — the identity a semantic diff and a three-way merge address.
    u32 stable_id = 0;
};

/// One channel of typed keyed data.
struct Channel {
    explicit Channel(Allocator& allocator) noexcept : keys(allocator) {}

    Name property;
    ChannelType type = ChannelType::Scalar;
    Array<Key> keys;
    u32 stable_id = 0;
};

/// What happens to what a section touched when it ends. `sequencing-and-cinematics` — "State
/// capture and restoration".
enum class CompletionPolicy : u8 {
    /// Put back the value the section found. Requires the adapter to support capture and restore.
    Restore = 0,
    /// Leave the last evaluated value in place.
    HoldFinal,
    /// The change is the point — a door that stays open.
    KeepPermanently,
    /// The adapter decides, and says so in its diagnostics.
    Custom,
    Count,
};

[[nodiscard]] const char* completion_policy_name(CompletionPolicy policy) noexcept;

/// How a section's contribution combines with everything else driving the same target.
enum class BlendMode : u8 {
    /// Blend toward this section's value by its weight.
    Absolute = 0,
    /// Add this section's value on top of what is below.
    Additive,
    /// Take this section's value outright at full weight, subject to priority.
    Override,
    Count,
};

[[nodiscard]] const char* blend_mode_name(BlendMode mode) noexcept;

enum class LoopBehaviour : u8 {
    None = 0,
    Loop,
    PingPong,
    Count,
};

/// One section: a range of a track with its own channels and policies.
struct Section {
    explicit Section(Allocator& allocator) noexcept : channels(allocator) {}

    Name name;
    SequenceTime start;
    SequenceTime end;
    /// Higher wins arbitration. Not the same number as a camera stack priority, though the camera
    /// adapter passes it through as one.
    i32 priority = 0;
    f32 weight = 1.0F;
    BlendMode blend = BlendMode::Absolute;
    /// A blend group name: contributions sharing one are weight-normalised against each other.
    Name blend_group;
    /// Starting a section in an exclusive group stops or suspends the others in it.
    Name exclusive_group;
    /// How long before `start` the subsystem is asked to prepare, and how long after `end` it is
    /// kept alive. Both in sequence ticks.
    SequenceTime pre_roll;
    SequenceTime post_roll;
    LoopBehaviour loop = LoopBehaviour::None;
    CompletionPolicy completion = CompletionPolicy::HoldFinal;
    /// How this section's contribution transitions in and out. Sections declare "their range,
    /// priority, weight, blending, pre-roll and post-roll", and for a camera section these four are
    /// what the CAMERA STACK is handed — the blend is performed there, by the system that owns it,
    /// which is what stops a sequence from blending a camera itself.
    f32 blend_in_seconds = 0.5F;
    f32 blend_out_seconds = 0.5F;
    /// 0 linear, 1 ease in, 2 ease out, 3 ease in-out, 4 step (a cut). The camera stack's curve.
    u8 blend_curve = 3;
    bool blend_position = true;
    bool blend_rotation = true;
    bool blend_lens = true;
    /// A second binding this section frames — a camera section's subject. A TARGET, never a
    /// transform: the rig still decides where the camera goes.
    u32 framing_binding = 0;
    /// A `GameplayCommand` section's payload, emitted once when the section is entered. Intent, not
    /// content — the same rule `gameplay-framework` states for a command.
    u16 command_payload_size = 0;
    u8 command_payload[48] = {};
    /// Content this section spawns, and the lifetime it declares. Zero spawns nothing.
    u64 spawn_template = 0;
    /// 0 section, 1 sequence, 2 persistent, 3 manual — `SpawnLifetime` in dispatch.h, as a number,
    /// because the authored model is below the dispatch vocabulary.
    u8 spawn_lifetime = 0;
    Array<Channel> channels;
    /// The asset this section needs, if any. Feeds the preload plan; zero is none.
    u64 asset = 0;
    f32 asset_priority = 1.0F;
    u32 stable_id = 0;
};

/// What a sequence event is: a typed record with a time and a typed payload. Never a callback —
/// "Serialised function pointers or arbitrary callbacks SHALL NOT be the mechanism."
enum class SideEffectPolicy : u8 {
    /// May be fired and un-fired. A camera shake.
    Reversible = 0,
    /// Firing twice is the same as firing once. Setting a flag.
    Idempotent,
    /// May be fired on predicted state and reconciled later.
    Speculative,
    /// Fires only on the authority, and never during preview scrubbing. A permanent unlock.
    ConfirmedOnly,
    Count,
};

[[nodiscard]] const char* side_effect_policy_name(SideEffectPolicy policy) noexcept;

inline constexpr u16 kMaxEventPayload = 48;

struct EventDeclaration {
    SequenceTime time;
    Name type;
    SideEffectPolicy policy = SideEffectPolicy::Idempotent;
    /// The binding the event addresses, or zero for the sequence itself.
    u32 binding = 0;
    u16 payload_size = 0;
    u8 payload[kMaxEventPayload] = {};
    u32 stable_id = 0;
};

/// Editorial and synchronisation references, distinct from gameplay events and from animation
/// markers — the specification's "Three kinds of marker are distinct".
struct MarkerDeclaration {
    SequenceTime time;
    Name name;
    u32 stable_id = 0;
};

/// A track: sections over time, one binding, one authority class.
struct Track {
    explicit Track(Allocator& allocator) noexcept : sections(allocator), events(allocator) {}

    Name name;
    TrackKind kind = TrackKind::Property;
    AuthorityClass authority = AuthorityClass::PresentationOnly;
    /// Which adapter this track's channels resolve against, when the kind does not decide it. A
    /// `Property` track is the case: "intensity" means one thing to a light and another to an
    /// audio bus, and the track says which. `Count` means "take it from the kind".
    SubsystemId subsystem = SubsystemId::Count;
    /// The binding this track drives, by `BindingDeclaration::stable_id`. Zero addresses the
    /// sequence itself — a time-scale track and a marker track are the cases.
    u32 binding = 0;
    /// For `NestedSequence`: the child, and its time transform.
    u64 nested_sequence = 0;
    SequenceTime nested_offset;
    /// For `TimeScale`: which domain the track scales. 0 presentation, 1 simulation, 2 animation,
    /// 3 audio, 4 a declared project target. "A time scale track SHALL declare **which domain it
    /// scales** ... and SHALL NOT implicitly scale every clock."
    u8 time_scale_domain = 0;
    /// For `GameplayCommand`: the command type's stable identity, as `gameplay-framework` spells
    /// it. Resolved to a runtime `CommandTypeId` by the gameplay bridge, never here.
    u32 command_stable_id = 0;
    Array<Section> sections;
    Array<EventDeclaration> events;
    u32 stable_id = 0;
};

enum class BindingKind : u8 {
    Entity = 0,
    Participant,
    Camera,
    Interface,
    Service,
    WorldLayer,
    AudioBus,
    /// A project-defined target. The runtime resolves it; the compiler only checks that something
    /// declared it.
    Project,
    Count,
};

[[nodiscard]] const char* binding_kind_name(BindingKind kind) noexcept;

enum class BindingRequirement : u8 {
    Required = 0,
    Optional,
    Fallback,
    Count,
};

/// A binding: a stable identifier resolved at play time. "Bindings SHALL NEVER be raw pointers or
/// transient runtime indices" — so this carries an identifier and a kind and nothing else.
struct BindingDeclaration {
    u32 stable_id = 0;
    Name name;
    BindingKind kind = BindingKind::Entity;
    BindingRequirement requirement = BindingRequirement::Required;
    /// A capability the target must have, checked at play time by the host that resolves it. Zero
    /// is no constraint.
    u32 constraint = 0;
    /// Used when `requirement` is `Fallback`: the identity to resolve instead.
    u64 fallback_target = 0;
};

/// A typed parameter with a default, overridable at play time. Resolved to an index at compile
/// time: "runtime parameter application SHALL NOT require string lookup".
struct ParameterDeclaration {
    u32 stable_id = 0;
    Name name;
    ChannelValue default_value;
};

enum class SkipPolicy : u8 {
    NotSkippable = 0,
    /// Presentation only: there is nothing to apply, so skipping is stopping.
    PresentationOnly,
    /// Apply the declared required outcomes, then advance presentation to the end.
    ApplyRequiredOutcomes,
    Custom,
    Count,
};

[[nodiscard]] const char* skip_policy_name(SkipPolicy policy) noexcept;

/// What skipping must apply. `sequencing-and-cinematics` — "Skipping applies what it skips": "A
/// sequence carrying authoritative gameplay tracks that does not declare its required outcomes
/// SHALL NOT be marked skippable, and this SHALL be validated at compile time."
struct RequiredOutcome {
    /// The track whose outcome this is, by stable id. Zero means the sequence's own.
    u32 track = 0;
    /// The event that must fire, by `EventDeclaration::stable_id`. Zero means "the final value of
    /// the named track's last section".
    u32 event = 0;
};

enum class NetworkPolicy : u8 {
    LocalOnly = 0,
    ServerTriggered,
    Synchronised,
    Deterministic,
    Count,
};

[[nodiscard]] const char* network_policy_name(NetworkPolicy policy) noexcept;

enum class PersistenceClass : u8 {
    None = 0,
    Session,
    SaveGame,
    Count,
};

/// `sequencing-and-cinematics` — "Accessibility metadata".
struct AccessibilityMetadata {
    f32 camera_motion_intensity = 0.0F;
    f32 rapid_luminance_change = 0.0F;
    f32 shake_magnitude = 0.0F;
    bool subtitles_required = false;
    bool skippable = false;
};

/// A whole authored sequence.
struct SequenceSource {
    explicit SequenceSource(Allocator& allocator) noexcept
        : bindings(allocator),
          parameters(allocator),
          tracks(allocator),
          markers(allocator),
          required_outcomes(allocator) {}

    Name name;
    u32 stable_id = 0;
    Rate rate;
    ClockDomain domain = ClockDomain::Presentation;
    SkipPolicy skip = SkipPolicy::PresentationOnly;
    NetworkPolicy network = NetworkPolicy::LocalOnly;
    PersistenceClass persistence = PersistenceClass::None;
    /// Whether the sequence claims to be deterministic. A deterministic sequence may use only
    /// adapters that declare themselves deterministic — validated at compile time.
    bool deterministic_profile = false;
    /// The authored end. Sections beyond it are a diagnostic.
    SequenceTime duration;
    AccessibilityMetadata accessibility;

    Array<BindingDeclaration> bindings;
    Array<ParameterDeclaration> parameters;
    Array<Track> tracks;
    Array<MarkerDeclaration> markers;
    Array<RequiredOutcome> required_outcomes;
};

/// The subsystem a track dispatches to: its declared override, or the one its kind implies.
[[nodiscard]] SubsystemId track_subsystem(const Track& track) noexcept;

/// Author a transform as the three channels that interpolate correctly: `position` (vector),
/// `rotation` (rotation) and `scale` (vector). See `ChannelType::Transform` above for why this is a
/// helper rather than a channel type the compiler accepts.
[[nodiscard]] Status add_transform_channels(Section& section, u32 first_stable_id) noexcept;

}  // namespace cy::sequencing
