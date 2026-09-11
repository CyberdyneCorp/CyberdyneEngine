#pragma once
// The compiled program: what the runtime evaluates. M8.c tasks 3.1 and 3.6.
//
// ================================================================================================
// COST SCALES WITH WHAT IS ACTIVE, AND THIS HEADER IS WHERE THAT IS DECIDED
// ================================================================================================
//
// `sequencing-and-cinematics` — "Cost scales with what is active": the compiler "SHALL build
// **interval and event indexes**, so that evaluation and seeking locate active sections and crossed
// events without scanning tracks", and "Evaluation cost SHALL scale with **active sections, active
// channels, and events crossed in the current interval**, not with the number of authored keys."
//
// Two indexes, and neither is a tree:
//
//   * THE INTERVAL INDEX IS A BUCKET ARRAY. The timeline is divided into a bounded number of equal
//     buckets and each bucket holds the indices of the segments overlapping it. Locating the active
//     set is one division and one contiguous walk — no comparisons against segments that are not
//     active, which is the property the requirement asks for. A balanced interval tree would answer
//     the same question in O(log n + k) with a pointer chase per level; the bucket array answers it
//     in O(k) with one cache line per bucket, and its whole cost is bounded at compile time by
//     `kMaxBuckets` rather than by how long the cinematic is.
//   * THE EVENT INDEX IS A SORTED ARRAY. Events crossed by an interval are a CONTIGUOUS RANGE of
//   it,
//     so "which events did this frame cross" is two binary searches and a span — and, because it is
//     a span rather than a filtered walk, a seek of four minutes costs the same as a frame.
//
// A million authored keys therefore cost a million keys' MEMORY and nothing per frame: the keys a
// frame reads are the ones inside its active segments' channels, found through the index.
//
// ================================================================================================
// WHY A PROGRAM IS IMMUTABLE AND SHARED
// ================================================================================================
//
// "Many playing instances SHALL share one immutable program, with per-instance state limited to
// time, playback state, and active section bookkeeping." So nothing here is mutable after
// compilation and nothing here is per-instance: an instance is a handful of fields in player.h,
// and a hundred of them point at one of these.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/sequencing/source.h>
#include <cy/sequencing/time.h>

