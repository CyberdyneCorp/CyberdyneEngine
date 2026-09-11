#include <cy/networking/interest.h>

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

[[nodiscard]] i64 distance_squared_between(const PeerInterest& peer,
                                           const RelevanceSubject& subject) noexcept {
    // Integer arithmetic on purpose. A relevance boundary decided by floating point is a boundary
    // two peers can disagree about, and an entity that appears for one client and not another is a
    // bug report nobody can reproduce.
    const i64 dx = subject.position_x - peer.viewpoint_x;
    const i64 dy = subject.position_y - peer.viewpoint_y;
    const i64 dz = subject.position_z - peer.viewpoint_z;
    return (dx * dx) + (dy * dy) + (dz * dz);
}

}  // namespace

const char* relevance_rule_name(RelevanceRule rule) noexcept {
    switch (rule) {
        case RelevanceRule::None:
            return "None";
        case RelevanceRule::Ownership:
            return "Ownership";
        case RelevanceRule::AlwaysRelevant:
            return "AlwaysRelevant";
        case RelevanceRule::CellMembership:
            return "CellMembership";
        case RelevanceRule::Distance:
            return "Distance";
        case RelevanceRule::Team:
            return "Team";
        case RelevanceRule::CustomPredicate:
            return "CustomPredicate";
    }
    return "unknown";
}

InterestSet::InterestSet(Allocator& allocator) noexcept
    : allocator_(&allocator),
      subjects_(allocator),
      by_id_(allocator),
      cells_(allocator),
      cell_index_(allocator),
      always_relevant_(allocator),
      peers_(allocator) {}

InterestSet::~InterestSet() {
    for (auto& cell : cells_) {
        unmake(*allocator_, cell);
    }
    for (auto& peer : peers_) {
        unmake(*allocator_, peer);
    }
}

Expected<u32, Error> InterestSet::bucket_for(CellId cell) noexcept {
    if (const u32* found = cell_index_.find(cell); found != nullptr) {
        return *found;
    }
    auto* bucket = make<CellBucket>(*allocator_, *allocator_);
    if (bucket == nullptr) {
        return fail(ErrorCode::OutOfMemory, "networking: could not allocate a replication cell");
    }
    bucket->cell = cell;
    if (!cells_.push_back(bucket)) {
        unmake(*allocator_, bucket);
        return fail(ErrorCode::OutOfMemory, "networking: could not add a replication cell");
    }
    const u32 index = static_cast<u32>(cells_.size() - 1);
    if (!cell_index_.insert(cell, index)) {
        return fail(ErrorCode::OutOfMemory, "networking: could not index a replication cell");
    }
    return index;
}

Status InterestSet::place(const RelevanceSubject& subject) noexcept {
    if (!subject.id.valid()) {
        return fail(ErrorCode::InvalidArgument, "networking: the null network id is never placed");
    }
    u32* existing = by_id_.find(subject.id.value());
    if (existing != nullptr) {
        RelevanceSubject& stored = subjects_[*existing];
        if (stored.cell != subject.cell) {
            // The incremental half: a cell change is an index update, not a rebuild.
            if (stored.cell != kNoCell) {
                if (const u32* old = cell_index_.find(stored.cell); old != nullptr) {
                    Array<u32>& members = cells_[*old]->members;
                    for (usize slot = 0; slot < members.size(); ++slot) {
                        if (members[slot] == *existing) {
                            members.remove_unordered(slot);
                            break;
                        }
                    }
                }
            }
            if (subject.cell != kNoCell) {
                Expected<u32, Error> bucket = bucket_for(subject.cell);
                if (!bucket) {
                    return make_unexpected(bucket.error());
                }
                if (Status pushed = cells_[bucket.value()]->members.push_back(*existing); !pushed) {
                    return pushed;
                }
            }
        }
        const bool was_always = stored.always_relevant;
        stored = subject;
        if (subject.always_relevant && !was_always) {
            return always_relevant_.push_back(*existing);
        }
        if (!subject.always_relevant && was_always) {
            for (usize slot = 0; slot < always_relevant_.size(); ++slot) {
                if (always_relevant_[slot] == *existing) {
                    always_relevant_.remove_unordered(slot);
                    break;
                }
            }
        }
        return ok();
    }

    if (Status pushed = subjects_.push_back(subject); !pushed) {
        return pushed;
    }
    const u32 index = static_cast<u32>(subjects_.size() - 1);
    if (!by_id_.insert(subject.id.value(), index)) {
        return fail(ErrorCode::OutOfMemory, "networking: could not index a relevance subject");
    }
    if (subject.cell != kNoCell) {
        Expected<u32, Error> bucket = bucket_for(subject.cell);
        if (!bucket) {
            return make_unexpected(bucket.error());
        }
        if (Status added = cells_[bucket.value()]->members.push_back(index); !added) {
            return added;
        }
    }
    if (subject.always_relevant) {
        return always_relevant_.push_back(index);
    }
    return ok();
}

