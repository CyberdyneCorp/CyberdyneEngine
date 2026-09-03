#pragma once
// Tracy as a backend of the engine's trace, not a second timeline beside it.
//
// deps/manifest.toml states the rule for this dependency: "a backend of the engine's own trace in
// src/core/diagnostics/, never a second timeline beside it". A producer emits once, into the shared
// transport; this republishes what Tracy understands. With CY_PROFILING off there is no Tracy
// target to link (cmake/dependencies.cmake does not create one), the call below compiles to
// nothing, and the trace is complete without it.

// The generated feature header is where CY_PROFILING is defined; a file that tests the option
// without including it sees the option as permanently off, which is the silent failure this include
// exists to prevent. It is a private header of this module, so nothing outside inherits it.
#include <cy_features.h>

#include <cy/core/diagnostics/prelude.h>
#include <cy/core/diagnostics/trace.h>

namespace cy::diag {

#ifdef CY_PROFILING
void tracy_publish(EventKind kind, NameId name, CategoryId category, u64 a, u64 b) noexcept;
#else
inline void tracy_publish(EventKind, NameId, CategoryId, u64, u64) noexcept {}
#endif

}  // namespace cy::diag
