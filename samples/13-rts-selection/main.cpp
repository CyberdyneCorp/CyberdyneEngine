// SPDX-License-Identifier: MIT
// `cy_sample_rts_selection` — a strategy game's selection, drawn by the engine.
//
// ================================================================================================
// WHAT IT DOES
// ================================================================================================
//
//   cy_sample_rts_selection --out <dir> [--validate]
//
// A small field — a squad of four units, a building one of them has walked behind, three enemy
// units and a neutral one far back — rendered through the engine's own assembled frame, twice:
//
//   1. nothing selected                                         <dir>/rts-selection-before.png
//   2. the player drags a box over the squad and rests the      <dir>/rts-selection-after.png
//      cursor on an enemy                                       <dir>/rts-selection-detail.png
//   3. the same frame with the game's HUD and the developer     <dir>/rts-hud.png
//      console drawn over it by the engine's interface pass     <dir>/rts-hud-2x.png
//
// The GAME side is what a strategy game does: the drag box and the cursor are screen positions, the
// units they catch are found from the units' own bounds, and the result is written as a
// `SelectionHighlight` component on each unit's ENTITY — selected in the team's colour for the
// squad, hovered for the enemy under the cursor. The ENGINE side gathers those components into the
// `HighlightSet` the outline pass draws (`gather_highlights`), keyed by the entity's identity,
// which is the identity each unit's draws carry.
//
// The HUD is game code too (hud.h): a resource bar, a minimap with the camera's rectangle and a
// panel listing the units the drag box caught, built on CyberUI's `ElementStore` with the engine's
// developer console beside it, laid out, flattened and drawn by `ui::render::UiRenderer` at the
// frame's interface stage — after the tone curve and the outlines.
//
// The frame is `pipeline_test::FrameScene` — the scene the pipeline suites render, whose recorder,
// pipelines and bindings are the engine's — with its ring of boxes arranged as the field and its
// draws keyed by the entities' bits. `tests/render/golden.cpp` writes the PNGs, as it does for
// samples/12-beauty.
//
// No CTest entry: the picture needs a graphics device, and on a machine without one the program
// says which device it did not find and writes nothing. `render.selection_outlines` is the suite.

#include "frame_scene.h"
#include "golden.h"
#include "hud.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/rendering/selection/outline_pass.h>
#include <cy/rendering/selection/selection_component.h>
#include <cy/ui/console/console.h>
#include <cy/ui/layout.h>
#include <cy/ui/render/ui_renderer.h>
#include <cy/ui/text/builtin_font.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace cy;
using cy::pipeline_test::FrameScene;
using cy::pipeline_test::FrameSceneHooks;
using cy::pipeline_test::kHeight;
using cy::pipeline_test::kInstanceCount;
using cy::pipeline_test::kWidth;
using cy::pipeline_test::RecordMode;
using namespace cy::rendering::selection;

constexpr f32 kFloorTop = -1.9F + 0.125F;
constexpr f32 kUnit = 0.35F;

enum class Side : u8 { None, Player, Enemy, Neutral, Building };

struct Placement {
    Vec3 centre{};
    f32 half = 0.0F;
    Side side = Side::None;
};

/// Boxes 1 to 9 of the scene's ring; 10 and 11 are moved out of view.
const Placement kField[] = {
    {Vec3{-2.6F, kFloorTop + kUnit, -7.2F}, kUnit, Side::Player},
    {Vec3{-1.5F, kFloorTop + kUnit, -6.4F}, kUnit, Side::Player},
    {Vec3{-1.9F, kFloorTop + kUnit, -8.6F}, kUnit, Side::Player},
    // Behind the building's right edge: its lower left is hidden.
    {Vec3{1.05F, kFloorTop + kUnit, -10.2F}, kUnit, Side::Player},
    {Vec3{0.2F, kFloorTop + 0.6F, -8.0F}, 0.6F, Side::Building},
    {Vec3{2.5F, kFloorTop + kUnit, -6.6F}, kUnit, Side::Enemy},
    {Vec3{3.3F, kFloorTop + kUnit, -9.0F}, kUnit, Side::Enemy},
    {Vec3{4.1F, kFloorTop + kUnit, -11.8F}, kUnit, Side::Enemy},
    {Vec3{-4.6F, kFloorTop + kUnit, -16.0F}, kUnit, Side::Neutral},
};
constexpr u32 kFieldCount = sizeof(kField) / sizeof(kField[0]);

