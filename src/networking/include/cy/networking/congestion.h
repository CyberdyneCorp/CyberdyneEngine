// SPDX-License-Identifier: MIT
#pragma once
// Send-rate limiting, congestion response, round-trip time and loss measurement. Issue #82.
//
// ================================================================================================
// A TOKEN BUCKET WITH A RATE THAT ANSWERS THE NETWORK
// ================================================================================================
//
// `networking-and-replication` — "Bandwidth management" puts a budget on what the replication
// scheduler plans, per peer per tick. That budget is the application's; this file is the
// transport's own guard under it, for the two things the scheduler cannot see: retransmissions and
// a link that is worse than the budget assumed.
//
// `SendRateController` is a token bucket refilled at the current rate. The rate starts at the
// configured maximum and responds to the peer's loss report with additive increase and
// multiplicative decrease: loss above the threshold, or a round trip that has grown well past the
// smallest one seen, cuts the rate by a quarter; a clean report adds a sixteenth of the maximum
// back. It never goes below the configured minimum, because a session that cannot send its
// keep-alive pings has disconnected itself. Control traffic — acknowledgements, pings, the
// handshake, a disconnect — is charged but never refused, so a congested link still says why it is
// congested.
//
// ================================================================================================
// ROUND TRIP AND LOSS ARE MEASURED, NOT INFERRED FROM RETRANSMISSIONS
// ================================================================================================
//
// The round trip is a ping's echo, smoothed as RFC 6298 smooths it (an eighth of each new sample,
// and a quarter of the deviation for jitter). Loss is counted at the receiver from the packet
// counters the secure transport already authenticates — the packets that arrived against the
// highest counter seen — and reported back in the ping, so each side knows the loss of the
// direction it sends in rather than guessing it from its own retransmissions.

#include <cy/core/base/types.h>

namespace cy::net {

/// How fast a connection may send, and how readily it backs off. Per connection, in wire bytes.
struct RateConfig {
    /// The ceiling, in bytes per second of wire traffic, headers and tags included.
    u32 max_bytes_per_second = 512U * 1024U;
    /// The floor the congestion response may not go below.
    u32 min_bytes_per_second = 16U * 1024U;
    /// How much may be sent at once after an idle period, as milliseconds of the current rate.
    u32 burst_ms = 40;
    /// Loss in a report above which the rate is cut, per mille.
    u32 loss_threshold_per_mille = 100;
    /// A smoothed round trip this much above the smallest one seen is treated as queueing delay.
    u32 delay_threshold_us = 150'000;
};

/// The token bucket and its congestion response: what decides whether a queued datagram goes now.
class SendRateController {
public:
    explicit SendRateController(const RateConfig& config) noexcept;

    /// Add the tokens that accrued since the last call.
    void refill(u64 now_us) noexcept;

    /// Spend `bytes` when they are available. False leaves the bucket untouched.
    [[nodiscard]] bool try_spend(u32 bytes) noexcept;

    /// Spend `bytes` whether or not they are available — control traffic. The bucket may go into
    /// debt, which the next refill pays first.
    void spend_anyway(u32 bytes) noexcept;

    /// The peer's report on the direction this side sends in.
    void on_report(u32 loss_per_mille, u32 smoothed_rtt_us, u32 minimum_rtt_us) noexcept;

    [[nodiscard]] u32 rate() const noexcept { return rate_; }
    [[nodiscard]] i64 tokens() const noexcept { return tokens_; }
    [[nodiscard]] u32 decreases() const noexcept { return decreases_; }

private:
    [[nodiscard]] i64 capacity() const noexcept;

    RateConfig config_;
    u32 rate_;
    i64 tokens_ = 0;
    u64 last_refill_us_ = 0;
    bool refilled_ = false;
    u32 decreases_ = 0;
};

/// RFC 6298's smoothing, in microseconds. Kept scaled — eight times the smoothed round trip, four
/// times the deviation — as TCP implementations keep it, so integer division does not bias the
/// estimate below a constant round trip it is fed forever.
class RttEstimator {
public:
    void sample(u32 rtt_us) noexcept;

    [[nodiscard]] u32 smoothed() const noexcept { return static_cast<u32>(smoothed8_ / 8); }
    [[nodiscard]] u32 jitter() const noexcept { return static_cast<u32>(variance4_ / 4); }
    [[nodiscard]] u32 minimum() const noexcept { return minimum_; }
    [[nodiscard]] u32 samples() const noexcept { return samples_; }

private:
    u64 smoothed8_ = 0;
    u64 variance4_ = 0;
    u32 minimum_ = 0;
    u32 samples_ = 0;
};

/// Counts authenticated packets against the counters they carry. The receiving side's half of
/// loss; the ping carries `received()` and `expected()` to the sender, which turns two reports into
/// a loss rate with `interval_loss_per_mille()`.
class LossMeter {
public:
    void received(u64 counter) noexcept;

    [[nodiscard]] u64 received() const noexcept { return received_; }
    /// One more than the highest counter authenticated: what would have arrived with no loss.
    [[nodiscard]] u64 expected() const noexcept { return any_ ? highest_ + 1 : 0; }

private:
    u64 received_ = 0;
    u64 highest_ = 0;
    bool any_ = false;
};

/// Loss between two cumulative reports, per mille. Zero when nothing was expected between them.
[[nodiscard]] u32 interval_loss_per_mille(u64 previous_received, u64 previous_expected,
                                          u64 received, u64 expected) noexcept;

/// A smoothed loss rate from cumulative (received, expected) reports.
///
/// An interval is measured only once at least `kMinimumSample` packets were expected in it: an idle
/// link sends a handful of packets a second, and a rate computed over one or two of them is either
/// nothing or half the link. Each measured interval moves the estimate a quarter of the way.
class LossEstimate {
public:
    static constexpr u64 kMinimumSample = 16;

    /// True when the report closed an interval and moved the estimate.
    bool report(u64 received, u64 expected) noexcept;

    [[nodiscard]] u32 per_mille() const noexcept { return static_cast<u32>(scaled_ / 4); }

private:
    u64 last_received_ = 0;
    u64 last_expected_ = 0;
    u64 scaled_ = 0;
    bool measured_ = false;
};

}  // namespace cy::net
