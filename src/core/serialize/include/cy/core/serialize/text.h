#pragma once
// The canonical text form: what goes under version control. Task 3.2.3.
//
// `serialization-and-prefabs` — "Text and binary forms" — asks for a text form that is
// deterministic (stable ordering of entities, components and fields; no volatile data; float
// formatting that round-trips exactly) and whose diffs are tractable: one changed property is one
// changed line, and two developers adding different entities touch different regions.
//
// This header is the *lexical* half — indentation, tokens, numbers, and the record grammar. The
// document grammar built on it lives at layer 4 in `src/scene/serialization/`, because a prefab is
// not something layer 0 knows about.
//
// THREE DECISIONS THE DIFF PROPERTY DEPENDS ON.
//
// 1. LINE-ORIENTED AND INDENTED, NEVER BRACED. A textual merge works on lines. Nesting expressed by
//    indentation means adding an entity adds a run of lines and changes no line above or below it;
//    nesting expressed by brackets means the closing bracket of the previous sibling moves, which
//    is a conflict for no reason.
//
// 2. FIELDS ARE ADDRESSED BY NUMBER, AND THE NUMBER IS ALL THAT IS WRITTEN. A field's name is
//    metadata (`core-type-system`), and renaming one is a manifest edit. If the name were in the
//    file, a rename would rewrite every scene and prefab that touches the field — which is exactly
//    the cost the identity model exists to avoid, reintroduced in the one place a designer would
//    notice it as a merge conflict. `TextOptions::annotate` adds names as trailing comments for a
//    human reading a file, and it is **off in the canonical form**: a canonical file is what is
//    committed, and it contains no name that a rename could churn.
//
// 3. FLOATS ROUND-TRIP EXACTLY, AND ARE THE SHORTEST FORM THAT DOES. `format_f32` tries increasing
//    precision until parsing the result reproduces the original bits. The value is therefore exact,
//    and it is also stable: the same float always produces the same characters, so a rewrite of an
//    unchanged value produces no diff. Writing `%.9g` unconditionally would be exact and would
//    render 0.5 as `0.5`, but 0.1F as `0.100000001`, and every designer-typed value in the project
//    would look like a rounding error.
//
//   record grammar, at some indent depth D:
//
//       record <type_id> <schema_version>
//         <field_id> <wire_type> <byte_count> <value>
//
//   value forms:  true|false · a decimal integer · a shortest-round-trip float · `-` or lowercase
//                 hex for an opaque run · `@<local>` for a local reference · `<32 hex>@<local>` for
//                 an external one.
//
//   A reference is spelled with `@` and not with `#`, because `#` starts a comment: the scanner
//   stops a line at the first word beginning with one, and a value that began with `#` would be
//   read as the start of a trailing comment and vanish. Found by the first document containing a
//   reference, which is a good argument for having written that test.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/type_info.h>
#include <cy/core/serialize/value_record.h>
#include <cy/core/serialize/wire.h>

#include <string_view>

namespace cy::serialize {

/// How a text form is written. The defaults are the canonical form — the one that is committed, and
/// the one a round-trip test compares.
struct TextOptions {
    /// Append `  # name` to a field line, and the type's name to a record line. Off in the
    /// canonical form; on for a human reading a file or for a diff in a review tool.
    bool annotate = false;
};

/// The most whitespace-separated words one line may carry. A record line has three, a field line
/// four, and a document's override line — the widest in the engine — has eight. The limit exists so
/// that a malformed file is a diagnostic rather than an allocation, and it has headroom because a
/// grammar that grows a word should not have to change a constant in another module to do it.
inline constexpr usize kMaxLineWords = 12;

/// Two spaces per level. Fixed rather than configurable: the indent is part of the canonical form,
/// and a project that could choose would have files that differ by whitespace alone.
inline constexpr u32 kIndentWidth = 2;

/// Appends UTF-8 text to a caller-owned array. No trailing whitespace is ever written and every
/// line ends in a single `\n`, because a canonical form with two spellings is not one.
class TextWriter {
public:
    explicit TextWriter(Array<char>& out, TextOptions options = {}) noexcept
        : out_(&out), options_(options) {}

    [[nodiscard]] const TextOptions& options() const noexcept { return options_; }