constexpr HighlightColour kTeamBlue{40, 140, 255, 255};
constexpr HighlightColour kEnemyHover{255, 96, 64, 255};

/// The player's input this frame, in pixels: a drag box and the cursor.
constexpr f32 kDragMin[2] = {110.0F, 165.0F};
constexpr f32 kDragMax[2] = {300.0F, 222.0F};
constexpr f32 kCursor[2] = {346.0F, 196.0F};

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Renderer);
}

u32 g_validation_errors = 0;

void count_validation(rhi::ValidationSeverity severity, const char* message, void* user) noexcept {
    if (severity == rhi::ValidationSeverity::Error && user != nullptr) {
        ++*static_cast<u32*>(user);
        std::fprintf(stderr, "graphics validation error: %s\n", message != nullptr ? message : "");
    }
}

/// The game's world: one entity per box, and the selection component registered.
struct Game {
    ecs::World world{allocator()};
    ecs::ComponentTypeId highlight = ecs::kInvalidComponent;
    ecs::Entity entities[kInstanceCount] = {};
    /// The boxes the drag box caught, in field order: what the HUD's selection panel lists.
    u32 selected[kFieldCount] = {};
    u32 selected_count = 0;
};

/// What each of the player's units is called and how hurt it is, by field index.
struct UnitCard {
    const char* name;
    u32 health;
    u32 max_health;
};
const UnitCard kPlayerUnits[] = {
    {"Rifleman", 100, 100}, {"Rifleman", 64, 100}, {"Engineer", 30, 80}, {"Scout", 55, 60}};

struct Frame {
    Game* game = nullptr;
    OutlinePass* pass = nullptr;
    HighlightSet* highlights = nullptr;
    FrameScene* scene = nullptr;
    OutlineSettings settings{};
    /// The interface's renderer, once the HUD is on: the frame's `UiAndDebug` stage.
    ui::render::UiRenderer* ui_pass = nullptr;
};

void place_box(u32 which, Vec3& centre, f32& half, void* /*user*/) noexcept {
    if (which >= 1U && which <= kFieldCount) {
        centre = kField[which - 1U].centre;
        half = kField[which - 1U].half;
        return;
    }
    centre = Vec3{static_cast<f32>(which) * 3.0F, 0.0F, 60.0F};
    half = 0.3F;
}

/// Each box's draws carry its entity's identity — what the extract stage gives a real entity's.
u64 stable_id(u32 which, void* user) noexcept {
    const auto* frame = static_cast<const Frame*>(user);
    return frame->game->entities[which].bits();
}

void configure(rendering::assembly::AssemblyDescription& description, void* /*user*/) noexcept {
    description.pin_jitter = true;
    description.selection_outlines = true;
}

Status before_assemble(rendering::RenderGraph& /*graph*/,
                       rendering::assembly::AssemblyView& /*view*/,
                       rendering::assembly::FrameSinks& sinks, void* user) noexcept {
    auto* frame = static_cast<Frame*>(user);
    if (Status set = frame->pass->set_highlights(frame->highlights, frame->settings); !set) {
        return set;
    }
    sinks.selection_outlines = frame->pass->stage(frame->scene->recorder());
    if (frame->ui_pass != nullptr) {
        sinks.ui = frame->ui_pass->stage();
    }
    return ok();
}

/// A box's screen rectangle from its eight corners: what a game's box selection tests against.
void screen_rect(FrameScene& scene, u32 which, f32 (&min)[2], f32 (&max)[2]) noexcept {
    const Aabb& box = scene.boxes()[which];
    const Mat4 clip = scene.projection() * scene.view();
    min[0] = min[1] = INFINITY;
    max[0] = max[1] = -INFINITY;
    for (u32 corner = 0; corner < 8U; ++corner) {
        const Vec4 point{(corner & 1U) != 0 ? box.max.x : box.min.x,
                         (corner & 2U) != 0 ? box.max.y : box.min.y,
                         (corner & 4U) != 0 ? box.max.z : box.min.z, 1.0F};
        const Vec4 projected = clip * point;
        const f32 x = (((projected.x / projected.w) * 0.5F) + 0.5F) * static_cast<f32>(kWidth);
        const f32 y = (0.5F - ((projected.y / projected.w) * 0.5F)) * static_cast<f32>(kHeight);
        min[0] = std::min(min[0], x);
        min[1] = std::min(min[1], y);
        max[0] = std::max(max[0], x);
        max[1] = std::max(max[1], y);
    }
}

