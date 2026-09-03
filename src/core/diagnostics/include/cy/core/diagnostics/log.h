#pragma once
// Structured logging, on the trace's timeline rather than beside it.
//
// `diagnostics-profiling-and-crash` — "Structured logging": a category, a severity, a message
// identifier, a source location and typed fields, not only a formatted string. Formatting occurs at
// presentation, so a log nobody reads costs one identifier and its fields. Because the fields are
// typed and classified, a log is searchable, aggregatable and redactable without parsing text.
//
// A log is an EventKind::Log record: `a` is the level, `b` is the source-location identifier. One
// timeline, one clock, one artefact — a streaming stall, a task stall and the log line that
// describes them are read together because they were recorded through one transport.

#include <cy/core/diagnostics/field.h>
#include <cy/core/diagnostics/prelude.h>
#include <cy/core/diagnostics/trace.h>

namespace cy::diag {

/// The global floor. A record below it is not emitted, and its fields are not evaluated.
LogLevel log_level() noexcept;
void set_log_level(LogLevel level) noexcept;

/// A per-category floor, for turning one subsystem up without turning everything up. Categories are
/// stable identifiers and extensible by plugins, so this takes an id rather than a name.
void set_category_level(CategoryId category, LogLevel level) noexcept;
bool log_should_emit(CategoryId category, LogLevel level) noexcept;

void log_emit(CategoryId category, LogLevel level, NameId message, NameId site,
              const FieldValue* fields, u32 field_count) noexcept;

const char* log_level_name(LogLevel level) noexcept;

namespace detail {

/// Collect a call's fields into one array without a heap allocation and without a zero-length array
/// when there are none. The trailing default element is never passed on.
template <class... Fields>
inline void log_dispatch(CategoryId category, LogLevel level, NameId message, NameId site,
                         const Fields&... fields) noexcept {
    const FieldValue values[] = {fields..., FieldValue{}};
    log_emit(category, level, message, site, values, static_cast<u32>(sizeof...(Fields)));
}

template <class... Fields>
inline void instant_dispatch(NameId name, CategoryId category, Channel channel,
                             const Fields&... fields) noexcept {
    const FieldValue values[] = {fields..., FieldValue{}};
    trace_instant(name, category, channel, values, static_cast<u32>(sizeof...(Fields)));
}

}  // namespace detail
}  // namespace cy::diag

#define CY_DIAG_STRINGIFY_(x) #x
#define CY_DIAG_STRINGIFY(x) CY_DIAG_STRINGIFY_(x)

/// Declare a log category. `CY_LOG_CATEGORY(net, "net")` yields `net()`, a stable identifier.
#define CY_LOG_CATEGORY(ident, literal) CY_TRACE_CATEGORY(ident, literal)

/// Emit a structured log record.
///
///   CY_LOG(log::net(), cy::diag::LogLevel::Warning, "peer.rejected",
///          cy::diag::field_u64(field::peer_id(), id));
///
/// The message is an identifier, not a format string: it is registered once and resolved by the
/// viewer from the capture's metadata. Fields carry the values, and each carries a classification.
#define CY_LOG(category, level, message, ...)                                                     \
    do {                                                                                          \
        if (::cy::diag::log_should_emit((category), (level))) {                                   \
            static const ::cy::diag::NameId cy_log_message_ = ::cy::diag::register_name(message); \
            static const ::cy::diag::NameId cy_log_site_ =                                        \
                ::cy::diag::register_name(__FILE__ ":" CY_DIAG_STRINGIFY(__LINE__));              \
            ::cy::diag::detail::log_dispatch((category), (level), cy_log_message_,                \
                                             cy_log_site_ __VA_OPT__(, ) __VA_ARGS__);            \
        }                                                                                         \
    } while (false)

/// Emit an instant with structured fields.
#define CY_TRACE_INSTANT(literal, category, channel, ...)                                      \
    do {                                                                                       \
        static const ::cy::diag::NameId cy_instant_name_ = ::cy::diag::register_name(literal); \
        ::cy::diag::detail::instant_dispatch(cy_instant_name_, (category),                     \
                                             (channel)__VA_OPT__(, ) __VA_ARGS__);             \
    } while (false)
