// SPDX-License-Identifier: MIT
// The runtime interface on a Vulkan device, with validation and synchronisation validation on.
// `render.ui`.
//
// ================================================================================================
// THE FRAME
// ================================================================================================
//
// `pipeline_test::FrameScene` — the one scene the pipeline suites render — with an interface drawn
// over it at the frame's `UiAndDebug` stage by `ui::render::UiRenderer`. The interface is built
// with CyberUI's own store, laid out by `ui::layout`, its text through `TextPainter` on the
// built-in font, and flattened by `ui::flatten` — the path a game takes.
//
// ================================================================================================
// SEVEN PROPERTIES, EACH A CASE
// ================================================================================================
//
//   (a) with no interface attached, and with one attached that draws nothing, the frame is the
//       frame from before the interface pass existed — a committed reference — byte for byte;
//   (b) the device draws the stream as `draw_reference`, the shader on the host, does: every pixel
//       within one 8-bit step, over the frame it was drawn on;
//   (c) order: a later sibling covers an earlier one, and a label's glyphs cover its panel, in the
//       colours asked for, byte for byte;
//   (d) clipping: a scroll view inside a scroll view draws exactly the intersection of the two, and
//       every pixel outside it is the frame's own;
//   (e) opacity: a half-transparent panel blends premultiplied over what is beneath, an invisible
//       one draws nothing, and there is one indirect draw per batch `flatten()` made;
//   (f) a strategy game's HUD and the developer console over the scene, against a committed golden
//       image — the picture docs/design/images/ shows;
//   (g) on an `Rgba8Srgb` output an opaque colour lands as the same bytes a UNORM output holds, and
//       a translucent one blends in linear light — the `kUiOutputLinear` path, alone on a target.

#include "frame_scene.h"
#include "golden.h"
#include "hud.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/command_buffer.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/executor.h>
#include <cy/test/test.h>
#include <cy/ui/console/console.h>
#include <cy/ui/layout.h>
#include <cy/ui/render/text_atlas.h>
#include <cy/ui/render/ui_renderer.h>
#include <cy/ui/text/builtin_font.h>
#include <cy/ui/text/interface_font.h>
#include <cy_features.h>

#if defined(CY_TEXT)
#    include <cy/backends/text/complete_backend.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace cy;
using cy::pipeline_test::FrameScene;
using cy::pipeline_test::FrameSceneHooks;
using cy::pipeline_test::kHeight;
using cy::pipeline_test::kWidth;
using cy::pipeline_test::RecordMode;
using namespace cy::ui;
using namespace cy::ui::render;

namespace {

constexpr usize kPixels = static_cast<usize>(kWidth) * kHeight;
constexpr u16 kGlyphPage = 1;
constexpr u16 kImagePage = 2;
/// The interface font's three pages — coverage, distance field, colour — clear of the two above.
constexpr u16 kTextPages = 4;
constexpr Vec2 kViewport{static_cast<f32>(kWidth), static_cast<f32>(kHeight)};

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
    }
    std::fprintf(stderr, "graphics validation %s: %s\n",
                 severity == rhi::ValidationSeverity::Error ? "error" : "warning",
                 message != nullptr ? message : "");
}

class DeviceFixture {
public:
    DeviceFixture() noexcept : allocator_(system_allocator(MemoryDomain::Gpu)) {
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_test_render_ui";
        description.enable_validation = true;
        description.enable_synchronisation_validation = true;
        device_ = rhi::create_device(allocator_, "vulkan", description, selection_);
        if (device_.has_value()) {
            device_.value()->set_validation_callback(&count_validation, &errors_);
        }
    }
    ~DeviceFixture() {
        if (device_.has_value()) {
            (void)device_.value()->wait_idle();
            rhi::destroy_device(allocator_, device_.value());
        }
    }
    DeviceFixture(const DeviceFixture&) = delete;
    DeviceFixture& operator=(const DeviceFixture&) = delete;

    [[nodiscard]] bool has_gpu() const noexcept {
        return device_.has_value() &&
               device_.value()->capabilities().backend() == rhi::BackendKind::Vulkan;
    }
    [[nodiscard]] rhi::Device& device() const noexcept { return *device_.value(); }
    [[nodiscard]] u32 validation_errors() const noexcept { return errors_; }
    void report_skip() const noexcept {
        std::fprintf(stderr,
                     "no requested graphics device on this machine; the backend selected was '%s' "
                     "because %s\n",
                     selection_.selected != nullptr ? selection_.selected : "(none)",
                     selection_.reason != nullptr ? selection_.reason : "(no reason given)");
    }

private:
    Allocator& allocator_;
    rhi::BackendSelection selection_{};
    u32 errors_ = 0;
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
};

const char* artefact_directory() noexcept {
    const char* directory = std::getenv("CY_TEST_ARTEFACT_DIR");
    return (directory == nullptr || *directory == '\0') ? CY_TEST_BINARY_DIR : directory;
}

