#pragma once
// The bus graph: a directed acyclic graph of mixing nodes with `Master` at its root. Task 4.3.5.
//
// `audio` — "Bus graph": audio "SHALL be routed through a graph of **buses**, each with a volume,
// mute, solo, and bypass flag, an ordered effect chain, and a send to another bus"; "The graph
// SHALL be a directed acyclic graph, with a `Master` bus as the root. Cycles SHALL be rejected when
// configured"; and "Buses SHALL support multiple sends with independent levels, enabling reverb and
// submix routing."
//
// ================================================================================================
// THE CYCLE CHECK IS AT CONFIGURATION TIME, WHICH IS THE ONLY PLACE IT CAN BE
// ================================================================================================
//
// A cycle in a mixing graph is not a slow mix, it is an infinite one, and the thread it would hang
// is the realtime thread — the symptom is silence and a locked-up device rather than a stack trace.
// So `set_output()` and `add_send()` each ask whether the edge they are about to add would close a
// loop, and refuse it naming both ends. That is `audio`'s "**WHEN** a send would create a cycle
// **THEN** the configuration SHALL be rejected with a diagnostic", and it is why the graph is
// mutated through methods rather than by writing a field.
//
// ================================================================================================
// SOLO IS A PROPERTY OF THE GRAPH, NOT OF A BUS
// ================================================================================================
//
// "**WHEN** a bus is soloed **THEN** buses that do not feed it SHALL be silenced for the mix."
// Whether a bus is audible therefore depends on every other bus's solo flag and on the edges
// between them — it cannot be answered by looking at the bus. `audible()` answers it by asking
// whether the bus reaches any soloed bus, which is the requirement stated as a reachability
// question.
//
// ================================================================================================
// THE EFFECT SET IS SMALL ON PURPOSE
// ================================================================================================
//
// `audio`'s "Effects" requirement lists twenty: EQ, compressor, limiter, gate, reverb, delay,
// chorus, flanger, phaser, distortion, pitch shift, widener, metering. Four are implemented here —
// gain, one-pole low pass, one-pole high pass and a hard limiter — because those four are what the
// Seed tier's own requirements need: a bus gain, the filter that filter-based occlusion is made of,
// and something that stops a summed mix clipping. The other sixteen are M8's, and they are ABSENT
// rather than declared and silently passed through: an enumerator named `Reverb` that did nothing
// would make an authored reverb bus produce a dry mix with no diagnostic, which is exactly the
// class of failure a stub buys.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/servers/audio/handles.h>

namespace cy::audio {

/// The effects implemented at Seed. See the header comment for the sixteen that are not here.
enum class EffectKind : u8 {
    /// A flat gain, in linear amplitude.
    Gain = 0,
    /// One-pole low pass. `cutoff_hz` in `parameter_a`. What filter-based occlusion is made of.
    LowPass,
    /// One-pole high pass. `cutoff_hz` in `parameter_a`.
    HighPass,
    /// Hard limiter at `parameter_a` (linear amplitude, default 1.0).
    Limiter,
    Count,
};

[[nodiscard]] const char* effect_kind_name(EffectKind kind) noexcept;

/// One entry of a bus's ordered effect chain.
struct BusEffect {
    EffectKind kind = EffectKind::Gain;
    f32 parameter_a = 1.0F;
    f32 parameter_b = 0.0F;
    bool bypass = false;
    /// Frames of processing latency this effect introduces, so the engine can compensate. Zero for
    /// all four of the effects here — none of them looks ahead — and non-zero for the partitioned
    /// convolution that arrives at M8, which is why the field exists now rather than then.
    u32 latency_frames = 0;
};

/// A bus's authored state.
struct BusDescription {
    Name name;
    /// Where this bus's output goes. Null means `Master`; `Master`'s own output is null and stays
    /// null, which is what makes it the root.
    BusHandle output;
    f32 volume = 1.0F;
    bool mute = false;
    bool solo = false;
    /// Skip this bus's effect chain, keeping its routing and its gain. What a sound designer
    /// toggles to hear what an effect is doing.
    bool bypass = false;
};

/// One send: a fraction of a bus's output routed somewhere else as well.
struct BusSend {
    BusHandle target;
    f32 level = 0.0F;
};

/// The mixing graph.
///
/// NOT a general graph container: it is walked by the realtime mixer, so the compiled order is
/// computed on the game thread by `compile()` and read on the audio thread. Mutating the graph
/// without recompiling leaves the mixer on the previous order, which is correct — it is the
/// previous graph, mixed consistently — rather than half of each.
class BusGraph {
public:
    /// The number of sends one bus may have. Fixed, so a bus record is a fixed size and the mixer's
    /// inner loop has no indirection. Four covers a main path plus reverb, submix and a metering
    /// tap; a design that needs more sends than that is describing a bus.
    static constexpr u32 kMaxSends = 4;
    /// The depth the effect chain is walked to. Same reasoning.
    static constexpr u32 kMaxEffects = 8;

    explicit BusGraph(Allocator& allocator) noexcept;

    /// `Master`, created by the constructor and never destroyable. The root every path ends at.
    [[nodiscard]] BusHandle master() const noexcept { return master_; }

