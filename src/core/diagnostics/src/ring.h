#pragma once
// One producer's bounded buffer, and the loss policy expressed as its admission rule.
//
// `diagnostics-profiling-and-crash` — "Buffering and loss policy": per-thread buffers with no
// shared lock, drained by a background consumer; channels declare a priority and under pressure the
// system drops by priority, discarding verbose first and preserving breadcrumbs, tick boundaries
// and task lifecycle last; dropped events are counted and reported; a producer never blocks waiting
// for a consumer.
//
// Single producer, single consumer. The producer owns `write_`, the consumer owns `read_`, and the
// release store that publishes a record is the only synchronisation either performs. There is no
// lock in this file, so there is no lock in the emission path.
//
// Records never straddle the end of the buffer: when one does not fit in the tail, a Padding record
// fills the remainder and the record starts at zero. That costs a few bytes and buys a consumer
// that reads a record with one pointer and no wrap arithmetic.

#include <cy/core/diagnostics/format.h>
#include <cy/core/diagnostics/prelude.h>
#include <cy/core/diagnostics/trace.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace cy::diag {

/// The fraction of the buffer each channel may fill, in per cent. A channel is refused once the
/// buffer is fuller than its share, so a flood of verbose records cannot crowd out the critical
/// ones behind it — the policy the specification states, as a number.
inline constexpr u32 kChannelFillPercent[kChannelCount] = {
    100,  // Critical: breadcrumbs, tick boundaries, loss records themselves
    85,   // Important
    50,   // Verbose
    25,   // Sampled
};

class ThreadRing {
public:
    ThreadRing() = default;
    ~ThreadRing() { shutdown(); }

    ThreadRing(const ThreadRing&) = delete;
    ThreadRing& operator=(const ThreadRing&) = delete;
    ThreadRing(ThreadRing&&) = delete;
    ThreadRing& operator=(ThreadRing&&) = delete;

    /// One allocation, when the thread first emits. Never in steady state.
    bool initialize(u32 capacity_bytes) noexcept {
        u32 capacity = 1024;
        while (capacity < capacity_bytes && capacity < (1u << 26)) {
            capacity <<= 1;
        }
        data_ = static_cast<u8*>(std::malloc(capacity));
        if (data_ == nullptr) {
            return false;
        }
        capacity_ = capacity;
        mask_ = capacity - 1;
        return true;
    }

    void shutdown() noexcept {
        std::free(data_);
        data_ = nullptr;
        capacity_ = 0;
        mask_ = 0;
    }

    [[nodiscard]] bool ready() const noexcept { return data_ != nullptr; }
    [[nodiscard]] u32 capacity() const noexcept { return capacity_; }

    /// Reserve `size` contiguous bytes for a record. Null when the loss policy refuses it, in which
    /// case the refusal has already been counted. `size` must be a multiple of eight.
    u8* reserve(u32 size, Channel channel) noexcept {
        const u64 write = write_.load(std::memory_order_relaxed);
        const u64 read = read_.load(std::memory_order_acquire);
        const u32 used = static_cast<u32>(write - read);
        const u32 offset = static_cast<u32>(write & mask_);
        const u32 to_end = capacity_ - offset;
        const u32 padding = (to_end < size) ? to_end : 0;

        if (size > capacity_) {
            count_drop(channel, format::LossReason::RecordTooLarge);
            return nullptr;
        }
        if (!admits(channel, used + padding + size)) {
            count_drop(channel, format::LossReason::BufferPressure);
            return nullptr;
        }
        if (padding != 0) {
            write_padding(offset, padding);
        }
        pending_padding_ = padding;
        pending_size_ = size;
        return data_ + ((padding != 0) ? 0 : offset);
    }

    /// Publish the reserved record. The release store is what makes it visible to the consumer.
    void commit() noexcept {
        const u64 write = write_.load(std::memory_order_relaxed);
        write_.store(write + pending_padding_ + pending_size_, std::memory_order_release);
        pending_padding_ = 0;
        pending_size_ = 0;
    }

    /// Hand every published record to `visit(const u8*, u32)`, oldest first. Consumer side only.
    template <class Visitor>
    u32 drain(Visitor&& visit) noexcept {
        u64 read = read_.load(std::memory_order_relaxed);
        const u64 write = write_.load(std::memory_order_acquire);
        u32 records = 0;
        while (read < write) {
            const u8* record = data_ + static_cast<u32>(read & mask_);
            format::RecordHeader header{};
            std::memcpy(&header, record, sizeof(header));
            if (header.size == 0) {
                break;  // impossible unless the buffer is corrupt; stop rather than spin
            }
            if (header.kind != static_cast<u8>(EventKind::Padding)) {
                visit(record, static_cast<u32>(header.size));
                ++records;
            }
            read += header.size;
        }
        read_.store(read, std::memory_order_release);
        return records;
    }

    /// Read and clear one channel's refusal count. Called by the consumer, which turns it into a
    /// Loss record on the timeline and a LOSS chunk entry in the artefact.
    u64 take_drops(u32 channel) noexcept {
        return drops_[channel].exchange(0, std::memory_order_relaxed);
    }

    u64 take_oversized() noexcept { return oversized_.exchange(0, std::memory_order_relaxed); }

private:
    [[nodiscard]] bool admits(Channel channel, u32 wanted) const noexcept {
        const u32 percent = kChannelFillPercent[static_cast<u32>(channel)];
        const u64 limit = (static_cast<u64>(capacity_) * percent) / 100u;
        return wanted <= limit;
    }

    /// One counter per reason, because "dropped" and "could never have fitted" are different
    /// defects and a report that conflates them sends the reader to the wrong place.
    void count_drop(Channel channel, format::LossReason reason) noexcept {
        if (reason == format::LossReason::RecordTooLarge) {
            oversized_.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        drops_[static_cast<u32>(channel)].fetch_add(1, std::memory_order_relaxed);
    }

    void write_padding(u32 offset, u32 bytes) noexcept {
        format::RecordHeader header{};
        header.size = static_cast<u16>(bytes);
        header.kind = static_cast<u8>(EventKind::Padding);
        std::memcpy(data_ + offset, &header, sizeof(header));
    }

    u8* data_ = nullptr;
    u32 capacity_ = 0;
    u32 mask_ = 0;
    u32 pending_padding_ = 0;
    u32 pending_size_ = 0;
    alignas(64) std::atomic<u64> write_{0};
    alignas(64) std::atomic<u64> read_{0};
    std::atomic<u64> drops_[kChannelCount] = {};
    std::atomic<u64> oversized_{0};
};

}  // namespace cy::diag
