// The tracking wrapper. Tasks 2.1 and 2.11.

#include <cy/core/memory/tracking_allocator.h>

#include <cy/core/memory/attribution.h>

#include <cy/core/memory/lifetime.h>

#include <algorithm>
#include <cstring>

namespace cy {
namespace {

constexpr u64 kLiveMagic = 0x435954524143'4b31ull;  // "CYTRACK1"
constexpr u64 kDeadMagic = 0x435954524143'4446ull;  // "CYTRACDF" — freed
constexpr u8 kRedZoneByte = 0xAB;
constexpr u8 kFreedPoisonByte = 0xFE;

const AllocationSite kUnknownSite{"<unknown>", "<unknown>", 0};

thread_local const AllocationSite* t_site = &kUnknownSite;

}  // namespace

const char* capture_mode_name(CaptureMode mode) noexcept {
    switch (mode) {
        case CaptureMode::Off:
            return "off";
        case CaptureMode::Sampled:
            return "sampled";
        case CaptureMode::Full:
            return "full";
    }
    return "unknown";
}

const AllocationSite& current_allocation_site() noexcept {
    return *t_site;
}

AllocationSiteScope::AllocationSiteScope(const AllocationSite& site) noexcept : previous_(t_site) {
    t_site = &site;
}

AllocationSiteScope::~AllocationSiteScope() {
    t_site = previous_;
}

TrackingAllocator::TrackingAllocator(Allocator& upstream, MemoryDomain domain,
                                     AllocationTag tag) noexcept
    : Allocator(domain, tag), upstream_(&upstream) {}

TrackingAllocator::~TrackingAllocator() {
    // Whatever is still live goes back upstream. Reporting it is the caller's business — a
    // destructor that printed would print from a test that meant to leak on purpose — but leaving
    // the blocks behind would turn one report into two.
    Header* header = live_;
    while (header != nullptr) {
        Header* following = header->next;
        // NOLINTNEXTLINE(bugprone-casting-through-void) — see the note in do_allocate().
        u8* const block = static_cast<u8*>(static_cast<void*>(header)) - header->padding;
        upstream_->deallocate(block, header->block_bytes, header->alignment);
        header = following;
    }
    live_ = nullptr;
}

void TrackingAllocator::set_capture_mode(CaptureMode mode, u32 sample_interval) noexcept {
    capture_ = mode;
    sample_interval_ = (sample_interval == 0) ? 1 : sample_interval;
}

bool TrackingAllocator::should_capture() const noexcept {
    switch (capture_) {
        case CaptureMode::Off:
            return false;
        case CaptureMode::Full:
            return true;
        case CaptureMode::Sampled:
            return (sequence_ % sample_interval_) == 0;
    }
    return false;
}

void* TrackingAllocator::do_allocate(usize size, usize alignment) noexcept {
    // The block holds a header, whatever padding the requested alignment needs, the payload, and a
    // red zone. The alignment asked of upstream is the stronger of the caller's and the header's,
    // so that stepping back by sizeof(Header) from an aligned payload lands on an aligned header.
    const usize block_alignment = (alignment < alignof(Header)) ? alignof(Header) : alignment;
    const usize red_zone = red_zones_ ? kRedZoneBytes : 0;
    const usize block_bytes = sizeof(Header) + block_alignment + size + red_zone;

    void* block = upstream_->allocate(block_bytes, block_alignment);
    if (block == nullptr) {
        return nullptr;
    }

    auto* const base = static_cast<u8*>(block);
    auto* const payload = static_cast<u8*>(align_up(base + sizeof(Header), block_alignment));
    // Through void* on purpose: the header and its payload are one byte block seen at two offsets,
    // and a direct reinterpret_cast between `u8*` and `Header*` is what -Wcast-align reports under
    // the engine's -Werror. `block_alignment` is at least alignof(Header), established above.
    // NOLINTNEXTLINE(bugprone-casting-through-void)
    auto* const header = static_cast<Header*>(static_cast<void*>(payload - sizeof(Header)));

    header->magic = kLiveMagic;
    header->previous = nullptr;
    header->next = live_;
    header->payload_bytes = size;
    header->block_bytes = block_bytes;
    header->padding = static_cast<usize>((payload - sizeof(Header)) - base);
    header->alignment = block_alignment;
    header->tag = tag_;
    header->site = should_capture() ? *t_site : kUnknownSite;
    // The four axes M7 task 3.1 adds. Recorded unconditionally rather than under `should_capture`:
    // capture mode is about the COST of a call site, which is a stack walk in the modes above
    // `Off`, and four integers copied from a thread-local is not that cost. A sampled report that
    // attributed one allocation in sixty-four to a world cell would be a report nobody could use.
    header->attribution = current_attribution();
    header->thread = current_thread_ordinal();
    header->sequence = sequence_++;

    if (live_ != nullptr) {
        live_->previous = header;
    }
    live_ = header;

    if (red_zones_) {
        std::memset(payload + size, kRedZoneByte, kRedZoneBytes);
    }

    ++live_allocations_;
    live_bytes_ += size;
    peak_bytes_ = std::max(peak_bytes_, live_bytes_);
    return payload;
}

void* TrackingAllocator::do_reallocate(void* pointer, usize old_size, usize new_size,
                                       usize alignment) noexcept {
    // Always allocate-copy-free, so the new block gets its own header, red zone and call site
    // rather than inheriting the old one's — a reallocation is where a buffer's owner most often
    // changes, and a report that named the original allocation would send the reader to the wrong
    // place.
    return reallocate_by_copy(pointer, old_size, new_size, alignment);
}

void TrackingAllocator::unlink(Header& header) noexcept {
    if (header.previous != nullptr) {
        header.previous->next = header.next;
    } else {
        live_ = header.next;
    }
    if (header.next != nullptr) {
        header.next->previous = header.previous;
    }
    header.previous = nullptr;
    header.next = nullptr;
}

bool TrackingAllocator::was_recently_freed(const void* pointer) const noexcept {
    return std::ranges::any_of(recent_frees_,
                               [pointer](const void* candidate) { return candidate == pointer; });
}

void TrackingAllocator::remember_free(const void* pointer) noexcept {
    recent_frees_[recent_free_next_] = pointer;
    recent_free_next_ = (recent_free_next_ + 1) % kRecentFrees;
}

void TrackingAllocator::do_deallocate(void* pointer, usize size, usize alignment) noexcept {
    (void)size;
    (void)alignment;

    // Checked before anything is read through `pointer`: the block is already back with the
    // upstream allocator, so reading its header would be a use-after-free. See the note in the
    // header on what this ring does and does not cover.
    if (was_recently_freed(pointer)) {
        ++double_frees_;
        return;
    }

    auto* const payload = static_cast<u8*>(pointer);
    // NOLINTNEXTLINE(bugprone-casting-through-void) — see the note in do_allocate().
    auto* const header = static_cast<Header*>(static_cast<void*>(payload - sizeof(Header)));

    if (header->magic != kLiveMagic) {
        // A pointer this allocator never handed out. Counted rather than asserted: an assertion is
        // compiled out of the two configurations where this is hardest to find, and a counter is
        // not.
        ++double_frees_;
        return;
    }

    if (red_zones_) {
        const u8* zone = payload + header->payload_bytes;
        for (usize index = 0; index < kRedZoneBytes; ++index) {
            if (zone[index] != kRedZoneByte) {
                ++overruns_;
                break;
            }
        }
    }

    unlink(*header);
    --live_allocations_;
    live_bytes_ -= header->payload_bytes;

    const usize payload_bytes = header->payload_bytes;
    const usize block_bytes = header->block_bytes;
    const usize padding = header->padding;
    const usize block_alignment = header->alignment;
    header->magic = kDeadMagic;

    if (poison_) {
        std::memset(payload, kFreedPoisonByte, payload_bytes);
    }
    withdraw_process_lifetime(payload);
    remember_free(payload);
    upstream_->deallocate(payload - sizeof(Header) - padding, block_bytes, block_alignment);
}

LeakReport TrackingAllocator::report_leaks(LeakSink sink, void* user) const noexcept {
    LeakReport report;
    for (const Header* header = live_; header != nullptr; header = header->next) {
        // NOLINTBEGIN(bugprone-casting-through-void) — see the note in do_allocate().
        const auto* payload =
            static_cast<const u8*>(static_cast<const void*>(header)) + sizeof(Header);
        // NOLINTEND(bugprone-casting-through-void)
        const bool intentional = is_process_lifetime(payload);

        ++report.live_allocations;
        report.live_bytes += header->payload_bytes;

        if (intentional) {
            ++report.process_lifetime_allocations;
            report.process_lifetime_bytes += header->payload_bytes;
            continue;  // declared, so not a defect and not in the report
        }

        ++report.leaked_allocations;
        report.leaked_bytes += header->payload_bytes;
        if (sink != nullptr) {
            TrackedAllocation entry;
            entry.pointer = payload;
            entry.bytes = header->payload_bytes;
            entry.tag = header->tag;
            entry.site = header->site;
            entry.attribution = header->attribution;
            entry.thread = header->thread;
            entry.sequence = header->sequence;
            entry.process_lifetime = false;
            sink(entry, user);
        }
    }
    return report;
}

namespace {

/// The key one header contributes on one axis. `high` is non-zero only for the asset axis.
struct AxisKey {
    u64 low = 0;
    u64 high = 0;