void save(const char* name, const std::vector<u32>& texels) noexcept {
    render_test::Image image(allocator());
    if (!render_test::adopt(image, Span<const u32>(texels.data(), texels.size()), kWidth, kHeight)
             .has_value()) {
        return;
    }
    char path[1024];
    (void)std::snprintf(path, sizeof(path), "%s/%s", artefact_directory(), name);
    if (render_test::write_png(path, image).has_value()) {
        std::fprintf(stderr, "wrote %s (%ux%u)\n", path, kWidth, kHeight);
    }
}

const char* reference_path(const char* name) noexcept {
    static char storage[1024];
    (void)std::snprintf(storage, sizeof(storage), "%s/references/%s", CY_UI_RENDER_TEST_DIR, name);
    return storage;
}

bool updating_references() noexcept {
    const char* value = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

/// Compare `texels` with the committed reference `name`, byte for byte, or rewrite it under
/// `CY_RENDER_UPDATE_GOLDEN` and fail on purpose.
void check_against_reference(const char* name, const std::vector<u32>& texels) {
    render_test::Image rendered(allocator());
    CY_REQUIRE(
        render_test::adopt(rendered, Span<const u32>(texels.data(), texels.size()), kWidth, kHeight)
            .has_value());
    if (updating_references()) {
        CY_CHECK(render_test::write_png(reference_path(name), rendered).has_value());
        std::fprintf(stderr, "wrote %s — look at it, then commit it. This run FAILS on purpose.\n",
                     reference_path(name));
        CY_TEST_FAIL_CHECK("references were regenerated; this mode never passes");
        return;
    }
    render_test::Image reference(allocator());
    const Status read = render_test::read_png(reference_path(name), reference);
    if (!read) {
        std::fprintf(stderr, "%s: %s\n", reference_path(name), read.error().message);
    }
    CY_REQUIRE(read.has_value());
    const render_test::Comparison comparison = render_test::compare(reference, rendered);
    std::fprintf(stderr, "%s: %u texels differ, the largest by %u at (%u, %u)\n", name,
                 comparison.differing, comparison.max_channel_delta, comparison.worst_x,
                 comparison.worst_y);
    CY_CHECK(comparison.comparable);
    CY_CHECK_EQ(comparison.differing, 0U);
    CY_CHECK_EQ(comparison.max_channel_delta, 0U);
}

[[nodiscard]] u32 channel_delta(u32 a, u32 b) noexcept {
    u32 worst = 0;
    for (u32 shift = 0; shift < 32U; shift += 8U) {
        const u32 x = (a >> shift) & 0xFFU;
        const u32 y = (b >> shift) & 0xFFU;
        worst = std::max(worst, x > y ? x - y : y - x);
    }
    return worst;
}

/// RGBA8 with red in the low byte, from the interface's premultiplied 0xAARRGGBB: how an opaque
/// colour lands in the `Rgba8Unorm` output.
[[nodiscard]] constexpr u32 texel_of(u32 argb) noexcept {
    return ((argb >> 16U) & 0xFFU) | (argb & 0xFF00U) | ((argb & 0xFFU) << 16U) |
           (argb & 0xFF000000U);
}

[[nodiscard]] usize at(u32 x, u32 y) noexcept {
    return (static_cast<usize>(y) * kWidth) + x;
}

/// The 4x4 premultiplied checker the image case draws, red in the low byte of each texel.
[[nodiscard]] std::vector<u8> checker_page() {
    std::vector<u8> texels(64);
    for (u32 index = 0; index < 16U; ++index) {
        const bool light = (((index % 4U) + (index / 4U)) % 2U) == 0U;
        texels[(index * 4U) + 0U] = light ? 230U : 40U;
        texels[(index * 4U) + 1U] = light ? 230U : 40U;
        texels[(index * 4U) + 2U] = light ? 120U : 200U;
        texels[(index * 4U) + 3U] = 255U;
    }
    return texels;
}

// --- An interface over the scene -------------------------------------------------------------

/// Everything one rendered frame of `render.ui` owns: the scene, the interface's store, text,
/// consumers and renderer, in the order a game creates them.
class UiRun {
public:
    using Build = bool (*)(UiRun& run) noexcept;

    UiRun(DeviceFixture& fixture, bool attached, Build build, bool interface_font = false) noexcept
        : fixture_(fixture),
          scene_(allocator()),
          attached_(attached),
          interface_font_(interface_font),
          build_(build) {}

    ~UiRun() {
        (void)fixture_.device().wait_idle();
        renderer.destroy();
        scene_.release();
    }

    UiRun(const UiRun&) = delete;
    UiRun& operator=(const UiRun&) = delete;
    UiRun(UiRun&&) = delete;
    UiRun& operator=(UiRun&&) = delete;

    [[nodiscard]] bool render() noexcept {
        FrameSceneHooks hooks;
        hooks.user = this;
        hooks.before_assemble = &before_assemble;
        scene_.set_hooks(hooks);
        if (!scene_.build(fixture_.device()).has_value()) {
            return false;
        }
        scene_.set_read_back(true);
        if (!start_interface() || (build_ != nullptr && !build_(*this)) || !draw_interface()) {
            return false;
        }
        rendering::assembly::AssemblyReport report{};
        if (!scene_.render(RecordMode::Callbacks, report).has_value()) {
            return false;
        }
        if (!fixture_.device().wait_idle().has_value()) {
            return false;
        }
        pixels_.assign(scene_.pixels().begin(), scene_.pixels().end());
        return pixels_.size() == kPixels;
    }

    [[nodiscard]] const std::vector<u32>& pixels() const noexcept { return pixels_; }

    /// An element at `x, y` in its parent's content box, `w` by `h`, filled with `colour`.
    [[nodiscard]] ElementId box(ElementId parent, f32 x, f32 y, f32 w, f32 h, u32 colour) noexcept {
        auto made = store.create(parent, Name::intern("box"));
        if (!made.has_value()) {
            return kNoElement;
        }
        LayoutInput* input = store.layout_input(*made);
        input->model = LayoutModel::Absolute;
        input->offset_min = Vec2{x, y};
        input->offset_max = Vec2{x + w, y + h};
        store.paint(*made)->background = colour;
        return *made;
    }

    ElementStore store{allocator()};
#if defined(CY_TEXT)
    // Before the server: it closes its faces through the backend on the way out.
    cy::text::CompleteTextBackend backend;
#endif
    cy::text::TextServer server;
    TextPainter text{allocator()};
    UiRenderer renderer{allocator()};
    sample::rts::Hud hud;
    DevConsole console{allocator()};
    PrimitiveBuffer buffer{allocator()};
    FlattenReport flattened{};
    ElementId root;

private:
    [[nodiscard]] bool start_interface() noexcept {
        UiRendererDescription description;
        description.width = kWidth;
        description.height = kHeight;
        // The format the post chain ended in: the tonemapped output's.
        description.output_format = scene_.pipelines().setup().output_format;
        if (!renderer.create(fixture_.device(), description).has_value() || !start_text()) {
            return false;
        }
        const u32 extent = text.atlas_extent();
        if (!interface_font_ && !renderer
                                     .upload_atlas(kGlyphPage, rhi::Format::R8Unorm, extent, extent,
                                                   text.atlas_pixels())
                                     .has_value()) {
            return false;
        }
        auto made = store.create(kNoElement, Name::intern("screen"));
        if (!made.has_value()) {
            return false;
        }
        root = *made;
        store.layout_input(root)->model = LayoutModel::Absolute;
        return true;
    }

    /// The built-in font, as every case before issue #86 draws; or the interface font through the
    /// complete text backend, whose pages are uploaded once the document is built.
    [[nodiscard]] bool start_text() noexcept {
        if (!interface_font_) {
            return server.start(cy::text::TextServerConfig{}).has_value() &&
                   text.start(server, builtin_font(), kGlyphPage).has_value();
        }
#if defined(CY_TEXT)
        cy::text::TextServerConfig config;
        config.atlas.initial_extent = 512;
        return backend.start().has_value() && server.start_with(config, backend).has_value() &&
               text.start(server, cy::text::FontSource{interface_font_bytes(), 0},
                          interface_font_desc(), kTextPages)
                   .has_value();
#else
        return false;
#endif
    }

    [[nodiscard]] bool draw_interface() noexcept {
        if (interface_font_ && !upload_text_atlases(renderer, text).has_value()) {
            return false;
        }
        ScaleSettings settings;
        settings.mode = ScaleMode::FixedPixel;
        LayoutReport laid{};
        if (!layout(store, settings, kViewport, &text, laid).has_value()) {
            return false;
        }
        if (!flatten(store, ui::Rect{0.0F, 0.0F, kViewport.x, kViewport.y}, buffer, flattened,
                     &text)
                 .has_value()) {
            return false;
        }
        return renderer.submit(buffer, 1.0F).has_value();
    }

    static Status before_assemble(rendering::RenderGraph& /*graph*/,
                                  rendering::assembly::AssemblyView& /*view*/,
                                  rendering::assembly::FrameSinks& sinks, void* user) noexcept {
        auto* run = static_cast<UiRun*>(user);
        if (run->attached_) {
            sinks.ui = run->renderer.stage();
        }
        return ok();
    }

    DeviceFixture& fixture_;
    FrameScene scene_;
    bool attached_ = false;
    bool interface_font_ = false;
    Build build_ = nullptr;
    std::vector<u32> pixels_;
};

/// The frame with no interface at all: what every case compares against.
[[nodiscard]] bool render_bare(DeviceFixture& fixture, std::vector<u32>& out) {
    UiRun bare(fixture, false, nullptr);
    if (!bare.render()) {
        return false;
    }
    out = bare.pixels();
    return true;
}

// --- The documents ------------------------------------------------------------------------------

constexpr u32 kRed = 0xFFE0302AU;
constexpr u32 kBlue = 0xFF2A5CE0U;
constexpr u32 kGreen = 0xFF34C25AU;
constexpr u32 kGrey = 0xFF404040U;
constexpr u32 kYellow = 0xFFFFD040U;
constexpr u32 kPanel = 0xFF182028U;

/// Every behaviour on one screen: order, a glyph run, nested scroll views, opacity, a rounded and
/// bordered panel, and an image.
bool build_everything(UiRun& run) noexcept {
    ElementStore& store = run.store;
    // (c) Order: blue over red where they overlap.
    (void)run.box(run.root, 20.0F, 30.0F, 60.0F, 40.0F, kRed);
    (void)run.box(run.root, 50.0F, 50.0F, 60.0F, 40.0F, kBlue);

    // (d) A scroll view at (140, 30) holding a scroll view at (180, 60), each 100 by 80, holding a
    // panel larger than both: (120, 10) to (340, 190) on screen.
    const ElementId outer = run.box(run.root, 140.0F, 30.0F, 100.0F, 80.0F, 0);
    const ElementId inner = run.box(outer, 40.0F, 30.0F, 100.0F, 80.0F, 0);
    (void)run.box(inner, -60.0F, -50.0F, 220.0F, 180.0F, kGreen);
    for (const ElementId view : {outer, inner}) {
        if (!store.set_flags(view, ElementFlags::Visible | ElementFlags::ClipsChildren)
                 .has_value()) {
            return false;
        }
    }

    // (e) Half white over grey, and an invisible panel beside it.
    (void)run.box(run.root, 300.0F, 20.0F, 80.0F, 60.0F, kGrey);
    const ElementId glass = run.box(run.root, 310.0F, 30.0F, 60.0F, 40.0F, 0xFFFFFFFFU);
    store.paint(glass)->opacity = 0.5F;
    const ElementId ghost = run.box(run.root, 390.0F, 20.0F, 60.0F, 60.0F, kRed);
    store.paint(ghost)->opacity = 0.0F;

    // A rounded, bordered panel.
    const ElementId rounded = run.box(run.root, 20.0F, 110.0F, 110.0F, 60.0F, kPanel);
    store.paint(rounded)->corner_radius = 10.0F;
    store.paint(rounded)->border_width = 2.0F;
    store.paint(rounded)->border_colour = kYellow;

    // (c) A label: its glyphs over its own panel.
    const ElementId label = run.box(run.root, 20.0F, 190.0F, 200.0F, 30.0F, kPanel);
    TextStyle style;
    style.colour = kYellow;
    style.pixel_scale = 2;
    if (!run.text.set_text(label, "CyberUI 1250", style).has_value()) {
        return false;
    }

    // An image: a 4x4 premultiplied checker on its own page, drawn at 64 by 64 with rounded
    // corners.
    const std::vector<u8> checker = checker_page();
    if (!run.renderer
             .upload_atlas(kImagePage, rhi::Format::Rgba8Unorm, 4, 4,
                           Span<const u8>(checker.data(), checker.size()))
             .has_value()) {
        return false;
    }
    const ElementId image = run.box(run.root, 250.0F, 140.0F, 64.0F, 64.0F, 0xFFFFFFFFU);
    PaintData* paint = store.paint(image);
    paint->material = material_index(BuiltinMaterial::Image);
    paint->atlas = kImagePage;
    paint->uv = ui::Rect{0.0F, 0.0F, 1.0F, 1.0F};
    paint->corner_radius = 8.0F;
    return true;
}

/// What a strategy game draws: the HUD and the developer console.
bool build_hud(UiRun& run) noexcept {
    sample::rts::Hud& hud = run.hud;
    if (!hud.create(run.store, run.text, run.root).has_value() ||
        !hud.set_resources(sample::rts::Resources{1250, 830, 42, 60}).has_value()) {
        return false;
    }
    const sample::rts::SelectedUnit units[] = {
        {"Rifleman", 100, 100}, {"Rifleman", 64, 100}, {"Engineer", 30, 80}};
    if (!hud.set_selection(Span<const sample::rts::SelectedUnit>(units, 3)).has_value()) {
        return false;
    }
    const sample::rts::MinimapDot dots[] = {
        {0.30F, 0.62F, sample::rts::Team::Player}, {0.34F, 0.66F, sample::rts::Team::Player},
        {0.27F, 0.70F, sample::rts::Team::Player}, {0.62F, 0.58F, sample::rts::Team::Enemy},
        {0.70F, 0.52F, sample::rts::Team::Enemy},  {0.15F, 0.25F, sample::rts::Team::Neutral}};
    // The camera looks past the map's left edge: its frame is clipped by the minimap.
    if (!hud.set_minimap(Span<const sample::rts::MinimapDot>(dots, 6),
                         ui::Rect{-0.1F, 0.45F, 0.5F, 0.4F})
             .has_value()) {
        return false;
    }

    ConsoleStyle style;
    style.anchor_min = Vec2{0.0F, 0.0F};
    style.anchor_max = Vec2{0.0F, 0.0F};
    style.offset_min = Vec2{6.0F, 22.0F};
    style.offset_max = Vec2{300.0F, 22.0F + (4.0F * 13.0F) + 10.0F};
    style.visible_rows = 3;
    DevConsole& console = run.console;
    return console.create(run.store, run.text, run.root, style).has_value() &&
           console.print("CyberUI console - type help", kConsoleInfo).has_value() &&
           console.print("squad selected: 3 units", kConsoleEcho).has_value() &&
           console.print("warning: food 42/60", kConsoleWarning).has_value() &&
           console.type("spawn tank").has_value();
}

}  // namespace

