// SPDX-License-Identifier: MIT
// render.rts_api_hud — the HUD samples/13-rts-api's Swift game builds through ABI 1.6, against the
// HUD samples/13-rts-selection builds in C++, drawing the same thing.
//
// The game module is loaded into this process and a `HudShowcase` behaviour is created on a bare
// entity: its `onCreate` mounts `RtsHud` (game/Hud.swift) through CyberdyneKit and shows
// `HudModel.showcase` — the numbers render.ui's `build_hud` gives the C++ `Hud`. Nothing else of
// the game runs. The interface it builds is in a CyberUI store behind a `UiAdapter`, exactly as in
// the sample's host.
//
//   (a) the two stores flatten to the same primitive stream — every rectangle, colour, radius,
//       border, glyph and clip — which needs no device;
//   (b) on a Vulkan device, over `pipeline_test::FrameScene`, the two frames are the same bytes;
//   (c) the Swift HUD with its Build button, which the C++ HUD does not have, against a committed
//       golden image: the picture docs/guides/ui.md shows.

#include <cy/abi/cy_abi.h>
#include <cy/abi/host.h>
#include <cy/abi/module.h>
#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/game_backend/ui_backend.h>
#include <cy/test/test.h>
#include <cy/ui/layout.h>
#include <cy/ui/render/ui_renderer.h>
#include <cy/ui/text/builtin_font.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "frame_scene.h"
#include "golden.h"
#include "hud.h"

using namespace cy;
using cy::pipeline_test::FrameScene;
using cy::pipeline_test::FrameSceneHooks;
using cy::pipeline_test::kHeight;
using cy::pipeline_test::kWidth;
using cy::pipeline_test::RecordMode;

namespace {

constexpr u16 kGlyphPage = 1;
constexpr Vec2 kViewport{static_cast<f32>(kWidth), static_cast<f32>(kHeight)};

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

// --- The game module, once per process
// -------------------------------------------------------------

/// The Swift module, loaded once: a hot-reload generation is a different file, and this one is
/// never reloaded. Behaviours are created on bare entities of a world nothing else uses.
class GameModule {
public:
    /// Never destroyed: the image is never unloaded either (cy/abi/module.h), and an instance
    /// destroyed through a Swift vtable during static destruction would run after the process
    /// stopped being one anybody can debug.
    static GameModule& get() noexcept {
        static GameModule* module = new GameModule();
        return *module;
    }

    [[nodiscard]] bool loaded() const noexcept { return loaded_; }
    [[nodiscard]] const char* failure() const noexcept { return failure_; }

    /// Create `type` on a fresh entity with `ui` as the interface, and unbind it again: the
    /// behaviour's `onCreate` does all its interface work at once.
    [[nodiscard]] bool create(const char* type, game_backend::UiAdapter& ui) noexcept {
        const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
        const CyEntity entity = iface->world_create_entity(&binding_);
        game_backend::bind(host_, &ui);
        const Expected<u32, Error> made = runtime_.create(type, entity);
        game_backend::bind(host_, nullptr);
        return made.has_value() && runtime_.instance(*made) != nullptr;
    }

    GameModule(const GameModule&) = delete;
    GameModule& operator=(const GameModule&) = delete;

private:
    GameModule() noexcept
        : world_(system_allocator(MemoryDomain::Scripting)),
          binding_(system_allocator(MemoryDomain::Scripting), world_),
          host_(system_allocator(MemoryDomain::Scripting)),
          runtime_(system_allocator(MemoryDomain::Scripting), host_),
          manifest_text_(system_allocator(MemoryDomain::Scripting)) {
        host_.bind_world(&binding_);
        loaded_ = load();
    }

    [[nodiscard]] bool load() noexcept {
        std::FILE* file = std::fopen(CY_RTS_API_MODULE_MANIFEST, "rb");
        if (file == nullptr) {
            failure_ = "the module manifest could not be opened";
            return false;
        }
        char chunk[512];
        usize read = 0;
        while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0) {
            for (usize index = 0; index < read; ++index) {
                (void)manifest_text_.push_back(chunk[index]);
            }
        }
        (void)std::fclose(file);
        (void)manifest_text_.push_back('\0');
        Expected<abi::ModuleManifest, Error> parsed =
            abi::parse_module_manifest(manifest_text_.data());
        if (!parsed.has_value()) {
            failure_ = parsed.error().message;
            return false;
        }
        manifest_ = *parsed;
        const auto brought_up = runtime_.load(manifest_, CY_RTS_API_MODULE_LIBRARY);
        if (!brought_up.has_value()) {
            failure_ = brought_up.error().message;
            return false;
        }
        return true;
    }

