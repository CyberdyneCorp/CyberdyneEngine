#pragma once
// The vocabulary the diagnostics headers are written in, taken from `src/core/base/`.
//
// `base` (tasks 3.1.1 and 3.1.3) owns the engine's fixed-width type aliases, the `Error` model and
// `cy::Expected<T, Error>`; this module uses them rather than declaring a second set. The aliases
// below are re-exported into `cy::diag` only so that the diagnostics sources read the same whether
// they name a type or a function, and each is the same type, not a parallel one.

#include <cy/core/base/error.h>
#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>

namespace cy::diag {

using u8 = ::cy::u8;
using u16 = ::cy::u16;
using u32 = ::cy::u32;
using u64 = ::cy::u64;
using i32 = ::cy::i32;
using i64 = ::cy::i64;
using f32 = ::cy::f32;
using f64 = ::cy::f64;
using usize = ::cy::usize;

using ErrorCode = ::cy::ErrorCode;

// `Error` itself is deliberately not re-exported: `cy::diag::LogLevel::Error` would then shadow it,
// and the engine builds with -Wshadow -Werror. A diagnostics signature spells it `cy::Error`, which
// is also the honest spelling — the error model belongs to base.
template <class T, class E = ::cy::Error>
using Expected = ::cy::Expected<T, E>;

/// `return fail(ErrorCode::Io, "the trace file could not be written");` — base's spelling, brought
/// into this namespace so a diagnostics source does not qualify it at every return.
using ::cy::fail;

}  // namespace cy::diag
