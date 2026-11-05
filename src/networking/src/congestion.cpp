// SPDX-License-Identifier: MIT
#include <cy/networking/congestion.h>
#include <cy/networking/replay_window.h>

#include <algorithm>
#include <utility>

namespace cy::net {

// --- ReplayWindow --------------------------------------------------------------------------------

bool ReplayWindow::rejects(u64 counter) const noexcept {
    if (!any_ || counter > highest_) {
        return false;
    }
    if (highest_ - counter >= kWindow) {
        return true;
    }
    return test(counter);
}

void ReplayWindow::mark(u64 counter) noexcept {
    if (!any_) {
        any_ = true;
        highest_ = counter;
        set(counter);
        return;
    }
    if (counter > highest_) {
        // Everything between the old newest and this one has not been seen: clear those bits,
        // which still hold counters a whole window older.
        const u64 gap = counter - highest_;
        if (gap >= kWindow) {
            for (u64& word : bits_) {
                word = 0;
            }
        } else {
            for (u64 step = 1; step <= gap; ++step) {
                clear(highest_ + step);
            }
        }
        highest_ = counter;
    }
    set(counter);
}

// --- SendRateController --------------------------------------------------------------------------

SendRateController::SendRateController(const RateConfig& config) noexcept
    : config_(config), rate_(config.max_bytes_per_second) {
    config_.min_bytes_per_second =
        std::min(config_.min_bytes_per_second, config_.max_bytes_per_second);
    tokens_ = capacity();
}

i64 SendRateController::capacity() const noexcept {
    const i64 burst = (static_cast<i64>(rate_) * config_.burst_ms) / 1000;
    // Never below one full datagram, or a slow link could never send a large one at all.
    return burst < 1500 ? 1500 : burst;
}

void SendRateController::refill(u64 now_us) noexcept {
    if (!refilled_) {
        refilled_ = true;
        last_refill_us_ = now_us;
        return;
    }
    if (now_us <= last_refill_us_) {
        return;
    }
    const u64 elapsed = now_us - last_refill_us_;
    last_refill_us_ = now_us;
    const i64 added = static_cast<i64>((static_cast<u64>(rate_) * elapsed) / 1'000'000ULL);
    tokens_ += added;
    tokens_ = std::min(tokens_, capacity());
}

bool SendRateController::try_spend(u32 bytes) noexcept {
    if (std::cmp_less(tokens_, bytes)) {
        return false;
    }
    tokens_ -= static_cast<i64>(bytes);
    return true;
}

void SendRateController::spend_anyway(u32 bytes) noexcept {
    tokens_ -= static_cast<i64>(bytes);
}

void SendRateController::on_report(u32 loss_per_mille, u32 smoothed_rtt_us,
                                   u32 minimum_rtt_us) noexcept {
    const bool lossy = loss_per_mille > config_.loss_threshold_per_mille;
    const bool queueing =
        minimum_rtt_us != 0 && smoothed_rtt_us > minimum_rtt_us + config_.delay_threshold_us;
    if (lossy || queueing) {
        const u32 reduced = rate_ - (rate_ / 4);
        rate_ = reduced < config_.min_bytes_per_second ? config_.min_bytes_per_second : reduced;
        ++decreases_;
        return;
    }
    const u32 step = config_.max_bytes_per_second / 16;
    const u32 raised = rate_ + (step == 0 ? 1 : step);
    rate_ = raised > config_.max_bytes_per_second ? config_.max_bytes_per_second : raised;
}

// --- RttEstimator --------------------------------------------------------------------------------

void RttEstimator::sample(u32 rtt_us) noexcept {
    if (samples_ == 0) {
        smoothed8_ = static_cast<u64>(rtt_us) * 8;
        variance4_ = static_cast<u64>(rtt_us) * 2;  // RFC 6298: rttvar = r / 2
        minimum_ = rtt_us;
        samples_ = 1;
        return;
    }
    ++samples_;
    minimum_ = std::min(minimum_, rtt_us);
    const u64 smoothed = smoothed8_ / 8;
    const u64 deviation = rtt_us > smoothed ? rtt_us - smoothed : smoothed - rtt_us;
    // rttvar += (|srtt - r| - rttvar) / 4 and srtt += (r - srtt) / 8, on the scaled values.
    variance4_ = variance4_ + deviation - (variance4_ / 4);
    smoothed8_ = smoothed8_ + rtt_us - smoothed;
}

// --- LossMeter -----------------------------------------------------------------------------------

void LossMeter::received(u64 counter) noexcept {
    ++received_;
    if (!any_ || counter > highest_) {
        highest_ = counter;
        any_ = true;
    }
}

u32 interval_loss_per_mille(u64 previous_received, u64 previous_expected, u64 received,
                            u64 expected) noexcept {
    if (expected <= previous_expected || received < previous_received) {
        return 0;
    }
    const u64 wanted = expected - previous_expected;
    const u64 got = received - previous_received;
    if (got >= wanted) {
        // Reordering across the report boundary can deliver more than the interval expected.
        return 0;
    }
    return static_cast<u32>(((wanted - got) * 1000) / wanted);
}

bool LossEstimate::report(u64 received, u64 expected) noexcept {
    if (expected < last_expected_ || received < last_received_) {
        // A counter went backwards: a new connection's numbers. Start again from them.
        last_received_ = received;
        last_expected_ = expected;
        return false;
    }
    if (expected - last_expected_ < kMinimumSample) {
        return false;
    }
    const u64 sample = interval_loss_per_mille(last_received_, last_expected_, received, expected);
    last_received_ = received;
    last_expected_ = expected;
    if (!measured_) {
        measured_ = true;
        scaled_ = sample * 4;
        return true;
    }
    scaled_ = scaled_ + sample - (scaled_ / 4);
    return true;
}

}  // namespace cy::net
