#pragma once
// The two seams `src/core/base/` declares, filled in by diagnostics.
//
// base is the lower half of layer 0 and cannot link this module, so the direction is inverted: base
// declares a function pointer, and diagnostics installs itself into it. Both are installed by
// trace_open() and restored by trace_close(); a host that wants either without a trace installs it
// itself.
//
//   the assertion seam    cy::set_assertion_handler() — `src/core/base/assert.h`, task 3.1.2.
//                         A failed CY_ASSERT leaves a breadcrumb, a Fatal record on the timeline,
//                         and a crash artefact, and then base aborts as it was always going to.
//                         `diagnostics-profiling-and-crash` requires the assertion levels and their
//                         behaviour per configuration to be declared; base's assert.h declares
//                         them, and this decides only what a failure *records*.
//
//   the diagnostic sink   cy::set_diagnostic_sink() — `src/core/base/diagnostic_sink.h`. The
//   warning
//                         a layer above emits — an unsupported window feature ignored rather than
//                         failing — becomes a structured log record on the same timeline instead of
//                         a line on standard error nobody correlates with anything.

#include <cy/core/diagnostics/prelude.h>

namespace cy::diag {

/// Route base's assertion failures through diagnostics. Returns false if a handler was already
/// installed by something else, which is left alone.
bool install_assertion_bridge() noexcept;
void uninstall_assertion_bridge() noexcept;

/// Route base's diagnostic sink into the trace as structured log records.
bool install_diagnostic_bridge() noexcept;
void uninstall_diagnostic_bridge() noexcept;

}  // namespace cy::diag