namespace cy::sequencing {

/// The upper bound on the interval index. A ten-minute cinematic at 24 frames per second is 14,400
/// frames; 4,096 buckets puts three or four frames in each, which is well under the granularity at
/// which a section's start matters. Bounded rather than proportional so that a long sequence costs
/// memory for its keys and not for its index.
inline constexpr u32 kMaxBuckets = 4096;

/// A property resolved at compile time. `sequencing-and-cinematics`: "Property access SHALL use
/// **resolved bindings**; string paths SHALL NOT be looked up during evaluation."
///
/// Two small integers: which adapter owns the property, and its index within that adapter. The
/// `Name` that produced them is kept in the debug map and read by nothing that runs per frame.
struct ResolvedProperty {
    u32 adapter = 0;
    u32 property = 0;
    [[nodiscard]] bool valid() const noexcept { return adapter != 0 || property != 0; }
};

struct CompiledKey {
    i64 ticks = 0;
    f32 value[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    u32 integer = 0;
    Interpolation interpolation = Interpolation::Linear;
};

struct CompiledChannel {
    ChannelType type = ChannelType::Scalar;
    ResolvedProperty target;
    u32 key_begin = 0;
    u32 key_count = 0;
    /// True when the cook folded this channel to one value. Evaluation skips the search entirely.
    bool constant = false;
};

/// One evaluation segment: a section of a track, flattened, with everything evaluation needs.
struct Segment {
    i64 start = 0;
    i64 end = 0;
    /// The range the index is built over: `start - pre_roll` to `end + post_roll`. A section is
    /// *prepared* over the pre-roll and *evaluated* over its own range, which is why both exist.
    i64 active_start = 0;
    i64 active_end = 0;
    u32 channel_begin = 0;
    u32 channel_count = 0;
    u32 binding = 0;
    /// Index into `Program::debug()`, which carries the authored identities. Never read per frame.
    u32 debug = 0;
    TrackKind kind = TrackKind::Property;
    AuthorityClass authority = AuthorityClass::PresentationOnly;
    SubsystemId subsystem = SubsystemId::Camera;
    BlendMode blend = BlendMode::Absolute;
    LoopBehaviour loop = LoopBehaviour::None;
    CompletionPolicy completion = CompletionPolicy::HoldFinal;
    i32 priority = 0;
    f32 weight = 1.0F;
    Name blend_group;
    Name exclusive_group;
    /// The command type's stable identity for a `GameplayCommand` track; zero otherwise, and the
    /// payload the section emits once when it is entered.
    u32 command_stable_id = 0;
    u16 command_payload_size = 0;
    u8 command_payload[kMaxEventPayload] = {};
    /// The transition this section declares, handed to the camera stack for a camera section.
    f32 blend_in_seconds = 0.5F;
    f32 blend_out_seconds = 0.5F;
    u8 blend_curve = 3;
    bool blend_position = true;
    bool blend_rotation = true;
    bool blend_lens = true;
    /// The subject a camera section frames, by binding stable id. Zero frames nothing.
    u32 framing_binding = 0;
    /// Content this section spawns, and its declared lifetime (dispatch.h's `SpawnLifetime`).
    u64 spawn_template = 0;
    u8 spawn_lifetime = 0;
    /// The domain a `TimeScale` track scales.
    u8 time_scale_domain = 0;
    /// The asset this section declared, for the preload plan and for a prepare.
    u64 asset = 0;
};

struct CompiledEvent {
    i64 ticks = 0;
    Name type;
    SideEffectPolicy policy = SideEffectPolicy::Idempotent;
    u32 binding = 0;
    u32 track_stable_id = 0;
    u32 stable_id = 0;
    u16 payload_size = 0;
    u8 payload[kMaxEventPayload] = {};
};

struct CompiledMarker {
    i64 ticks = 0;
    Name name;
};

struct CompiledBinding {
    u32 stable_id = 0;
    Name name;
    BindingKind kind = BindingKind::Entity;
    BindingRequirement requirement = BindingRequirement::Required;
    u32 constraint = 0;
    u64 fallback_target = 0;
};

struct CompiledParameter {
    u32 stable_id = 0;
    Name name;
    ChannelValue default_value;
};

/// One asset the sequence needs, when it needs it, and when it may go. The differentiator the
/// specification names: "a sequence knows exactly where its camera will be in six seconds and what
/// each shot needs".
struct PreloadEntry {
    u64 asset = 0;
    i64 required_at = 0;
    i64 releasable_at = 0;
    f32 priority = 1.0F;
    u32 segment = 0;
};

/// The map back to authored identity. Read by the debugger, by a diagnostic and by a preload miss
/// report — never by evaluation. Provenance survives flattening because `nested_sequence` names the
/// child a flattened segment came from.
struct DebugEntry {
    Name track_name;
    Name section_name;
    u32 track_stable_id = 0;
    u32 section_stable_id = 0;
    u64 nested_sequence = 0;
};

/// Evaluate a run of keys at an instant. The one interpolation path in this module: `Program`
/// calls it per channel and the compiler calls it to MEASURE the error its compression introduced,
/// so a compression report can never disagree with what evaluation will actually produce.
[[nodiscard]] bool sample_keys(Span<const CompiledKey> keys, ChannelType type, bool constant,
                               SequenceTime local, ChannelValue& out) noexcept;

/// Blend `b` over `a` by `t`, respecting the type: a rotation takes the short arc, a boolean and an
/// enumeration switch at the halfway point rather than producing a value between two enumerators.
/// The one blend in this module — arbitration calls it and so does anything else that mixes two
/// channel values, because two blends would eventually disagree about a quaternion.
[[nodiscard]] ChannelValue blend_values(const ChannelValue& a, const ChannelValue& b,
                                        f32 t) noexcept;

/// `base` plus `delta` scaled by `weight`, for an additive contribution. A rotation composes rather
/// than adding component-wise; a boolean or an enumeration has no additive meaning and is returned
/// unchanged, which is why this is a function and not an operator.
[[nodiscard]] ChannelValue add_values(const ChannelValue& base, const ChannelValue& delta,
                                      f32 weight) noexcept;

/// A compiled sequence. Immutable once `SequenceCompiler` has filled it.
class Program {
public:
    explicit Program(Allocator& allocator) noexcept;

    Program(const Program&) = delete;
    Program& operator=(const Program&) = delete;
    Program(Program&&) noexcept = default;
    Program& operator=(Program&&) noexcept = default;

    [[nodiscard]] Name name() const noexcept { return name_; }
    [[nodiscard]] u32 stable_id() const noexcept { return stable_id_; }
    [[nodiscard]] Rate rate() const noexcept { return rate_; }
    [[nodiscard]] ClockDomain domain() const noexcept { return domain_; }
    [[nodiscard]] SkipPolicy skip_policy() const noexcept { return skip_; }
    [[nodiscard]] NetworkPolicy network_policy() const noexcept { return network_; }
    [[nodiscard]] PersistenceClass persistence() const noexcept { return persistence_; }
    [[nodiscard]] bool deterministic() const noexcept { return deterministic_; }
    [[nodiscard]] SequenceTime duration() const noexcept { return SequenceTime::from_ticks(end_); }
    [[nodiscard]] const AccessibilityMetadata& accessibility() const noexcept {
        return accessibility_;
    }
    /// The generation this program was compiled at. Hot reload publishes a new one; an instance
    /// carries the generation it started on, so "continue at the equivalent time" can tell whether
    /// it is running the program it was started with.
    [[nodiscard]] u32 generation() const noexcept { return generation_; }

