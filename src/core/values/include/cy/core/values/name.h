#pragma once
// `Name` — the interned, immutable string. Task 1.3.5.
//
// `core-type-system` — "String interning": type names, field names, node names, signal names and
// animation track paths are `Name`s. Comparison and hashing are O(1) and never touch character
// data, a copy is one 32-bit word, and interning the same text on two threads yields one entry.
//
// A `Name` is an index into a process-wide table, not a hash. A hash would make comparison a
// comparison of hashes, which is a comparison that is wrong on a collision rather than slow — and
// the whole point of the type is that two `Name`s compare equal exactly when their text does. The
// index costs an indirection to recover the text, which happens at presentation and never on a
// path that matters.
//
// The table never releases an entry. That is deliberate: a `Name` handed out in frame 1 must still
// resolve in frame 100000, and reference counting interned strings buys nothing against the size of
// the set an engine actually interns. The storage is reachable from a process-lifetime root for
// exactly that reason, so a leak detector sees it as live rather than as leaked — see name.cpp.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

#include <string_view>

namespace cy {

/// An interned string. Trivially copyable, 32 bits wide, and default-constructed to the empty name.
///
/// Ordering is by table index, which is interning order: it is a total order, usable as a map key
/// within one process, and it is NOT lexicographic and NOT stable across runs. Anything that writes
/// an ordered sequence of names to disk sorts by `text()`.
class Name {
public:
    /// The empty name. Index 0 is reserved for it, so a zeroed `Name` is the empty one rather than
    /// an arbitrary entry, and a zeroed component holding a `Name` is meaningful.
    constexpr Name() noexcept = default;

    /// Intern `text`, returning the one `Name` for it. Thread-safe. Interning the same text twice
    /// returns the same value and stores one entry.
    ///
    /// Text longer than `kMaxLength` is rejected and yields the empty name: a name is an
    /// identifier, and an identifier that long is a payload that took a wrong turn.
    [[nodiscard]] static Name intern(std::string_view text) noexcept;

    /// Look up without interning. The empty name when `text` has never been interned, which lets a
    /// lookup path avoid growing the table with keys that were only ever probed.
    [[nodiscard]] static Name find(std::string_view text) noexcept;

    /// The interned text. Valid for the lifetime of the process, NUL-terminated at `data()[size()]`
    /// so it can be handed to a C interface without a copy.
    [[nodiscard]] std::string_view text() const noexcept;
    [[nodiscard]] const char* c_str() const noexcept;

    [[nodiscard]] constexpr u32 index() const noexcept { return index_; }
    [[nodiscard]] constexpr bool is_empty() const noexcept { return index_ == 0; }

    /// Reconstruct from a previously obtained index. Runtime-only: an index is a position in this
    /// process's table and is never serialized — `core-type-system` addresses persistent things by
    /// `TypeId` and `FieldId`, which are assigned and recorded, not by an interning order.
    [[nodiscard]] static constexpr Name from_index(u32 index) noexcept { return Name(index); }

    friend constexpr bool operator==(Name a, Name b) noexcept { return a.index_ == b.index_; }
    friend constexpr bool operator!=(Name a, Name b) noexcept { return a.index_ != b.index_; }
    friend constexpr bool operator<(Name a, Name b) noexcept { return a.index_ < b.index_; }

    /// The longest text that may be interned.
    static constexpr usize kMaxLength = 1024;

private:
    explicit constexpr Name(u32 index) noexcept : index_(index) {}

    u32 index_ = 0;
};

static_assert(sizeof(Name) == 4, "Name is one 32-bit word: copying it must cost nothing");

/// Hash for the standard associative containers. The index is already a dense counter, so the hash
/// is the index — spreading it would only cost cycles to undo a property the table already has.
struct NameHash {
    [[nodiscard]] usize operator()(Name name) const noexcept {
        return static_cast<usize>(name.index());
    }
};

/// What the intern table holds. Reported through `values_diagnostics()`; exposed here so a test can
/// assert that interning the same text twice stored one entry rather than two.
struct NameTableStats {
    u32 entries = 0;     ///< including the empty name at index 0
    u64 bytes = 0;       ///< text storage in use, excluding the index
    u64 lookups = 0;     ///< intern() and find() calls
    u64 insertions = 0;  ///< entries actually created
    u64 rejections = 0;  ///< text over kMaxLength
};

[[nodiscard]] NameTableStats name_table_stats() noexcept;

}  // namespace cy

// `Name` is a runtime table index, so there is no constant expression that names an entry: the
// table does not exist until the process does. CY_NAME is what compile-time construction means
// here — the literal is fixed at compile time and the interning happens once, at the declaration
// site, on first evaluation. The static is function-local, so its initialisation is thread-safe and
// costs one already-initialised load afterwards.
//
//   const cy::Name kOnHit = CY_NAME("on_hit");
//
// This is the same shape `CY_TRACE_NAME` uses in diagnostics, for the same reason.
#define CY_NAME(literal)                                                         \
    ([]() noexcept -> ::cy::Name {                                               \
        static const ::cy::Name cy_interned_name_ = ::cy::Name::intern(literal); \
        return cy_interned_name_;                                                \
    }())