void InterestSet::remove(NetworkId id) noexcept {
    const u32* found = by_id_.find(id.value());
    if (found == nullptr) {
        return;
    }
    const u32 index = *found;
    const RelevanceSubject subject = subjects_[index];
    if (subject.cell != kNoCell) {
        if (const u32* bucket = cell_index_.find(subject.cell); bucket != nullptr) {
            Array<u32>& members = cells_[*bucket]->members;
            for (usize slot = 0; slot < members.size(); ++slot) {
                if (members[slot] == index) {
                    members.remove_unordered(slot);
                    break;
                }
            }
        }
    }
    for (usize slot = 0; slot < always_relevant_.size(); ++slot) {
        if (always_relevant_[slot] == index) {
            always_relevant_.remove_unordered(slot);
            break;
        }
    }
    // The subject's slot is emptied rather than removed: the cell buckets hold indices into this
    // array, and compacting it would move every one of them.
    subjects_[index].id = NetworkId{};
    subjects_[index].cell = kNoCell;
    (void)by_id_.remove(id.value());
}

Expected<InterestSet::PeerState*, Error> InterestSet::peer_state(PeerId peer) noexcept {
    for (auto* state : peers_) {
        if (state->peer == peer) {
            return state;
        }
    }
    auto* state = make<PeerState>(*allocator_, *allocator_);
    if (state == nullptr) {
        return fail(ErrorCode::OutOfMemory, "networking: could not allocate a peer's interest");
    }
    state->peer = peer;
    if (!peers_.push_back(state)) {
        unmake(*allocator_, state);
        return fail(ErrorCode::OutOfMemory, "networking: could not add a peer's interest");
    }
    return state;
}

Status InterestSet::set_interest_cells(PeerId peer, Span<const CellId> cells) noexcept {
    Expected<PeerState*, Error> state = peer_state(peer);
    if (!state) {
        return make_unexpected(state.error());
    }
    state.value()->cells.clear();
    return state.value()->cells.append(cells);
}

RelevanceRule InterestSet::admits(const PeerInterest& peer, const RelevanceSubject& subject,
                                  i64& distance_squared) const noexcept {
    distance_squared = distance_squared_between(peer, subject);
    if (subject.owner == peer.peer && subject.owner.valid()) {
        return RelevanceRule::Ownership;
    }
    if (subject.always_relevant) {
        return RelevanceRule::AlwaysRelevant;
    }
    if (peer.team_is_relevant && subject.team == peer.team && subject.team != 0) {
        return RelevanceRule::Team;
    }
    if (peer.relevance_distance_squared > 0 && distance_squared > peer.relevance_distance_squared) {
        // Outside the peer's relevance distance. `networking-and-replication`'s "Information
        // leakage": nothing about it is sent, so a modified client cannot observe it.
        if (predicate_ != nullptr && predicate_(predicate_user_, peer, subject)) {
            return RelevanceRule::CustomPredicate;
        }
        return RelevanceRule::None;
    }
    return RelevanceRule::CellMembership;
}

Status InterestSet::consider(const PeerInterest& peer, u32 subject_index,
                             Array<Candidate>& out) noexcept {
    ++examined_last_;
    const RelevanceSubject& subject = subjects_[subject_index];
    if (!subject.id.valid()) {
        return ok();
    }
    i64 distance = 0;
    const RelevanceRule rule = admits(peer, subject, distance);
    if (rule == RelevanceRule::None) {
        return ok();
    }
    Candidate candidate;
    candidate.id = subject.id;
    candidate.rule = rule;
    candidate.distance_squared = distance;
    candidate.importance = subject.importance;
    return out.push_back(candidate);
}

Expected<RelevanceDelta, Error> InterestSet::evaluate(const PeerInterest& peer,
                                                      Array<Candidate>& out,
                                                      Array<NetworkId>& left) noexcept {
    Expected<PeerState*, Error> state = peer_state(peer.peer);
    if (!state) {
        return make_unexpected(state.error());
    }
    PeerState& peer_state_ref = *state.value();

    const usize first = out.size();
    examined_last_ = 0;

    for (const u32 subject : always_relevant_) {
        if (Status considered = consider(peer, subject, out); !considered) {
            return make_unexpected(considered.error());
        }
    }
    for (const CellId cell : peer_state_ref.cells) {
        const u32* bucket = cell_index_.find(cell);
        if (bucket == nullptr) {
            continue;
        }
        const Array<u32>& members = cells_[*bucket]->members;
        for (const u32 member : members) {
            const RelevanceSubject& subject = subjects_[member];
            if (subject.always_relevant) {
                continue;  // Already considered above; admitting it twice would send it twice.
            }
            if (Status considered = consider(peer, member, out); !considered) {
                return make_unexpected(considered.error());
            }
        }
    }

    RelevanceDelta delta;
    peer_state_ref.current.clear();
    for (usize index = first; index < out.size(); ++index) {
        if (Status inserted = peer_state_ref.current.insert(out[index].id.value()); !inserted) {
            return make_unexpected(inserted.error());
        }
        if (!peer_state_ref.previous.contains(out[index].id.value())) {
            ++delta.entered;
        }
    }
    for (auto entry = peer_state_ref.previous.begin(); entry != peer_state_ref.previous.end();
         ++entry) {
        const u64 id_value = (*entry).key;
        if (peer_state_ref.current.contains(id_value)) {
            continue;
        }
        ++delta.left;
        if (Status pushed =
                left.push_back(NetworkId::make(static_cast<u16>(id_value >> 48), id_value));
            !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    peer_state_ref.previous.clear();
    for (usize index = first; index < out.size(); ++index) {
        if (Status inserted = peer_state_ref.previous.insert(out[index].id.value()); !inserted) {
            return make_unexpected(inserted.error());
        }
    }
    return delta;
}

}  // namespace cy::net
