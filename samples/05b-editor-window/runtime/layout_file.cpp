// SPDX-License-Identifier: MIT
#include "layout_file.h"

#include <cstdio>
#include <cstring>

namespace cy::sample::editor_window {

namespace {

/// A fixed buffer filled by successive `snprintf` calls, which remembers whether any overflowed.
struct Line {
    char text[LayoutFile::kCapacity] = {};
    usize used = 0;
    bool fits = true;

    /// Account for one `snprintf` into `tail()`; its format stays a literal at the call site.
    void took(int wrote) noexcept {
        if (!fits || wrote < 0 || static_cast<usize>(wrote) >= room()) {
            fits = false;
            return;
        }
        used += static_cast<usize>(wrote);
    }
    [[nodiscard]] char* tail() noexcept { return text + used; }
    [[nodiscard]] usize room() const noexcept { return fits ? sizeof(text) - used : 0; }
};

/// Everything in the file except the frame identifier: the part a reader acts on.
[[nodiscard]] bool format_geometry(Line& line, const render::GizmoLayout& layout) noexcept {
    line.took(std::snprintf(
        line.tail(), line.room(), R"("mode":"%s","centre":[%.2f,%.2f],"extent":%.2f,"handles":{)",
        render::gizmo_mode_name(layout.mode), static_cast<double>(layout.centre_x),
        static_cast<double>(layout.centre_y), static_cast<double>(layout.extent)));
    const char* separator = "";
    for (const render::GizmoHandleSpot& spot : layout.spots) {
        line.took(std::snprintf(line.tail(), line.room(), R"(%s"%s":[%.2f,%.2f,%.2f])", separator,
                                render::gizmo_handle_name(spot.handle), static_cast<double>(spot.x),
                                static_cast<double>(spot.y), static_cast<double>(spot.radius)));
        separator = ",";
    }
    line.took(std::snprintf(line.tail(), line.room(), "}"));
    return line.fits;
}

}  // namespace

bool LayoutFile::write(const char* path, const render::GizmoLayout& layout) noexcept {
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    Line geometry;
    if (!format_geometry(geometry, layout)) {
        return false;
    }
    if (written_ && std::strcmp(geometry.text, geometry_) == 0) {
        return false;
    }

    // Written to a temporary and renamed, so a reader never sees half a line: the driver and the
    // runtime are two processes and the file is the only thing between them.
    char temporary[512] = {};
    const int named = std::snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    if (named < 0 || static_cast<usize>(named) >= sizeof(temporary)) {
        return false;
    }
    std::FILE* file = std::fopen(temporary, "w");
    if (file == nullptr) {
        return false;
    }
    const int wrote = std::fprintf(file, "{\"frame\":%llu,%s}\n",
                                   static_cast<unsigned long long>(layout.frame_id), geometry.text);
    const bool closed = std::fclose(file) == 0;
    if (wrote < 0 || !closed || std::rename(temporary, path) != 0) {
        (void)std::remove(temporary);
        return false;
    }
    std::memcpy(geometry_, geometry.text, sizeof(geometry_));
    written_ = true;
    return true;
}

}  // namespace cy::sample::editor_window
