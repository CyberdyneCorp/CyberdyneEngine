#pragma once
// Engine-owned decoder for the editor's short-lived material canvas interchange.

#include <cy/core/base/expected.h>
#include <cy/graph/cybergraph.h>

#include <string_view>

namespace cy::graph::material {

inline constexpr u32 kCanvasVersion = 1;

struct AuthoredCanvas {
    Name name;
    u32 nodes = 0;
    u32 links = 0;
};

/// Decode the editor interchange into the engine graph model. The canonical `.cygraph` writer
/// remains the only persisted graph encoder.
[[nodiscard]] Expected<AuthoredCanvas, Error> read_canvas(std::string_view text,
                                                          Graph& out) noexcept;

}  // namespace cy::graph::material
