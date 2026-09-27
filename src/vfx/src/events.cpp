// GPU-to-GPU event channels and the bounded readback path. M8.c task 2.5.
//
// THE ONE PLACE `src/vfx/` NAMES THE DETERMINISM FIREWALL. `ReadbackQueue::deliver` opens a
// `cy::ecs::WriteScope` of origin `Vfx` around the caller's sink, so a gameplay write reached from
// a collision readback arrives at the ECS labelled as VFX's and is refused there. VFX declares what
// it is; it does not decide what it is allowed to do.

#include <cy/vfx/events.h>

#include <cy/ecs/firewall.h>

#include <utility>

namespace cy::vfx {
namespace {

/// The rank comparison, spelled once: higher rank wins, and a tie is broken by ARRIVAL ORDER so
/// that the surviving set is a function of the sequence and not of the allocator.
[[nodiscard]] bool outranks(f32 rank, u32 arrival, f32 other_rank, u32 other_arrival) noexcept {
    if (rank != other_rank) {
        return rank > other_rank;
    }
    return arrival < other_arrival;
}

}  // namespace

EventRouter::EventRouter(Allocator& allocator) noexcept
    : channels_(allocator), reports_(allocator) {}

Status EventRouter::declare(const EventChannelDecl& decl) noexcept {
    if (decl.max_events_per_frame == 0 || decl.max_chain_depth == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "vfx: an event channel declares both bounds and neither may be zero");
    }
    if (find(decl.name) != nullptr) {
        return fail(ErrorCode::AlreadyExists, "vfx: that event channel is already declared");
    }
    Channel channel{decl, Array<EventRecord>(channels_.allocator()),
                    Array<u32>(channels_.allocator()), 0};
    if (Status reserved = channel.live.reserve(decl.max_events_per_frame); !reserved) {
        return reserved;
    }
    if (Status reserved = channel.arrival.reserve(decl.max_events_per_frame); !reserved) {
        return reserved;
    }
    if (Status pushed = channels_.push_back(std::move(channel)); !pushed) {
        return pushed;
    }
    ChannelReport report;
    report.channel = decl.name;
    report.max_events_per_frame = decl.max_events_per_frame;
    report.max_chain_depth = decl.max_chain_depth;
    report.readback = decl.readback;
    return reports_.push_back(report);
}

EventRouter::Channel* EventRouter::find(Name channel) noexcept {
    for (Channel& candidate : channels_) {
        if (candidate.decl.name == channel) {
            return &candidate;
        }
    }
    return nullptr;
}

ChannelReport* EventRouter::mutable_report(Name channel) noexcept {
    for (ChannelReport& candidate : reports_) {
        if (candidate.channel == channel) {
            return &candidate;
        }
    }
    return nullptr;
}

const ChannelReport* EventRouter::report(Name channel) const noexcept {
    for (const ChannelReport& candidate : reports_) {
        if (candidate.channel == channel) {
            return &candidate;
        }
    }
    return nullptr;
}

void EventRouter::begin_frame() noexcept {
    for (Channel& channel : channels_) {
        channel.live.clear();
        channel.arrival.clear();
        channel.next_arrival = 0;
    }
    for (ChannelReport& report : reports_) {
        report.raised = 0;
        report.delivered = 0;
        report.dropped = 0;
        report.truncated = 0;
    }
}

Status EventRouter::raise(Name channel_name, const EventRecord& event) noexcept {
    Channel* channel = find(channel_name);
    if (channel == nullptr) {
        return fail(ErrorCode::NotFound, "vfx: no event channel of that name is declared");
    }
    // `channels_` and `reports_` are appended together by `declare` and never removed from, so a
    // channel that was found above has a report. Written as a search that can fail anyway, because
    // a null dereference guarded only by an invariant is a null dereference the next edit breaks.
    ChannelReport* report = mutable_report(channel_name);
    if (report == nullptr) {
        return fail(ErrorCode::Internal, "vfx: an event channel has no report");
    }
    ++report->raised;

    // THE CHAIN DEPTH LIMIT. "WHEN an effect graph creates events whose chain exceeds the
    // configured maximum depth THEN the chain SHALL TERMINATE at that depth and the truncation
    // SHALL be reported."
    if (event.depth >= channel->decl.max_chain_depth) {
        ++report->truncated;
        return ok();
    }

    const u32 arrival = channel->next_arrival++;
    if (channel->live.size() < channel->decl.max_events_per_frame) {
        if (Status pushed = channel->live.push_back(event); !pushed) {
            return pushed;
        }
        if (Status pushed = channel->arrival.push_back(arrival); !pushed) {
            return pushed;
        }
        ++report->delivered;
        return ok();
    }

    // FULL. Find the weakest live event; if the new one outranks it, it replaces it. Either way one
    // event is dropped and counted — the loop cannot diverge, which is the whole requirement.
    usize weakest = 0;
    for (usize index = 1; index < channel->live.size(); ++index) {
        if (outranks(channel->live[weakest].rank, channel->arrival[weakest],
                     channel->live[index].rank, channel->arrival[index])) {
            weakest = index;
        }
    }
    ++report->dropped;
    if (outranks(event.rank, arrival, channel->live[weakest].rank, channel->arrival[weakest])) {
        channel->live[weakest] = event;
        channel->arrival[weakest] = arrival;
    }
    return ok();
}

