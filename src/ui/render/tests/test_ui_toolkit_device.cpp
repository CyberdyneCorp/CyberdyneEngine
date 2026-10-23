// SPDX-License-Identifier: MIT
// The widget toolkit, opacity groups and the HDR composite on a Vulkan device, with validation and
// synchronisation validation on. `render.ui_toolkit`. Issue #91, stages 2, 4 and 5.
//
//   (a) a started toolkit with nothing in it draws nothing: the frame is the frame without an
//       interface, byte for byte;
//   (b) an opacity group on the device: the overlap of two children of a panel at 50 % is the same
//       colour as either child alone, every pixel within one step of the host reference, and the
//       same panel multiplied down darkens its overlap;
//   (c) an HDR document composited before the tone curve blooms — the light around an emissive
//       panel rises — and the same document drawn after the tone curve does not;
//   (d) the widget gallery, `src/ui/toolkit/tests/data/gallery.cyui`, against a golden image,
//       and again with its dialog open and the focus ring on a button;
//   (e) on the null device: the HDR stage is declared after motion blur and before bloom when a
//       producer is given, and not at all when none is.

#include "frame_scene.h"
#include "golden.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/command_buffer.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/graph/executor.h>
#include <cy/test/test.h>
#include <cy/ui/layout.h>
#include <cy/ui/render/text_atlas.h>
#include <cy/ui/render/ui_renderer.h>
#include <cy/ui/text/builtin_font.h>
#include <cy/ui/text/interface_font.h>
#include <cy/ui/toolkit/document_host.h>
#include <cy/ui/toolkit/toolkit.h>
#include <cy_features.h>

#if defined(CY_TEXT)
#    include <cy/backends/text/complete_backend.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
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

