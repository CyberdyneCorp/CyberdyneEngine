// TypeId and FieldId — the identifiers every artefact written from M1 onward encodes. Task 1.2.1.
//
// An identifier is an **opaque number assigned on first sight and recorded in the committed
// manifest** (identity/manifest.toml, design.md §1). It is not a hash of the name, not a hash of
// the type, not an index into anything, and not a content digest.
//
// The reason is one sentence long: anything derived from a name changes when the name changes, and
// a rename is precisely the event the manifest exists to survive. A hash also collides silently,
// where a counter cannot — and a collision in this particular number produces data that loads
// successfully and is wrong, which is the failure mode with no diagnostic.
//
// Nothing in this header computes an identifier. There is no constructor from a name, no hash
// helper, and no way to derive one at run time: the only source is the manifest, through generated
// code. That is deliberate — the invariant is easier to keep when the tempting shortcut does not
// compile.
//
// Zero is the null identifier and is never assigned, so a default-constructed id is invalid rather
// than being some other type's.

#ifndef CY_CORE_REFLECT_IDS_H
#define CY_CORE_REFLECT_IDS_H

#include <cy/core/base/types.h>

#include <compare>

namespace cy::reflect {

/// The persistent identity of a reflected type. Assigned once, recorded, never reused.
class TypeId {
public:
    constexpr TypeId() noexcept = default;

    /// Explicit, and taking a plain number, because the only legitimate caller is generated code
    /// reading a value the manifest already decided. A conversion from a name would be the bug.
    explicit constexpr TypeId(u32 value) noexcept : value_(value) {}

    [[nodiscard]] constexpr u32 value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

    friend constexpr bool operator==(TypeId, TypeId) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(TypeId, TypeId) noexcept = default;

private:
    u32 value_ = 0;
};

/// The persistent identity of one field of a reflected type. Unique within its type, never reused
/// after removal — a tombstone in the manifest holds the number out of circulation permanently.
class FieldId {
public:
    constexpr FieldId() noexcept = default;
    explicit constexpr FieldId(u32 value) noexcept : value_(value) {}

    [[nodiscard]] constexpr u32 value() const noexcept { return value_; }
    [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0; }

    friend constexpr bool operator==(FieldId, FieldId) noexcept = default;
    friend constexpr std::strong_ordering operator<=>(FieldId, FieldId) noexcept = default;

private:
    u32 value_ = 0;
};

}  // namespace cy::reflect

#endif  // CY_CORE_REFLECT_IDS_H
