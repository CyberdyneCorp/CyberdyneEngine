// The small lookup tables that turn an enumerator into text. None of this is reachable from the
// emission path: a name is resolved when a report is written or a record is presented.

#include <cy/core/diagnostics/log.h>
#include <cy/core/diagnostics/prelude.h>
#include <cy/core/diagnostics/privacy.h>

namespace cy {

const char* privacy_name(Privacy value) noexcept {
    switch (value) {
        case Privacy::Public:
            return "public";
        case Privacy::Developer:
            return "developer";
        case Privacy::PotentiallyPersonal:
            return "potentially-personal";
        case Privacy::Sensitive:
            return "sensitive";
        case Privacy::Secret:
            return "secret";
    }
    return "unknown";
}

}  // namespace cy

namespace cy::diag {

const char* log_level_name(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Trace:
            return "trace";
        case LogLevel::Debug:
            return "debug";
        case LogLevel::Info:
            return "info";
        case LogLevel::Warning:
            return "warning";
        case LogLevel::Error:
            return "error";
        case LogLevel::Fatal:
            return "fatal";
        case LogLevel::Off:
            return "off";
    }
    return "unknown";
}

}  // namespace cy::diag
