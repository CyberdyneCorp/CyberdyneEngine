#pragma once
// Batched subsystem dispatch and arbitration. M8.c task 3.5.
//
// ================================================================================================
// A TRACK NEVER CALLS A SUBSYSTEM. IT FILLS A BATCH.
// ================================================================================================
//
// `sequencing-and-cinematics` — "Batched subsystem dispatch": evaluation "SHALL fill
// **per-subsystem command buffers** ... which subsystems consume at their defined points", "Tracks
// SHALL NOT invoke subsystems directly during traversal, and SHALL NOT mutate arbitrary engine
// state mid-evaluation", and evaluation "SHALL proceed in defined phases — prepare, evaluate,
// resolve, dispatch".
//
// So `DispatchBatches` is the whole interface between a sequence and the engine. It contains
// values, not calls; nothing in this header can reach a camera, an ECS world or an audio voice, and
// the two bridges that can — `cy::sequencing-camera` and `cy::sequencing-gameplay` — are separate
// targets that consume a batch and are consumed by nobody.
//
// ================================================================================================
// THE CAMERA REQUEST HAS NO TRANSFORM IN IT, AND THAT IS THE MILESTONE'S EXIT CRITERION
// ================================================================================================
//
// M8.c: "A sequence drives cameras through the camera stack and does not write camera transforms."
// `CameraRequest` therefore carries a rig selection, a priority, a weight, a blend policy and a
// cut — the vocabulary `camera-system`'s stack already speaks — and there is NO field for a pose.
// A sequence that wanted to write one would have to add a field to this struct, which is a change a
// reviewer sees, rather than calling a setter, which is a change nobody sees.
//
// ================================================================================================
// ORDER COMES FROM IDENTITY, NEVER FROM A WORKER
// ================================================================================================
//
// "Independent tracks and sections SHALL be evaluable in parallel through the task system, with
// results merged in a **stable order** derived from sequence instance, track, section, and event
// index — never from worker identity." Every request below carries `Provenance`, `stable_less()`
// orders by exactly those four numbers, and `DispatchBatches::sort()` is what the resolve phase
// calls. `tests/test_dispatch.cpp` evaluates the same instances in a shuffled order and asserts
// byte-identical batches, which is the only way to demonstrate that the order came from the
// identities rather than from the traversal.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/sequencing/program.h>
#include <cy/sequencing/source.h>

namespace cy::sequencing {

/// Where a request came from, in authored terms. The whole of the ordering key, and the whole of
/// what a diagnostic needs to name the thing that produced a value.
struct Provenance {
    u32 instance = 0;
    u32 track_stable_id = 0;
    u32 section_stable_id = 0;
    /// The segment index within the program: the tie-break that makes two sections of one track at
    /// one instant resolve in authored order.
    u32 order = 0;
};

[[nodiscard]] bool stable_less(const Provenance& a, const Provenance& b) noexcept;

/// One binding, resolved for one instance. The host resolves; the sequence never looks anything up.
struct BindingResolution {
    u32 stable_id = 0;
    /// Whatever the host uses as identity — an entity id, a camera rig's stable id, an audio bus.
    /// Opaque here on purpose: `src/sequencing/` is above no world and names no entity type.
    u64 target = 0;
    bool resolved = false;
};

/// A value driven at a resolved property of a resolved target. Six of the nine subsystems take
/// their whole contribution as these.
struct ValueRequest {
    SubsystemId subsystem = SubsystemId::Animation;
    u64 target = 0;
    u32 binding = 0;
    ResolvedProperty property;
    ChannelValue value;
    i32 priority = 0;
    f32 weight = 1.0F;
    BlendMode blend = BlendMode::Absolute;
    Name blend_group;
    Provenance provenance;
};

/// How a camera contribution transitions. The camera stack's own vocabulary, carried rather than
/// re-invented: `cy::sequencing::camera` maps each field onto `cy::camera::BlendPolicy`.
struct CameraBlend {
    f32 duration_seconds = 0.5F;
    /// 0 linear, 1 ease in, 2 ease out, 3 ease in-out, 4 step. The camera stack's `BlendCurve`,
    /// as a number, because `src/sequencing/` may not include a server's header — see README.md.
    u8 curve = 3;
    bool position = true;
    bool rotation = true;
    bool lens = true;
};

/// What a camera track asks for. NO POSE. See the header comment.
struct CameraRequest {
    u32 binding = 0;
    /// The rig this shot selects, as the binding resolved it.
    u64 rig = 0;
    i32 priority = 0;
    f32 weight = 1.0F;
    CameraBlend blend_in;
    CameraBlend blend_out;
    /// The section ended: the contribution blends out rather than disappearing.
    bool release = false;
    /// A camera-cut track crossed a cut at this instant.
    bool cut = false;
    /// Announced in advance so temporal history, shadow caches and residency prepare rather than
    /// react — `camera-system`'s "anticipated cut".
    bool anticipated = false;
    f32 cut_lead_seconds = 0.0F;
    /// A framing target the shot parameterises the rig with. A target, not a transform: the rig
    /// still decides where the camera goes.
    bool has_framing_target = false;
    u64 framing_target = 0;
    Name exclusive_group;
    Provenance provenance;
};

/// A gameplay command, as the sequence produced it. Turned into a `cy::gameplay::Command` by
/// `cy::sequencing-gameplay` and by nothing else — this module names no gameplay type.
struct CommandRequest {
    /// `CommandDeclaration::stable_id`. Resolved to a runtime type id by the bridge.
    u32 command_stable_id = 0;
    u64 target = 0;
    u32 binding = 0;
    u16 payload_size = 0;
    u8 payload[kMaxEventPayload] = {};
    Provenance provenance;
};

/// A typed sequence event or marker crossing. Never a callback — see source.h.
struct EventRequest {
    Name type;
    SideEffectPolicy policy = SideEffectPolicy::Idempotent;
    u64 target = 0;
    u32 binding = 0;
    i64 ticks = 0;
    /// True when the event was applied by a skip or a reconstructing seek rather than crossed in
    /// play. Carried so a listener can tell an outcome that was applied from one that happened.
    bool applied_by_skip = false;
    /// True for a MARKER crossing. Markers are editorial and synchronisation references and are a
    /// distinct concept from gameplay events — "Three kinds of marker are distinct" — so they
    /// travel in the same buffer with a flag rather than being turned into events nobody declared.
    bool marker = false;
    u16 payload_size = 0;
    u8 payload[kMaxEventPayload] = {};
    Provenance provenance;
};

/// A subsystem asked to get ready before a section becomes visible. `sequencing-and-cinematics` —
/// "Pre-roll prepares": "**WHEN** a section declares pre-roll **THEN** its subsystem SHALL be
/// prepared before the section becomes visible."
///
/// Emitted once, when the section's pre-roll range is entered and before its own range is. A
/// subsystem that needs no preparation ignores it; one that does — an audio stream to buffer, a
/// particle system to warm — has the lead time the author declared.
struct PrepareRequest {
    SubsystemId subsystem = SubsystemId::Animation;
    u64 target = 0;
    u32 binding = 0;
    /// The asset the section declared, or zero.
    u64 asset = 0;
    /// Ticks from now until the section's own range begins.
    i64 lead_ticks = 0;
    Provenance provenance;
};

/// A time scale, scoped to the domain the track declared. Never applied here: the host owns its
/// clocks, and a sequence that scaled them itself would be the "parallel path" the specification's
/// first requirement forbids.
struct TimeScaleRequest {
    /// 0 presentation, 1 simulation, 2 animation, 3 audio, 4 a declared project target.
    u8 domain = 0;
    f32 scale = 1.0F;
    Provenance provenance;
};

/// Content a sequence spawned and the lifetime it declared. `sequencing-and-cinematics` — "Spawned
/// content": "Ownership SHALL never be left ambiguous when a sequence ends."
enum class SpawnLifetime : u8 {
    Section = 0,
    Sequence,
    Persistent,
    Manual,
    Count,
};

struct SpawnRequest {
    u64 template_id = 0;
    u64 target = 0;
    SpawnLifetime lifetime = SpawnLifetime::Section;
    Provenance provenance;
};

/// The counterpart, emitted when the declaring scope ends. A `Persistent` spawn emits a HANDOVER
/// rather than a release, which is the explicit ownership transfer the requirement demands.
struct ReleaseRequest {
    u64 spawned = 0;
    SpawnLifetime lifetime = SpawnLifetime::Section;
    bool handover = false;
    Provenance provenance;
};

/// The per-subsystem buffers one evaluation fills.
class DispatchBatches {
public:
    explicit DispatchBatches(Allocator& allocator) noexcept;

