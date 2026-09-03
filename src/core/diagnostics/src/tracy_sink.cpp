// Tracy, republishing the engine's trace.
//
// One producer call, one transport. A subsystem emits into the shared trace; this turns the subset
// Tracy models — zones, plots, messages, frame marks — into Tracy's own events. Tracy is a viewer
// for the engine's timeline, never a second timeline a subsystem has to know about, which is the
// rule deps/manifest.toml records for this dependency.
//
// Compiled to nothing when CY_PROFILING is off: cmake/dependencies.cmake creates no cy::dep::tracy
// target then, the header below is not reachable, and tracy_publish() is an inline empty function.

#include "tracy_sink.h"

#ifdef CY_PROFILING

#    include <cy/core/diagnostics/field.h>

#    include <cstring>

#    include <tracy/TracyC.h>

namespace cy::diag {
namespace {

/// Zones nest, so a thread keeps the contexts it opened. Fixed depth: a scope stack deeper than
/// this is a defect in the caller, and dropping the excess is better than allocating in a profiler.
constexpr u32 kMaxZoneDepth = 64;

thread_local TracyCZoneCtx t_zones[kMaxZoneDepth];
thread_local u32 t_depth = 0;

void begin_zone(NameId name) noexcept {
    const char* text = lookup_name(name);
    if (text == nullptr || t_depth >= kMaxZoneDepth) {
        return;
    }
    const u64 source = ___tracy_alloc_srcloc_name(0, "", 0, "", 0, text, std::strlen(text), 0);
    t_zones[t_depth] = ___tracy_emit_zone_begin_alloc(source, 1);
    ++t_depth;
}

void end_zone() noexcept {
    if (t_depth == 0) {
        return;
    }
    --t_depth;
    ___tracy_emit_zone_end(t_zones[t_depth]);
}

}  // namespace

void tracy_publish(EventKind kind, NameId name, CategoryId category, u64 a, u64 b) noexcept {
    (void)category;
    switch (kind) {
        case EventKind::ScopeBegin:
        case EventKind::TaskBegin:
        case EventKind::GpuBegin:
            begin_zone(name);
            break;
        case EventKind::ScopeEnd:
        case EventKind::TaskEnd:
        case EventKind::GpuEnd:
            end_zone();
            break;
        case EventKind::Counter: {
            const char* text = lookup_name(name);
            if (text != nullptr) {
                f64 value = 0.0;
                if (b == 1) {
                    std::memcpy(&value, &a, sizeof(value));
                } else {
                    value = static_cast<f64>(a);
                }
                ___tracy_emit_plot(text, value);
            }
            break;
        }
        case EventKind::FrameEnd:
            ___tracy_emit_frame_mark(nullptr);
            break;
        case EventKind::Log:
        case EventKind::Instant:
        case EventKind::Breadcrumb: {
            const char* text = lookup_name(name);
            if (text != nullptr) {
                ___tracy_emit_message(text, std::strlen(text), 0);
            }
            break;
        }
        default:
            break;
    }
}

}  // namespace cy::diag

#endif  // CY_PROFILING
