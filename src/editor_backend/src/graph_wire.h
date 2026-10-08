// SPDX-License-Identifier: MIT
#pragma once
// What every graph-authoring service writes the same way: a node type's stable identity in the
// material catalogue's schema 3, and a compile's diagnostics. Private to src/editor_backend; the
// `script.*` and `animation.*` operations share it so one Rust decoder reads both.

#include <cy/graph/cybergraph.h>

#include <string_view>

#include "service_wire.h"

namespace cy::editor::wire {

/// A stable node-type identity: FNV-1a over the name, never zero.
[[nodiscard]] inline u32 type_identity(std::string_view name) noexcept {
    u32 hash = 2166136261U;
    for (const char character : name) {
        hash ^= static_cast<u8>(character);
        hash *= 16777619U;
    }
    return hash == 0 ? 1U : hash;
}

/// A diagnostic's severity on the wire: 0 info, 1 warning, 2 error.
[[nodiscard]] inline u8 severity_of(graph::Severity severity) noexcept {
    switch (severity) {
        case graph::Severity::Info:
            return 0;
        case graph::Severity::Warning:
            return 1;
        case graph::Severity::Error:
            return 2;
    }
    return 2;
}

/// `u32 count` and per diagnostic (u8 severity, text code, u64 node, text pin, text message, text
/// detail, u64 related node).
inline void encode_diagnostics(Writer& out, const graph::DiagnosticSink& sink) noexcept {
    out.u32v(static_cast<u32>(sink.entries().size()));
    for (const graph::Diagnostic& diagnostic : sink.entries()) {
        out.u8v(severity_of(diagnostic.severity)).text(diagnostic.code).u64v(diagnostic.node);
        out.text(diagnostic.pin.text()).text(diagnostic.message).text(diagnostic.detail.text());
        out.u64v(diagnostic.related_node);
    }
}

}  // namespace cy::editor::wire
