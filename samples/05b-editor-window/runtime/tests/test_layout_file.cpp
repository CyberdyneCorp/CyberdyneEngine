// SPDX-License-Identifier: MIT
// The `--layout` file, and the rewrites it no longer does. M11.c.
//
// The runtime used to rewrite this file on every published layout, and on a busy ext4 disk each
// rename blocked its render thread for a quarter of a second — long enough for the editor to call
// the runtime wedged and for no frame with a gizmo in it to reach the window. `layout_file.h` has
// the measurement. These cases hold the fix: a layout whose geometry is already on disk does not
// touch the disk, and one whose geometry moved does.
//
// The file goes to CY_TEST_ARTEFACT_DIR when the harness names one, else to this test's build
// directory, and never to the caller's working directory — a test that dirties the tree cannot be
// used as evidence.

#include <cy/servers/render/gizmo.h>
#include <cy/test/test.h>

#include "layout_file.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using cy::render::GizmoHandle;
using cy::render::GizmoHandleSpot;
using cy::render::GizmoLayout;
using cy::sample::editor_window::LayoutFile;

namespace {

/// Where this test's file goes, removed first so a previous run cannot answer for this one.
struct Scratch {
    char path[1024] = {};

    explicit Scratch(const char* name) {
        const char* directory = std::getenv("CY_TEST_ARTEFACT_DIR");
        if (directory == nullptr || *directory == '\0') {
            directory = CY_TEST_BINARY_DIR;
        }
        const int written = std::snprintf(path, sizeof(path), "%s/%s", directory, name);
        CY_REQUIRE(written > 0);
        CY_REQUIRE(static_cast<cy::usize>(written) < sizeof(path));
        (void)std::remove(path);
    }

    ~Scratch() { (void)std::remove(path); }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
    Scratch(Scratch&&) = delete;
    Scratch& operator=(Scratch&&) = delete;

    /// The file's one line, or an empty string when it is not there.
    [[nodiscard]] bool read(char (&line)[LayoutFile::kCapacity]) const {
        std::memset(line, 0, sizeof(line));
        std::FILE* file = std::fopen(path, "r");
        if (file == nullptr) {
            return false;
        }
        const bool got = std::fgets(line, sizeof(line), file) != nullptr;
        (void)std::fclose(file);
        return got;
    }
};

[[nodiscard]] GizmoLayout translate_at(cy::u64 frame, cy::f32 centre_x) {
    GizmoLayout layout;
    layout.frame_id = frame;
    layout.mode = cy::render::GizmoMode::Translate;
    layout.centre_x = centre_x;
    layout.centre_y = 270.0F;
    layout.extent = 64.0F;
    GizmoHandleSpot spot;
    spot.handle = GizmoHandle::AxisX;
    spot.x = centre_x + 49.0F;
    spot.y = 296.0F;
    spot.radius = 5.0F;
    CY_REQUIRE(layout.spots.push_back(spot));
    return layout;
}

}  // namespace

CY_TEST_CASE("a layout whose geometry is already on disk does not rewrite the file") {
    const Scratch scratch("editor-window-layout-unchanged.json");
    LayoutFile file;

    CY_CHECK(file.write(scratch.path, translate_at(1, 467.5F)));
    char line[LayoutFile::kCapacity];
    CY_REQUIRE(scratch.read(line));
    CY_CHECK(std::strstr(line, "\"frame\":1,") != nullptr);
    CY_CHECK(std::strstr(line, "\"axis-x\":[516.50,296.00,5.00]") != nullptr);

    // The next frame, the same geometry: the per-frame case, and the one that used to block the
    // render thread. The file must still name the first frame, because it was not touched.
    for (cy::u64 frame = 2; frame < 40; ++frame) {
        CY_CHECK(!file.write(scratch.path, translate_at(frame, 467.5F)));
    }
    CY_REQUIRE(scratch.read(line));
    CY_CHECK(std::strstr(line, "\"frame\":1,") != nullptr);
}

CY_TEST_CASE("a layout whose geometry moved is written, with the frame that moved it") {
    const Scratch scratch("editor-window-layout-moved.json");
    LayoutFile file;

    CY_CHECK(file.write(scratch.path, translate_at(1, 467.5F)));
    CY_CHECK(file.write(scratch.path, translate_at(7, 510.0F)));
    char line[LayoutFile::kCapacity];
    CY_REQUIRE(scratch.read(line));
    CY_CHECK(std::strstr(line, "\"frame\":7,") != nullptr);
    CY_CHECK(std::strstr(line, "\"centre\":[510.00,270.00]") != nullptr);

    // And an empty layout — the selection was cleared — is a change too, so a driver waiting for
    // the handles to go away sees them go.
    GizmoLayout empty;
    empty.frame_id = 9;
    CY_CHECK(file.write(scratch.path, empty));
    CY_REQUIRE(scratch.read(line));
    CY_CHECK(std::strstr(line, "\"handles\":{}") != nullptr);
}

CY_TEST_CASE("no path writes nothing") {
    LayoutFile file;
    CY_CHECK(!file.write(nullptr, translate_at(1, 467.5F)));
    CY_CHECK(!file.write("", translate_at(1, 467.5F)));
}