    [[nodiscard]] Span<const Segment> segments() const noexcept { return segments_.span(); }
    [[nodiscard]] Span<const CompiledChannel> channels() const noexcept { return channels_.span(); }
    [[nodiscard]] Span<const CompiledKey> keys() const noexcept { return keys_.span(); }
    [[nodiscard]] Span<const CompiledEvent> events() const noexcept { return events_.span(); }
    [[nodiscard]] Span<const CompiledMarker> markers() const noexcept { return markers_.span(); }
    [[nodiscard]] Span<const CompiledBinding> bindings() const noexcept { return bindings_.span(); }
    [[nodiscard]] Span<const CompiledParameter> parameters() const noexcept {
        return parameters_.span();
    }
    [[nodiscard]] Span<const PreloadEntry> preload_plan() const noexcept { return preload_.span(); }
    [[nodiscard]] Span<const DebugEntry> debug() const noexcept { return debug_.span(); }
    [[nodiscard]] Span<const RequiredOutcome> required_outcomes() const noexcept {
        return outcomes_.span();
    }

    // --- The indexes ---------------------------------------------------------------------------

    /// The segments active at `time`, appended to `out` in segment order — which is authored order,
    /// because the compiler sorts segments by (start, track, section) and never by anything a
    /// worker decides. `out` is cleared first.
    ///
    /// This is the function the requirement is about: it touches one bucket and the segments in it.
    [[nodiscard]] Status active_at(SequenceTime time, Array<u32>& out) const noexcept;

    /// The events in `(from, to]`, as a contiguous span of the event index. An empty span when
    /// nothing was crossed; the whole point is that a four-minute seek costs the same as a frame.
    [[nodiscard]] Span<const CompiledEvent> events_between(SequenceTime from,
                                                           SequenceTime to) const noexcept;

    /// A marker by name, or null. Play-time, not per-frame: "jump to a marker" is a control call.
    [[nodiscard]] const CompiledMarker* find_marker(Name marker) const noexcept;

    /// A parameter's index, or `kInvalidIndex`. Resolved once at play time, never per frame.
    static constexpr u32 kInvalidIndex = 0xFFFFFFFFU;
    [[nodiscard]] u32 parameter_index(Name parameter) const noexcept;
    [[nodiscard]] u32 binding_index(u32 stable_id) const noexcept;

    /// Evaluate one channel at a local instant. Returns false for a channel with no keys.
    [[nodiscard]] bool sample(u32 channel, SequenceTime local, ChannelValue& out) const noexcept;

    /// How many buckets the interval index has, and the ticks each spans. Reported so a test can
    /// assert the index is an index rather than a scan.
    [[nodiscard]] u32 bucket_count() const noexcept { return bucket_count_; }
    [[nodiscard]] i64 bucket_span() const noexcept { return bucket_span_; }

private:
    friend class SequenceCompiler;

    Name name_;
    u32 stable_id_ = 0;
    u32 generation_ = 1;
    Rate rate_;
    ClockDomain domain_ = ClockDomain::Presentation;
    SkipPolicy skip_ = SkipPolicy::PresentationOnly;
    NetworkPolicy network_ = NetworkPolicy::LocalOnly;
    PersistenceClass persistence_ = PersistenceClass::None;
    bool deterministic_ = false;
    AccessibilityMetadata accessibility_;
    i64 begin_ = 0;
    i64 end_ = 0;

    Array<Segment> segments_;
    Array<CompiledChannel> channels_;
    Array<CompiledKey> keys_;
    Array<CompiledEvent> events_;
    Array<CompiledMarker> markers_;
    Array<CompiledBinding> bindings_;
    Array<CompiledParameter> parameters_;
    Array<PreloadEntry> preload_;
    Array<DebugEntry> debug_;
    Array<RequiredOutcome> outcomes_;

    /// The interval index: `bucket_offsets_` has `bucket_count_ + 1` entries into `bucket_items_`.
    Array<u32> bucket_offsets_;
    Array<u32> bucket_items_;
    u32 bucket_count_ = 0;
    i64 bucket_span_ = 1;
};

}  // namespace cy::sequencing
