#include <cy/networking/profiler.h>

#include <algorithm>

namespace cy::net {

const char* traffic_category_name(TrafficCategory category) noexcept {
    switch (category) {
        case TrafficCategory::State:
            return "State";
        case TrafficCategory::Rpc:
            return "Rpc";
        case TrafficCategory::Spawn:
            return "Spawn";
        case TrafficCategory::Despawn:
            return "Despawn";
        case TrafficCategory::Acknowledgement:
            return "Acknowledgement";
        case TrafficCategory::Overhead:
            return "Overhead";
        case TrafficCategory::Count:
            return "Count";
    }
    return "unknown";
}

Status NetworkProfiler::note_traffic(const TrafficRecord& record) noexcept {
    newest_ = std::max(record.tick, newest_);
    if (Status pushed = records_.push_back(record); !pushed) {
        return pushed;
    }
    const u64 oldest = newest_ > kProfilerTicks ? newest_ - kProfilerTicks : 0;
    usize index = 0;
    while (index < records_.size()) {
        if (records_[index].tick < oldest) {
            records_.remove_unordered(index);
            ++evicted_;
            continue;
        }
        ++index;
    }
    return ok();
}

Status NetworkProfiler::note_correction(const CorrectionExplanation& correction) noexcept {
    ++counters_.reconciliations;
    counters_.largest_prediction_error =
        std::max(correction.magnitude, counters_.largest_prediction_error);
    return corrections_.push_back(correction);
}

ReplicationExplanation NetworkProfiler::why_replicated(PeerId peer,
                                                       NetworkId entity) const noexcept {
    ReplicationExplanation explanation;
    for (const auto& record : records_) {
        if (!(record.peer == peer) || !(record.entity == entity)) {
            continue;
        }
        // The most recent send is the one a question about "why was it sent" means. Records are
        // appended in order and evicted out of order, so the tick is compared rather than the
        // index.
        if (explanation.found && record.tick < explanation.tick) {
            continue;
        }
        explanation.found = true;
        explanation.tick = record.tick;
        explanation.rule = record.rule;
        explanation.score = record.score;
        explanation.band = record.band;
        explanation.bytes = record.bytes;
    }
    return explanation;
}

BandwidthAttribution NetworkProfiler::attribute(PeerId peer, u64 tick) const noexcept {
    BandwidthAttribution attribution;
    attribution.tick = tick;
    for (const auto& record : records_) {
        if (!(record.peer == peer) || record.tick != tick) {
            continue;
        }
        attribution.total_bytes += record.bytes;
        attribution.by_category[static_cast<u32>(record.category)] += record.bytes;
        ++attribution.entities;
        if (record.bytes > attribution.largest_bytes) {
            attribution.largest_bytes = record.bytes;
            attribution.largest_entity = record.entity;
            attribution.largest_schema = record.schema;
        }
    }
    return attribution;
}

CorrectionExplanation NetworkProfiler::explain_correction(NetworkId entity) const noexcept {
    (void)entity;
    CorrectionExplanation newest;
    for (const auto& correction : corrections_) {
        if (!newest.found || correction.noticed_tick >= newest.noticed_tick) {
            newest = correction;
            newest.found = true;
        }
    }
    return newest;
}

void NetworkProfiler::clear() noexcept {
    records_.clear();
    corrections_.clear();
    counters_ = NetworkCounters{};
    newest_ = 0;
    evicted_ = 0;
}

}  // namespace cy::net
