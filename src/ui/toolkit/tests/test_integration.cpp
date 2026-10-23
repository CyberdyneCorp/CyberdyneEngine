// SPDX-License-Identifier: MIT
// `integration.ui_toolkit`: what costs more than a unit case's millisecond — the containers and
// the widget gallery laid out against their goldens, the ten-thousand-item list held to a frame
// budget, and a document reloaded when the file on disk changes.

#include "fixture.h"

#include <cy/core/assets/file.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/memory/ownership.h>
#include <cy/ui/toolkit/document_host.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

using namespace cy;
using namespace cy::ui;
using namespace cy::ui::toolkit_test;

namespace {

[[nodiscard]] std::string data_path(const char* name) {
    return std::string(CY_UI_TOOLKIT_DATA_DIR) + "/" + name;
}

[[nodiscard]] std::string read_text(const std::string& path) {
    std::string text;
    if (FILE* file = std::fopen(path.c_str(), "rb"); file != nullptr) {
        char buffer[4096];
        usize got = 0;
        while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
            text.append(buffer, got);
        }
        (void)std::fclose(file);
    }
    return text;
}

void write_text(const std::string& path, std::string_view text) {
    FILE* file = std::fopen(path.c_str(), "wb");
    CY_REQUIRE(file != nullptr);
    CY_REQUIRE_EQ(std::fwrite(text.data(), 1, text.size(), file), text.size());
    CY_REQUIRE_EQ(std::fclose(file), 0);
}