    [[nodiscard]] bool is_unattributed() const noexcept { return low == 0 && high == 0; }
};

[[nodiscard]] AxisKey key_of(const MemoryAttribution& attribution, u32 thread,
                             AttributionAxis axis) noexcept {
    switch (axis) {
        case AttributionAxis::Type:
            return AxisKey{attribution.type, 0};
        case AttributionAxis::Thread:
            return AxisKey{thread, 0};
        case AttributionAxis::WorldCell:
            return AxisKey{attribution.world_cell, 0};
        case AttributionAxis::Asset:
            return AxisKey{attribution.asset_low, attribution.asset_high};
    }
    return AxisKey{};
}

}  // namespace

MemoryAttributionSummary TrackingAllocator::report_attribution(
    AttributionAxis axis, Span<MemoryAttributionRow> out) const noexcept {
    MemoryAttributionSummary summary;
    for (MemoryAttributionRow& row : out) {
        row = MemoryAttributionRow{};
    }

    usize used = 0;
    for (const Header* header = live_; header != nullptr; header = header->next) {
        const AxisKey key = key_of(header->attribution, header->thread, axis);
        if (key.is_unattributed()) {
            // Nobody declared this axis for this allocation. Reported as its own figure rather
            // than folded into a row, because "most of this is unattributed" is a true and useful
            // answer and a zero key pretending to be a type is not.
            summary.unattributed_bytes += header->payload_bytes;
            continue;
        }

        usize slot = used;
        for (usize index = 0; index < used; ++index) {
            if (out[index].key == key.low && out[index].key_high == key.high) {
                slot = index;
                break;
            }
        }
        if (slot == used) {
            if (used == out.size()) {
                // The span is full and this is a key it has not seen. Counted, and its bytes are
                // reported as unreported — see the note on `distinct_keys`.
                ++summary.distinct_keys;
                summary.unreported_bytes += header->payload_bytes;
                continue;
            }
            out[used].key = key.low;
            out[used].key_high = key.high;
            ++used;
            ++summary.distinct_keys;
        }
        out[slot].live_bytes += header->payload_bytes;
        ++out[slot].live_allocations;
        summary.reported_bytes += header->payload_bytes;
    }

    // Descending by live bytes. Insertion sort over a span whose size is the caller's own row
    // budget — tens, not thousands — and it allocates nothing, which is the property a memory
    // report cannot do without.
    for (usize index = 1; index < used; ++index) {
        MemoryAttributionRow row = out[index];
        usize position = index;
        while (position > 0 && out[position - 1].live_bytes < row.live_bytes) {
            out[position] = out[position - 1];
            --position;
        }
        out[position] = row;
    }

    summary.rows = static_cast<u32>(used);
    return summary;
}

}  // namespace cy
