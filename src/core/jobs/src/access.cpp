// The access set, its conflict test, and the counted check for an access nobody declared.
//
// The set is kept sorted by (domain, id) at insertion. Insertion is therefore O(n) in the number of
// entries and the conflict test is a single merge over two sorted arrays rather than a nested loop
// or a hash table. With 32 entries the sort costs nothing and it is what makes the conflict test's
// cost predictable — which matters because `SystemSchedule::add` runs it against every already
// registered system.

#include <cy/core/jobs/access.h>

#include <atomic>

namespace cy::jobs {
namespace {

std::atomic<u64> g_undeclared{0};
std::atomic<const char*> g_last_system{""};
std::atomic<u32> g_last_id{0};

/// The sort key. Domain first so that component 3 and resource 3 are never adjacent by accident.
constexpr u64 order_key(AccessDomain domain, u32 id) noexcept {
    return (static_cast<u64>(domain) << 32) | id;
}

constexpr u64 order_key(const AccessEntry& entry) noexcept {
    return order_key(entry.domain, entry.id);
}

/// Two declarations of the same thing, one from each system. See access.h for the table; the whole
/// rule is that a Write conflicts with anything that is not an Exclude.
constexpr bool accesses_conflict(Access first, Access second) noexcept {
    if (first == Access::Exclude || second == Access::Exclude) {
        return false;
    }
    return first == Access::Write || second == Access::Write;
}

}  // namespace

const char* access_domain_name(AccessDomain domain) noexcept {
    switch (domain) {
        case AccessDomain::Component:
            return "component";
        case AccessDomain::Resource:
            return "resource";
        case AccessDomain::EventChannel:
            return "event channel";
    }
    return "unknown";
}

const char* access_name(Access access) noexcept {
    switch (access) {
        case Access::Read:
            return "Read";
        case Access::Write:
            return "Write";
        case Access::Exclude:
            return "Exclude";
    }
    return "Unknown";
}

Status AccessSet::declare(AccessDomain domain, u32 id, Access access) noexcept {
    const u64 key = order_key(domain, id);

    u32 position = 0;
    while (position < count_ && order_key(entries_[position]) < key) {
        ++position;
    }
    if (position < count_ && order_key(entries_[position]) == key) {
        if (entries_[position].access == access) {
            return fail(ErrorCode::AlreadyExists,
                        "this access is already declared; a declaration that repeats itself is a "
                        "copy-paste rather than an intention");
        }
        // Read and Write of the same component is not a richer declaration, it is two
        // declarations that cannot both be honoured: the scheduler would have to treat the system
        // as a reader and a writer of one thing at once. Write alone already permits reading.
        return fail(ErrorCode::InvalidArgument,
                    "this component, resource or channel is already declared with a different "
                    "access; declare Write alone where a system both reads and writes");
    }
    if (count_ == kMaxEntries) {
        return fail(ErrorCode::OutOfRange,
                    "a system may declare at most AccessSet::kMaxEntries accesses; a system that "
                    "touches more distinct state than that is almost certainly two systems");
    }

    for (u32 i = count_; i > position; --i) {
        entries_[i] = entries_[i - 1];
    }
    entries_[position].domain = domain;
    entries_[position].id = id;
    entries_[position].access = access;
    ++count_;
    return ok();
}

bool AccessSet::find(AccessDomain domain, u32 id, Access& out) const noexcept {
    const u64 key = order_key(domain, id);
    for (u32 i = 0; i < count_; ++i) {
        const u64 candidate = order_key(entries_[i]);
        if (candidate == key) {
            out = entries_[i].access;
            return true;
        }
        if (candidate > key) {
            return false;
        }
    }
    return false;
}

bool AccessSet::conflicts_with(const AccessSet& other, AccessConflict& out) const noexcept {
    u32 left = 0;
    u32 right = 0;
    while (left < count_ && right < other.count_) {
        const u64 left_key = order_key(entries_[left]);
        const u64 right_key = order_key(other.entries_[right]);
        if (left_key < right_key) {
            ++left;
        } else if (right_key < left_key) {
            ++right;
        } else {
            if (accesses_conflict(entries_[left].access, other.entries_[right].access)) {
                out.domain = entries_[left].domain;
                out.id = entries_[left].id;
                out.first = entries_[left].access;
                out.second = other.entries_[right].access;
                return true;
            }
            ++left;
            ++right;
        }
    }
    return false;
}

bool SystemAccessGuard::check(AccessDomain domain, u32 id, Access requested) const noexcept {
    Access declared = Access::Read;
    if (access_ != nullptr && access_->find(domain, id, declared)) {
        // A writer may read what it writes; a reader may not write what it reads. Exclude declares
        // no data access at all, so any data access under it is undeclared.
        const bool permitted = declared == Access::Write
                                   ? requested != Access::Exclude
                                   : declared == requested;
        if (permitted) {
            return true;
        }
    }

    g_undeclared.fetch_add(1, std::memory_order_relaxed);
    g_last_system.store(system_ != nullptr ? system_ : "", std::memory_order_relaxed);
    g_last_id.store(id, std::memory_order_relaxed);
    return false;
}

u64 undeclared_access_violations() noexcept {
    return g_undeclared.load(std::memory_order_relaxed);
}

const char* last_undeclared_access_system() noexcept {
    return g_last_system.load(std::memory_order_relaxed);
}

u32 last_undeclared_access_id() noexcept {
    return g_last_id.load(std::memory_order_relaxed);
}

void reset_undeclared_access_violations() noexcept {
    g_undeclared.store(0, std::memory_order_relaxed);
    g_last_system.store("", std::memory_order_relaxed);
    g_last_id.store(0, std::memory_order_relaxed);
}

}  // namespace cy::jobs