    ecs::World world_;
    abi::World binding_;
    abi::Host host_;
    abi::BehaviourRuntime runtime_;
    Array<char> manifest_text_;
    abi::ModuleManifest manifest_;
    bool loaded_ = false;
    const char* failure_ = "";
};

// --- One interface, built one of two ways
// ---------------------------------------------------------

/// A screen root in a store, text on the built-in font, and the adapter a Swift module reaches it
/// through: everything a frame of interface needs except the device.
struct Interface {
    Interface() noexcept {
        started = server.start(cy::text::TextServerConfig{}).has_value() &&
                  text.start(server, ui::builtin_font(), kGlyphPage).has_value() &&
                  root.is_valid() && adapter.start().has_value();
    }

    /// The Swift HUD: `type` created with this interface bound.
    [[nodiscard]] bool build_swift(const char* type) noexcept {
        return started && GameModule::get().loaded() && GameModule::get().create(type, adapter);
    }

    /// samples/13-rts-selection's C++ HUD with render.ui's numbers, and no console.
    [[nodiscard]] bool build_cpp() noexcept {
        if (!started || !hud.create(store, text, root).has_value() ||
            !hud.set_resources(sample::rts::Resources{1250, 830, 42, 60}).has_value()) {
            return false;
        }
        const sample::rts::SelectedUnit units[] = {
            {"Rifleman", 100, 100}, {"Rifleman", 64, 100}, {"Engineer", 30, 80}};
        const sample::rts::MinimapDot dots[] = {
            {0.30F, 0.62F, sample::rts::Team::Player}, {0.34F, 0.66F, sample::rts::Team::Player},
            {0.27F, 0.70F, sample::rts::Team::Player}, {0.62F, 0.58F, sample::rts::Team::Enemy},
            {0.70F, 0.52F, sample::rts::Team::Enemy},  {0.15F, 0.25F, sample::rts::Team::Neutral}};
        return hud.set_selection(Span<const sample::rts::SelectedUnit>(units, 3)).has_value() &&
               hud.set_minimap(Span<const sample::rts::MinimapDot>(dots, 6),
                               ui::Rect{-0.1F, 0.45F, 0.5F, 0.4F})
                   .has_value();
    }

    [[nodiscard]] bool flatten() noexcept {
        ui::ScaleSettings settings;
        settings.mode = ui::ScaleMode::FixedPixel;
        ui::LayoutReport laid{};
        ui::FlattenReport flattened{};
        return ui::layout(store, settings, kViewport, &text, laid).has_value() &&
               ui::flatten(store, ui::Rect{0.0F, 0.0F, kViewport.x, kViewport.y}, buffer, flattened,
                           &text)
                   .has_value();
    }

    cy::text::TextServer server;
    ui::TextPainter text{allocator()};
    ui::ElementStore store{allocator()};
    ui::ElementId root = make_root(store);
    game_backend::UiAdapter adapter{allocator(), store, text, root};
    sample::rts::Hud hud;
    ui::PrimitiveBuffer buffer{allocator()};
    bool started = false;

private:
    static ui::ElementId make_root(ui::ElementStore& in) noexcept {
        auto made = in.create(ui::kNoElement, Name::intern("screen"));
        if (!made.has_value()) {
            return ui::kNoElement;
        }
        in.layout_input(*made)->model = ui::LayoutModel::Absolute;
        return *made;
    }
};

[[nodiscard]] bool same_rect(const ui::Rect& x, const ui::Rect& y) noexcept {
    return x.x == y.x && x.y == y.y && x.width == y.width && x.height == y.height;
}

/// Every field a primitive draws with; `source` is an element id, which is the store's business.
[[nodiscard]] bool same_drawing(const ui::Primitive& a, const ui::Primitive& b) noexcept {
    return same_rect(a.bounds, b.bounds) && same_rect(a.uv, b.uv) && a.material == b.material &&
           a.clip == b.clip && a.transform == b.transform && a.atlas == b.atlas &&
           a.colour == b.colour && a.corner_radius == b.corner_radius &&
           a.border_width == b.border_width && a.border_colour == b.border_colour;
}

// --- The device
// --------------------------------------------------------------------------------------

void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
    }
    std::fprintf(stderr, "graphics validation: %s\n", message != nullptr ? message : "");
}

