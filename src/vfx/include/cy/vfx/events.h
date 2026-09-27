#pragma once
// GPU-TO-GPU EVENTS AND THE BOUNDED READBACK PATH. M8.c task 2.5.
//
// ================================================================================================
// TWO MECHANISMS, AND THE SPECIFICATION IS EMPHATIC ABOUT WHICH IS THE DEFAULT
// ================================================================================================
//
// `EventRouter` is the GPU-to-GPU path: an emitter raises an event, another emitter consumes it in
// its spawn or event stage, and nothing crosses to the CPU. `ReadbackQueue` is the other one, and
// `vfx-system` says of it: "Readback SHALL be opt-in per channel. The documentation SHALL state
// that it is NOT THE DEFAULT MECHANISM and that GPU-to-GPU events should be preferred."
//
// So `EventChannelDecl::readback` defaults to false and a channel that did not ask for it never
// reaches `ReadbackQueue` at all.
//
// ================================================================================================
// BOTH BOUNDS ARE ENFORCED, AND OVERFLOW IS REPORTED RATHER THAN COMPOUNDED
// ================================================================================================
//
// "Every event channel SHALL declare a maximum events per frame and every chain a maximum depth.
// Exceeding either SHALL DROP EVENTS BY A DETERMINISTIC RANK and report the overflow, rather than
// compounding."
//
// The rank is the event's own `rank` field, and the drop is deterministic in the strong sense: a
// full channel keeps the `max_events_per_frame` highest-ranked events it was offered, whatever
// order they arrived in, and ties are broken by arrival index. So the same sequence of raises
// always yields the same surviving set — which is what makes the feedback-bound test a check rather
// than a sample.
//
// ================================================================================================
// THE READBACK PATH IS WHERE VFX MEETS THE DETERMINISM FIREWALL
// ================================================================================================
//
// `vfx-system`: "Development builds SHALL detect and report attempts to write replicated or
// physics-owned components from VFX-driven code paths." M8.c section 1 built that enforcement point
// in the ECS write path, and `ReadbackQueue::deliver` is where VFX holds up its end: it opens a
// `cy::ecs::WriteScope(WriteOrigin::Vfx, "vfx.collision-readback")` around the caller's sink, so
// any ECS write the sink performs — directly or four frames deep in a gameplay call — arrives at
// the firewall labelled as VFX's.
//
// VFX DOES NOT ASK THE FIREWALL FOR PERMISSION AND IT DOES NOT ENFORCE THE RULE. It declares its
// origin; the ECS refuses. That split is deliberate: a subsystem that checked and then wrote anyway
// would be a subsystem whose check is decorative.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/vfx/asset.h>

namespace cy::vfx {

/// One event. Fixed width, because an event buffer is a GPU allocation whose stride the channel
/// declares — a variable payload would be a second allocator on the device.
struct EventRecord {
    /// Which particle raised it.
    u32 source = 0;
    /// How many events deep in a chain this one is. Zero for an event raised by a stage rather
    /// than by another event.
    u32 depth = 0;
    /// THE DETERMINISTIC RANK. Higher survives when the channel is full.
    f32 rank = 0.0F;
    f32 payload[4] = {0.0F, 0.0F, 0.0F, 0.0F};
};

static_assert(sizeof(EventRecord) == 28,
              "EventRecord is a GPU-side stride: a member that changed its size would change every "
              "declared channel's buffer layout without changing any declaration");

/// What one channel did this frame.
struct ChannelReport {
    Name channel;
    u32 raised = 0;
    u32 delivered = 0;
    /// Dropped at the per-frame cap, by rank.
    u32 dropped = 0;
    /// Refused because the chain reached its maximum depth.
    u32 truncated = 0;
    u32 max_events_per_frame = 0;
    u32 max_chain_depth = 0;
    bool readback = false;
};

/// The GPU-to-GPU event path. One router per simulation world.
class EventRouter {
public:
    explicit EventRouter(Allocator& allocator) noexcept;

    EventRouter(const EventRouter&) = delete;
    EventRouter& operator=(const EventRouter&) = delete;

    [[nodiscard]] Status declare(const EventChannelDecl& decl) noexcept;
    [[nodiscard]] const ChannelReport* report(Name channel) const noexcept;
    [[nodiscard]] Span<const ChannelReport> reports() const noexcept { return reports_.span(); }