/// The game's selection logic: the player's units whose screen centre is inside the drag box are
/// selected; the nearest enemy whose rectangle holds the cursor is hovered.
Status apply_input(Game& game, FrameScene& scene) noexcept {
    u32 hovered = 0;
    f32 hovered_depth = INFINITY;
    for (u32 which = 1; which <= kFieldCount; ++which) {
        f32 min[2];
        f32 max[2];
        screen_rect(scene, which, min, max);
        const f32 centre[2] = {(min[0] + max[0]) * 0.5F, (min[1] + max[1]) * 0.5F};
        const Side side = kField[which - 1U].side;
        if (side == Side::Player && centre[0] >= kDragMin[0] && centre[0] <= kDragMax[0] &&
            centre[1] >= kDragMin[1] && centre[1] <= kDragMax[1]) {
            const SelectionHighlight mark = SelectionHighlight::selected(kTeamBlue);
            if (Status added = game.world.add(game.entities[which], game.highlight, &mark);
                !added) {
                return added;
            }
            game.selected[game.selected_count++] = which;
        }
        const f32 depth = -kField[which - 1U].centre.z;
        if (side == Side::Enemy && kCursor[0] >= min[0] && kCursor[0] <= max[0] &&
            kCursor[1] >= min[1] && kCursor[1] <= max[1] && depth < hovered_depth) {
            hovered = which;
            hovered_depth = depth;
        }
    }
    if (hovered != 0) {
        const SelectionHighlight mark = SelectionHighlight::hovered(kEnemyHover);
        return game.world.add(game.entities[hovered], game.highlight, &mark);
    }
    return ok();
}

bool save(const std::string& path, Span<const u32> texels, u32 width, u32 height) {
    render_test::Image image(allocator());
    if (!render_test::adopt(image, texels, width, height).has_value()) {
        return false;
    }
    if (!render_test::write_png(path.c_str(), image).has_value()) {
        std::fprintf(stderr, "could not write %s\n", path.c_str());
        return false;
    }
    std::printf("wrote %s (%ux%u)\n", path.c_str(), width, height);
    return true;
}

/// The building and the unit behind it, enlarged four times with nearest-neighbour sampling so the
/// dashed occluded outline and the tint read at a glance.
std::vector<u32> detail(Span<const u32> pixels, u32& width, u32& height) {
    constexpr u32 kScale = 4;
    constexpr u32 kX0 = 200;
    constexpr u32 kY0 = 140;
    constexpr u32 kCropWidth = 110;
    constexpr u32 kCropHeight = 80;
    width = kCropWidth * kScale;
    height = kCropHeight * kScale;
    std::vector<u32> out(static_cast<usize>(width) * height);
    for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
            const usize source =
                (static_cast<usize>(kY0 + (y / kScale)) * kWidth) + kX0 + (x / kScale);
            out[(static_cast<usize>(y) * width) + x] = pixels[source];
        }
    }
    return out;
}

std::string option(int argc, char** argv, const char* name, const char* fallback) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::strcmp(argv[index], name) == 0) {
            return argv[index + 1];
        }
    }
    return fallback;
}

bool flag(int argc, char** argv, const char* name) {
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], name) == 0) {
            return true;
        }
    }
    return false;
}

// --- The HUD ------------------------------------------------------------------------------------

/// The interface's half of the game: its store, its text, the HUD and the console, and the pass
/// that draws them.
struct Interface {
    cy::text::TextServer server;
    ui::TextPainter text{allocator()};
    ui::ElementStore store{allocator()};
    ui::PrimitiveBuffer buffer{allocator()};
    ui::render::UiRenderer renderer{allocator()};
    sample::rts::Hud hud;
    ui::DevConsole console{allocator()};
    ui::ElementId root;
};