class DeviceFixture {
public:
    DeviceFixture() noexcept : allocator_(system_allocator(MemoryDomain::Gpu)) {
        (void)rhi::vulkan::register_vulkan_backend();
        (void)rhi::null::register_null_backend();
        rhi::DeviceDescription description;
        description.application_name = "cy_test_render_rts_api_hud";
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
        std::fprintf(stderr, "no requested graphics device on this machine; selected '%s': %s\n",
                     selection_.selected != nullptr ? selection_.selected : "(none)",
                     selection_.reason != nullptr ? selection_.reason : "(no reason given)");
    }

private:
    Allocator& allocator_;
    rhi::BackendSelection selection_{};
    u32 errors_ = 0;
    Expected<rhi::Device*, Error> device_ = fail(ErrorCode::Unavailable, "not created");
};

/// `interface` drawn over the scene at the frame's interface stage; the frame's pixels in `out`.
[[nodiscard]] bool draw_frame(DeviceFixture& fixture, Interface& interface, std::vector<u32>& out) {
    FrameScene scene(allocator());
    ui::render::UiRenderer renderer(allocator());
    struct Hook {
        ui::render::UiRenderer* renderer;
        static Status before_assemble(rendering::RenderGraph& /*graph*/,
                                      rendering::assembly::AssemblyView& /*view*/,
                                      rendering::assembly::FrameSinks& sinks, void* user) noexcept {
            sinks.ui = static_cast<Hook*>(user)->renderer->stage();
            return ok();
        }
    } hook{&renderer};
    FrameSceneHooks hooks;
    hooks.user = &hook;
    hooks.before_assemble = &Hook::before_assemble;
    scene.set_hooks(hooks);
    bool drawn = scene.build(fixture.device()).has_value();
    scene.set_read_back(true);
    ui::render::UiRendererDescription description;
    description.width = kWidth;
    description.height = kHeight;
    description.output_format = scene.pipelines().setup().output_format;
    const u32 extent = interface.text.atlas_extent();
    drawn = drawn && renderer.create(fixture.device(), description).has_value() &&
            renderer
                .upload_atlas(kGlyphPage, rhi::Format::R8Unorm, extent, extent,
                              interface.text.atlas_pixels())
                .has_value() &&
            renderer.submit(interface.buffer, 1.0F).has_value();
    rendering::assembly::AssemblyReport report{};
    drawn = drawn && scene.render(RecordMode::Callbacks, report).has_value() &&
            fixture.device().wait_idle().has_value();
    if (drawn) {
        out.assign(scene.pixels().begin(), scene.pixels().end());
    }
    (void)fixture.device().wait_idle();
    renderer.destroy();
    scene.release();
    return drawn && out.size() == static_cast<usize>(kWidth) * kHeight;
}

const char* artefact_directory() noexcept {
    const char* directory = std::getenv("CY_TEST_ARTEFACT_DIR");
    return (directory == nullptr || *directory == '\0') ? CY_TEST_BINARY_DIR : directory;
}

[[nodiscard]] bool to_image(const std::vector<u32>& texels, render_test::Image& out) noexcept {
    return render_test::adopt(out, Span<const u32>(texels.data(), texels.size()), kWidth, kHeight)
        .has_value();
}

void save(const char* name, const std::vector<u32>& texels) noexcept {
    render_test::Image image(allocator());
    char path[1024];
    (void)std::snprintf(path, sizeof(path), "%s/%s", artefact_directory(), name);
    if (to_image(texels, image) && render_test::write_png(path, image).has_value()) {
        std::fprintf(stderr, "wrote %s\n", path);
    }
}

bool updating_references() noexcept {
    const char* value = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

}  // namespace

