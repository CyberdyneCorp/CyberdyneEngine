#pragma once
// The three identities a save is addressed by: the entity, the region it belongs to, and the scope
// its state has a lifetime in. Task 6.1.
//
// `save-and-persistence` — "Persistent identity" — requires that saved references use stable
// identities and that "runtime entity indices, memory addresses, pointers, and archetype positions
// SHALL NEVER be serialised". `world-partition-and-streaming` — "Persistent entity identity" —
// adds that the identity is assigned at authoring time, is independent of runtime entity
// identifiers and of which file the entity is stored in, and is stable across editing, cooking,
// saving, networking and world reload.
//
// M6's design.md §5 pins the reason this file is written now rather than later: *persistent
// identity is stable across unload*, and "a save written before this is a save that cannot be
// migrated". An identity that is a runtime index is one that changes the moment a cell is unloaded
// and reloaded, and every save written against it is a save whose references mean something else
// after the next traversal.
//
// WHY THREE TYPES AND NOT ONE INTEGER. Each of the three is a different question — *which entity*,
// *which region of the world*, *which lifetime* — and a u64 answers all three identically, so the
// compiler cannot tell a caller that swapped two of them. `PersistentId` is 128 bits and
// `RegionKey` is 64, so the two do not fit in one another; neither converts to the other in either
// direction. This is the same argument `core-type-system` makes about `AssetId` against a runtime
// handle, and it is made structurally for the same reason.
//
// THE REGION KEY'S ENCODING IS NOT THIS MODULE'S. `world-partition-and-streaming` — "Stable cell
// identity" — says a cell identifier encodes "its partition, hierarchy level, and spatial position,
// generated deterministically from the partitioner's configuration", and that "the encoding SHALL
// be opaque to consumers". A save is a consumer. `RegionKey` therefore carries the number and
// interprets nothing about it: no arithmetic, no neighbour, no level accessor. When the partition
// module lands, its cell identifier is converted into one of these at the boundary and the save
// keeps working, because there was never anything here to disagree with.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <string_view>
#include <type_traits>

namespace cy::save {

/// The authoring-time identity of a persistent entity: 128 bits, minted once, never derived.
///
/// Not derived from position, file, index or path — `world-partition-and-streaming` rules all four
/// out by name, because every one of them changes under an edit the identity exists to survive.
/// Held as two 64-bit halves in big-endian reading order, so that sorting ids sorts their text form
/// the same way and a chunk table written on one machine is byte-identical on another.
class PersistentId {
public:
    /// The nil identity. Names no entity; a zeroed field is unset rather than a reference to
    /// entity zero.
    constexpr PersistentId() noexcept = default;
    constexpr PersistentId(u64 high, u64 low) noexcept : high_(high), low_(low) {}

    [[nodiscard]] constexpr u64 high() const noexcept { return high_; }
    [[nodiscard]] constexpr u64 low() const noexcept { return low_; }
    [[nodiscard]] constexpr bool is_nil() const noexcept { return high_ == 0 && low_ == 0; }
    explicit constexpr operator bool() const noexcept { return !is_nil(); }

    /// The canonical text form: 32 lowercase hex digits, no separators, plus a terminator.
    static constexpr usize kTextLength = 32;
    usize format(char (&out)[kTextLength + 1]) const noexcept;

    /// Parse the canonical form. Rejects anything that is not exactly 32 hex digits rather than
    /// stopping at the first bad character and returning a partial identity.
    [[nodiscard]] static Expected<PersistentId, Error> parse(std::string_view text) noexcept;

    friend constexpr bool operator==(PersistentId a, PersistentId b) noexcept {
        return a.high_ == b.high_ && a.low_ == b.low_;
    }
    friend constexpr bool operator!=(PersistentId a, PersistentId b) noexcept { return !(a == b); }
    friend constexpr bool operator<(PersistentId a, PersistentId b) noexcept {
        return a.high_ != b.high_ ? a.high_ < b.high_ : a.low_ < b.low_;
    }

private:
    u64 high_ = 0;
    u64 low_ = 0;
};

static_assert(sizeof(PersistentId) == 16, "a persistent identity is 128 bits");
static_assert(std::is_trivially_copyable_v<PersistentId>);

/// Hash for the associative containers, mixing the halves so a 32-bit `usize` keeps both.
struct PersistentIdHash {
    [[nodiscard]] usize operator()(PersistentId id) const noexcept {
        const u64 mixed = id.high() ^ (id.low() * 0x9e37'79b9'7f4a'7c15ULL);
        return static_cast<usize>(mixed ^ (mixed >> 32U));
    }
};

/// The region of the world a persistent record belongs to. An opaque cell identifier.
///
/// The persistent state store is organised by this and by nothing else, which is what makes
/// "producing a save SHALL NOT require loading regions that are not resident" structural rather
/// than a promise: the store is keyed by region, and a region's key is known without the region.
class RegionKey {
public:
    constexpr RegionKey() noexcept = default;
    explicit constexpr RegionKey(u64 value) noexcept : value_(value) {}

    [[nodiscard]] constexpr u64 value() const noexcept { return value_; }

    friend constexpr bool operator==(RegionKey a, RegionKey b) noexcept {
        return a.value_ == b.value_;
    }
    friend constexpr bool operator!=(RegionKey a, RegionKey b) noexcept { return !(a == b); }
    friend constexpr bool operator<(RegionKey a, RegionKey b) noexcept {
        return a.value_ < b.value_;
    }

private:
    u64 value_ = 0;
};

static_assert(std::is_trivially_copyable_v<RegionKey>);

/// The region that holds state belonging to no cell: `AlwaysLoaded` entities, world variables and
/// anything a scope fragment carries. Zero is reserved for it, so a partitioner never issues it.
inline constexpr RegionKey kGlobalRegion{0};

struct RegionKeyHash {
    [[nodiscard]] usize operator()(RegionKey key) const noexcept {
        const u64 mixed = key.value() * 0x9e37'79b9'7f4a'7c15ULL;
        return static_cast<usize>(mixed ^ (mixed >> 32U));
    }
};

/// The lifetimes saved state is organised into. `save-and-persistence` — "Persistence scopes" —
/// gives the six and forbids the monolith: "graphics settings, unlocked achievements, campaign
/// world state, and match state have different lifetimes, different cloud semantics, and different
/// sharing rules".
///
/// Persistent: the numbers are written into manifests, so an enumerator is added at the end and
/// never renumbered.
enum class Scope : u8 {
    /// The player, across every game they own of this project: settings, input maps, unlocks.
    Profile = 0,
    /// One installed game's own state, shared by every campaign in it.
    GameInstance = 1,
    /// One playthrough: progression, quest state, and the world it belongs to.
    Campaign = 2,
    /// One sitting or one match. Discarded when it ends.
    Session = 3,
    /// The persistence overlay itself: what the world is, as against what it was authored as.
    World = 4,
    /// One participant's state within a session, so a co-operative save carries both players.
    Participant = 5,

    /// One past the last defined value, for a table sized by it. Never written to a file.
    Count = 6,
};

/// The enumerator's own spelling, for a diagnostic and for the inspector. Never null.
const char* scope_name(Scope scope) noexcept;

/// True when `value` names a scope this build knows. A manifest carrying anything else is a save
/// written by a newer build, which is a diagnostic rather than a reinterpretation.
[[nodiscard]] constexpr bool is_known_scope(u8 value) noexcept {
    return value < static_cast<u8>(Scope::Count);
}

}  // namespace cy::save