constexpr u16 kGlyphPage = 1;

ui::ConsoleReply report_selection(std::string_view /*arguments*/, void* user) noexcept {
    static char line[48];
    const auto* game = static_cast<const Game*>(user);
    const int length =
        std::snprintf(line, sizeof(line), "squad selected: %u units", game->selected_count);
    return ui::ConsoleReply{std::string_view(line, length > 0 ? static_cast<usize>(length) : 0U),
                            ui::kConsoleEcho};
}

/// The field's units on the minimap: x across, depth down — the far end of the field at the top.
sample::rts::MinimapDot dot_for(const Placement& placement) noexcept {
    sample::rts::MinimapDot dot;
    dot.x = (placement.centre.x + 6.0F) / 12.0F;
    dot.y = (placement.centre.z + 20.0F) / 16.0F;
    dot.team = placement.side == Side::Enemy     ? sample::rts::Team::Enemy
               : placement.side == Side::Neutral ? sample::rts::Team::Neutral
                                                 : sample::rts::Team::Player;
    return dot;
}

/// Build the HUD and the console from the game's state, lay them out and flatten them.
Status build_interface(Interface& ui_state, rhi::Device& device, Game& game,
                       const FrameScene& scene) noexcept {
    ui::render::UiRendererDescription description;
    description.width = kWidth;
    description.height = kHeight;
    description.output_format = scene.pipelines().setup().output_format;
    if (Status made = ui_state.renderer.create(device, description); !made) {
        return made;
    }
    if (Status started = ui_state.server.start(cy::text::TextServerConfig{}); !started) {
        return started;
    }
    if (Status started = ui_state.text.start(ui_state.server, ui::builtin_font(), kGlyphPage);
        !started) {
        return started;
    }
    const u32 extent = ui_state.text.atlas_extent();
    if (Status uploaded = ui_state.renderer.upload_atlas(kGlyphPage, rhi::Format::R8Unorm, extent,
                                                         extent, ui_state.text.atlas_pixels());
        !uploaded) {
        return uploaded;
    }
    Expected<ui::ElementId, Error> root =
        ui_state.store.create(ui::kNoElement, Name::intern("hud"));
    if (!root.has_value()) {
        return make_unexpected(root.error());
    }
    ui_state.root = *root;
    ui_state.store.layout_input(ui_state.root)->model = ui::LayoutModel::Absolute;
    if (Status made = ui_state.hud.create(ui_state.store, ui_state.text, ui_state.root); !made) {
        return made;
    }

    // THE GAME'S STATE, AS THE HUD SHOWS IT.
    if (Status set = ui_state.hud.set_resources(sample::rts::Resources{1250, 830, 42, 60}); !set) {
        return set;
    }
    sample::rts::SelectedUnit units[kFieldCount];
    for (u32 index = 0; index < game.selected_count; ++index) {
        const UnitCard& card = kPlayerUnits[game.selected[index] - 1U];
        units[index] = sample::rts::SelectedUnit{card.name, card.health, card.max_health};
    }
    if (Status set = ui_state.hud.set_selection(
            Span<const sample::rts::SelectedUnit>(units, game.selected_count));
        !set) {
        return set;
    }
    sample::rts::MinimapDot dots[kFieldCount];
    u32 dot_count = 0;
    for (const Placement& placement : kField) {
        if (placement.side != Side::Building) {
            dots[dot_count++] = dot_for(placement);
        }
    }
    // The camera sees most of the field and a little past its near edge, which the minimap clips.
    if (Status set = ui_state.hud.set_minimap(Span<const sample::rts::MinimapDot>(dots, dot_count),
                                              ui::Rect{0.12F, 0.25F, 0.76F, 0.8F});
        !set) {
        return set;
    }

    ui::ConsoleStyle style;
    style.offset_min = Vec2{6.0F, 22.0F};
    style.offset_max = Vec2{300.0F, 22.0F + (4.0F * 13.0F) + 10.0F};
    style.anchor_max = Vec2{0.0F, 0.0F};
    style.visible_rows = 3;
    if (Status made = ui_state.console.create(ui_state.store, ui_state.text, ui_state.root, style);
        !made) {
        return made;
    }
    if (Status added = ui_state.console.add_command("selection", &report_selection, &game);
        !added) {
        return added;
    }
    // The player opens the console and asks what is selected.
    if (Status printed = ui_state.console.print("CyberUI console - type help"); !printed) {
        return printed;
    }
    if (Status typed = ui_state.console.type("selection"); !typed) {
        return typed;
    }
    if (Status submitted = ui_state.console.submit(); !submitted) {
        return submitted;
    }
    if (Status typed = ui_state.console.type("spawn tank"); !typed) {
        return typed;
    }

    ui::ScaleSettings scale;
    scale.mode = ui::ScaleMode::FixedPixel;
    ui::LayoutReport laid{};
    const Vec2 viewport{static_cast<f32>(kWidth), static_cast<f32>(kHeight)};
    if (Status done = ui::layout(ui_state.store, scale, viewport, &ui_state.text, laid); !done) {
        return done;
    }
    ui::FlattenReport flattened{};
    if (Status done = ui::flatten(ui_state.store, ui::Rect{0.0F, 0.0F, viewport.x, viewport.y},
                                  ui_state.buffer, flattened, &ui_state.text);
        !done) {
        return done;
    }
    std::printf("interface: %zu primitives in %u batches\n", ui_state.buffer.primitives().size(),
                flattened.batches);
    return ui_state.renderer.submit(ui_state.buffer, 1.0F);
}

