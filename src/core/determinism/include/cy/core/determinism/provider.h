#pragma once
// State providers: the authoritative state that is not component data. Task 4.2.6.
//
// `simulation-and-determinism` — "State providers": authoritative state that is not ECS component
// data — session state, rules, teams, participants, random stream state, the world persistence
// index, subsystem state — is exposed through providers that can capture and restore themselves,
// and **each declares which mechanisms it participates in**: rollback, replay checkpointing,
// saving, hashing. "Participation SHALL be explicit rather than assumed."
//
// The declaration is the interesting half. A provider that is expensive to capture and irrelevant
// to rollback says so, and the rollback path skips it without a special case anywhere; the
// alternative — every mechanism holding its own list of what to ask — is four lists that drift.
//
// Registration order does not decide anything: `finalize()` sorts by name, which is
// `simulation-and-determinism`'s "Registries whose contents affect simulation SHALL be finalised in
// a deterministic order derived from stable identifiers before simulation begins". A provider added
// by a plugin loaded second therefore hashes in the same position as one added by a plugin loaded
// first.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/determinism/hash.h>
#include <cy/core/memory/array.h>

namespace cy::determinism {

/// Which mechanisms a provider takes part in. A bit set, because the four are independent: the
/// random source is hashed and checkpointed but has nothing to save, and a spatial index
/// participates in none of them.
enum class Participates : u8 {
    None = 0,
    Hash = 1U << 0U,
    Rollback = 1U << 1U,
    Checkpoint = 1U << 2U,
    Save = 1U << 3U,
};

[[nodiscard]] constexpr Participates operator|(Participates a, Participates b) noexcept {
    return static_cast<Participates>(static_cast<u8>(a) | static_cast<u8>(b));
}
[[nodiscard]] constexpr bool participates_in(Participates set, Participates one) noexcept {
    return (static_cast<u8>(set) & static_cast<u8>(one)) != 0;
}

/// Authoritative state that is not a component.
///
/// `capture`/`restore` are byte-oriented rather than reflected because a provider's state is
/// whatever it says it is — a random source is a seed, a rules block is a struct, a persistence
/// index is a table. What they are not is optional: a provider that declares Checkpoint and cannot
/// capture is a provider that fails at the first checkpoint, so the registry checks the pairing at
/// registration.
class StateProvider {
public:
    StateProvider() = default;
    virtual ~StateProvider() = default;

    StateProvider(const StateProvider&) = delete;
    StateProvider& operator=(const StateProvider&) = delete;
    StateProvider(StateProvider&&) = delete;
    StateProvider& operator=(StateProvider&&) = delete;

    /// The stable identifier the registry orders by and a divergence report names. A literal, or
    /// storage outliving the registry.
    [[nodiscard]] virtual const char* name() const noexcept = 0;
    [[nodiscard]] virtual Participates participation() const noexcept = 0;

    /// Fold this provider's authoritative state into the open node. Called only when the provider
    /// declared `Hash`; the default refuses, so a provider that declares Hash and forgets to
    /// implement it is a failure at the first hash rather than a silently missing subtree.
    [[nodiscard]] virtual Status hash(StateHashTree& tree) const noexcept;

    /// Append this provider's state to `out`. Called for Rollback, Checkpoint and Save alike: the
    /// three differ in *when* and in what else is captured beside them, not in what a provider has
    /// to say about itself.
    [[nodiscard]] virtual Status capture(Array<u8>& out) const noexcept;
    [[nodiscard]] virtual Status restore(Span<const u8> bytes) noexcept;
};

/// The providers of one simulation, in a fixed order.
///
/// Holds pointers and owns nothing: a provider is part of the subsystem it describes and outlives
/// its registration there. Registration after `finalize()` is refused, because a provider that
/// joins mid-session would change the hash's shape at a tick nobody chose.
class StateProviderRegistry {
public:
    explicit StateProviderRegistry(Allocator& allocator) noexcept : providers_(allocator) {}

    StateProviderRegistry(const StateProviderRegistry&) = delete;
    StateProviderRegistry& operator=(const StateProviderRegistry&) = delete;

    /// Refuses a null provider, a duplicate name, and a provider registered after `finalize()`.
    [[nodiscard]] Status add(StateProvider& provider) noexcept;

    /// Fix the order: sorted by name. Idempotent.
    void finalize() noexcept;
    [[nodiscard]] bool finalized() const noexcept { return finalized_; }

    [[nodiscard]] u32 size() const noexcept { return static_cast<u32>(providers_.size()); }
    [[nodiscard]] StateProvider& at(u32 index) const noexcept { return *providers_[index]; }
    [[nodiscard]] StateProvider* find(const char* name) const noexcept;

    /// Fold every provider that declared `Hash` into `tree`, in the finalised order, each as its
    /// own `Subsystem` node. Refuses if the registry has not been finalised — an unfinalised
    /// registry has an order that depends on when plugins loaded, and hashing it would make load
    /// order visible in the result.
    [[nodiscard]] Status hash_all(StateHashTree& tree) const noexcept;

private:
    Array<StateProvider*> providers_;
    bool finalized_ = false;
};

}  // namespace cy::determinism
