// SPDX-License-Identifier: MIT
#pragma once
// The packet-level replay window of the secure transport. Issue #82.
//
// Every encrypted packet carries a 64-bit counter that is also its nonce, so a counter is used
// once per key and never again. The receiver remembers the newest counter it has authenticated and
// the 1024 before it, one bit each. A packet whose counter is marked, or older than the window, is
// refused **before** decryption is attempted — a replayed packet is a verbatim copy of an authentic
// one and would otherwise decrypt perfectly.
//
// This is a second window beside `reliability.h`'s, and they answer different questions. The
// reliability window is per channel, sixteen bits wide and about delivery; this one is per
// connection, 64 bits wide and about authenticity. A replayed packet carries reliability datagrams
// whose own sequences may well be inside their window, so without this check an attacker could
// re-deliver an unreliable state update or a disconnect.
//
// A counter is marked only after its packet authenticates. Marking it first would let a forger burn
// a future counter with a packet that fails its tag, and the genuine packet would then be refused.

#include <cy/core/base/types.h>

namespace cy::net {

/// Which packet counters have been authenticated, over the newest 1024 — see the header comment.
class ReplayWindow {
public:
    static constexpr u64 kWindow = 1024;

    /// True when `counter` must be refused: already seen, or too old to tell.
    [[nodiscard]] bool rejects(u64 counter) const noexcept;

    /// Record an authenticated counter.
    void mark(u64 counter) noexcept;

    [[nodiscard]] bool any() const noexcept { return any_; }
    [[nodiscard]] u64 highest() const noexcept { return highest_; }

private:
    [[nodiscard]] bool test(u64 counter) const noexcept {
        return ((bits_[(counter % kWindow) / 64] >> (counter % 64)) & 1ULL) != 0;
    }
    void set(u64 counter) noexcept { bits_[(counter % kWindow) / 64] |= 1ULL << (counter % 64); }
    void clear(u64 counter) noexcept {
        bits_[(counter % kWindow) / 64] &= ~(1ULL << (counter % 64));
    }

    u64 bits_[kWindow / 64] = {};
    u64 highest_ = 0;
    bool any_ = false;
};

}  // namespace cy::net