constexpr u16 kGlyphPage = 1;
constexpr u16 kImagePage = 2;
constexpr u16 kTextPages = 4;

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
    explicit DeviceFixture(const char* backend = "vulkan") noexcept
        : allocator_(system_allocator(MemoryDomain::Gpu)) {
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_test_render_ui_toolkit";
        description.enable_validation = true;
        description.enable_synchronisation_validation = true;
        device_ = rhi::create_device(allocator_, backend, description, selection_);
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
    [[nodiscard]] bool ok() const noexcept { return device_.has_value(); }
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

void save(const char* name, const std::vector<u32>& texels, u32 width, u32 height) noexcept {
    render_test::Image image(allocator());
    if (!render_test::adopt(image, Span<const u32>(texels.data(), texels.size()), width, height)
             .has_value()) {
        return;
    }
    char path[1024];
    (void)std::snprintf(path, sizeof(path), "%s/%s", artefact_directory(), name);
    if (render_test::write_png(path, image).has_value()) {
        std::fprintf(stderr, "wrote %s (%ux%u)\n", path, width, height);
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

void check_against_reference(const char* name, const std::vector<u32>& texels, u32 width,
                             u32 height) {
    render_test::Image rendered(allocator());
    CY_REQUIRE(
        render_test::adopt(rendered, Span<const u32>(texels.data(), texels.size()), width, height)
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

/// The 4x4 premultiplied checker the images sample, red in the low byte of each texel.
[[nodiscard]] std::vector<u8> checker_page() {
    std::vector<u8> texels(64);
    for (u32 index = 0; index < 16U; ++index) {
        const bool light = (((index % 4U) + (index / 4U)) % 2U) == 0U;
        texels[(index * 4U) + 0U] = light ? 230U : 40U;
        texels[(index * 4U) + 1U] = light ? 200U : 60U;
        texels[(index * 4U) + 2U] = light ? 120U : 160U;
        texels[(index * 4U) + 3U] = 255U;
    }
    return texels;
}

// --- A canvas: the interface alone, on a cleared target ------------------------------------------

struct Readback {
    rendering::ResourceId target = rendering::kInvalidResource;
    rhi::BufferHandle buffer;
    u32 width = 0;
    u32 height = 0;
    u32 clear = 0;
};

void record_clear(const rendering::PassContext& context, void* user) noexcept {
    const auto* readback = static_cast<const Readback*>(user);
    rhi::RenderAttachment target;
    target.view = context.executor->view(readback->target);
    target.load = rhi::LoadOp::Clear;
    target.store = rhi::StoreOp::Store;
    for (u32 channel = 0; channel < 4U; ++channel) {
        target.clear.color[channel] =
            static_cast<f32>((readback->clear >> (channel * 8U)) & 0xFFU) / 255.0F;
    }
    rhi::RenderingInfo info;
    info.render_area = rhi::Rect2D{0, 0, readback->width, readback->height};
    info.color_attachments = Span<const rhi::RenderAttachment>(&target, 1);
    context.commands->begin_rendering(info);
    context.commands->end_rendering();
}

void record_readback(const rendering::PassContext& context, void* user) noexcept {
    const auto* readback = static_cast<const Readback*>(user);
    rhi::BufferTextureCopy region;
    region.texture_extent = rhi::Extent3D{readback->width, readback->height, 1};
    context.commands->copy_texture_to_buffer(context.executor->texture(readback->target),
                                             readback->buffer,
                                             Span<const rhi::BufferTextureCopy>(&region, 1));
}

/// Draw what `renderer` was submitted over a target cleared to `clear` (RGBA8, red low), and read
/// it back.
[[nodiscard]] bool draw_canvas(rhi::Device& device, UiRenderer& renderer, u32 width, u32 height,
                               u32 clear, std::vector<u32>& out) {
    rhi::BufferDescription description;
    description.name = "ui canvas readback";
    description.size = u64{width} * height * 4U;
    description.usage = rhi::BufferUsage::TransferDestination;
    description.memory = rhi::MemoryUse::Readback;
    Expected<rhi::BufferHandle, Error> buffer = device.create_buffer(description);
    if (!buffer || !device.begin_frame().has_value()) {
        return false;
    }
    bool drawn = false;
    {
        rendering::RenderGraph graph(allocator());
        rendering::GraphExecutor executor(allocator(), device);
        Readback readback;
        readback.width = width;
        readback.height = height;
        readback.clear = clear;
        readback.buffer = *buffer;
        rendering::TextureRequest request;
        request.name = "ui canvas";
        request.format = rhi::Format::Rgba8Unorm;
        request.width = width;
        request.height = height;
        readback.target = graph.create_texture(request);
        graph.add_pass("clear", rhi::QueueKind::Graphics)
            .write(readback.target, rhi::Access::ColorAttachmentWrite)
            .record(&record_clear, &readback);
        rendering::ScreenSpaceStageInputs inputs;
        inputs.target = readback.target;
        inputs.width = width;
        inputs.height = height;
        rendering::BufferRequest read_request;
        read_request.name = "ui canvas readback";
        read_request.size = description.size;
        read_request.extra_usage = rhi::BufferUsage::TransferDestination;
        const rendering::ResourceId read_id = graph.import_buffer(read_request, *buffer);
        const bool declared = renderer.declare(graph, inputs) != rendering::kInvalidPass;
        graph.add_pass("readback", rhi::QueueKind::Graphics)
            .read(readback.target, rhi::Access::TransferRead)
            .write(read_id, rhi::Access::TransferWrite)
            .record(&record_readback, &readback);
        graph.add_pass("host", rhi::QueueKind::Graphics)
            .read(read_id, rhi::Access::HostRead)
            .side_effect();
        drawn = declared && graph.status().has_value() &&
                executor.execute(graph, rendering::CompileOptions{}, rendering::ExecuteOptions{})
                    .has_value() &&
                device.wait_idle().has_value();
        executor.release();
    }
    drawn = device.end_frame().has_value() && drawn;
    if (drawn) {
        const auto* texels = static_cast<const u32*>(device.buffer_mapped_pointer(*buffer));
        out.assign(texels, texels + (static_cast<usize>(width) * height));
    }
    device.destroy_buffer(*buffer);
    return drawn;
}

// --- The gallery ---------------------------------------------------------------------------------

/// A toolkit on its own store and text, drawing to `renderer`.
struct Interface {
    explicit Interface(rhi::Device& device, u32 width, u32 height, bool interface_font) noexcept {
        UiRendererDescription description;
        description.width = width;
        description.height = height;
        description.output_format = rhi::Format::Rgba8Unorm;
        ready = renderer.create(device, description).has_value() && start_text(interface_font) &&
                toolkit.start().has_value();
        const std::vector<u8> checker = checker_page();
        ready = ready && renderer
                             .upload_atlas(kImagePage, rhi::Format::Rgba8Unorm, 4, 4,
                                           Span<const u8>(checker.data(), checker.size()))
                             .has_value();
    }
    ~Interface() { renderer.destroy(); }
    Interface(const Interface&) = delete;
    Interface& operator=(const Interface&) = delete;
    Interface(Interface&&) = delete;
    Interface& operator=(Interface&&) = delete;

    [[nodiscard]] bool start_text(bool interface_font) noexcept {
        if (!interface_font) {
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

    /// Update the toolkit at `scale`, flatten, upload the text's pages and submit.
    [[nodiscard]] bool submit(Vec2 output, f32 scale) noexcept {
        ScaleSettings settings;
        settings.mode = ScaleMode::FixedPixel;
        settings.dpi_scale = scale;
        if (!toolkit.update(1.0F / 60.0F, settings, output).has_value()) {
            return false;
        }
        const Vec2 reference{output.x / scale, output.y / scale};
        if (!flatten(store, ui::Rect{0.0F, 0.0F, reference.x, reference.y}, buffer, flattened,
                     &text)
                 .has_value()) {
            return false;
        }
        // The pages again only when the text made new glyphs resident, as the guide says.
        if (text.atlas_revision() != uploaded_revision) {
            if (!upload_text_atlases(renderer, text).has_value()) {
                return false;
            }
            uploaded_revision = text.atlas_revision();
        }
        return renderer.submit(buffer, scale).has_value();
    }

    ElementStore store{allocator()};
#if defined(CY_TEXT)
    cy::text::CompleteTextBackend backend;
#endif
    cy::text::TextServer server;
    TextPainter text{allocator()};
    Toolkit toolkit{allocator(), store, text};
    UiRenderer renderer{allocator()};
    PrimitiveBuffer buffer{allocator()};
    u32 uploaded_revision = ~0U;
    FlattenReport flattened{};
    bool ready = false;
};

[[nodiscard]] std::string read_text(const std::string& path) {
    std::string text;
    if (FILE* file = std::fopen(path.c_str(), "rb"); file != nullptr) {
        char chunk[4096];
        usize got = 0;
        while ((got = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
            text.append(chunk, got);
        }
        (void)std::fclose(file);
    }
    return text;
}

// --- The HDR frame ------------------------------------------------------------------------------

/// The pipeline suites' scene with bloom, and an emissive panel drawn either before the tone curve
/// (`hdr`) or after it, or not at all.
class BloomRun {
public:
    enum class Panel : u8 { None, AfterToneCurve, BeforeToneCurve };

    BloomRun(DeviceFixture& fixture, Panel panel) noexcept
        : fixture_(fixture), scene_(allocator()), panel_(panel) {}
    ~BloomRun() {
        (void)fixture_.device().wait_idle();
        renderer_.destroy();
        scene_.release();
    }
    BloomRun(const BloomRun&) = delete;
    BloomRun& operator=(const BloomRun&) = delete;
    BloomRun(BloomRun&&) = delete;
    BloomRun& operator=(BloomRun&&) = delete;

    [[nodiscard]] bool render() noexcept {
        FrameSceneHooks hooks;
        hooks.user = this;
        hooks.before_assemble = &before_assemble;
        scene_.set_hooks(hooks);
        // A strong bloom, so the light a panel adds is plain.
        rendering::BloomSettings bloom;
        bloom.intensity = 0.2F;
        if (!scene_.build(fixture_.device(), &bloom).has_value()) {
            return false;
        }
        scene_.set_read_back(true);
        if (panel_ != Panel::None && !build_panel()) {
            return false;
        }
        rendering::assembly::AssemblyReport report{};
        if (!scene_.render(RecordMode::Callbacks, report).has_value() ||
            !fixture_.device().wait_idle().has_value()) {
            return false;
        }
        pixels_.assign(scene_.pixels().begin(), scene_.pixels().end());
        return pixels_.size() == static_cast<usize>(kWidth) * kHeight;
    }

    [[nodiscard]] const std::vector<u32>& pixels() const noexcept { return pixels_; }
    [[nodiscard]] const UiRenderReport& report() const noexcept { return renderer_.report(); }

    /// The panel's place on screen, in pixels.
    static constexpr u32 kLeft = 200;
    static constexpr u32 kTop = 110;
    static constexpr u32 kSide = 40;

private:
    [[nodiscard]] bool build_panel() noexcept {
        UiRendererDescription description;
        description.width = kWidth;
        description.height = kHeight;
        // Before the tone curve the colour is the frame's linear HDR colour; after it, the output.
        description.output_format = panel_ == Panel::BeforeToneCurve
                                        ? scene_.pipelines().setup().color_format
                                        : scene_.pipelines().setup().output_format;
        // The scene is exposed at -11.4 stops: an interface colour of one is drawn at the radiance
        // that exposure shows as one, so intensity is the panel's brightness over its own colour.
        description.scene_scale = std::exp2(11.4F);
        if (!renderer_.create(fixture_.device(), description).has_value()) {
            return false;
        }
        // A root covers the window; the panel is placed in it.
        auto screen = store_.create(kNoElement, Name::intern("screen"));
        if (!screen.has_value()) {
            return false;
        }
        store_.layout_input(*screen)->model = LayoutModel::Absolute;
        auto made = store_.create(*screen, Name::intern("emissive"));
        if (!made.has_value()) {
            return false;
        }
        LayoutInput* input = store_.layout_input(*made);
        input->model = LayoutModel::Absolute;
        input->offset_min = Vec2{static_cast<f32>(kLeft), static_cast<f32>(kTop)};
        input->offset_max = Vec2{static_cast<f32>(kLeft + kSide), static_cast<f32>(kTop + kSide)};
        PaintData* paint = store_.paint(*made);
        paint->background = 0xFFFFE0B0U;
        // An emissive panel: sixteen times its colour's radiance, which only a float target keeps.
        paint->intensity = 16.0F;
        ScaleSettings settings;
        settings.mode = ScaleMode::FixedPixel;
        LayoutReport laid{};
        const Vec2 viewport{static_cast<f32>(kWidth), static_cast<f32>(kHeight)};
        return layout(store_, settings, viewport, nullptr, laid).has_value() &&
               flatten(store_, ui::Rect{0.0F, 0.0F, viewport.x, viewport.y}, buffer_, flattened_)
                   .has_value() &&
               renderer_.submit(buffer_, 1.0F).has_value();
    }

    static Status before_assemble(rendering::RenderGraph& /*graph*/,
                                  rendering::assembly::AssemblyView& /*view*/,
                                  rendering::assembly::FrameSinks& sinks, void* user) noexcept {
        auto* run = static_cast<BloomRun*>(user);
        if (run->panel_ == Panel::BeforeToneCurve) {
            sinks.ui_hdr = run->renderer_.stage();
        } else if (run->panel_ == Panel::AfterToneCurve) {
            sinks.ui = run->renderer_.stage();
        }
        return ok();
    }

    DeviceFixture& fixture_;
    FrameScene scene_;
    Panel panel_;
    ElementStore store_{allocator()};
    UiRenderer renderer_{allocator()};
    PrimitiveBuffer buffer_{allocator()};
    FlattenReport flattened_{};
    std::vector<u32> pixels_;
};

/// The mean of the red, green and blue bytes in a ring `inner` to `outer` pixels out from the
/// panel.
[[nodiscard]] f64 ring_brightness(const std::vector<u32>& pixels, u32 inner, u32 outer) noexcept {
    f64 sum = 0.0;
    u32 count = 0;
    const i32 left = static_cast<i32>(BloomRun::kLeft);
    const i32 top = static_cast<i32>(BloomRun::kTop);
    const i32 side = static_cast<i32>(BloomRun::kSide);
    for (i32 y = top - static_cast<i32>(outer); y < top + side + static_cast<i32>(outer); ++y) {
        for (i32 x = left - static_cast<i32>(outer); x < left + side + static_cast<i32>(outer);
             ++x) {
            const i32 dx = std::max({left - x, x - (left + side - 1), 0});
            const i32 dy = std::max({top - y, y - (top + side - 1), 0});
            const i32 distance = std::max(dx, dy);
            if (distance < static_cast<i32>(inner) || distance >= static_cast<i32>(outer) ||
                x < 0 || y < 0 || x >= static_cast<i32>(kWidth) || y >= static_cast<i32>(kHeight)) {
                continue;
            }
            const u32 texel = pixels[(static_cast<usize>(y) * kWidth) + static_cast<usize>(x)];
            sum += static_cast<f64>((texel & 0xFFU) + ((texel >> 8U) & 0xFFU) +
                                    ((texel >> 16U) & 0xFFU)) /
                   3.0;
            ++count;
        }
    }
    return count > 0 ? sum / count : 0.0;
}

}  // namespace

CY_TEST_CASE("(a) a started toolkit with nothing in it draws nothing, byte for byte") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    Interface interface(fixture.device(), 320, 200, false);
    CY_REQUIRE(interface.ready);
    CY_REQUIRE(interface.submit(Vec2{320.0F, 200.0F}, 1.0F));
    CY_CHECK_EQ(interface.buffer.primitives().size(), 0U);
    std::vector<u32> drawn;
    CY_REQUIRE(draw_canvas(fixture.device(), interface.renderer, 320, 200, 0xFF302010U, drawn));
    u32 changed = 0;
    for (const u32 texel : drawn) {
        changed += texel != 0xFF302010U ? 1U : 0U;
    }
    CY_CHECK_EQ(changed, 0U);
    CY_CHECK_FALSE(interface.renderer.report().recorded);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(b) an opacity group's overlap is its non-overlap on the device, as on the host") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    constexpr u32 kSize = 128;
    for (const bool grouped : {true, false}) {
        const std::string_view mode = grouped ? "grouped" : "multiplied";
        CY_TEST_INFO(mode);
        Interface interface(fixture.device(), kSize, kSize, false);
        CY_REQUIRE(interface.ready);
        ElementStore& store = interface.store;
        const auto box = [&store](ElementId parent, f32 x, f32 y, f32 w, f32 h,
                                  u32 colour) noexcept {
            auto made = store.create(parent, Name::intern("box"));
            LayoutInput* input = store.layout_input(*made);
            input->model = LayoutModel::Absolute;
            input->offset_min = Vec2{x, y};
            input->offset_max = Vec2{x + w, y + h};
            store.paint(*made)->background = colour;
            return *made;
        };
        const ElementId panel = box(interface.toolkit.root(), 16.0F, 16.0F, 96.0F, 64.0F, 0U);
        (void)box(panel, 8.0F, 8.0F, 40.0F, 40.0F, 0xFFE0302AU);
        (void)box(panel, 32.0F, 16.0F, 40.0F, 40.0F, 0xFF2A5CE0U);
        store.paint(panel)->opacity = 0.5F;
        if (grouped) {
            CY_REQUIRE(store.set_flags(panel, store.flags(panel) | ElementFlags::OpacityGroup)
                           .has_value());
        }
        CY_REQUIRE(interface.submit(Vec2{static_cast<f32>(kSize), static_cast<f32>(kSize)}, 1.0F));
        std::vector<u32> drawn;
        CY_REQUIRE(
            draw_canvas(fixture.device(), interface.renderer, kSize, kSize, 0xFF000000U, drawn));
        CY_CHECK_EQ(interface.renderer.report().groups, grouped ? 1U : 0U);
        // (60, 50) is where blue covers red; (80, 50) is blue alone.
        const u32 overlap = drawn[(50U * kSize) + 60U];
        const u32 alone = drawn[(50U * kSize) + 80U];
        if (grouped) {
            CY_CHECK_EQ(overlap, alone);
            save("ui-opacity-group.png", drawn, kSize, kSize);
        } else {
            CY_CHECK_NE(overlap, alone);
        }
        // The device is the host reference, to a step.
        std::vector<u32> host(static_cast<usize>(kSize) * kSize, 0xFF000000U);
        CY_REQUIRE(draw_reference(interface.renderer.draws(), {}, kSize, kSize,
                                  Span<u32>(host.data(), host.size()))
                       .has_value());
        u32 worst = 0;
        for (usize index = 0; index < host.size(); ++index) {
            worst = std::max(worst, channel_delta(host[index], drawn[index]));
        }
        CY_CHECK_LE(worst, 1U);
    }
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(c) an HDR document composited before the tone curve blooms, and after it does not") {
    // `ui-system`: "WHEN a document opts into HDR THEN it SHALL be composited before tonemapping so
    // emissive UI participates in bloom and exposure."
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    BloomRun bare(fixture, BloomRun::Panel::None);
    BloomRun after(fixture, BloomRun::Panel::AfterToneCurve);
    BloomRun before(fixture, BloomRun::Panel::BeforeToneCurve);
    CY_REQUIRE(bare.render());
    CY_REQUIRE(after.render());
    CY_REQUIRE(before.render());
    CY_CHECK(before.report().recorded);
    const f64 bare_ring = ring_brightness(bare.pixels(), 2, 14);
    const f64 after_ring = ring_brightness(after.pixels(), 2, 14);
    const f64 before_ring = ring_brightness(before.pixels(), 2, 14);
    std::fprintf(stderr, "ring brightness: bare %.2f, after the tone curve %.2f, before it %.2f\n",
                 bare_ring, after_ring, before_ring);
    const usize centre =
        (static_cast<usize>(BloomRun::kTop + 20U) * kWidth) + BloomRun::kLeft + 20U;
    std::fprintf(stderr, "panel centre: bare %08x, after %08x, before %08x\n",
                 bare.pixels()[centre], after.pixels()[centre], before.pixels()[centre]);
    // After the tone curve the panel is drawn over the bloomed frame: the ring is the frame's own.
    CY_CHECK_LT(std::fabs(after_ring - bare_ring), 0.01);
    // Before it, the panel's light is in the HDR colour bloom reads: the ring brightens.
    CY_CHECK_GT(before_ring, bare_ring + 10.0);
    save("ui-hdr-bloom.png", before.pixels(), kWidth, kHeight);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(d) the widget gallery against its golden image, closed and with its dialog open") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
#if defined(CY_TEXT)
    constexpr bool kInterfaceFont = true;
#else
    constexpr bool kInterfaceFont = false;
#endif
    // Drawn at twice the reference resolution, which is what a distance-field font is for.
    constexpr f32 kScale = 2.0F;
    constexpr u32 kGalleryWidth = 1920;
    constexpr u32 kGalleryHeight = 1080;
    Interface interface(fixture.device(), kGalleryWidth, kGalleryHeight, kInterfaceFont);
    CY_REQUIRE(interface.ready);
    FileDocumentResolver files(allocator(), CY_UI_TOOLKIT_DATA_DIR);
    DocumentInstance gallery(allocator(), interface.toolkit);
    const std::string document = read_text(std::string(CY_UI_TOOLKIT_DATA_DIR) + "/gallery.cyui");
    CY_REQUIRE(gallery.load(document, kNoElement, &files, nullptr).has_value());
    // A selection in the list, the second tab's title focused by a gamepad, and the list scrolled.
    const WidgetId inventory = gallery.find("inventory");
    const std::string_view items[] = {"Rifle",   "Medkit",  "Grenade", "Ration", "Radio",
                                      "Compass", "Map",     "Rope",    "Torch",  "Binoculars",
                                      "Shovel",  "Canteen", "Flare"};
    CY_REQUIRE(interface.toolkit.set_items(inventory, Span<const std::string_view>(items, 13))
                   .has_value());
    CY_REQUIRE(interface.toolkit.set_selected(inventory, 2).has_value());
    const Vec2 output{static_cast<f32>(kGalleryWidth), static_cast<f32>(kGalleryHeight)};
    CY_REQUIRE(interface.submit(output, kScale));
    CY_REQUIRE(interface.toolkit.focus(gallery.find("volume")).has_value());
    CY_REQUIRE(interface.submit(output, kScale));
    std::vector<u32> closed;
    CY_REQUIRE(draw_canvas(fixture.device(), interface.renderer, kGalleryWidth, kGalleryHeight,
                           0xFF000000U, closed));
    const UiRenderReport drawn = interface.renderer.report();
    // The measured cost the guide quotes: a frame of the whole gallery on the CPU — bindings,
    // styles, layout, the list's rows, flattening and the rows' upload — with nothing changed.
    constexpr u32 kFrames = 60;
    const auto begin = std::chrono::steady_clock::now();
    for (u32 frame = 0; frame < kFrames; ++frame) {
        CY_REQUIRE(interface.submit(output, kScale));
    }
    const f64 us =
        std::chrono::duration<f64, std::micro>(std::chrono::steady_clock::now() - begin).count() /
        kFrames;
    std::fprintf(stderr,
                 "gallery: %u widgets, %u primitives, %u draws, %u page binds, %.1f us a frame "
                 "on the CPU\n",
                 interface.toolkit.widget_count(), drawn.primitives, drawn.draws, drawn.page_binds,
                 us);
    save("ui-widget-gallery.png", closed, kGalleryWidth, kGalleryHeight);
    if (kInterfaceFont) {
        check_against_reference("ui_widget_gallery.png", closed, kGalleryWidth, kGalleryHeight);
    }

    // The dialog: modal, over a dimmed backdrop, its first button focused and ringed. Drawn after
    // its pop-in has finished, which `settings().reduced_motion` makes the first frame.
    interface.toolkit.settings().reduced_motion = true;
    CY_REQUIRE(interface.toolkit.open(gallery.find("confirm")).has_value());
    CY_REQUIRE(interface.submit(output, kScale));
    CY_REQUIRE(interface.submit(output, kScale));
    CY_CHECK(interface.toolkit.focus_visible());
    std::vector<u32> open;
    CY_REQUIRE(draw_canvas(fixture.device(), interface.renderer, kGalleryWidth, kGalleryHeight,
                           0xFF000000U, open));
    save("ui-widget-gallery-dialog.png", open, kGalleryWidth, kGalleryHeight);
    if (kInterfaceFont) {
        check_against_reference("ui_widget_gallery_dialog.png", open, kGalleryWidth,
                                kGalleryHeight);
    }
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(e) the HDR stage is declared before bloom with a producer, and not without one") {
    DeviceFixture fixture("null");
    CY_REQUIRE(fixture.ok());
    struct Producer {
        static rendering::PassId declare(rendering::RenderGraph& graph,
                                         const rendering::ScreenSpaceStageInputs& inputs,
                                         void* user) noexcept {
            ++*static_cast<u32*>(user);
            return graph.add_pass("ui hdr", rhi::QueueKind::Graphics)
                .use(inputs.target, rhi::Access::ColorAttachmentReadWrite)
                .id();
        }
    };
    for (const bool produced : {false, true}) {
        u32 declared = 0;
        FrameScene scene(allocator());
        FrameSceneHooks hooks;
        struct Context {
            bool produced = false;
            u32* declared = nullptr;
        } context{produced, &declared};
        hooks.user = &context;
        hooks.before_assemble = [](rendering::RenderGraph& /*graph*/,
                                   rendering::assembly::AssemblyView& /*view*/,
                                   rendering::assembly::FrameSinks& sinks, void* user) noexcept {
            const auto* own = static_cast<const Context*>(user);
            if (own->produced) {
                sinks.ui_hdr = rendering::FrameStageDeclaration{&Producer::declare, own->declared};
            }
            return ok();
        };
        scene.set_hooks(hooks);
        rendering::BloomSettings bloom;
        CY_REQUIRE(scene.build(fixture.device(), &bloom).has_value());
        rendering::assembly::AssemblyReport report;
        CY_REQUIRE(scene.render(RecordMode::Callbacks, report).has_value());
        const rendering::ForwardFrame& frame = scene.assembly().frame();
        const rendering::PassId hdr = frame.pass_of(rendering::FramePassKind::UiHdr);
        CY_CHECK_EQ(declared, produced ? 1U : 0U);
        CY_CHECK_EQ(hdr != rendering::kInvalidPass, produced);
        if (produced) {
            // Before the first of bloom's passes, which read what it drew.
            CY_CHECK_LT(hdr, frame.pass_of(rendering::FramePassKind::Bloom));
            CY_CHECK_EQ(
                std::string_view(rendering::frame_pass_kind_name(rendering::FramePassKind::UiHdr)),
                std::string_view("ui hdr"));
        }
    }
}