/// Nearest-neighbour, twice the size: the HUD's one-pixel font read at a glance.
std::vector<u32> doubled(Span<const u32> pixels) {
    std::vector<u32> out(static_cast<usize>(kWidth) * kHeight * 4U);
    for (u32 y = 0; y < kHeight * 2U; ++y) {
        for (u32 x = 0; x < kWidth * 2U; ++x) {
            out[(static_cast<usize>(y) * kWidth * 2U) + x] =
                pixels[(static_cast<usize>(y / 2U) * kWidth) + (x / 2U)];
        }
    }
    return out;
}

int run(rhi::Device& device, const std::string& out) {
    Game game;
    if (!game.world.initialize().has_value()) {
        std::fprintf(stderr, "the game world did not initialise\n");
        return 1;
    }
    Expected<ecs::ComponentTypeId, Error> highlight = register_selection_highlight(game.world);
    if (!highlight.has_value()) {
        std::fprintf(stderr, "%s\n", highlight.error().message);
        return 1;
    }
    game.highlight = *highlight;
    for (ecs::Entity& entity : game.entities) {
        Expected<ecs::Entity, Error> created = game.world.create();
        if (!created.has_value()) {
            std::fprintf(stderr, "%s\n", created.error().message);
            return 1;
        }
        entity = *created;
    }

    FrameScene scene(allocator());
    OutlinePass pass;
    HighlightSet highlights(allocator());
    Frame frame{&game, &pass, &highlights, &scene, OutlineSettings{}};
    FrameSceneHooks hooks;
    hooks.user = &frame;
    hooks.configure = &configure;
    hooks.before_assemble = &before_assemble;
    hooks.place_box = &place_box;
    hooks.stable_id = &stable_id;
    scene.set_hooks(hooks);
    if (Status built = scene.build(device); !built) {
        std::fprintf(stderr, "the scene did not build: %s\n", built.error().message);
        return 1;
    }
    scene.set_read_back(true);
    OutlinePassDescription description;
    description.width = kWidth;
    description.height = kHeight;
    if (Status created = pass.create(device, scene.pipelines(), description); !created) {
        std::fprintf(stderr, "the outline pass was not created: %s\n", created.error().message);
        return 1;
    }

    rendering::assembly::AssemblyReport report{};
    if (Status rendered = scene.render(RecordMode::Callbacks, report); !rendered) {
        std::fprintf(stderr, "the first frame failed: %s\n", rendered.error().message);
        return 1;
    }
    (void)device.wait_idle();
    const std::vector<u32> before(scene.pixels().begin(), scene.pixels().end());

    if (Status applied = apply_input(game, scene); !applied) {
        std::fprintf(stderr, "the selection was not applied: %s\n", applied.error().message);
        return 1;
    }
    Expected<GatherReport, Error> gathered =
        gather_highlights(game.world, game.highlight, highlights);
    if (!gathered.has_value()) {
        std::fprintf(stderr, "the marks were not gathered: %s\n", gathered.error().message);
        return 1;
    }
    if (Status rendered = scene.render(RecordMode::Callbacks, report); !rendered) {
        std::fprintf(stderr, "the second frame failed: %s\n", rendered.error().message);
        return 1;
    }
    (void)device.wait_idle();
    const std::vector<u32> after(scene.pixels().begin(), scene.pixels().end());
    std::printf("marked %u entities; the mask drew %u draws; composited: %s\n", gathered->marked,
                pass.report().marked_draws, pass.report().composited ? "yes" : "no");

    bool wrote = save(out + "/rts-selection-before.png",
                      Span<const u32>(before.data(), before.size()), kWidth, kHeight);
    wrote = save(out + "/rts-selection-after.png", Span<const u32>(after.data(), after.size()),
                 kWidth, kHeight) &&
            wrote;
    u32 width = 0;
    u32 height = 0;
    const std::vector<u32> enlarged =
        detail(Span<const u32>(after.data(), after.size()), width, height);
    wrote = save(out + "/rts-selection-detail.png",
                 Span<const u32>(enlarged.data(), enlarged.size()), width, height) &&
            wrote;

    // THE HUD: the same frame, with the game's interface drawn over it.
    Interface hud_state;
    if (Status built = build_interface(hud_state, device, game, scene); !built) {
        std::fprintf(stderr, "the interface was not built: %s\n", built.error().message);
        return 1;
    }
    frame.ui_pass = &hud_state.renderer;
    if (Status rendered = scene.render(RecordMode::Callbacks, report); !rendered) {
        std::fprintf(stderr, "the HUD frame failed: %s\n", rendered.error().message);
        return 1;
    }
    (void)device.wait_idle();
    std::printf("the interface pass recorded %u draws\n", hud_state.renderer.report().draws);
    const std::vector<u32> with_hud(scene.pixels().begin(), scene.pixels().end());
    wrote = save(out + "/rts-hud.png", Span<const u32>(with_hud.data(), with_hud.size()), kWidth,
                 kHeight) &&
            wrote;
    const std::vector<u32> big = doubled(Span<const u32>(with_hud.data(), with_hud.size()));
    wrote = save(out + "/rts-hud-2x.png", Span<const u32>(big.data(), big.size()), kWidth * 2U,
                 kHeight * 2U) &&
            wrote;

    (void)device.wait_idle();
    frame.ui_pass = nullptr;
    hud_state.renderer.destroy();
    pass.destroy();
    scene.release();
    return wrote ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string out = option(argc, argv, "--out", ".");
    Allocator& gpu = system_allocator(MemoryDomain::Gpu);
    (void)rhi::vulkan::register_vulkan_backend();
    (void)rhi::null::register_null_backend();
    rhi::DeviceDescription description;
    description.application_name = "cy_sample_rts_selection";
    description.enable_validation = flag(argc, argv, "--validate");
    description.enable_synchronisation_validation = description.enable_validation;
    rhi::BackendSelection selection{};
    Expected<rhi::Device*, Error> device =
        rhi::create_device(gpu, "vulkan", description, selection);
    if (!device.has_value() ||
        device.value()->capabilities().backend() != rhi::BackendKind::Vulkan) {
        std::fprintf(stderr,
                     "no Vulkan device on this machine (selected '%s': %s); nothing was written\n",
                     selection.selected != nullptr ? selection.selected : "(none)",
                     selection.reason != nullptr ? selection.reason : "(no reason given)");
        if (device.has_value()) {
            rhi::destroy_device(gpu, device.value());
        }
        return 0;
    }
    device.value()->set_validation_callback(&count_validation, &g_validation_errors);
    const int status = run(*device.value(), out);
    (void)device.value()->wait_idle();
    rhi::destroy_device(gpu, device.value());
    if (g_validation_errors != 0) {
        std::fprintf(stderr, "%u validation errors\n", g_validation_errors);
        return 1;
    }
    return status;
}