Span<const EventRecord> EventRouter::consume(Name channel) const noexcept {
    for (const Channel& candidate : channels_) {
        if (candidate.decl.name == channel) {
            return candidate.live.span();
        }
    }
    return {};
}

u32 EventRouter::total_dropped() const noexcept {
    u32 total = 0;
    for (const ChannelReport& report : reports_) {
        total += report.dropped;
    }
    return total;
}

u32 EventRouter::total_truncated() const noexcept {
    u32 total = 0;
    for (const ChannelReport& report : reports_) {
        total += report.truncated;
    }
    return total;
}

// --- The bounded readback path -------------------------------------------------------------------

ReadbackQueue::ReadbackQueue(Allocator& allocator) noexcept
    : pending_(allocator), ready_(allocator) {
    report_.budget_bytes = 4096;
}

ReadbackQueue::Pending* ReadbackQueue::find(Array<Pending>& list, Name channel) noexcept {
    for (Pending& candidate : list) {
        if (candidate.channel == channel) {
            return &candidate;
        }
    }
    return nullptr;
}

Status ReadbackQueue::publish(Name channel, Span<const EventRecord> events) noexcept {
    Pending* entry = find(pending_, channel);
    if (entry == nullptr) {
        Pending created{channel, Array<EventRecord>(pending_.allocator())};
        if (Status pushed = pending_.push_back(std::move(created)); !pushed) {
            return pushed;
        }
        entry = &pending_[pending_.size() - 1];
    }
    if (Status appended = entry->events.append(events); !appended) {
        return appended;
    }
    report_.published += static_cast<u32>(events.size());
    return ok();
}

Status ReadbackQueue::begin_frame() noexcept {
    // AT LEAST ONE FRAME OF LATENCY, and it is here rather than in a comment: what `publish` was
    // given last frame becomes deliverable now, and what was deliverable and did not fit the budget
    // stays at the front of the queue.
    for (Pending& source : pending_) {
        Pending* destination = find(ready_, source.channel);
        if (destination == nullptr) {
            Pending created{source.channel, Array<EventRecord>(ready_.allocator())};
            if (Status pushed = ready_.push_back(std::move(created)); !pushed) {
                return pushed;
            }
            destination = &ready_[ready_.size() - 1];
        }
        if (Status appended = destination->events.append(source.events.span()); !appended) {
            return appended;
        }
        source.events.clear();
    }
    return ok();
}

Status ReadbackQueue::deliver(ReadbackSink sink, void* user) noexcept {
    if (sink == nullptr) {
        return fail(ErrorCode::InvalidArgument, "vfx: a readback needs a sink");
    }
    report_.delivered = 0;
    report_.deferred = 0;
    report_.bytes_delivered = 0;

    bool anything = false;
    for (Pending& channel : ready_) {
        anything = anything || !channel.events.empty();
    }
    if (!anything) {
        // NEVER STALLS. There is nothing to wait on and nothing to wait with: the caller proceeds
        // with what it already had, and the stale read is counted so the absence is visible.
        ++report_.stale_reads;
        return ok();
    }

    // THE FIREWALL SCOPE. Everything the sink does — and everything the sink calls — is VFX's, and
    // the ECS write path is what refuses an authoritative write made inside it. Opened once around
    // the whole delivery rather than per channel, because a sink that stored a pointer and wrote
    // later would be outside a per-callback scope and the scope has to cover the call tree.
    const ecs::WriteScope scope(ecs::WriteOrigin::Vfx, "vfx.collision-readback");

    const u64 stride = sizeof(EventRecord);
    u64 remaining = report_.budget_bytes / (stride == 0 ? 1 : stride);
    for (Pending& channel : ready_) {
        if (channel.events.empty()) {
            continue;
        }
        const usize count = channel.events.size() < remaining ? channel.events.size()
                                                              : static_cast<usize>(remaining);
        if (count != 0) {
            sink(channel.channel, Span<const EventRecord>(channel.events.data(), count), user);
            report_.delivered += static_cast<u32>(count);
            report_.bytes_delivered += static_cast<u64>(count) * stride;
            remaining -= count;
        }
        // THE EXCESS IS DEFERRED, NOT TRUNCATED. What did not fit stays at the front of the queue
        // and is delivered next frame, and the count says how much.
        const usize left = channel.events.size() - count;
        report_.deferred += static_cast<u32>(left);
        for (usize index = 0; index < left; ++index) {
            channel.events[index] = channel.events[index + count];
        }
        if (Status sized = channel.events.resize(left); !sized) {
            return sized;
        }
    }
    return ok();
}

Span<const EventRecord> ReadbackQueue::available(Name channel) const noexcept {
    for (const Pending& candidate : ready_) {
        if (candidate.channel == channel) {
            return candidate.events.span();
        }
    }
    return {};
}

}  // namespace cy::vfx
