#pragma once
// `AssetId` — the persistent identity of content. Task 1.3.2.
//
// `core-type-system` — "Asset ids are distinct from handles": an `AssetId` is a 128-bit stable
// identifier for content, and it is NOT a runtime handle. Assets are referenced by id in serialized
// data; handles are runtime-only and are never serialized. The two are confused constantly in
// engines that spell them the same way, and the confusion is silent — a saved scene that stored a
// handle loads into a process where that slot holds something else.
//
// So the distinction is made structurally rather than by convention:
//
//   * different width — 128 bits against 64, so they do not even fit in one another;
//   * no conversion in either direction, explicit or implicit, and no shared base;
//   * no ordering or equality operator that mixes them.
//
// `src/core/values/tests/compile_fail/` compiles four programs that try each confusion and requires
// every one of them to fail. `core-assets-and-io` (task 3.3.1) owns how an id is *derived* from
// content; this file owns only what the type is and how it is spelled.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <string_view>
#include <type_traits>

namespace cy {

/// A 128-bit content identifier, held as two 64-bit halves in big-endian reading order: `high` is
/// the first 16 hex digits of the canonical text form, `low` the last 16.
class AssetId {
public:
    /// The nil id. Names no asset; a zeroed field is unset rather than a reference to asset zero.
    constexpr AssetId() noexcept = default;

    constexpr AssetId(u64 high, u64 low) noexcept : high_(high), low_(low) {}

    [[nodiscard]] constexpr u64 high() const noexcept { return high_; }
    [[nodiscard]] constexpr u64 low() const noexcept { return low_; }
    [[nodiscard]] constexpr bool is_nil() const noexcept { return high_ == 0 && low_ == 0; }
    explicit constexpr operator bool() const noexcept { return !is_nil(); }

    /// The canonical text form: 32 lowercase hex digits, no separators. Writes 32 characters and a
    /// terminating NUL, and returns the number of characters written.
    static constexpr usize kTextLength = 32;
    usize format(char (&out)[kTextLength + 1]) const noexcept;

    /// Parse the canonical form. Rejects anything that is not exactly 32 hex digits, rather than
    /// stopping at the first bad character and returning a partial id.
    [[nodiscard]] static Expected<AssetId, Error> parse(std::string_view text) noexcept;

    friend constexpr bool operator==(AssetId a, AssetId b) noexcept {
        return a.high_ == b.high_ && a.low_ == b.low_;
    }
    friend constexpr bool operator!=(AssetId a, AssetId b) noexcept { return !(a == b); }
    /// A total order, for use as a map key and for writing a sorted table to disk. Big-endian
    /// reading order, so sorting ids sorts their text form the same way.
    friend constexpr bool operator<(AssetId a, AssetId b) noexcept {
        return a.high_ != b.high_ ? a.high_ < b.high_ : a.low_ < b.low_;
    }

private:
    u64 high_ = 0;
    u64 low_ = 0;
};

static_assert(sizeof(AssetId) == 16, "an asset id is 128 bits");
static_assert(std::is_trivially_copyable_v<AssetId>);

/// Hash for the standard associative containers.
struct AssetIdHash {
    [[nodiscard]] usize operator()(AssetId id) const noexcept {
        // The halves of a content-derived id are already well distributed; mixing them with a
        // multiply-xor keeps a 32-bit size_t from throwing half of one away.
        const u64 mixed = id.high() ^ (id.low() * 0x9e3779b97f4a7c15ULL);
        return static_cast<usize>(mixed ^ (mixed >> 32));
    }
};

}  // namespace cy
