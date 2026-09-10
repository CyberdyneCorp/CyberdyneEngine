// Time domains and per-domain timers. M8.b task 3.3.

#include <cy/gameplay/time.h>

#include <utility>

namespace cy::gameplay {

const char* time_domain_kind_name(TimeDomainKind kind) noexcept {
    switch (kind) {
        case TimeDomainKind::Real:
            return "Real";
        case TimeDomainKind::Gameplay:
            return "Gameplay";
        case TimeDomainKind::Simulation:
            return "Simulation";
        case TimeDomainKind::Interface:
            return "Interface";
        case TimeDomainKind::Cinematic:
            return "Cinematic";
        case TimeDomainKind::Count:
            break;
    }
    return "Real";
}

TimeDomains::TimeDomains(Allocator& allocator) noexcept : domains_(allocator) {
    for (u32 index = 0; index < static_cast<u32>(TimeDomainKind::Count); ++index) {
        const auto kind = static_cast<TimeDomainKind>(index);
        TimeDomain domain;
        domain.name = Name::intern(time_domain_kind_name(kind));
        domain.unscalable = kind == TimeDomainKind::Real;
        builtins_[index] = domains_.push_back(domain).has_value()
                               ? static_cast<DomainId>(domains_.size() - 1)
                               : kInvalidDomain;
    }
}

DomainId TimeDomains::builtin(TimeDomainKind kind) const noexcept {
    const auto index = static_cast<usize>(kind);
    return index < static_cast<usize>(TimeDomainKind::Count) ? builtins_[index] : kInvalidDomain;
}

Expected<DomainId, Error> TimeDomains::declare(Name name, bool unscalable) noexcept {
    if (const DomainId existing = find(name); existing != kInvalidDomain) {
        return existing;
    }
    if (domains_.size() >= kMaxDomains) {
        return make_unexpected(
            Error{ErrorCode::OutOfRange, "more domains than a session holds", 0});
    }
    TimeDomain domain;
    domain.name = name;
    domain.unscalable = unscalable;
    if (Status pushed = domains_.push_back(domain); !pushed) {
        return make_unexpected(pushed.error());
    }
    return static_cast<DomainId>(domains_.size() - 1);
}

DomainId TimeDomains::find(Name name) const noexcept {
    for (usize index = 0; index < domains_.size(); ++index) {
        if (domains_[index].name == name) {
            return static_cast<DomainId>(index);
        }
    }
    return kInvalidDomain;
}

Status TimeDomains::set_scale(DomainId domain, f32 scale) noexcept {
    if (domain >= domains_.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such time domain", 0});
    }
    if (domains_[domain].unscalable) {
        return make_unexpected(
            Error{ErrorCode::PermissionDenied, "this domain declared itself unscalable", 0});
    }
    domains_[domain].scale = scale;
    return ok();
}

Status TimeDomains::set_paused(DomainId domain, bool paused) noexcept {
    if (domain >= domains_.size()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such time domain", 0});
    }
    if (domains_[domain].unscalable) {
        return make_unexpected(
            Error{ErrorCode::PermissionDenied, "this domain declared itself unpausable", 0});
    }
    if (paused && !pause_allowed_) {
        return make_unexpected(
            Error{ErrorCode::PermissionDenied, "this session forbids pausing", 0});
    }
    domains_[domain].paused = paused;
    return ok();
}

void TimeDomains::advance(f32 real_delta) noexcept {
    for (TimeDomain& domain : domains_) {
        if (domain.unscalable) {
            domain.delta = real_delta;
        } else if (domain.paused) {
            domain.delta = 0.0F;
        } else {
            domain.delta = real_delta * domain.scale;
        }
        domain.elapsed += static_cast<f64>(domain.delta);
    }
}

f32 TimeDomains::delta(DomainId domain) const noexcept {
    return domain < domains_.size() ? domains_[domain].delta : 0.0F;
}

f64 TimeDomains::elapsed(DomainId domain) const noexcept {
    return domain < domains_.size() ? domains_[domain].elapsed : 0.0;
}

f32 TimeDomains::scale(DomainId domain) const noexcept {
    return domain < domains_.size() ? domains_[domain].scale : 1.0F;
}

bool TimeDomains::paused(DomainId domain) const noexcept {
    return domain < domains_.size() && domains_[domain].paused;
}

TimerWheel::TimerWheel(Allocator& allocator) noexcept
    : entries_(allocator), heads_(allocator), tails_(allocator), due_(allocator) {}

Expected<TimerId, Error> TimerWheel::schedule(u64 due_tick, u64 user) noexcept {
    Entry entry;
    entry.due_tick = due_tick;
    entry.user = user;
    if (Status pushed = entries_.push_back(entry); !pushed) {
        return make_unexpected(pushed.error());
    }
    const auto id = static_cast<TimerId>(entries_.size() - 1);

    if (u32* tail = tails_.find(due_tick); tail != nullptr) {
        entries_[*tail].next = id;
        *tail = id;
        ++pending_;
        return id;
    }
    if (auto head = heads_.insert(due_tick, id); !head) {
        entries_.pop_back();
        return make_unexpected(head.error());
    }
    if (auto tail = tails_.insert(due_tick, id); !tail) {
        (void)heads_.remove(due_tick);
        entries_.pop_back();
        return make_unexpected(tail.error());
    }
    // A new bucket: record its tick in the min-heap so an advance finds it without scanning.
    if (Status pushed = due_.push_back(due_tick); !pushed) {
        (void)heads_.remove(due_tick);
        (void)tails_.remove(due_tick);
        entries_.pop_back();
        return make_unexpected(pushed.error());
    }
    usize child = due_.size() - 1;
    while (child > 0) {
        const usize parent = (child - 1) / 2;
        if (due_[parent] <= due_[child]) {
            break;
        }
        std::swap(due_[parent], due_[child]);
        child = parent;
    }
    ++pending_;
    return id;
}

bool TimerWheel::cancel(TimerId id) noexcept {
    if (id >= entries_.size() || entries_[id].cancelled) {
        return false;
    }
    entries_[id].cancelled = true;
    if (pending_ > 0) {
        --pending_;
    }
    return true;
}

u32 TimerWheel::advance_to(u64 tick, TimerExpiry* out, u32 capacity) noexcept {
    examined_ = 0;
    u32 fired = 0;
    while (!due_.empty() && due_[0] <= tick) {
        const u64 bucket = due_[0];
        // Pop the heap's minimum.
        due_[0] = due_[due_.size() - 1];
        due_.pop_back();
        usize parent = 0;
        while (true) {
            const usize left = (parent * 2) + 1;
            const usize right = left + 1;
            usize smallest = parent;
            if (left < due_.size() && due_[left] < due_[smallest]) {
                smallest = left;
            }
            if (right < due_.size() && due_[right] < due_[smallest]) {
                smallest = right;
            }
            if (smallest == parent) {
                break;
            }
            std::swap(due_[parent], due_[smallest]);
            parent = smallest;
        }

        const u32* head = heads_.find(bucket);
        u32 walk = head != nullptr ? *head : kInvalidTimer;
        (void)heads_.remove(bucket);
        (void)tails_.remove(bucket);
        while (walk != kInvalidTimer) {
            const Entry& entry = entries_[walk];
            ++examined_;
            if (!entry.cancelled) {
                if (out != nullptr && fired < capacity) {
                    out[fired] = TimerExpiry{walk, entry.due_tick, entry.user};
                }
                ++fired;
                if (pending_ > 0) {
                    --pending_;
                }
            }
            walk = entry.next;
        }
    }
    now_ = tick;
    return fired;
}

TimerService::TimerService(Allocator& allocator, const TimeDomains& domains) noexcept
    : allocator_(&allocator), domains_(&domains), wheels_(allocator) {}

Status TimerService::add_domain(DomainId domain) noexcept {
    if (domain >= domains_->count()) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such time domain", 0});
    }
    if (wheel(domain) != nullptr) {
        return ok();
    }
    return wheels_.push_back(Row{domain, TimerWheel(*allocator_)});
}

TimerWheel* TimerService::wheel(DomainId domain) noexcept {
    for (Row& row : wheels_) {
        if (row.domain == domain) {
            return &row.wheel;
        }
    }
    return nullptr;
}

const TimerWheel* TimerService::wheel(DomainId domain) const noexcept {
    for (const Row& row : wheels_) {
        if (row.domain == domain) {
            return &row.wheel;
        }
    }
    return nullptr;
}

}  // namespace cy::gameplay
