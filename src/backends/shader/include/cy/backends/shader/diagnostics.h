#pragma once
// Shader diagnostics: what a compilation said, and where. Tasks 3.2 and 3.7.
//
// `shader-system` — "Compile error surfaces with source location": a failed shader compile SHALL
// carry the Slang source file, line and column, and appear in the editor's shader editor and in the
// build log. A `cy::Error` cannot carry that: it is trivially copyable, holds no storage, and its
// `message` must outlive it (see cy/core/base/error.h). So a compilation returns an `Error` for the
// *outcome* and a `DiagnosticLog` for the *detail*, and the log owns its text.
//
// THE LOG IS PARSED, NOT PRINTED. A compiler back end hands the engine a wall of text; the engine
// turns it into records. That is what lets the editor put a squiggle on line 41 of a file rather
// than a paragraph in a console, and it is the reason `parse_compiler_output` is a function of this
// module rather than something the editor is expected to do again. The grammar it accepts is the
// one Slang, glslang, DXC, MSVC and Clang all emit — `file(line): severity code: text` and
// `file:line:column: severity: text` — because a diagnostic format that only one tool produces is a
// diagnostic format that breaks when the tool is replaced.
//
// It is also where a *structural* rejection is reported: the descriptor-set convention
// (reflection.h) and the permutation budget (permutation.h) both fail a build with a diagnostic
// rather than with a bare error code, so a violated convention reads like a compile error to the
// person who has to fix it.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>

#include <string_view>

namespace cy::shader {

enum class Severity : u8 { Note = 0, Warning = 1, Error = 2 };

const char* severity_name(Severity severity) noexcept;

/// Where a diagnostic points. `file` is owned by the `DiagnosticLog` that produced it and is valid
/// until that log is cleared or destroyed; `line` and `column` are one-based, and zero means the
/// compiler did not say.
struct SourceLocation {
    const char* file = "";
    u32 line = 0;
    u32 column = 0;
};

struct Diagnostic {
    Severity severity = Severity::Error;
    SourceLocation location;
    /// The compiler's own text, without the location prefix. Owned by the log.
    const char* message = "";
    /// The tool's diagnostic number where it emitted one ("E00100", "39001"), empty otherwise.
    const char* code = "";
};

/// A compilation's diagnostics, with their text.
///
/// Every string a `Diagnostic` points at lives in this object's own arena, so a log can be moved
/// into a cache entry and read back a frame later. Copying is explicit (`clone`) for the reason
/// every container in the engine makes it explicit: the cost is visible at the call site.
class DiagnosticLog {
public:
    explicit DiagnosticLog(Allocator& allocator) noexcept;

    DiagnosticLog(const DiagnosticLog&) = delete;
    DiagnosticLog& operator=(const DiagnosticLog&) = delete;
    DiagnosticLog(DiagnosticLog&&) noexcept = default;
    DiagnosticLog& operator=(DiagnosticLog&&) noexcept = default;

    /// `location.file`, `message` and `code` are copied. They may NOT point into this log's own
    /// storage: interning grows an arena, and a source that aliased it would be read after the
    /// growth freed it. Nothing in the engine does that, and `append` reads from a *different* log
    /// for exactly this reason.
    [[nodiscard]] Status add(Severity severity, const SourceLocation& location,
                             std::string_view message, std::string_view code = {}) noexcept;

    /// The common case: a diagnostic with no source position, which is what a structural rejection
    /// or an I/O failure produces.
    [[nodiscard]] Status add(Severity severity, std::string_view message) noexcept {
        return add(severity, SourceLocation{}, message);
    }

    /// Turn a compiler's textual output into records, one per recognised line.
    ///
    /// A line the grammar does not recognise is kept, as a `Note` with no location: dropping it
    /// would lose the "see declaration here" half of a two-part error, and the caller is better
    /// placed than this parser to decide whether unstructured text matters.
    [[nodiscard]] Status parse_compiler_output(std::string_view text) noexcept;

    [[nodiscard]] usize size() const noexcept { return records_.size(); }
    [[nodiscard]] bool empty() const noexcept { return records_.empty(); }

    /// One diagnostic, assembled from the record and the arena.
    ///
    /// Built on demand rather than stored, because a `Diagnostic` holds pointers into an arena that
    /// grows: keeping a parallel array of them would mean re-pointing every entry after every
    /// append, which is a quadratic amount of work to avoid an addition.
    [[nodiscard]] Diagnostic at(usize index) const noexcept;

    [[nodiscard]] bool has_errors() const noexcept { return error_count_ != 0; }
    [[nodiscard]] u32 error_count() const noexcept { return error_count_; }
    [[nodiscard]] u32 warning_count() const noexcept { return warning_count_; }

    void clear() noexcept;

    /// Append every entry of `other` to this log, copying its text. Used to fold a per-permutation
    /// compilation's diagnostics into a whole-library report.
    [[nodiscard]] Status append(const DiagnosticLog& other) noexcept;

private:
    /// Copy `text` into the arena and return its offset. Offset zero is always the empty string, so
    /// a zeroed record reads as "no file, no code" rather than as a dangling pointer. `text` must
    /// not alias the arena — see `add`.
    [[nodiscard]] Expected<u32, Error> intern(std::string_view text) noexcept;
    /// The one place a record is built. The public `add` is this with the file taken from a
    /// `SourceLocation`; the parser calls it directly, with views into the compiler's output.
    [[nodiscard]] Status add_parts(Severity severity, std::string_view file, u32 line, u32 column,
                                   std::string_view message, std::string_view code) noexcept;
    /// A `--> file:line:column` line, attached to the record just emitted. True when it was one.
    [[nodiscard]] Expected<bool, Error> apply_location_continuation(std::string_view line) noexcept;
    [[nodiscard]] const char* at_offset(u32 offset) const noexcept { return text_.data() + offset; }

    struct Record {
        Severity severity = Severity::Error;
        u32 file = 0;
        u32 message = 0;
        u32 code = 0;
        u32 line = 0;
        u32 column = 0;
    };

    Array<char> text_;
    Array<Record> records_;
    u32 error_count_ = 0;
    u32 warning_count_ = 0;
};

}  // namespace cy::shader