CY_TEST_CASE(
    "(a) with nothing to draw the frame is the frame before the interface, byte for byte") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    std::vector<u32> bare;
    CY_REQUIRE(render_bare(fixture, bare));
    save("ui-absent.png", bare);
    check_against_reference("ui_absent.png", bare);

    // Attached, with a store that flattens to nothing visible: the pass is declared and records
    // nothing, and not one byte moves.
    UiRun empty(fixture, true, nullptr);
    CY_REQUIRE(empty.render());
    CY_CHECK_FALSE(empty.renderer.report().recorded);
    usize moved = 0;
    for (usize index = 0; index < kPixels; ++index) {
        moved += static_cast<usize>(empty.pixels()[index] != bare[index]);
    }
    CY_CHECK_EQ(moved, 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(b) the device draws the stream as the shader on the host does") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    std::vector<u32> bare;
    CY_REQUIRE(render_bare(fixture, bare));
    UiRun run(fixture, true, &build_everything);
    CY_REQUIRE(run.render());
    save("ui-everything.png", run.pixels());
    CY_REQUIRE(run.renderer.report().recorded);

    // The host reference over the frame the interface was drawn on, with the same pages.
    std::vector<u32> expected = bare;
    const std::vector<u8> checker = checker_page();
    const ReferenceAtlas pages[3] = {
        ReferenceAtlas{},
        ReferenceAtlas{run.text.atlas_pixels(), run.text.atlas_extent(), run.text.atlas_extent(),
                       1},
        ReferenceAtlas{Span<const u8>(checker.data(), checker.size()), 4, 4, 4},
    };
    CY_REQUIRE(draw_reference(run.renderer.draws(), Span<const ReferenceAtlas>(pages, 3), kWidth,
                              kHeight, Span<u32>(expected.data(), expected.size()))
                   .has_value());

    u32 worst = 0;
    usize touched = 0;
    for (usize index = 0; index < kPixels; ++index) {
        worst = std::max(worst, channel_delta(run.pixels()[index], expected[index]));
        touched += static_cast<usize>(run.pixels()[index] != bare[index]);
    }
    std::fprintf(stderr, "(b) %zu pixels drawn; the largest difference from the host is %u\n",
                 touched, worst);
    // One step: the blend unit and the host may round a blended or partly covered value to either
    // side of a quantisation boundary. Two would be a different picture.
    CY_CHECK_LE(worst, 1U);
    CY_CHECK_GT(touched, 10000U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(c) a later sibling covers an earlier one and glyphs cover their panel") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    UiRun run(fixture, true, &build_everything);
    CY_REQUIRE(run.render());
    const std::vector<u32>& pixels = run.pixels();
    CY_CHECK_EQ(pixels[at(25, 35)], texel_of(kRed));
    CY_CHECK_EQ(pixels[at(55, 55)], texel_of(kBlue));
    CY_CHECK_EQ(pixels[at(105, 85)], texel_of(kBlue));
    // The label: its panel where no glyph lit, the glyph colour where one did. 'C' at twice the
    // font's size starts at (20, 190); its left stroke is cell column 0, rows 4 to 8.
    CY_CHECK_EQ(pixels[at(21, 191)], texel_of(kPanel));
    CY_CHECK_EQ(pixels[at(21, 200)], texel_of(kYellow));
    usize lit = 0;
    usize other = 0;
    for (u32 y = 190; y < 220U; ++y) {
        for (u32 x = 20; x < 220U; ++x) {
            const u32 texel = pixels[at(x, y)];
            lit += static_cast<usize>(texel == texel_of(kYellow));
            other += static_cast<usize>(texel != texel_of(kYellow) && texel != texel_of(kPanel));
        }
    }
    // A bitmap font at a whole-number scale, point-sampled: every pixel is glyph or panel.
    CY_CHECK_GT(lit, 300U);
    CY_CHECK_EQ(other, 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(d) a scroll view inside a scroll view draws exactly the intersection") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    std::vector<u32> bare;
    CY_REQUIRE(render_bare(fixture, bare));
    UiRun run(fixture, true, &build_everything);
    CY_REQUIRE(run.render());
    const std::vector<u32>& pixels = run.pixels();
    // The green panel spans (120, 10) to (340, 190); the outer view (140, 30) to (240, 110), the
    // inner (180, 60) to (280, 140). Green exactly on the intersection, and the frame's own pixels
    // everywhere else in the panel's span down to the outer view's bottom edge — inside the outer
    // view alone, inside neither, and inside the panel's own bounds outside both.
    usize green = 0;
    usize wrong = 0;
    for (u32 y = 10; y < 110U; ++y) {
        for (u32 x = 120; x < 250U; ++x) {
            if (x >= 180U && x < 240U && y >= 60U) {
                const bool drawn = pixels[at(x, y)] == texel_of(kGreen);
                green += static_cast<usize>(drawn);
                wrong += static_cast<usize>(!drawn);
            } else {
                wrong += static_cast<usize>(pixels[at(x, y)] != bare[at(x, y)]);
            }
        }
    }
    CY_CHECK_EQ(green, 60U * 50U);
    CY_CHECK_EQ(wrong, 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE(
    "(e) opacity blends premultiplied, an invisible panel draws nothing, one draw a batch") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    std::vector<u32> bare;
    CY_REQUIRE(render_bare(fixture, bare));
    UiRun run(fixture, true, &build_everything);
    CY_REQUIRE(run.render());
    const std::vector<u32>& pixels = run.pixels();
    // Half white over 0x40 grey: 128 + 64 x (1 - 128/255) = 159.9.
    const u32 glass = pixels[at(340, 50)];
    for (u32 shift = 0; shift < 24U; shift += 8U) {
        const u32 channel = (glass >> shift) & 0xFFU;
        CY_CHECK_GE(channel, 159U);
        CY_CHECK_LE(channel, 160U);
    }
    // The grey around it is untouched by the glass.
    CY_CHECK_EQ(pixels[at(303, 23)], texel_of(kGrey));
    // The invisible panel left the frame's own pixels.
    usize moved = 0;
    for (u32 y = 20; y < 80U; ++y) {
        for (u32 x = 390; x < 450U; ++x) {
            moved += static_cast<usize>(pixels[at(x, y)] != bare[at(x, y)]);
        }
    }
    CY_CHECK_EQ(moved, 0U);
    // One indirect draw per batch `flatten()` made — none of these batches is clipped away.
    CY_CHECK_EQ(run.renderer.report().draws, run.flattened.batches);
    CY_CHECK_EQ(run.renderer.report().clipped_batches, 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(f) a strategy game's HUD and the console over the scene, against its golden image") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    UiRun run(fixture, true, &build_hud);
    CY_REQUIRE(run.render());
    save("ui-hud.png", run.pixels());
    check_against_reference("ui_hud.png", run.pixels());
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

// --- (g) An sRGB output --------------------------------------------------------------------------

namespace {

constexpr u32 kSrgbSide = 32;
constexpr usize kSrgbTexels = static_cast<usize>(kSrgbSide) * kSrgbSide;
/// The display-encoded byte the target starts as: `0x40` grey, opaque.
constexpr u32 kSrgbBackground = 0xFF404040U;

/// The copies on either side of the interface pass: a known picture in, the result out.
struct SrgbTarget {
    rhi::TextureHandle texture;
    rhi::BufferHandle upload;
    rhi::BufferHandle readback;
};

void record_srgb_fill(const rendering::PassContext& context, void* user) noexcept {
    const auto* target = static_cast<const SrgbTarget*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{kSrgbSide, kSrgbSide, 1};
    context.commands->copy_buffer_to_texture(target->upload, target->texture,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

void record_srgb_capture(const rendering::PassContext& context, void* user) noexcept {
    const auto* target = static_cast<const SrgbTarget*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{kSrgbSide, kSrgbSide, 1};
    context.commands->copy_texture_to_buffer(target->texture, target->readback,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

[[nodiscard]] f32 srgb_decode(f32 value) noexcept {
    return value <= 0.04045F ? value / 12.92F : std::pow((value + 0.055F) / 1.055F, 2.4F);
}

[[nodiscard]] u32 srgb_encode_byte(f32 value) noexcept {
    const f32 clamped = std::clamp(value, 0.0F, 1.0F);
    const f32 encoded = clamped <= 0.0031308F ? clamped * 12.92F
                                              : (1.055F * std::pow(clamped, 1.0F / 2.4F)) - 0.055F;
    return static_cast<u32>(std::lround(encoded * 255.0F));
}

/// The interface drawn by `renderer` over an `Rgba8Srgb` target filled with `kSrgbBackground`.
[[nodiscard]] bool render_srgb(DeviceFixture& fixture, UiRenderer& renderer,
                               const PrimitiveBuffer& buffer, std::vector<u32>& out) {
    rhi::Device& device = fixture.device();
    SrgbTarget target;
    rhi::TextureDescription texture;
    texture.name = "ui srgb target";
    texture.format = rhi::Format::Rgba8Srgb;
    texture.extent = rhi::Extent3D{kSrgbSide, kSrgbSide, 1};
    texture.usage = rhi::TextureUsage::ColorAttachment | rhi::TextureUsage::TransferSource |
                    rhi::TextureUsage::TransferDestination;
    rhi::BufferDescription upload;
    upload.name = "ui srgb fill";
    upload.size = kSrgbTexels * sizeof(u32);
    upload.usage = rhi::BufferUsage::TransferSource;
    upload.memory = rhi::MemoryUse::Upload;
    rhi::BufferDescription readback = upload;
    readback.name = "ui srgb capture";
    readback.usage = rhi::BufferUsage::TransferDestination;
    readback.memory = rhi::MemoryUse::Readback;
    auto made_texture = device.create_texture(texture);
    auto made_upload = device.create_buffer(upload);
    auto made_readback = device.create_buffer(readback);
    bool drawn = made_texture.has_value() && made_upload.has_value() && made_readback.has_value();
    if (drawn) {
        target = SrgbTarget{*made_texture, *made_upload, *made_readback};
        auto* fill = static_cast<u32*>(device.buffer_mapped_pointer(target.upload));
        drawn = fill != nullptr;
        if (drawn) {
            std::fill(fill, fill + kSrgbTexels, kSrgbBackground);
        }
    }
    drawn = drawn && device.begin_frame().has_value();
    if (drawn) {
        bool executed = renderer.submit(buffer, 1.0F).has_value();
        rendering::RenderGraph graph(allocator());
        rendering::TextureRequest request;
        request.name = "ui srgb target";
        request.format = rhi::Format::Rgba8Srgb;
        request.width = kSrgbSide;
        request.height = kSrgbSide;
        const rendering::ResourceId colour =
            graph.import_texture(request, target.texture, rhi::ImageUse::Undefined);
        graph.add_pass("ui srgb fill", rhi::QueueKind::Graphics)
            .write(colour, rhi::Access::TransferWrite)
            .record(&record_srgb_fill, &target);
        rendering::ScreenSpaceStageInputs inputs;
        inputs.target = colour;
        inputs.width = kSrgbSide;
        inputs.height = kSrgbSide;
        executed = executed && renderer.declare(graph, inputs) != rendering::kInvalidPass;
        rendering::BufferRequest capture;
        capture.name = "ui srgb capture";
        capture.size = readback.size;
        capture.extra_usage = rhi::BufferUsage::TransferDestination;
        const rendering::ResourceId destination = graph.import_buffer(capture, target.readback);
        graph.add_pass("ui srgb capture", rhi::QueueKind::Graphics)
            .read(colour, rhi::Access::TransferRead)
            .write(destination, rhi::Access::TransferWrite)
            .record(&record_srgb_capture, &target);
        graph.add_pass("ui srgb capture host", rhi::QueueKind::Graphics)
            .read(destination, rhi::Access::HostRead)
            .side_effect();
        {
            rendering::GraphExecutor executor(allocator(), device);
            executed =
                executed && graph.status().has_value() &&
                executor.execute(graph, rendering::CompileOptions{}, rendering::ExecuteOptions{})
                    .has_value() &&
                device.wait_idle().has_value();
            executor.release();
        }
        const auto* mapped = static_cast<const u32*>(device.buffer_mapped_pointer(target.readback));
        executed = executed && mapped != nullptr;
        if (executed) {
            out.assign(mapped, mapped + kSrgbTexels);
        }
        drawn = device.end_frame().has_value() && executed;
    }
    (void)device.wait_idle();
    if (made_readback.has_value()) {
        device.destroy_buffer(*made_readback);
    }
    if (made_upload.has_value()) {
        device.destroy_buffer(*made_upload);
    }
    if (made_texture.has_value()) {
        device.destroy_texture(*made_texture);
    }
    return drawn;
}

}  // namespace

CY_TEST_CASE("(g) on an sRGB output the interface's bytes are its colours, blended in linear") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    // An opaque red square and, beside it, half white over the grey the target starts as.
    ElementStore store(allocator());
    auto made = store.create(kNoElement, Name::intern("screen"));
    CY_REQUIRE(made.has_value());
    const ElementId root = *made;
    store.layout_input(root)->model = LayoutModel::Absolute;
    const auto square = [&store, root](f32 x, u32 colour) noexcept {
        auto element = store.create(root, Name::intern("box"));
        if (!element.has_value()) {
            return kNoElement;
        }
        LayoutInput* input = store.layout_input(*element);
        input->model = LayoutModel::Absolute;
        input->offset_min = Vec2{x, 4.0F};
        input->offset_max = Vec2{x + 12.0F, 16.0F};
        store.paint(*element)->background = colour;
        return *element;
    };
    CY_REQUIRE(square(2.0F, kRed) != kNoElement);
    const ElementId glass = square(18.0F, 0xFFFFFFFFU);
    CY_REQUIRE(glass != kNoElement);
    store.paint(glass)->opacity = 0.5F;
    ScaleSettings settings;
    settings.mode = ScaleMode::FixedPixel;
    const Vec2 viewport{static_cast<f32>(kSrgbSide), static_cast<f32>(kSrgbSide)};
    LayoutReport laid{};
    CY_REQUIRE(layout(store, settings, viewport, nullptr, laid).has_value());
    PrimitiveBuffer buffer(allocator());
    FlattenReport flattened{};
    CY_REQUIRE(flatten(store, ui::Rect{0.0F, 0.0F, viewport.x, viewport.y}, buffer, flattened)
                   .has_value());

    UiRenderer renderer(allocator());
    UiRendererDescription description;
    description.width = kSrgbSide;
    description.height = kSrgbSide;
    description.output_format = rhi::Format::Rgba8Srgb;
    CY_REQUIRE(renderer.create(fixture.device(), description).has_value());
    std::vector<u32> pixels;
    CY_REQUIRE(render_srgb(fixture, renderer, buffer, pixels));
    renderer.destroy();
    const auto pixel = [&pixels](u32 x, u32 y) { return pixels[(y * kSrgbSide) + x]; };

    // The opaque colour's display bytes, as a UNORM output holds them: decoded by the shader,
    // encoded again by the target.
    CY_CHECK_LE(channel_delta(pixel(8, 10), texel_of(kRed)), 1U);
    // Nothing drawn is untouched.
    CY_CHECK_EQ(pixel(1, 1), kSrgbBackground);
    // Half white over 0x40 grey, blended by the hardware in linear light: 0.502 + 0.051 x 0.498.
    const f32 alpha = 128.0F / 255.0F;
    const u32 expected = srgb_encode_byte(alpha + (srgb_decode(64.0F / 255.0F) * (1.0F - alpha)));
    for (u32 shift = 0; shift < 24U; shift += 8U) {
        const u32 channel = (pixel(24, 10) >> shift) & 0xFFU;
        CY_CHECK_GE(channel + 1U, expected);
        CY_CHECK_LE(channel, expected + 1U);
    }
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

#if defined(CY_TEXT)

namespace {

constexpr u32 kInk = 0xFFF2F2F2U;

/// Four lines of a paragraph in the interface font, as labels with no panel of their own — so
/// nothing between their glyphs breaks the batch.
bool build_paragraph(UiRun& run) noexcept {
    const char* lines[] = {
        "Text in the interface font is a distance",
        "field: one atlas entry draws a glyph at",
        "any size, and a paragraph whose glyphs come",
        "from one page is submitted as one draw.",
    };
    (void)run.box(run.root, 16.0F, 16.0F, 448.0F, 104.0F, kPanel);
    TextStyle style;
    style.colour = kInk;
    style.size = 18.0F;
    f32 y = 22.0F;
    for (const char* line : lines) {
        const ElementId label = run.box(run.root, 24.0F, y, 430.0F, 24.0F, 0);
        if (!run.text.set_text(label, line, style).has_value()) {
            return false;
        }
        y += 24.0F;
    }
    return true;
}

/// A title drawn the way a game draws one over a busy scene: an outline and a drop shadow, both
/// from the glyphs' own distance-field entries.
bool build_outlined(UiRun& run) noexcept {
    TextStyle style;
    style.colour = kYellow;
    style.size = 40.0F;
    style.outline_width = 2.5F;
    style.outline_colour = 0xFF101010U;
    style.shadow_colour = 0xA0000000U;
    style.shadow_offset = Vec2{3.0F, 4.0F};
    style.gradient_colour = 0xFFE0302AU;
    const ElementId title = run.box(run.root, 40.0F, 60.0F, 400.0F, 60.0F, 0);
    return run.text.set_text(title, "Victory 1250!", style).has_value();
}

/// The host reference of `run`'s interface over `bare`, with the interface font's pages.
[[nodiscard]] std::vector<u32> reference_of(UiRun& run, const std::vector<u32>& bare) {
    std::vector<u32> expected = bare;
    ReferenceAtlas pages[kTextPages + cy::text::kPixelFormatCount] = {};
    for (u32 format = 0; format < cy::text::kPixelFormatCount; ++format) {
        const auto kind = static_cast<cy::text::PixelFormat>(format);
        const u32 extent = run.text.atlas_extent(kind);
        pages[run.text.atlas_page(kind)] =
            ReferenceAtlas{run.text.atlas_pixels(kind), extent, extent,
                           kind == cy::text::PixelFormat::Coverage ? 1U : 4U};
    }
    CY_REQUIRE(draw_reference(run.renderer.draws(),
                              Span<const ReferenceAtlas>(pages, std::size(pages)), kWidth, kHeight,
                              Span<u32>(expected.data(), expected.size()))
                   .has_value());
    return expected;
}

}  // namespace

CY_TEST_CASE("(h) a paragraph in the interface font is one draw, shaded as the host shades it") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    std::vector<u32> bare;
    CY_REQUIRE(render_bare(fixture, bare));
    UiRun run(fixture, true, &build_paragraph, true);
    CY_REQUIRE(run.render());
    save("ui-paragraph.png", run.pixels());

    // `text-and-fonts` — Batched text: the panel is one draw and the paragraph's glyphs, from one
    // page, are ONE more, whatever the number of lines.
    const UiDrawList& list = run.renderer.draws();
    CY_REQUIRE_EQ(list.draws.size(), 2U);
    CY_CHECK_EQ(list.draws[1].material, material_index(BuiltinMaterial::GlyphField));
    CY_CHECK_EQ(list.draws[1].atlas, run.text.atlas_page(cy::text::PixelFormat::DistanceField));
    CY_CHECK_GT(list.draws[1].count, 120U);
    CY_CHECK_EQ(run.flattened.batches, 2U);

    const std::vector<u32> expected = reference_of(run, bare);
    u32 worst = 0;
    usize differing = 0;
    for (usize index = 0; index < kPixels; ++index) {
        const u32 delta = channel_delta(run.pixels()[index], expected[index]);
        worst = std::max(worst, delta);
        differing += static_cast<usize>(delta > 1U);
    }
    std::fprintf(stderr, "(h) the largest difference from the host is %u; %zu pixels over one\n",
                 worst, differing);
    // One step, as (b) allows: the field is sampled linearly on both sides, and the device's
    // filtering lands within a quantisation boundary of the host's float interpolation. A field
    // read with the wrong sign, channel, range or page is the whole glyph, not a step.
    CY_CHECK_LE(worst, 1U);
    CY_CHECK_EQ(differing, 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(i) an outlined, shadowed title matches its golden image") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    UiRun run(fixture, true, &build_outlined, true);
    CY_REQUIRE(run.render());
    save("ui-outlined.png", run.pixels());
    // One draw: every shadow and then every glyph, one material, one page.
    CY_CHECK_EQ(run.renderer.draws().draws.size(), 1U);
    check_against_reference("ui_text_outlined.png", run.pixels());
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

#endif
