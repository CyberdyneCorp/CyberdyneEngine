#include <cy/networking/scheduler.h>

#include <algorithm>
#include <utility>

namespace cy::net {
namespace {

template <class T, class... Args>
[[nodiscard]] T* make(Allocator& allocator, Args&&... args) noexcept {
    void* storage = allocator.allocate(sizeof(T), alignof(T));
    if (storage == nullptr) {
        return nullptr;
    }
    return construct_at<T>(storage, std::forward<Args>(args)...);
}

template <class T>
void unmake(Allocator& allocator, T* object) noexcept {
    if (object == nullptr) {
        return;
    }
    object->~T();
    allocator.deallocate(object, sizeof(T), alignof(T));
}

/// The one ordering. Score descending, then network id ascending — a **total** order, so two runs
/// that produced the same candidates produce the same packet whatever `std::sort`'s internals do
/// with equal elements. `determinism::sort_by_key`'s tie-break rule, spelled as a comparator
/// because this sequence is thousands of entries long and that function is an insertion sort.
[[nodiscard]] bool scheduled_before(const ScheduledEntry& left,
                                    const ScheduledEntry& right) noexcept {
    if (left.forced_by_staleness != right.forced_by_staleness) {
        return left.forced_by_staleness;
    }
    if (left.score != right.score) {
        return left.score > right.score;
    }
    return left.id.value() < right.id.value();
}

}  // namespace

const char* network_lod_band_name(NetworkLodBand band) noexcept {
    switch (band) {
        case NetworkLodBand::OwnedOrSelected:
            return "OwnedOrSelected";
        case NetworkLodBand::NearChanging:
            return "NearChanging";
        case NetworkLodBand::NearIdle:
            return "NearIdle";
        case NetworkLodBand::Far:
            return "Far";
        case NetworkLodBand::VeryFar:
            return "VeryFar";
        case NetworkLodBand::Dormant:
            return "Dormant";
    }
    return "unknown";
}

PriorityScheduler::PriorityScheduler(Allocator& allocator) noexcept
    : allocator_(&allocator), entities_(allocator), peers_(allocator), scratch_(allocator) {}

PriorityScheduler::~PriorityScheduler() {
    for (auto& peer : peers_) {
        unmake(*allocator_, peer);
    }
}

Expected<PriorityScheduler::PeerState*, Error> PriorityScheduler::state_for(PeerId peer) noexcept {
    for (auto* state : peers_) {
        if (state->peer == peer) {
            return state;
        }
    }
    auto* state = make<PeerState>(*allocator_, *allocator_);
    if (state == nullptr) {
        return fail(ErrorCode::OutOfMemory, "networking: could not allocate a peer's schedule");
    }
    state->peer = peer;
    if (!peers_.push_back(state)) {
        unmake(*allocator_, state);
        return fail(ErrorCode::OutOfMemory, "networking: could not add a peer's schedule");
    }
    return state;
}

const PriorityScheduler::PeerState* PriorityScheduler::state_for(PeerId peer) const noexcept {
    for (auto* state : peers_) {
        if (state->peer == peer) {
            return state;
        }
    }
    return nullptr;
}

Status PriorityScheduler::note_changed(NetworkId id, u64 tick) noexcept {
    EntityState* state = entities_.find(id.value());
    if (state == nullptr) {
        Expected<EntityState*, Error> inserted =
            entities_.insert(id.value(), EntityState{tick, false});
        return inserted ? ok() : Status{make_unexpected(inserted.error())};
    }
    const bool woke = state->dormant;
    state->last_changed_tick = tick;
    state->dormant = false;
    if (woke) {
        // Waking up re-arms the signal: the next time it goes dormant every peer is told again.
        for (auto& peer : peers_) {
            if (PeerEntityState* entry = peer->entities.find(id.value()); entry != nullptr) {
                entry->dormancy_told = false;
            }
        }
    }
    return ok();
}

Status PriorityScheduler::note_sent(PeerId peer, NetworkId id, u64 tick) noexcept {
    Expected<PeerState*, Error> state = state_for(peer);
    if (!state) {
        return make_unexpected(state.error());
    }
    PeerEntityState* entry = state.value()->entities.find(id.value());
    if (entry == nullptr) {
        PeerEntityState fresh;
        fresh.last_sent_tick = tick;
        Expected<PeerEntityState*, Error> inserted =
            state.value()->entities.insert(id.value(), fresh);
        return inserted ? ok() : Status{make_unexpected(inserted.error())};
    }
    entry->last_sent_tick = tick;
    const EntityState* entity = entities_.find(id.value());
    if (entity != nullptr && entity->dormant) {
        // The dormancy signal is sent once. Recorded here rather than when it is scheduled, because
        // an entity that was scheduled and did not fit has not been told anything.
        entry->dormancy_told = true;
    }
    return ok();
}

Status PriorityScheduler::note_visible(PeerId peer, NetworkId id, bool visible) noexcept {
    Expected<PeerState*, Error> state = state_for(peer);
    if (!state) {
        return make_unexpected(state.error());
    }
    PeerEntityState* entry = state.value()->entities.find(id.value());
    if (entry == nullptr) {
        PeerEntityState fresh;
        fresh.visible = visible;
        Expected<PeerEntityState*, Error> inserted =
            state.value()->entities.insert(id.value(), fresh);
        return inserted ? ok() : Status{make_unexpected(inserted.error())};
    }
    entry->visible = visible;
    return ok();
}

NetworkLodBand PriorityScheduler::band_for(PeerId peer, const Candidate& candidate,
                                           u64 tick) const noexcept {
    const EntityState* entity = entities_.find(candidate.id.value());
    const PeerState* state = state_for(peer);
    const PeerEntityState* per_peer =
        state == nullptr ? nullptr : state->entities.find(candidate.id.value());

    if (entity != nullptr && entity->dormant) {
        return NetworkLodBand::Dormant;
    }
    if (candidate.rule == RelevanceRule::Ownership ||
        (per_peer != nullptr && per_peer->visible && candidate.distance_squared <= near_squared_ &&
         candidate.importance >= 100)) {
        return NetworkLodBand::OwnedOrSelected;
    }
    const bool changing = entity != nullptr && tick >= entity->last_changed_tick &&
                          tick - entity->last_changed_tick <= 8;
    if (candidate.distance_squared <= near_squared_) {
        return changing ? NetworkLodBand::NearChanging : NetworkLodBand::NearIdle;
    }
    return candidate.distance_squared <= far_squared_ ? NetworkLodBand::Far
                                                      : NetworkLodBand::VeryFar;
}

u32 PriorityScheduler::score_of(const Candidate& candidate, const EntityState& entity,
                                const PeerEntityState& per_peer, u64 tick) const noexcept {
    u64 score = 1;
    if (candidate.rule == RelevanceRule::Ownership) {
        score += weights_.ownership;
    }
    if (per_peer.visible) {
        score += weights_.visibility;
    }
    score += static_cast<u64>(candidate.importance) * weights_.importance;

    const u64 since_change = tick >= entity.last_changed_tick ? tick - entity.last_changed_tick : 0;
    if (since_change < 16) {
        score += static_cast<u64>(weights_.recency_of_change) * (16 - since_change) / 16;
    }
    const u64 staleness = tick >= per_peer.last_sent_tick ? tick - per_peer.last_sent_tick : 0;
    score += staleness * weights_.staleness_per_tick;

    // The distance penalty, in bands rather than continuously: a continuous term would make the
    // order depend on the last bit of a squared distance, which is exactly the kind of tie two
    // builds can order differently.
    u64 penalty = 0;
    if (candidate.distance_squared > far_squared_) {
        penalty = static_cast<u64>(weights_.distance_penalty) * 2;
    } else if (candidate.distance_squared > near_squared_) {
        penalty = weights_.distance_penalty;
    }
    score = score > penalty ? score - penalty : 1;
    return score > 0xFFFF'FFFFULL ? 0xFFFF'FFFFU : static_cast<u32>(score);
}

Expected<SchedulerReport, Error> PriorityScheduler::select(PeerId peer,
                                                           Span<const Candidate> candidates,
                                                           u64 tick, const BandwidthBudget& budget,
                                                           CostFn cost, void* user,
                                                           Array<ScheduledEntry>& out) noexcept {
    Expected<PeerState*, Error> peer_state = state_for(peer);
    if (!peer_state) {
        return make_unexpected(peer_state.error());
    }
    PeerState& state = *peer_state.value();

    SchedulerReport report;
    report.candidates = static_cast<u32>(candidates.size());
    report.bytes_budget = budget.bytes_per_tick;
    scratch_.clear();

    for (const auto& candidate : candidates) {
        EntityState* entity = entities_.find(candidate.id.value());
        if (entity == nullptr) {
            Expected<EntityState*, Error> inserted =
                entities_.insert(candidate.id.value(), EntityState{tick, false});
            if (!inserted) {
                return make_unexpected(inserted.error());
            }
            entity = inserted.value();
        }
        PeerEntityState* per_peer = state.entities.find(candidate.id.value());
        if (per_peer == nullptr) {
            Expected<PeerEntityState*, Error> inserted =
                state.entities.insert(candidate.id.value(), PeerEntityState{});
            if (!inserted) {
                return make_unexpected(inserted.error());
            }
            per_peer = inserted.value();
        }

        // Step three of the degradation order, applied before the budget is even consulted: an
        // entity nothing has changed for becomes dormant and is told so exactly once.
        const u64 since_change =
            tick >= entity->last_changed_tick ? tick - entity->last_changed_tick : 0;
        if (!entity->dormant && budget.dormant_after_ticks != 0 &&
            since_change >= budget.dormant_after_ticks) {
            entity->dormant = true;
        }
        if (entity->dormant) {
            if (per_peer->dormancy_told) {
                continue;
            }
            ScheduledEntry signalled;
            signalled.id = candidate.id;
            signalled.band = NetworkLodBand::Dormant;
            signalled.dormancy_signal = true;
            signalled.score = 0xFFFF'FFFFU;  // Told once, and told before anything else.
            signalled.bytes = cost == nullptr ? 0 : cost(user, candidate.id, 0);
            if (Status pushed = scratch_.push_back(signalled); !pushed) {
                return make_unexpected(pushed.error());
            }
            ++report.marked_dormant;
            continue;
        }

        const NetworkLodBand band = band_for(peer, candidate, tick);
        const BandPolicy& policy = bands_.of(band);
        const u64 since_sent =
            tick >= per_peer->last_sent_tick ? tick - per_peer->last_sent_tick : 0;
        const bool forced =
            policy.guaranteed_interval_ticks != 0 && since_sent >= policy.guaranteed_interval_ticks;
        if (!forced && policy.interval_ticks != 0 && since_sent < policy.interval_ticks &&
            per_peer->last_sent_tick != 0) {
            ++report.not_due;
            continue;
        }

        ScheduledEntry entry;
        entry.id = candidate.id;
        entry.band = band;
        entry.precision_percent = policy.precision_percent;
        entry.forced_by_staleness = forced;
        entry.score = score_of(candidate, *entity, *per_peer, tick);
        entry.bytes = cost == nullptr ? 0 : cost(user, candidate.id, entry.precision_percent);
        if (Status pushed = scratch_.push_back(entry); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (forced) {
            ++report.forced_by_staleness;
        }
    }

    std::sort(scratch_.data(), scratch_.data() + scratch_.size(), scheduled_before);

    // Steps one and two of the degradation order, in order. Under pressure a low band's update is
    // first made less frequent — which for this tick means deferred, its staleness rising — and
    // then, if that is not enough, sent at reduced precision. Only what is still over budget after
    // both is omitted.
    u64 planned = 0;
    for (auto entry : scratch_) {
        if (planned + entry.bytes <= budget.bytes_per_tick) {
            planned += entry.bytes;
            if (Status pushed = out.push_back(entry); !pushed) {
                return make_unexpected(pushed.error());
            }
            ++report.selected;
            continue;
        }
        if (entry.band != NetworkLodBand::OwnedOrSelected &&
            entry.band != NetworkLodBand::Dormant && !entry.forced_by_staleness) {
            ++report.frequency_reduced;
            ++report.deferred;
            continue;
        }
        // Owned, or forced in by staleness: it may not simply be dropped, so precision goes first.
        const u32 reduced = entry.bytes / 2;
        if (planned + reduced <= budget.bytes_per_tick) {
            entry.precision_percent = entry.precision_percent / 2;
            entry.bytes = reduced;
            planned += reduced;
            if (Status pushed = out.push_back(entry); !pushed) {
                return make_unexpected(pushed.error());
            }
            ++report.selected;
            ++report.precision_reduced;
            continue;
        }
        ++report.deferred;
    }

    report.bytes_planned = planned;
    return report;
}

}  // namespace cy::net
