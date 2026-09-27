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
//
// The GAME side is what a strategy game does: the drag box and the cursor are screen positions, the
// units they catch are found from the units' own bounds, and the result is written as a
// `SelectionHighlight` component on each unit's ENTITY — selected in the team's colour for the
// squad, hovered for the enemy under the cursor. The ENGINE side gathers those components into the
// `HighlightSet` the outline pass draws (`gather_highlights`), keyed by the entity's identity,
// which is the identity each unit's draws carry.
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

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/rendering/selection/outline_pass.h>
#include <cy/rendering/selection/selection_component.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
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
};

struct Frame {
    Game* game = nullptr;
    OutlinePass* pass = nullptr;
    HighlightSet* highlights = nullptr;
    FrameScene* scene = nullptr;
    OutlineSettings settings{};
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
    (void)device.wait_idle();
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
