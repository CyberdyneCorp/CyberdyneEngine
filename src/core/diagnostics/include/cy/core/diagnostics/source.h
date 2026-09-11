#pragma once
// Source locations, as classified data rather than as names.
//
// `diagnostics-profiling-and-crash` — "Privacy classification" states it in as many words:
//
//   **A source location is classified data, not a name.** Any value carrying a filesystem path —
//   including one the compiler injects through `__FILE__` or an equivalent — SHALL be a classified
//   field, reachable by the writer's redaction. Registering such a value as an event or scope
//   *name* places it structurally beyond redaction, and SHALL NOT be done.
//
//   Compiler flags that strip source prefixes are a **mitigation and not the mechanism**: they do
//   not exist on every toolchain, and a privacy system that depends on one is only as good as the
//   compiler that happened to build the artefact.
//
// M0 did the forbidden thing and its gate found it: `CY_LOG` registered `__FILE__ ":" __LINE__` in
// the NAME table, `on_assertion_failure()` registered `AssertionFailure::file` in the same table,
// and the fix was `-fmacro-prefix-map` in cmake/compilers.cmake. That flag repairs the instance on
// two of the three toolchains this engine targets — MSVC has no equivalent, and a plugin or a
// third-party translation unit compiled without the engine's flags hands the runtime an absolute
// path whatever the engine was built with. Closing the CLASS means the artefact is clean because of
// something the writer does, not because of something the compiler did.
//
// SO A LOCATION IS AN ENTRY IN ITS OWN TABLE, AND THE TABLE IS CLASSIFIED.
//
//   * `register_source_location()` interns the location once, at the declaration site, never in the
//     emission path. A record carries the resulting `LocationId` in its `b` word — one `u32`, no
//     string work where the producer runs.
//   * The writer resolves the table into the META chunk, and that is where BOTH rules apply: the
//     path is sanitised so no absolute path ever reaches an artefact, and the entry is then subject
//     to the artefact's `ExportPolicy` like any classified value. Removals are counted in the LOSS
//     chunk, so a reader sees that something was removed rather than meeting a silent gap.
//
// The sanitisation is deliberately toolchain-independent: it needs no compiler flag, no debug
// information and no filesystem access, because it is a rule about the STRING.

#include <cy/core/diagnostics/prelude.h>
#include <cy/core/diagnostics/privacy.h>

namespace cy::diag {

using LocationId = u32;

inline constexpr LocationId kInvalidLocation = 0;

/// The fixed capacity of the location table. Registration allocates nothing; an overflow is counted
/// through `registry_stats().rejected` rather than resized into.
inline constexpr u32 kMaxSourceLocations = 2048;

/// The longest sanitised path an artefact carries. A path longer than this keeps its tail — the
/// part that names the file — rather than its head, which is the part that names the machine.
inline constexpr u32 kMaxSourcePathBytes = 192;

/// Intern a source location. Idempotent for the same (file pointer or text, line).
///
/// `file` must outlive the process: it is a string literal at a call site, or a literal the
/// compiler injected. Nothing is copied here and nothing is copied at emission.
LocationId register_source_location(const char* file, u32 line) noexcept;

struct SourceLocation {
    /// As the compiler or the caller gave it — NOT what an artefact receives. Every consumer that
    /// writes it out goes through `sanitise_source_path()` first.
    const char* file = nullptr;
    u32 line = 0;
    /// What an artefact must be allowed to carry before the path is written. A path names the tree
    /// a build came from, which is developer data; a policy tighter than that removes it.
    Privacy privacy = Privacy::Developer;
};

bool lookup_source_location(LocationId id, SourceLocation& out) noexcept;

/// How many locations are interned. The writer walks 1..count when it writes the META table.
u32 source_location_count() noexcept;

/// What `sanitise_source_path()` had to do, so the caller can count it.
enum class PathForm : u8 {
    /// Empty in, empty out. Nothing was carried and nothing was removed.
    Empty = 0,
    /// The path was already relative, or the declared source root prefixed it and was removed.
    Relative = 1,
    /// The path was absolute and belonged to no known root, so only its final component survives.
    /// This is the case `-fmacro-prefix-map` would have handled and cannot on every toolchain.
    Basename = 2,
};

/// Rewrite `file` into the only form an artefact may carry, into `out`, NUL-terminated. Returns
/// what it had to do and writes the resulting length to `length` when it is not null.
///
/// The rule, in full, and it depends on no compiler and no filesystem:
///
///   1. A path under the source root the build declared loses that prefix.
///   2. A path that is still ABSOLUTE — a leading '/' or '\', or a `X:` drive — keeps only its
///      final component. An absolute path names the machine it was built on, and this is the step
///      that means no artefact carries one whatever compiled the translation unit.
///   3. Anything else is already relative and passes through, truncated from the LEFT if it is
///      longer than `kMaxSourcePathBytes`, because the tail is the half that identifies the file.
PathForm sanitise_source_path(const char* file, char* out, u32 capacity, u32* length) noexcept;

/// The source root this build declared, or "" when it declared none. Exposed so a test can state
/// what it is asserting about rather than hard-coding a path.
const char* source_root() noexcept;

}  // namespace cy::diag

/// Intern THIS site's location, once. The identifier is a `LocationId`, never a `NameId`.
#define CY_SOURCE_LOCATION()                                                              \
    ([]() noexcept -> ::cy::diag::LocationId {                                            \
        static const ::cy::diag::LocationId cy_source_location_ =                         \
            ::cy::diag::register_source_location(__FILE__,                                \
                                                 static_cast<::cy::diag::u32>(__LINE__)); \
        return cy_source_location_;                                                       \
    }())