[[nodiscard]] bool updating_goldens() noexcept {
    const char* value = std::getenv("CY_UI_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

/// Every widget under `widget`, one a line: its kind, its id, and its rect.
void dump(const Toolkit& toolkit, const ElementStore& store, WidgetId widget, u32 depth,
          std::string& out) {
    Array<WidgetId> children(allocator());
    if (!toolkit.children(widget, children)) {
        return;
    }
    for (const WidgetId child : children.span()) {
        const LayoutOutput* output = store.layout_output(child);
        char line[256];
        (void)std::snprintf(line, sizeof(line), "%*s%s #%s %.2f %.2f %.2f %.2f\n",
                            static_cast<int>(depth * 2), "", widget_kind_name(toolkit.kind(child)),
                            toolkit.id_of(child).text().data(),
                            static_cast<double>(output->rect.x),
                            static_cast<double>(output->rect.y),
                            static_cast<double>(output->rect.width),
                            static_cast<double>(output->rect.height));
        out += line;
        dump(toolkit, store, child, depth + 1, out);
    }
}

/// Lay a document out at `scale` and compare every widget's rect with the golden `golden`.
void check_layout(const char* document, const char* golden, f32 dpi_scale) {
    Kit kit;
    CY_REQUIRE(kit.started);
    kit.settings.dpi_scale = dpi_scale;
    kit.viewport = Vec2{960.0F * dpi_scale, 540.0F * dpi_scale};
    FileDocumentResolver files(allocator(), CY_UI_TOOLKIT_DATA_DIR);
    DocumentInstance instance(allocator(), kit.toolkit);
    const std::string text = read_text(data_path(document));
    const Status loaded = instance.load(text, kNoElement, &files, nullptr);
    for (const DocumentDiagnostic& diagnostic : instance.diagnostics()) {
        std::fprintf(stderr, "%s:%u: %s %s\n", document, diagnostic.line, diagnostic.message,
                     diagnostic.detail.text().data());
    }
    CY_REQUIRE(loaded.has_value());
    CY_REQUIRE(kit.frame());
    CY_REQUIRE(kit.frame());
    std::string laid;
    dump(kit.toolkit, kit.store, kNoElement, 0, laid);
    if (updating_goldens()) {
        write_text(data_path(golden), laid);
        std::fprintf(stderr, "wrote %s — read it, then commit it. This run FAILS on purpose.\n",
                     data_path(golden).c_str());
        CY_TEST_FAIL_CHECK("goldens were regenerated; this mode never passes");
        return;
    }
    const std::string expected = read_text(data_path(golden));
    if (laid != expected) {
        std::fprintf(stderr, "%s laid out as:\n%s", document, laid.c_str());
    }
    CY_CHECK(laid == expected);
}

/// The inventory a list asks for rows of.
class Inventory final : public ListSource {
public:
    explicit Inventory(u32 count) noexcept : count_(count) {}
    [[nodiscard]] u32 count() const noexcept override { return count_; }
    [[nodiscard]] Status text(u32 index, Array<char>& out) const noexcept override {
        char text[32] = {};
        const int length = std::snprintf(text, sizeof(text), "Slot %05u", index);
        return out.append(Span<const char>(text, static_cast<usize>(length)));
    }

private:
    u32 count_;
};

/// The worst frame of a list scrolled through `frames` frames, in microseconds, and the rows it
/// realised at most.
struct Scrolled {
    f64 worst_us = 0.0;
    f64 mean_us = 0.0;
    usize rows = 0;
};

[[nodiscard]] Scrolled scroll_through(u32 items, u32 frames) {
    Kit kit;
    Inventory inventory(items);
    WidgetDesc desc;
    desc.kind = WidgetKind::ListView;
    desc.row_height = 20.0F;
    auto list = kit.toolkit.create(kNoElement, desc);
    CY_REQUIRE(list.has_value());
    CY_REQUIRE(kit.toolkit.set_anchors(*list, Vec2{0, 0}, Vec2{0, 0}, Vec2{10, 10}, Vec2{310, 410})
                   .has_value());
    CY_REQUIRE(kit.toolkit.set_list_source(*list, &inventory).has_value());
    CY_REQUIRE(kit.frame());
    PrimitiveBuffer buffer(allocator());
    FlattenReport flattened;
    Scrolled result;
    f64 total = 0.0;
    const f32 step = (static_cast<f32>(items) * 20.0F) / static_cast<f32>(frames);
    for (u32 frame = 0; frame < frames; ++frame) {
        const auto begin = std::chrono::steady_clock::now();
        CY_REQUIRE(kit.toolkit.set_scroll(*list, step * static_cast<f32>(frame)).has_value());
        CY_REQUIRE(kit.frame());
        CY_REQUIRE(flatten(kit.store, ui::Rect{0, 0, 800, 600}, buffer, flattened, &kit.text)
                       .has_value());
        const auto end = std::chrono::steady_clock::now();
        const f64 us = std::chrono::duration<f64, std::micro>(end - begin).count();
        result.worst_us = us > result.worst_us ? us : result.worst_us;
        total += us;
    }
    result.mean_us = total / static_cast<f64>(frames);
    Array<ElementId> rows(allocator());
    CY_REQUIRE(kit.store.children_of(kit.toolkit.part(*list, "list-content"), rows).has_value());
    result.rows = rows.size();
    return result;
}

}  // namespace

CY_TEST_CASE("ui_toolkit_layout: each container lays out as its golden says") {
    check_layout("containers.cyui", "containers_layout.txt", 1.0F);
}

CY_TEST_CASE("ui_toolkit_layout: at twice the DPI the reference layout is the same layout") {
    // Resolution independence: the DPI scale multiplies the output, never the layout, so the
    // reference rects at a DPI scale of two are the rects at one.
    check_layout("containers.cyui", "containers_layout.txt", 2.0F);
}

CY_TEST_CASE("ui_toolkit_layout: the widget gallery lays out as its golden says") {
    check_layout("gallery.cyui", "gallery_layout.txt", 1.0F);
}

CY_TEST_CASE("ui_toolkit_list: ten thousand items scroll at the cost of a hundred, inside a frame budget") {
    // `ui-system`: "WHEN a list view displays 100 000 items THEN only visible rows SHALL be realised,
    // and scrolling SHALL cost the same as for 100 items." Every frame scrolls to a new place: the
    // rows are given new items, laid out, styled and flattened.
    const Scrolled few = scroll_through(100, 60);
    const Scrolled many = scroll_through(10000, 60);
    std::fprintf(stderr,
                 "list of 100: mean %.1f us, worst %.1f us, %zu rows; of 10000: mean %.1f us, "
                 "worst %.1f us, %zu rows\n",
                 few.mean_us, few.worst_us, few.rows, many.mean_us, many.worst_us, many.rows);
    // THE STRUCTURAL CLAIM, in every configuration: the same rows exist for either.
    CY_CHECK_EQ(many.rows, few.rows);
    // A 400-unit window of 20-unit rows: twenty, one more partly in view, two either side.
    CY_CHECK_LE(many.rows, 25U);
    // THE BUDGET: a frame of a 60 Hz game is 16.7 ms and the interface's share of it a tenth. Mean,
    // because one frame of a shared machine is a draw; the worst frame is reported above.
#if !defined(CY_UNOPTIMISED)
    CY_CHECK_LT(many.mean_us, 1667.0 * test::kSanitizedAllowance);
#endif
}

CY_TEST_CASE("ui_toolkit_document: a document on disk reloads when it is saved, keeping its state") {
    const std::string directory = "cy_ui_toolkit_hot_reload";
    (void)assets::fs::remove_directory_recursive(directory.c_str());
    CY_REQUIRE(assets::fs::create_directories(directory.c_str()).has_value());
    write_text(directory + "/menu.cyui", R"(cyui 1
column #menu
  label #title text="Settings"
  slider #volume min=0 max=10 step=1 value=5
)");
    assets::VirtualFileSystem files;
    auto mount = assets::DirectoryMount::create(directory.c_str(), assets::MountKind::Project, false);
    CY_REQUIRE(mount.has_value());
    CY_REQUIRE(files.mount_owned(std::move(*mount), 0).has_value());

    Kit kit;
    CY_REQUIRE(kit.started);
    constexpr i64 kSettle = 100'000'000;
    DocumentHost host(allocator(), kit.toolkit, files, kSettle);
    CY_REQUIRE(host.open("menu.cyui", kNoElement, nullptr, 0).has_value());
    CY_REQUIRE(kit.frame());
    const WidgetId volume = host.instance().find("volume");
    CY_REQUIRE(kit.toolkit.focus(volume).has_value());
    CY_CHECK(kit.toolkit.action(UiAction::NavigateRight));  // the player's 6
    Expected<bool, Error> quiet = host.poll(kSettle);
    CY_REQUIRE(quiet.has_value());
    CY_CHECK_FALSE(*quiet);

    // The designer saves a new title. The first poll sees it change; it reloads once settled.
    write_text(directory + "/menu.cyui", R"(cyui 1
column #menu
  label #title text="Options"
  slider #volume min=0 max=10 step=1 value=5
)");
    Expected<bool, Error> seen = host.poll(2 * kSettle);
    CY_REQUIRE(seen.has_value());
    CY_CHECK_FALSE(*seen);
    Expected<bool, Error> settled = host.poll(4 * kSettle);
    CY_REQUIRE(settled.has_value());
    CY_CHECK(*settled);
    CY_CHECK_EQ(host.reloads(), 1U);
    CY_CHECK_EQ(kit.toolkit.text(host.instance().find("title")), std::string_view("Options"));
    CY_CHECK_EQ(host.instance().find("volume"), volume);
    CY_CHECK_EQ(kit.toolkit.value(volume), 6.0F);
    CY_CHECK_EQ(kit.toolkit.focused(), volume);

    // A save that does not parse keeps the interface and says why; the next good save reloads.
    write_text(directory + "/menu.cyui", "cyui 1\ncolumn #menu\n   label\n");
    (void)host.poll(5 * kSettle);
    Expected<bool, Error> broken = host.poll(7 * kSettle);
    CY_REQUIRE(broken.has_value());
    CY_CHECK_FALSE(*broken);
    CY_CHECK_EQ(host.failed_reloads(), 1U);
    CY_CHECK_FALSE(host.instance().diagnostics().empty());
    CY_CHECK_EQ(kit.toolkit.text(host.instance().find("title")), std::string_view("Options"));
    (void)assets::fs::remove_directory_recursive(directory.c_str());
}