    /// Begin a frame: every channel's pending list becomes the consumable list and the counters
    /// reset. An event raised this frame is consumable THIS frame where the dependency order
    /// permits — which is what `consume` returns — and the next one otherwise.
    void begin_frame() noexcept;

    /// Raise an event. Never fails for a full channel: it drops by rank and counts, because
    /// "exceeding either SHALL drop events by a deterministic rank and report the overflow, rather
    /// than compounding". Fails only for a channel that was never declared.
    [[nodiscard]] Status raise(Name channel, const EventRecord& event) noexcept;

    /// The events another emitter's spawn or event stage consumes.
    [[nodiscard]] Span<const EventRecord> consume(Name channel) const noexcept;

    [[nodiscard]] u32 total_dropped() const noexcept;
    [[nodiscard]] u32 total_truncated() const noexcept;

    [[nodiscard]] Allocator& allocator() const noexcept { return reports_.allocator(); }

private:
    struct Channel {
        EventChannelDecl decl;
        Array<EventRecord> live;
        /// Arrival index of each live event, so a rank tie breaks the same way every run.
        Array<u32> arrival;
        u32 next_arrival = 0;
    };

    [[nodiscard]] Channel* find(Name channel) noexcept;
    [[nodiscard]] ChannelReport* mutable_report(Name channel) noexcept;

    Array<Channel> channels_;
    Array<ChannelReport> reports_;
};

/// What one frame's readback moved, and what it could not.
struct ReadbackReport {
    u32 published = 0;
    u32 delivered = 0;
    /// Events the per-frame byte budget could not carry, held for the next frame rather than
    /// dropped. "the excess SHALL be deferred and reported, not silently truncated".
    u32 deferred = 0;
    u64 bytes_delivered = 0;
    u64 budget_bytes = 0;
    /// Frames the CPU asked and there was nothing new. `vfx-system`: "the system SHALL proceed with
    /// the most recent available data rather than blocking."
    u32 stale_reads = 0;
    /// Always at least one. The latency is a documented property, not an accident.
    u32 latency_frames = 1;
};

/// What a readback delivers into. A plain function pointer so the sink can be a free function and
/// the queue holds no allocation for it.
using ReadbackSink = void (*)(Name channel, Span<const EventRecord> events, void* user) noexcept;

/// The bounded readback path. `vfx-system`: "Readback SHALL have a documented latency of at least
/// one frame, SHALL be budgeted as a maximum bytes-per-frame, and SHALL NEVER STALL THE FRAME
/// waiting on the GPU."
///
/// There is no fence in this class and nowhere to put one, which is how "never stalls" is made
/// structural: `publish` hands over whatever the device produced and returns, and `deliver` reads
/// what was published at least one frame ago.
class ReadbackQueue {
public:
    explicit ReadbackQueue(Allocator& allocator) noexcept;

    ReadbackQueue(const ReadbackQueue&) = delete;
    ReadbackQueue& operator=(const ReadbackQueue&) = delete;

    /// The per-frame byte budget. Zero disables readback entirely, which is a legitimate shipping
    /// configuration and is reported rather than silently ignored.
    void set_budget(u64 bytes_per_frame) noexcept { report_.budget_bytes = bytes_per_frame; }

    /// The device side: what this frame's readback copy produced. Returns immediately.
    [[nodiscard]] Status publish(Name channel, Span<const EventRecord> events) noexcept;

    /// Advance one frame. What was published becomes deliverable; what was deliverable and did not
    /// fit the budget stays.
    [[nodiscard]] Status begin_frame() noexcept;

    /// Deliver to `sink`, inside a `cy::ecs::WriteScope` of origin `Vfx`. See the note at the top
    /// of this file: this is where VFX declares what it is, and the ECS is what refuses.
    [[nodiscard]] Status deliver(ReadbackSink sink, void* user) noexcept;

    [[nodiscard]] const ReadbackReport& report() const noexcept { return report_; }
    /// What is deliverable right now, without delivering it. Empty when the device has not
    /// produced anything a frame old yet, which is the ordinary state on the first frame and is
    /// counted as a stale read rather than waited on.
    [[nodiscard]] Span<const EventRecord> available(Name channel) const noexcept;

private:
    struct Pending {
        Name channel;
        Array<EventRecord> events;
    };

    [[nodiscard]] static Pending* find(Array<Pending>& list, Name channel) noexcept;

    Array<Pending> pending_;
    Array<Pending> ready_;
    ReadbackReport report_;
};

}  // namespace cy::vfx
