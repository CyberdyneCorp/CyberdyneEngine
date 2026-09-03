// Structured logging, emitted onto the trace.
//
// There is no second buffer, no second file and no second clock here: log_emit() composes an
// EventKind::Log record and hands it to the same transport a scope or a counter uses. That is what
// makes "correlating across subsystems" true by construction rather than by a later integration.

#include <cy/core/diagnostics/log.h>

#include <atomic>

namespace cy::diag {
namespace {

/// The per-category floor, indexed by category id. Categories are dense one-based identifiers with
/// a small fixed capacity, so a floor is one relaxed load rather than a lookup.
constexpr u32 kMaxCategoryLevels = 257;

std::atomic<u8> g_global_level{static_cast<u8>(LogLevel::Info)};
std::atomic<u8> g_category_level[kMaxCategoryLevels] = {};

/// Severity chooses the channel, which is what decides what survives buffer pressure. An error
/// outlives a debug line because it was recorded on a channel the loss policy protects.
Channel channel_for(LogLevel level) noexcept {
    switch (level) {
        case LogLevel::Fatal:
        case LogLevel::Error:
            return Channel::Critical;
        case LogLevel::Warning:
        case LogLevel::Info:
            return Channel::Important;
        default:
            return Channel::Verbose;
    }
}

}  // namespace

LogLevel log_level() noexcept {
    return static_cast<LogLevel>(g_global_level.load(std::memory_order_relaxed));
}

void set_log_level(LogLevel level) noexcept {
    g_global_level.store(static_cast<u8>(level), std::memory_order_relaxed);
}

void set_category_level(CategoryId category, LogLevel level) noexcept {
    if (category < kMaxCategoryLevels) {
        g_category_level[category].store(static_cast<u8>(level), std::memory_order_relaxed);
    }
}

bool log_should_emit(CategoryId category, LogLevel level) noexcept {
    const u8 wanted = static_cast<u8>(level);
    if (wanted < g_global_level.load(std::memory_order_relaxed)) {
        return false;
    }
    if (category < kMaxCategoryLevels &&
        wanted < g_category_level[category].load(std::memory_order_relaxed)) {
        return false;
    }
    return true;
}

void log_emit(CategoryId category, LogLevel level, NameId message, NameId site,
              const FieldValue* fields, u32 field_count) noexcept {
    trace_emit(EventKind::Log, channel_for(level), message, category, static_cast<u64>(level), site,
               fields, field_count);
}

}  // namespace cy::diag