    [[nodiscard]] Expected<BusHandle, Error> create(const BusDescription& description) noexcept;
    /// Destroy a bus. Refuses `Master`, and refuses a bus that anything still routes into — a
    /// dangling output would silently drop a submix.
    [[nodiscard]] Status destroy(BusHandle handle) noexcept;

    [[nodiscard]] bool alive(BusHandle handle) const noexcept;
    [[nodiscard]] usize size() const noexcept { return buses_.size(); }

    [[nodiscard]] const BusDescription* description(BusHandle handle) const noexcept;
    [[nodiscard]] Status set_volume(BusHandle handle, f32 volume) noexcept;
    [[nodiscard]] Status set_mute(BusHandle handle, bool mute) noexcept;
    [[nodiscard]] Status set_solo(BusHandle handle, bool solo) noexcept;
    [[nodiscard]] Status set_bypass(BusHandle handle, bool bypass) noexcept;

    /// Route `handle`'s output into `output`. Refused when it would close a cycle.
    [[nodiscard]] Status set_output(BusHandle handle, BusHandle output) noexcept;

    /// Add a send. Refused when it would close a cycle, when the bus already has `kMaxSends`, or
    /// when the same target is already a send of this bus — two sends to one target are one send
    /// with the sum of their levels, and keeping both would make the level depend on the order they
    /// were added in.
    [[nodiscard]] Status add_send(BusHandle from, BusHandle to, f32 level) noexcept;
    [[nodiscard]] Status set_send_level(BusHandle from, BusHandle to, f32 level) noexcept;
    [[nodiscard]] Status remove_send(BusHandle from, BusHandle to) noexcept;
    [[nodiscard]] Span<const BusSend> sends(BusHandle handle) const noexcept;

    [[nodiscard]] Status add_effect(BusHandle handle, const BusEffect& effect) noexcept;
    [[nodiscard]] Status clear_effects(BusHandle handle) noexcept;
    [[nodiscard]] Span<const BusEffect> effects(BusHandle handle) const noexcept;
    /// Total reported latency along the longest path to `Master`, which is what a caller
    /// synchronising to the audio clock must compensate for.
    [[nodiscard]] u32 latency_frames(BusHandle handle) const noexcept;

    /// Recompute the processing order. Called after any structural change, on the game thread.
    ///
    /// The order is "every bus before the buses it feeds", so processing it front to back mixes a
    /// send's source before its target. Deterministic: ties are broken by creation order, never by
    /// a container's layout.
    [[nodiscard]] Status compile() noexcept;
    /// The compiled order. Empty until `compile()` has run.
    [[nodiscard]] Span<const BusHandle> order() const noexcept { return order_.span(); }
    [[nodiscard]] bool compiled() const noexcept { return compiled_; }

    /// True when any bus is soloed, which is what puts the graph into solo mode.
    [[nodiscard]] bool any_solo() const noexcept;
    /// Whether `handle` is heard in the current mix: not muted, and either nothing is soloed or it
    /// reaches something that is. See the header comment.
    [[nodiscard]] bool audible(BusHandle handle) const noexcept;

    /// Dense index of a bus, for the mixer's per-bus buffers. `kInvalidIndex` for a stale handle.
    static constexpr u32 kInvalidIndex = 0xFFFFFFFFU;
    [[nodiscard]] u32 index_of(BusHandle handle) const noexcept;
    [[nodiscard]] BusHandle handle_at(u32 index) const noexcept;

private:
    struct Bus {
        BusDescription description;
        BusHandle handle;
        u32 generation = 1;
        bool live = false;
        u32 send_count = 0;
        BusSend send_storage[kMaxSends];
        u32 effect_count = 0;
        BusEffect effect_storage[kMaxEffects];
    };

    /// A bus's outgoing edges — its output and its sends — in one iterable. Every walk of this
    /// graph treats the two identically, and three hand-written copies of "the output, then the
    /// sends" would be three places for one of them to forget the output.
    struct Outgoing {
        BusHandle storage[kMaxSends + 1];
        u32 count = 0;

        [[nodiscard]] const BusHandle* begin() const noexcept { return storage; }
        [[nodiscard]] const BusHandle* end() const noexcept { return storage + count; }
    };

    [[nodiscard]] Bus* find(BusHandle handle) noexcept;
    [[nodiscard]] const Bus* find(BusHandle handle) const noexcept;
    [[nodiscard]] static Outgoing outgoing_of(const Bus& bus) noexcept;
    /// Does `from` reach `to` by following outputs and sends? The cycle check, and the solo check.
    [[nodiscard]] bool reaches(BusHandle from, BusHandle to) const noexcept;
    /// `compile()`'s two halves: the in-degree table, and the edges an emitted bus releases.
    void count_incoming(Array<u32>& remaining) const noexcept;
    void release_outgoing(const Bus& bus, Array<u32>& remaining) const noexcept;

    Array<Bus> buses_;
    Array<BusHandle> order_;
    BusHandle master_;
    bool compiled_ = false;
};

}  // namespace cy::audio