    void clear() noexcept;

    /// Sort every buffer by provenance. The resolve phase's first act; see the header comment.
    void sort() noexcept;

    Array<ValueRequest> values;
    Array<CameraRequest> cameras;
    Array<PrepareRequest> prepares;
    Array<TimeScaleRequest> time_scales;
    Array<CommandRequest> commands;
    Array<EventRequest> events;
    Array<SpawnRequest> spawns;
    Array<ReleaseRequest> releases;

    /// Values addressed to one subsystem, for a consumer that wants only its own. The values array
    /// is sorted by subsystem first after `sort()`, so this is a range rather than a filter.
    [[nodiscard]] Span<const ValueRequest> values_for(SubsystemId subsystem) const noexcept;
};

/// One contribution to an arbitrated value, kept so that "the debugger SHALL show each contribution
/// and its weight" is a fact rather than a recomputation.
struct Contribution {
    Provenance provenance;
    i32 priority = 0;
    /// The weight this contribution actually had after normalisation — not the weight it asked for.
    f32 effective_weight = 0.0F;
    f32 requested_weight = 0.0F;
    BlendMode blend = BlendMode::Absolute;
    Name blend_group;
    ChannelValue value;
};

/// One resolved value, and the range of `ArbitrationReport::contributions` that produced it.
struct ResolvedValue {
    SubsystemId subsystem = SubsystemId::Animation;
    u64 target = 0;
    ResolvedProperty property;
    ChannelValue value;
    u32 contribution_begin = 0;
    u32 contribution_count = 0;
};

struct ArbitrationReport {
    explicit ArbitrationReport(Allocator& allocator) noexcept
        : values(allocator), contributions(allocator), cameras(allocator) {}

    void clear() noexcept;

    Array<ResolvedValue> values;
    Array<Contribution> contributions;
    /// The camera requests that survived arbitration: one winner per binding, plus a release for
    /// every contribution the winner displaced. A subsystem receives a RESOLVED result.
    Array<CameraRequest> cameras;
};

/// Resolve every contribution in `batches` by declared arbitration — priority, blend group,
/// exclusive group and weight — and never by last writer.
///
/// `batches` must already be sorted; `arbitrate()` asserts nothing about that and simply produces
/// the same answer either way, because the grouping key is the identity rather than the position.
[[nodiscard]] Status arbitrate(const DispatchBatches& batches, ArbitrationReport& out) noexcept;

}  // namespace cy::sequencing
