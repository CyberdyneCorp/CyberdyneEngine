// SPDX-License-Identifier: MIT
#pragma once
// The gizmo layout this runtime published, written where the window artefact's driver reads it.
//
// ================================================================================================
// WHY THE FILE IS REWRITTEN ONLY WHEN THE GEOMETRY CHANGES
// ================================================================================================
//
// The editor asks for the gizmo once a frame, so the layout is published once a frame, and until
// M11.c the file was rewritten on every one of them: open a temporary, write it, rename it over the
// last. Renaming over an existing file makes ext4 flush the new file's data before the rename
// commits (`auto_da_alloc`), and on a disk that other builds are saturating that took 0.14 to 0.55
// seconds — ON THE RENDER THREAD. Measured under strace with the disk at 60 % full I/O pressure:
// every one of 54 renames blocked for about a quarter of a second, the heartbeat stopped, the
// editor declared the runtime wedged, and no frame with a gizmo in it ever reached the window. The
// window artefact then reported that the drag found no handle.
//
// Nothing the driver reads changes between most of those writes. The handles, the centre, the mode
// and the extent change when the object or the camera moves; the frame identifier changes every
// frame and is only ever printed. So the geometry is compared with what is already on disk and the
// file is left alone when it matches. The frame in the file is then the first frame that had this
// geometry, which is still a frame that had it.

#include <cy/core/base/types.h>
#include <cy/servers/render/gizmo.h>

namespace cy::sample::editor_window {

/// The file `--layout` names, and what was last written to it.
class LayoutFile {
public:
    /// The largest layout this writes, in bytes. Seven handles of a translate gizmo need about 400.
    static constexpr usize kCapacity = 2048;

    /// Write `layout` to `path` unless the geometry already there is the same.
    ///
    /// Returns whether the file was written. A null or empty path writes nothing and answers false;
    /// so does a layout too large to format, which is then left for the next frame rather than
    /// written truncated.
    bool write(const char* path, const render::GizmoLayout& layout) noexcept;

private:
    char geometry_[kCapacity] = {};
    bool written_ = false;
};

}  // namespace cy::sample::editor_window
