// SPDX-License-Identifier: MIT
#pragma once
// Messages larger than one datagram: split on send, reassembled on receipt. Issue #82.
//
// ================================================================================================
// WHY THIS LIVES ABOVE THE RELIABILITY LAYER
// ================================================================================================
//
// `Transport::send()` refuses a payload larger than `kMaxDatagramPayload` and says fragmentation
// "belongs to the layer that knows what it is sending". The secure transport is that layer for the
// session: a full-baseline snapshot of a few hundred entities is larger than a datagram, and it is
// one tick's state that must arrive whole or not at all — "a delta SHALL never mix state from
// different simulation ticks". So every application payload the secure transport carries starts
// with one kind byte: `Whole`, or `Fragment` followed by a message number, an index and a count.
//
// Fragments travel on the message's own channel and in its own delivery mode. A reliable message's
// fragments are each retransmitted until acknowledged, and an ordered channel delivers them in
// order. An unreliable message is delivered only when every fragment arrived; a message with a
// missing fragment is discarded when it ages out or when the reassembler needs its slot, and never
// handed up partly — which is exactly what an unreliable message means.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/networking/transport.h>

namespace cy::net {

/// The first byte of every payload the secure transport carries: a whole message or one fragment.
enum class MessageKind : u8 {
    Whole = 0,
    Fragment = 1,
};

/// A whole message's prefix: the kind byte.
inline constexpr u32 kWholeHeaderBytes = 1;
/// A fragment's prefix: the kind, a 16-bit message number, the index and the count.
inline constexpr u32 kFragmentHeaderBytes = 5;
/// The application bytes one fragment carries.
inline constexpr u32 kFragmentChunk = kMaxDatagramPayload - kFragmentHeaderBytes;
/// The largest unfragmented message.
inline constexpr u32 kMaxWholeMessage = kMaxDatagramPayload - kWholeHeaderBytes;
/// A fragment index is one byte, so a message is at most 255 fragments.
inline constexpr u32 kMaxFragments = 255;
/// The largest message the secure transport sends: 255 fragments, about 254 KiB.
inline constexpr u32 kMaxMessageBytes = kMaxFragments * kFragmentChunk;

/// How many fragments `size` bytes need; zero when it fits whole.
[[nodiscard]] constexpr u32 fragments_for(usize size) noexcept {
    if (size <= kMaxWholeMessage) {
        return 0;
    }
    return static_cast<u32>((size + kFragmentChunk - 1) / kFragmentChunk);
}

/// Write fragment `index` of `message` into `out` (cleared first): header and chunk.
[[nodiscard]] Status write_fragment(u16 message, u32 index, Span<const u8> whole,
                                    Array<u8>& out) noexcept;

/// Write a whole message into `out` (cleared first).
[[nodiscard]] Status write_whole(Span<const u8> message, Array<u8>& out) noexcept;

/// What the reassembler made of one payload.
enum class ReassemblyVerdict : u8 {
    /// A whole message, or the last fragment of one: `out` holds it.
    Complete = 0,
    /// A fragment of a message still missing others.
    Pending,
    /// Malformed: a bad kind, an index past the count, a count that changed mid-message, or a
    /// message larger than the configured maximum. Counted and dropped.
    Rejected,
};

/// One connection's partial messages, across every channel.
class Reassembler {
public:
    static constexpr u32 kMaxPartial = 16;

    Reassembler(Allocator& allocator, u32 max_message_bytes) noexcept;
    ~Reassembler();

    Reassembler(const Reassembler&) = delete;
    Reassembler& operator=(const Reassembler&) = delete;

    /// Take one payload from `channel`. On `Complete`, `out` views the message until the next call.
    [[nodiscard]] ReassemblyVerdict accept(ChannelId channel, Span<const u8> payload, u64 now_us,
                                           Span<const u8>& out) noexcept;

    /// Discard partial messages older than `max_age_us`.
    void expire(u64 now_us, u64 max_age_us) noexcept;

    [[nodiscard]] u32 partial() const noexcept { return static_cast<u32>(partials_.size()); }
    [[nodiscard]] u64 discarded() const noexcept { return discarded_; }
    [[nodiscard]] u64 rejected() const noexcept { return rejected_; }

private:
    struct Partial {
        ChannelId channel = 0;
        u16 message = 0;
        u32 count = 0;
        u32 arrived = 0;
        /// Known once the last fragment arrives, which is the only one that may be short.
        u32 last_size = 0;
        u64 first_seen_us = 0;
        u64 bits[4] = {};
        Array<u8> bytes;

        explicit Partial(Allocator& allocator) noexcept : bytes(allocator) {}
    };

    [[nodiscard]] Partial* find(ChannelId channel, u16 message) noexcept;
    [[nodiscard]] Partial* start(ChannelId channel, u16 message, u32 count, u64 now_us) noexcept;
    void drop(u32 index) noexcept;
    [[nodiscard]] ReassemblyVerdict accept_fragment(ChannelId channel, Span<const u8> payload,
                                                    u64 now_us, Span<const u8>& out) noexcept;
    [[nodiscard]] ReassemblyVerdict finish(u32 index, Span<const u8>& out) noexcept;

    Allocator* allocator_;
    u32 max_message_bytes_;
    Array<Partial*> partials_;
    Array<u8> completed_;
    u64 discarded_ = 0;
    u64 rejected_ = 0;
};

}  // namespace cy::net
