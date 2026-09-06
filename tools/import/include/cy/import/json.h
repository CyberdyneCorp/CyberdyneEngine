#ifndef CY_IMPORT_JSON_H
#define CY_IMPORT_JSON_H
// A strict JSON reader, for glTF. M5 task 5.1.
//
// --- WHY THIS EXISTS, WHICH IS A QUESTION WORTH ANSWERING ----------------------------------------
//
// `thirdparty-dependencies` says parsing an open specification is not differentiating and names
// **cgltf** for glTF, which brings its own JSON reader. That remains the right answer and this file
// is not an argument against it. It is here because at M5 no glTF dependency is integrated (the
// note at the head of `deps/manifest.toml` says why), and a glTF importer with no JSON reader is
// not an importer. It is deliberately the smallest thing that reads glTF correctly, it is tool-time
// only, and it is the file that goes away when cgltf is integrated.
//
// --- THE SHAPE, AND WHY IT IS FLAT ---------------------------------------------------------------
//
// Values live in one array and refer to each other by index rather than by pointer. Three
// consequences, all of which matter for a parser that runs at cook time over files an artist
// exported:
//
//   * Parsing is one growing array and one string blob, so a 40 MB glTF's JSON costs two
//     allocations that double rather than a hundred thousand small ones.
//   * A `JsonRef` is a `u32` and stays valid while the document lives, so a caller can hold one
//     across a call that parses more. A pointer into a growing array would not.
//   * Nothing recurses on destruction. A deeply nested document destroys in a loop rather than in a
//     stack that a malicious or merely strange file could overflow.
//
// Parsing itself IS recursive, and is bounded: `kMaxDepth` refuses a document nested deeper than
// any real glTF, because the alternative to a bound is a stack overflow on a file somebody
// downloaded.
//
// --- WHAT IT REFUSES -----------------------------------------------------------------------------
//
// A trailing comma, a comment, an unquoted key, a `NaN` or `Infinity` literal, a control character
// inside a string, a duplicate key, and anything after the root value. Every one of those is
// something a lenient parser accepts and a different parser rejects, which is how two tools come to
// disagree about what a file means.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

#include <string_view>

namespace cy::import {

/// An index into a document's value table. `JsonDocument::kNone` is the absent value.
using JsonRef = u32;

/// What a value is.
enum class JsonKind : u8 {
    Null = 0,
    Bool = 1,
    Number = 2,
    String = 3,
    Array = 4,
    Object = 5,
};

/// A parsed JSON document. Owns every value and every decoded string.
class JsonDocument {
public:
    /// The absent value. Returned by `member` for a key that is not there, which is the ordinary
    /// case in glTF where almost every field is optional.
    static constexpr JsonRef kNone = 0xFFFFFFFFU;

    /// The deepest nesting accepted. glTF's own deepest structure is about six; sixty-four is far
    /// past anything real and far below what would overflow a stack.
    static constexpr u32 kMaxDepth = 64;

    JsonDocument() noexcept = default;

    JsonDocument(const JsonDocument&) = delete;
    JsonDocument& operator=(const JsonDocument&) = delete;
    JsonDocument(JsonDocument&&) noexcept = default;
    JsonDocument& operator=(JsonDocument&&) noexcept = default;

    /// Parse a whole document. `text` is not retained: strings are copied and unescaped into the
    /// document's own storage, so the caller may free its buffer.
    ///
    /// Every failure is `InvalidArgument` with a message saying what was expected, except a
    /// document nested past `kMaxDepth`, which is `OutOfRange`.
    [[nodiscard]] static Expected<JsonDocument, Error> parse(std::string_view text) noexcept;

    [[nodiscard]] bool is_empty() const noexcept { return values_.empty(); }
    /// The root value. `kNone` for an empty document.
    [[nodiscard]] JsonRef root() const noexcept;

    [[nodiscard]] JsonKind kind(JsonRef value) const noexcept;
    [[nodiscard]] bool is(JsonRef value, JsonKind wanted) const noexcept;

    /// The number of elements of an array, or of members of an object. Zero for anything else.
    [[nodiscard]] usize size(JsonRef value) const noexcept;

    /// One element of an array. `kNone` when `value` is not an array or the index is past its end.
    [[nodiscard]] JsonRef at(JsonRef value, usize index) const noexcept;

    /// One member of an object by name. `kNone` when absent, which is how an optional field reads.
    [[nodiscard]] JsonRef member(JsonRef value, std::string_view key) const noexcept;

    /// The name of an object's nth member, for a caller that walks rather than looks up.
    [[nodiscard]] std::string_view key_at(JsonRef value, usize index) const noexcept;

    /// A number, or `fallback` when the value is absent or is not a number.
    ///
    /// The whole reader is written in this shape rather than as fallible getters, because glTF is a
    /// format of optional fields with specified defaults and `number_or(node, "scale", 1.0)` is the
    /// operation an importer performs a thousand times. A caller that must distinguish "absent"
    /// from "present and zero" asks `kind` first.
    [[nodiscard]] f64 number_or(JsonRef value, f64 fallback) const noexcept;
    [[nodiscard]] i64 integer_or(JsonRef value, i64 fallback) const noexcept;
    [[nodiscard]] bool bool_or(JsonRef value, bool fallback) const noexcept;
    [[nodiscard]] std::string_view string_or(JsonRef value,
                                             std::string_view fallback) const noexcept;

private:
    struct Node {
        JsonKind kind = JsonKind::Null;
        bool boolean = false;
        f64 number = 0.0;
        /// For a string: the slice of `text_`. For an object member's key: the same.
        u32 text_offset = 0;
        u32 text_length = 0;
        /// For an array or object: the range of `children_` it owns.
        u32 first_child = 0;
        u32 child_count = 0;
    };

    struct Child {
        u32 key_offset = 0;
        u32 key_length = 0;
        JsonRef value = kNone;
    };

    class Parser;

    [[nodiscard]] std::string_view text_of(u32 offset, u32 length) const noexcept;

    Array<Node> values_;
    Array<Child> children_;
    Array<char> text_;
};

}  // namespace cy::import

#endif  // CY_IMPORT_JSON_H
