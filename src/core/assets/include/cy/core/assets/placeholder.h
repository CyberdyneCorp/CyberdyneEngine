// src/core/assets/ — scaffold placeholder. Delete this file and src/placeholder.cpp when the
// first real header lands here; nothing should ever include it.
//
// It exists so the scaffold is checked rather than assumed: src/placeholder.cpp includes it as
// <cy/core/assets/placeholder.h>, so a wrong PUBLIC_INCLUDE_DIRS is a compile error at the moment
// the stub is created rather than a puzzle for the first agent to add a real header. An empty
// directory would prove nothing, and git would not carry it.

#pragma once

namespace cy {

// Defined in src/placeholder.cpp. A definition rather than nothing at all, because a static archive
// with no symbols draws a warning from some archivers.
void assets_scaffold_placeholder();

}  // namespace cy