    /// Start a line at `depth`. Every `word` after it is separated by one space.
    [[nodiscard]] Status begin_line(u32 depth) noexcept;
    [[nodiscard]] Status word(std::string_view text) noexcept;
    [[nodiscard]] Status word_u64(u64 value) noexcept;
    [[nodiscard]] Status word_i64(i64 value) noexcept;
    /// A quoted, escaped string. For names, which are the one place free text appears.
    [[nodiscard]] Status word_quoted(std::string_view text) noexcept;
    /// A trailing `# ...` comment. Ignored by the reader; written only when annotating.
    [[nodiscard]] Status comment(std::string_view text) noexcept;
    [[nodiscard]] Status end_line() noexcept;

    /// One value record, as a `record` line and one line per field.
    ///
    /// `type` is used only for annotation and may be null; the bytes written when annotation is off
    /// do not depend on it, which is what makes the canonical form independent of what happens to
    /// be registered in the process doing the writing.
    [[nodiscard]] Status write_record(u32 depth, const ValueRecord& record,
                                      const reflect::TypeInfo* type = nullptr) noexcept;

private:
    Array<char>* out_;
    TextOptions options_;
    bool line_open_ = false;
    bool needs_space_ = false;
};

/// One tokenised line.
class TextLine {
public:
    [[nodiscard]] u32 depth() const noexcept { return depth_; }
    /// One-based, for a diagnostic that a text editor can jump to.
    [[nodiscard]] u32 number() const noexcept { return number_; }
    [[nodiscard]] usize count() const noexcept { return count_; }
    [[nodiscard]] std::string_view word(usize index) const noexcept {
        return (index < count_) ? words_[index] : std::string_view{};
    }

    [[nodiscard]] Expected<u64, Error> word_u64(usize index) const noexcept;
    [[nodiscard]] Expected<i64, Error> word_i64(usize index) const noexcept;
    /// The unescaped contents of a quoted word, written into `out` and returned as a view of it.
    [[nodiscard]] Expected<std::string_view, Error> word_unquoted(usize index,
                                                                  Array<char>& out) const noexcept;

private:
    friend class TextScanner;

    std::string_view words_[kMaxLineWords];
    usize count_ = 0;
    u32 depth_ = 0;
    u32 number_ = 0;
};

/// Walks a text form line by line, skipping blank lines and whole-line comments.
class TextScanner {
public:
    explicit TextScanner(std::string_view text) noexcept : text_(text) {}

    /// The next significant line, or `NotFound` at the end of the input.
    [[nodiscard]] Expected<TextLine, Error> next() noexcept;

    /// The last line read, without consuming another. What a recursive-descent reader uses to hand
    /// a line back to its caller when the indent says the nesting has closed.
    [[nodiscard]] const TextLine& current() const noexcept { return current_; }
    [[nodiscard]] bool has_current() const noexcept { return has_current_; }
    /// Put the current line back, so the next `next()` returns it again.
    void push_back() noexcept { pushed_back_ = has_current_; }

private:
    std::string_view text_;
    usize offset_ = 0;
    u32 line_number_ = 0;
    TextLine current_;
    bool has_current_ = false;
    bool pushed_back_ = false;
};

/// Read one record, whose `record` line the scanner has already produced, together with the field
/// lines that follow it at `depth + 1`.
[[nodiscard]] Status read_record_text(TextScanner& scanner, const TextLine& record_line,
                                      ValueRecord& out) noexcept;

/// Append the value half of a field line: what `TextWriter::write_record` writes after the size.
///
/// Public because a document grammar built on this one has values of its own to write — a prefab
/// parameter's default, an instance's argument — and they must be spelled the same way a field's
/// value is, or the file would have two syntaxes for one thing.
[[nodiscard]] Status write_value_text(Array<char>& out, WireType wire,
                                      Span<const u8> bytes) noexcept;

/// Parse one, appending its encoded bytes to `out`. The inverse of `write_value_text`.
[[nodiscard]] Status read_value_text(const TextLine& line, usize index, WireType wire, u32 size,
                                     Array<u8>& out) noexcept;

/// The shortest decimal text that parses back to exactly this value. Writes at most `size` bytes
/// including the terminator and returns how many characters were written.
[[nodiscard]] Expected<usize, Error> format_f32(f32 value, char* out, usize size) noexcept;
[[nodiscard]] Expected<usize, Error> format_f64(f64 value, char* out, usize size) noexcept;

/// Bytes wide enough for any float either function above produces.
inline constexpr usize kFloatTextCapacity = 32;

}  // namespace cy::serialize