CY_TEST_CASE(
    "(a) the Swift HUD flattens to the C++ HUD's primitive stream, primitive for "
    "primitive") {
    if (!GameModule::get().loaded()) {
        std::fprintf(stderr, "the game module did not load: %s\n", GameModule::get().failure());
    }
    CY_REQUIRE(GameModule::get().loaded());
    Interface swift;
    Interface cpp;
    CY_REQUIRE(swift.build_swift("HudShowcase"));
    CY_REQUIRE(cpp.build_cpp());
    CY_REQUIRE(swift.flatten());
    CY_REQUIRE(cpp.flatten());

    // The Swift HUD made its elements through the ABI: 40 of them, every one the module's.
    CY_CHECK_EQ(swift.adapter.elements(), 40U);
    const Span<const ui::Primitive> mine = swift.buffer.primitives();
    const Span<const ui::Primitive> theirs = cpp.buffer.primitives();
    std::fprintf(stderr, "primitives: swift %zu, c++ %zu; batches: %zu, %zu\n", mine.size(),
                 theirs.size(), swift.buffer.batches().size(), cpp.buffer.batches().size());
    // Something was drawn — resources, panels, glyphs — and it is exactly what C++ draws.
    CY_CHECK_GT(theirs.size(), 80U);
    CY_REQUIRE_EQ(mine.size(), theirs.size());
    u32 different = 0;
    for (usize index = 0; index < mine.size(); ++index) {
        if (!same_drawing(mine[index], theirs[index])) {
            if (different == 0U) {
                std::fprintf(stderr,
                             "first difference at primitive %zu: (%.2f, %.2f %.2fx%.2f) %08X "
                             "against (%.2f, %.2f %.2fx%.2f) %08X\n",
                             index, static_cast<double>(mine[index].bounds.x),
                             static_cast<double>(mine[index].bounds.y),
                             static_cast<double>(mine[index].bounds.width),
                             static_cast<double>(mine[index].bounds.height), mine[index].colour,
                             static_cast<double>(theirs[index].bounds.x),
                             static_cast<double>(theirs[index].bounds.y),
                             static_cast<double>(theirs[index].bounds.width),
                             static_cast<double>(theirs[index].bounds.height),
                             theirs[index].colour);
            }
            ++different;
        }
    }
    CY_CHECK_EQ(different, 0U);
    CY_REQUIRE_EQ(swift.buffer.clips().size(), cpp.buffer.clips().size());
    for (usize index = 0; index < swift.buffer.clips().size(); ++index) {
        CY_CHECK(same_rect(swift.buffer.clips()[index], cpp.buffer.clips()[index]));
    }
    CY_CHECK_EQ(swift.buffer.batches().size(), cpp.buffer.batches().size());
}

CY_TEST_CASE("(b) on a device the Swift HUD and the C++ HUD are the same frame, byte for byte") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    CY_REQUIRE(GameModule::get().loaded());
    Interface swift;
    Interface cpp;
    CY_REQUIRE(swift.build_swift("HudShowcase"));
    CY_REQUIRE(cpp.build_cpp());
    CY_REQUIRE(swift.flatten());
    CY_REQUIRE(cpp.flatten());
    std::vector<u32> swift_frame;
    std::vector<u32> cpp_frame;
    CY_REQUIRE(draw_frame(fixture, swift, swift_frame));
    CY_REQUIRE(draw_frame(fixture, cpp, cpp_frame));
    save("rts-api-hud-swift.png", swift_frame);
    save("rts-api-hud-cpp.png", cpp_frame);

    render_test::Image reference(allocator());
    render_test::Image candidate(allocator());
    CY_REQUIRE(to_image(cpp_frame, reference));
    CY_REQUIRE(to_image(swift_frame, candidate));
    const render_test::Comparison comparison = render_test::compare(reference, candidate);
    std::fprintf(stderr, "swift against c++: %u texels differ, the largest by %u at (%u, %u)\n",
                 comparison.differing, comparison.max_channel_delta, comparison.worst_x,
                 comparison.worst_y);
    CY_CHECK(comparison.comparable);
    CY_CHECK_EQ(comparison.differing, 0U);
    CY_CHECK_EQ(comparison.max_channel_delta, 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(c) the Swift HUD with its Build button, against its golden image") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    CY_REQUIRE(GameModule::get().loaded());
    Interface swift;
    CY_REQUIRE(swift.build_swift("HudShowcaseWithButton"));
    CY_CHECK_EQ(swift.adapter.elements(), 41U);
    CY_REQUIRE(swift.flatten());
    std::vector<u32> frame;
    CY_REQUIRE(draw_frame(fixture, swift, frame));
    save("rts-api-hud.png", frame);

    render_test::Image rendered(allocator());
    CY_REQUIRE(to_image(frame, rendered));
    char path[1024];
    (void)std::snprintf(path, sizeof(path), "%s/references/rts_api_hud.png", CY_RTS_API_TEST_DIR);
    if (updating_references()) {
        CY_CHECK(render_test::write_png(path, rendered).has_value());
        std::fprintf(stderr, "wrote %s — look at it, then commit it. This run FAILS on purpose.\n",
                     path);
        CY_TEST_FAIL_CHECK("references were regenerated; this mode never passes");
        return;
    }
    render_test::Image reference(allocator());
    CY_REQUIRE(render_test::read_png(path, reference).has_value());
    const render_test::Comparison comparison = render_test::compare(reference, rendered);
    std::fprintf(stderr, "rts_api_hud.png: %u texels differ, the largest by %u at (%u, %u)\n",
                 comparison.differing, comparison.max_channel_delta, comparison.worst_x,
                 comparison.worst_y);
    CY_CHECK(comparison.comparable);
    CY_CHECK_EQ(comparison.differing, 0U);
    CY_CHECK_EQ(comparison.max_channel_delta, 0U);
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
