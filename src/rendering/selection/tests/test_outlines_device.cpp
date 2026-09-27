// SPDX-License-Identifier: MIT
// Selection outlines and unit highlights on a Vulkan device, with validation and synchronisation
// validation on. `render.selection_outlines`.
//
// ================================================================================================
// THE SCENE
// ================================================================================================
//
// `pipeline_test::FrameScene` — the one scene the pipeline suites render — with its ring of boxes
// rearranged into a small strategy-game field: five "units" resting on the floor slab and one
// "building" between the camera and the farthest of them.
//
//   A, B   selected, one team colour               box 1, box 2
//   C      hovered, the softer glow                box 3, in front of the building
//   D      selected, another colour, PARTLY behind box 4, whose lower-left the building hides
//   W      the building, never marked              box 5
//   E      an unmarked unit, far back              box 6
//
// Every other box of the ring is moved out of the view. A mark is keyed by the draw's stable
// identity, which `FrameScene` sets to 900 + the box's index.
//
// ================================================================================================
// FIVE PROPERTIES, EACH A CASE
// ================================================================================================
//
//   (a) outline pixels appear exactly on the silhouettes of the marked objects, within the
//       configured width, and nowhere else: every pixel the stage changed is within the width of a
//       marked pixel of the mask, the mask lies on the marked boxes' own projections and on the
//       frame's own silhouettes to the pixel (the scene's depth is never farther than a marked
//       texel's), and the device's decision at every pixel is the host reference's;
//   (b) unmarked objects are untouched: the far unit's every pixel, and every pixel farther than
//       the widest outline from every marked silhouette, is the frame without outlines;
//   (c) colour and width follow the API: a selected outline is the colour asked for, byte for byte,
//       exactly as wide as asked, and follows a change of either; a hovered glow blends toward its
//       colour and falls off;
//   (d) occluded parts show the occluded style only where the unit is hidden: the hidden part of D
//       lies under the building, is the only part of any unit tinted, and only the outline beside
//       it is dashed and dimmed — with the setting off, nothing at all is drawn for it;
//   (e) nothing marked means the frame is byte-identical to before: to a committed reference drawn
//       before this change, and to the frame with the stage absent.
//
// "The frame without outlines" is the same scene rendered again with the stage declared and
// nothing marked, which (e) holds byte-identical to the frame from before the change.

#include "frame_scene.h"
#include "golden.h"

#include <cy/backends/rhi/backend.h>
#include <cy/backends/rhi/null/null_device.h>
#include <cy/backends/rhi/vulkan/vulkan_backend.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/selection/outline_pass.h>
#include <cy/test/test.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <vector>

using namespace cy;
using cy::pipeline_test::FrameScene;
using cy::pipeline_test::FrameSceneHooks;
using cy::pipeline_test::kHeight;
using cy::pipeline_test::kWidth;
using cy::pipeline_test::RecordMode;
using namespace cy::rendering::selection;

namespace {

constexpr usize kPixels = static_cast<usize>(kWidth) * kHeight;
/// The extent as signed pixel coordinates, for the loops that step off it by an offset.
constexpr i32 kColumns = static_cast<i32>(kWidth);
constexpr i32 kRows = static_cast<i32>(kHeight);

/// The stable identities `FrameScene` gives its boxes: 900 + the box's index.
constexpr u64 kUnitA = 901;
constexpr u64 kUnitB = 902;
constexpr u64 kUnitC = 903;
constexpr u64 kUnitD = 904;
constexpr u64 kBuilding = 905;
constexpr u64 kUnitE = 906;

constexpr HighlightColour kTeamBlue{40, 140, 255, 255};
constexpr HighlightColour kTeamRed{235, 60, 45, 255};
constexpr HighlightColour kHoverWhite{255, 244, 214, 255};
constexpr HighlightColour kGold{255, 196, 40, 255};

constexpr f32 kFloorTop = -1.9F + 0.125F;

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
        description.application_name = "cy_test_render_selection_outlines";
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

void save(const char* name, const std::vector<u32>& texels) noexcept {
    render_test::Image image(allocator());
    if (!render_test::adopt(image, Span<const u32>(texels.data(), texels.size()), kWidth, kHeight)
             .has_value()) {
        return;
    }
    const char* directory = std::getenv("CY_TEST_ARTEFACT_DIR");
    if (directory == nullptr || *directory == '\0') {
        directory = CY_TEST_BINARY_DIR;
    }
    char path[1024];
    (void)std::snprintf(path, sizeof(path), "%s/%s", directory, name);
    if (render_test::write_png(path, image).has_value()) {
        std::fprintf(stderr, "wrote %s (%ux%u)\n", path, kWidth, kHeight);
    }
}

/// `references/selection_off.png`: this scene with the stage absent, rendered by the tree as it was
/// before the stage existed — the suite's (e) run once with `CY_RENDER_UPDATE_GOLDEN=1` against
/// main's frame shaders and code, before any of this change was built. The stage adds no field to
/// the frame block and changes no frame shader, so the frame without it is that frame.
const char* before_reference_path() noexcept {
    static char storage[1024];
    (void)std::snprintf(storage, sizeof(storage), "%s/references/selection_off.png",
                        CY_SELECTION_TEST_DIR);
    return storage;
}

bool updating_references() noexcept {
    const char* value = std::getenv("CY_RENDER_UPDATE_GOLDEN");
    return value != nullptr && value[0] != '\0' && value[0] != '0';
}

// --- The frame ---------------------------------------------------------------------------------

/// The field: five units on the floor, one building, and the rest of the ring out of view.
void place_box(u32 which, Vec3& centre, f32& half, void* /*user*/) noexcept {
    struct Placement {
        Vec3 centre{};
        f32 half = 0.0F;
    };
    static const Placement kField[] = {
        {Vec3{-2.4F, kFloorTop + 0.4F, -7.0F}, 0.4F},   // 1: A
        {Vec3{-1.0F, kFloorTop + 0.4F, -7.6F}, 0.4F},   // 2: B
        {Vec3{0.4F, kFloorTop + 0.4F, -6.2F}, 0.4F},    // 3: C
        {Vec3{3.0F, kFloorTop + 0.45F, -9.6F}, 0.45F},  // 4: D
        {Vec3{1.7F, kFloorTop + 0.45F, -7.4F}, 0.45F},  // 5: the building
        {Vec3{0.9F, kFloorTop + 0.4F, -16.0F}, 0.4F},   // 6: E
    };
    if (which >= 1U && which <= 6U) {
        centre = kField[which - 1U].centre;
        half = kField[which - 1U].half;
        return;
    }
    centre = Vec3{static_cast<f32>(which) * 3.0F, 0.0F, 60.0F};
    half = 0.3F;
}

/// What one run marks and how it outlines.
struct RunOptions {
    /// The stage declared at all. Off is the frame from before this change.
    bool attached = true;
    OutlineSettings settings{};
    std::function<void(HighlightSet&)> mark;
};

struct RunState {
    FrameScene* scene = nullptr;
    OutlinePass* pass = nullptr;
    HighlightSet* highlights = nullptr;
    const RunOptions* options = nullptr;
};

void configure_run(rendering::assembly::AssemblyDescription& description, void* user) noexcept {
    const auto* run = static_cast<const RunState*>(user);
    description.pin_jitter = true;
    description.selection_outlines = run->options->attached;
}

Status before_assemble(rendering::RenderGraph& /*graph*/,
                       rendering::assembly::AssemblyView& /*view*/,
                       rendering::assembly::FrameSinks& sinks, void* user) noexcept {
    auto* run = static_cast<RunState*>(user);
    if (!run->options->attached) {
        return ok();
    }
    if (Status set = run->pass->set_highlights(run->highlights, run->options->settings); !set) {
        return set;
    }
    sinks.selection_outlines = run->pass->stage(run->scene->recorder());
    return ok();
}

/// One scene, built with a run's marks, and what it rendered and read back.
class OutlineRun {
public:
    OutlineRun(DeviceFixture& fixture, RunOptions options)
        : device_(&fixture.device()),
          options_(std::move(options)),
          scene_(allocator()),
          highlights_(allocator()) {
        if (options_.mark) {
            options_.mark(highlights_);
        }
        state_ = RunState{&scene_, &pass_, &highlights_, &options_};
        FrameSceneHooks hooks;
        hooks.user = &state_;
        hooks.configure = &configure_run;
        hooks.before_assemble = &before_assemble;
        hooks.place_box = &place_box;
        scene_.set_hooks(hooks);
        const Status built = scene_.build(fixture.device());
        if (!built) {
            std::fprintf(stderr, "selection frame: build failed: %s\n", built.error().message);
            return;
        }
        scene_.set_read_back(true);
        OutlinePassDescription description;
        description.width = kWidth;
        description.height = kHeight;
        description.readback = true;
        const Status created = pass_.create(fixture.device(), scene_.pipelines(), description);
        if (!created) {
            std::fprintf(stderr, "selection frame: pass failed: %s\n", created.error().message);
            return;
        }
        ready_ = true;
    }
    ~OutlineRun() {
        (void)device_->wait_idle();
        pass_.destroy();
        scene_.release();
    }
    OutlineRun(const OutlineRun&) = delete;
    OutlineRun& operator=(const OutlineRun&) = delete;

    [[nodiscard]] bool render() {
        if (!ready_) {
            return false;
        }
        const Status rendered = scene_.render(RecordMode::Callbacks, report_);
        if (!rendered) {
            std::fprintf(stderr, "selection frame: render failed: %s\n", rendered.error().message);
            return false;
        }
        pixels_.assign(scene_.pixels().begin(), scene_.pixels().end());
        if (!options_.attached) {
            return true;
        }
        mask_.assign(kPixels, 0U);
        mask_depth_.assign(kPixels, 0.0F);
        scene_depth_.assign(kPixels, 0.0F);
        OutlineReadback out;
        out.mask = Span<u32>(mask_.data(), mask_.size());
        out.mask_depth = Span<f32>(mask_depth_.data(), mask_depth_.size());
        out.scene_depth = Span<f32>(scene_depth_.data(), scene_depth_.size());
        return pass_.read_back(out).has_value();
    }

    [[nodiscard]] const std::vector<u32>& pixels() const { return pixels_; }
    [[nodiscard]] const std::vector<u32>& mask() const { return mask_; }
    [[nodiscard]] const std::vector<f32>& mask_depth() const { return mask_depth_; }
    [[nodiscard]] const std::vector<f32>& scene_depth() const { return scene_depth_; }
    [[nodiscard]] const HighlightSet& highlights() const { return highlights_; }
    [[nodiscard]] const OutlinePass& pass() const { return pass_; }
    [[nodiscard]] const OutlineSettings& settings() const { return options_.settings; }
    [[nodiscard]] FrameScene& scene() { return scene_; }
    [[nodiscard]] const rendering::assembly::AssemblyReport& report() const { return report_; }

    /// The host reference over what the device read.
    [[nodiscard]] std::vector<OutlineDecision> reference() const {
        std::vector<GpuOutlineStyle> styles(highlights_.style_count());
        highlights_.write_styles(options_.settings,
                                 Span<GpuOutlineStyle>(styles.data(), styles.size()));
        const OutlineInputs inputs{kWidth, kHeight, Span<const u32>(mask_.data(), mask_.size()),
                                   Span<const f32>(mask_depth_.data(), mask_depth_.size()),
                                   Span<const f32>(scene_depth_.data(), scene_depth_.size())};
        std::vector<OutlineDecision> out(kPixels);
        const Status made = outline_reference(
            inputs, Span<const GpuOutlineStyle>(styles.data(), styles.size()),
            make_outline_constants(highlights_, options_.settings, kWidth, kHeight),
            Span<OutlineDecision>(out.data(), out.size()));
        CY_CHECK(made.has_value());
        return out;
    }

private:
    rhi::Device* device_ = nullptr;
    RunOptions options_;
    FrameScene scene_;
    HighlightSet highlights_;
    OutlinePass pass_;
    RunState state_{};
    rendering::assembly::AssemblyReport report_{};
    std::vector<u32> pixels_;
    std::vector<u32> mask_;
    std::vector<f32> mask_depth_;
    std::vector<f32> scene_depth_;
    bool ready_ = false;
};

/// The field as the cases mark it: A and B blue, C hovered, D red.
void mark_field(HighlightSet& set) {
    CY_REQUIRE(set.select(kUnitA, kTeamBlue).has_value());
    CY_REQUIRE(set.select(kUnitB, kTeamBlue).has_value());
    CY_REQUIRE(set.hover(kUnitC, kHoverWhite).has_value());
    CY_REQUIRE(set.select(kUnitD, kTeamRed).has_value());
}

// --- The geometry's answers ----------------------------------------------------------------------

struct PixelRect {
    i32 x0 = 0;
    i32 y0 = 0;
    i32 x1 = 0;
    i32 y1 = 0;

    [[nodiscard]] bool contains(i32 x, i32 y, i32 margin = 0) const noexcept {
        return x >= x0 - margin && x <= x1 + margin && y >= y0 - margin && y <= y1 + margin;
    }
};

/// The pixels a box's eight corners project to, inclusive — the box's silhouette lies within it.
[[nodiscard]] PixelRect projected(FrameScene& scene, u64 identity) noexcept {
    const Aabb& box = scene.boxes()[static_cast<usize>(identity - 900U)];
    const Mat4 clip = scene.projection() * scene.view();
    f32 min_x = INFINITY;
    f32 min_y = INFINITY;
    f32 max_x = -INFINITY;
    f32 max_y = -INFINITY;
    for (u32 corner = 0; corner < 8U; ++corner) {
        const Vec4 point{(corner & 1U) != 0 ? box.max.x : box.min.x,
                         (corner & 2U) != 0 ? box.max.y : box.min.y,
                         (corner & 4U) != 0 ? box.max.z : box.min.z, 1.0F};
        const Vec4 projected = clip * point;
        const f32 x = (((projected.x / projected.w) * 0.5F) + 0.5F) * static_cast<f32>(kWidth);
        const f32 y = (0.5F - ((projected.y / projected.w) * 0.5F)) * static_cast<f32>(kHeight);
        min_x = std::min(min_x, x);
        min_y = std::min(min_y, y);
        max_x = std::max(max_x, x);
        max_y = std::max(max_y, y);
    }
    return PixelRect{static_cast<i32>(std::floor(min_x)), static_cast<i32>(std::floor(min_y)),
                     static_cast<i32>(std::ceil(max_x)), static_cast<i32>(std::ceil(max_y))};
}

[[nodiscard]] usize index_of(i32 x, i32 y) noexcept {
    return (static_cast<usize>(y) * kWidth) + static_cast<usize>(x);
}

/// Squared distance, in whole pixels, from a pixel to the nearest mask pixel of `slot` (any
/// non-zero slot when `slot` is 0), searched out to `reach`; `reach * reach + 1` when none.
[[nodiscard]] i32 distance_to_mask(const std::vector<u32>& mask, i32 x, i32 y, u32 slot,
                                   i32 reach) noexcept {
    i32 best = (reach * reach) + 1;
    for (i32 dy = -reach; dy <= reach; ++dy) {
        for (i32 dx = -reach; dx <= reach; ++dx) {
            const i32 qx = x + dx;
            const i32 qy = y + dy;
            if (qx < 0 || qy < 0 || qx >= kColumns || qy >= kRows) {
                continue;
            }
            const u32 value = mask[index_of(qx, qy)];
            if (value != 0 && (slot == 0 || value == slot)) {
                best = std::min(best, (dx * dx) + (dy * dy));
            }
        }
    }
    return best;
}

[[nodiscard]] bool hidden(const OutlineRun& run, usize index) noexcept {
    const f32 marked = run.mask_depth()[index];
    return run.scene_depth()[index] > marked + (marked * run.settings().depth_tolerance);
}

[[nodiscard]] i32 channel_delta(u32 a, u32 b) noexcept {
    i32 worst = 0;
    for (u32 shift = 0; shift < 32U; shift += 8U) {
        const i32 delta = std::abs(static_cast<i32>((a >> shift) & 0xFFU) -
                                   static_cast<i32>((b >> shift) & 0xFFU));
        worst = std::max(worst, delta);
    }
    return worst;
}

[[nodiscard]] usize differing(const std::vector<u32>& a, const std::vector<u32>& b) noexcept {
    usize count = 0;
    for (usize index = 0; index < a.size() && index < b.size(); ++index) {
        count += static_cast<usize>(a[index] != b[index]);
    }
    return count;
}

/// The field with the stage declared and nothing marked: the frame without outlines.
[[nodiscard]] bool render_plain(DeviceFixture& fixture, std::vector<u32>& out) {
    RunOptions options;
    OutlineRun plain(fixture, options);
    if (!plain.render()) {
        return false;
    }
    out = plain.pixels();
    return true;
}

}  // namespace

CY_TEST_CASE("(e) nothing marked is the frame before the change, byte for byte") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    RunOptions absent;
    absent.attached = false;
    OutlineRun off(fixture, absent);
    CY_REQUIRE(off.render());
    save("selection-outlines-off.png", off.pixels());

    render_test::Image rendered(allocator());
    CY_REQUIRE(
        render_test::adopt(rendered, Span<const u32>(off.pixels().data(), kPixels), kWidth, kHeight)
            .has_value());
    if (updating_references()) {
        CY_CHECK(render_test::write_png(before_reference_path(), rendered).has_value());
        std::fprintf(stderr, "wrote %s — look at it, then commit it. This run FAILS on purpose.\n",
                     before_reference_path());
        CY_TEST_FAIL_CHECK("references were regenerated; this mode never passes");
        return;
    }
    render_test::Image reference(allocator());
    const Status read = render_test::read_png(before_reference_path(), reference);
    if (!read) {
        std::fprintf(stderr, "(e) %s: %s\n", before_reference_path(), read.error().message);
    }
    CY_REQUIRE(read.has_value());
    const render_test::Comparison comparison = render_test::compare(reference, rendered);
    std::fprintf(stderr,
                 "(e) the stage absent against the frame before the change: %u differing, "
                 "worst delta %u\n",
                 comparison.differing, comparison.max_channel_delta);
    CY_CHECK(comparison.comparable);
    CY_CHECK_EQ(comparison.differing, 0U);
    CY_CHECK_EQ(comparison.max_channel_delta, 0U);

    // The stage declared, its mask drawn and nothing marked: nothing composited, nothing changed.
    RunOptions empty;
    OutlineRun unmarked(fixture, empty);
    CY_REQUIRE(unmarked.render());
    CY_CHECK_FALSE(unmarked.pass().report().composited);
    CY_CHECK_EQ(unmarked.pass().report().marked_draws, 0U);
    CY_CHECK_EQ(differing(off.pixels(), unmarked.pixels()), usize{0});
    CY_CHECK_GT(unmarked.report().passes_declared, off.report().passes_declared);

    // Marks on identities the frame does not draw are nothing marked, too.
    RunOptions strangers;
    strangers.mark = [](HighlightSet& set) {
        CY_REQUIRE(set.select(12345, kGold).has_value());
        CY_REQUIRE(set.hover(99999, kHoverWhite).has_value());
    };
    OutlineRun elsewhere(fixture, strangers);
    CY_REQUIRE(elsewhere.render());
    CY_CHECK_EQ(differing(off.pixels(), elsewhere.pixels()), usize{0});

    // The control: the field marked is a different frame.
    RunOptions marked;
    marked.mark = mark_field;
    OutlineRun on(fixture, marked);
    CY_REQUIRE(on.render());
    save("selection-outlines-on.png", on.pixels());
    CY_CHECK(on.pass().report().composited);
    CY_CHECK_GT(differing(off.pixels(), on.pixels()), usize{200});
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE(
    "(a) outline pixels lie on the marked silhouettes, within the width, and nowhere else") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    std::vector<u32> plain;
    CY_REQUIRE(render_plain(fixture, plain));
    RunOptions options;
    options.mark = mark_field;
    OutlineRun run(fixture, options);
    CY_REQUIRE(run.render());
    CY_CHECK_EQ(run.pass().report().marked_draws, 4U);

    // THE MASK IS THE MARKED GEOMETRY. Each marked unit's slot lies within its own box's
    // projection and covers the middle of it; the unmarked building and unit have none.
    const HighlightSet& set = run.highlights();
    for (const u64 unit : {kUnitA, kUnitB, kUnitC, kUnitD}) {
        const PixelRect rect = projected(run.scene(), unit);
        const u32 slot = set.slot_of(unit);
        usize inside = 0;
        for (i32 y = rect.y0; y <= rect.y1; ++y) {
            for (i32 x = rect.x0; x <= rect.x1; ++x) {
                inside += static_cast<usize>(run.mask()[index_of(x, y)] == slot);
            }
        }
        CY_CHECK_GT(inside, usize{100});
        const i32 cx = (rect.x0 + rect.x1) / 2;
        const i32 cy = (rect.y0 + rect.y1) / 2;
        CY_CHECK_EQ(run.mask()[index_of(cx, cy)], slot);
    }
    for (const u64 unmarked : {kBuilding, kUnitE}) {
        CY_CHECK_EQ(set.slot_of(unmarked), 0U);
    }
    usize stray = 0;
    for (i32 y = 0; y < kRows; ++y) {
        for (i32 x = 0; x < kColumns; ++x) {
            if (run.mask()[index_of(x, y)] == 0) {
                continue;
            }
            bool within = false;
            for (const u64 unit : {kUnitA, kUnitB, kUnitC, kUnitD}) {
                within = within || projected(run.scene(), unit).contains(x, y, 1);
            }
            stray += static_cast<usize>(!within);
        }
    }
    CY_CHECK_EQ(stray, usize{0});

    // THE MASK LINES UP WITH THE FRAME'S OWN SILHOUETTES, to the pixel. A marked unit is part of
    // the scene, so wherever the mask covers it the scene's depth is that surface's or nearer
    // (reversed-Z: never smaller). A mask drawn even one pixel off the frame's rasterization puts
    // marked texels over the ground or sky behind the unit, where the scene is farther.
    usize misaligned = 0;
    for (usize index = 0; index < kPixels; ++index) {
        if (run.mask()[index] == 0) {
            continue;
        }
        const f32 marked = run.mask_depth()[index];
        if (run.scene_depth()[index] < marked - (marked * run.settings().depth_tolerance)) {
            ++misaligned;
        }
    }
    std::fprintf(stderr, "(a) %zu mask pixels off the frame's silhouettes\n", misaligned);
    CY_CHECK_EQ(misaligned, usize{0});

    // EVERY CHANGED PIXEL IS ON A MARKED SILHOUETTE OR WITHIN THE WIDTH OF ONE, and every pixel the
    // device changed is the host reference's decision blended over the plain frame.
    const i32 widest =
        static_cast<i32>(std::max(run.settings().selected_width, run.settings().hovered_width));
    const std::vector<OutlineDecision> decisions = run.reference();
    usize changed = 0;
    usize beyond = 0;
    usize disagreements = 0;
    usize outlined = 0;
    for (i32 y = 0; y < kRows; ++y) {
        for (i32 x = 0; x < kColumns; ++x) {
            const usize index = index_of(x, y);
            const u32 before = plain[index];
            const u32 after = run.pixels()[index];
            const OutlineDecision& decision = decisions[index];
            if (before != after) {
                ++changed;
                if (run.mask()[index] == 0 &&
                    distance_to_mask(run.mask(), x, y, 0, widest) > widest * widest) {
                    ++beyond;
                }
            }
            u32 expected = before;
            if (decision.texel != OutlineTexel::None) {
                const GpuOutlineStyle style = set.style(decision.slot, run.settings());
                expected = blend_outline(before, style.colour, decision.alpha);
                outlined += static_cast<usize>(decision.texel == OutlineTexel::Outline);
            }
            // One 8-bit step for a blend the device rounds its own way; none where nothing is
            // drawn, and none for a solid outline (checked exactly in (c)).
            const i32 allowed = decision.texel == OutlineTexel::None ? 0 : 1;
            if (channel_delta(expected, after) > allowed) {
                if (disagreements < 8U) {
                    std::fprintf(stderr,
                                 "(a) (%d, %d): device %08X, reference %08X (texel %u, slot %u, "
                                 "alpha %.3f)\n",
                                 x, y, after, expected, static_cast<u32>(decision.texel),
                                 decision.slot, static_cast<double>(decision.alpha));
                }
                ++disagreements;
            }
        }
    }
    std::fprintf(stderr,
                 "(a) %zu pixels changed, %zu outlined, %zu beyond the width, %zu against "
                 "the reference\n",
                 changed, outlined, beyond, disagreements);
    CY_CHECK_GT(outlined, usize{300});
    CY_CHECK_EQ(beyond, usize{0});
    CY_CHECK_EQ(disagreements, usize{0});
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(b) unmarked objects are untouched") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    std::vector<u32> plain;
    CY_REQUIRE(render_plain(fixture, plain));
    RunOptions options;
    options.mark = mark_field;
    OutlineRun run(fixture, options);
    CY_REQUIRE(run.render());

    // The far unit, E: nine pixels from the nearest marked silhouette, and every pixel of its
    // projection is the plain frame's.
    const PixelRect far = projected(run.scene(), kUnitE);
    usize e_pixels = 0;
    usize e_changed = 0;
    for (i32 y = far.y0; y <= far.y1; ++y) {
        for (i32 x = far.x0; x <= far.x1; ++x) {
            ++e_pixels;
            e_changed += static_cast<usize>(plain[index_of(x, y)] != run.pixels()[index_of(x, y)]);
        }
    }
    CY_CHECK_GT(e_pixels, usize{100});
    CY_CHECK_EQ(e_changed, usize{0});

    // The building: its pixels farther than the widest outline from any marked silhouette are the
    // plain frame's — the outline of D and the glow of C reach onto it and nothing else does.
    const PixelRect building = projected(run.scene(), kBuilding);
    const i32 widest =
        static_cast<i32>(std::max(run.settings().selected_width, run.settings().hovered_width));
    usize building_far = 0;
    usize building_changed = 0;
    for (i32 y = building.y0; y <= building.y1; ++y) {
        for (i32 x = building.x0; x <= building.x1; ++x) {
            const usize index = index_of(x, y);
            if (run.mask()[index] != 0 ||
                distance_to_mask(run.mask(), x, y, 0, widest) <= widest * widest) {
                continue;
            }
            ++building_far;
            building_changed += static_cast<usize>(plain[index] != run.pixels()[index]);
        }
    }
    CY_CHECK_GT(building_far, usize{200});
    CY_CHECK_EQ(building_changed, usize{0});

    // And the whole frame: nothing outside every silhouette and beyond every width changed.
    usize outside_changed = 0;
    for (i32 y = 0; y < kRows; ++y) {
        for (i32 x = 0; x < kColumns; ++x) {
            const usize index = index_of(x, y);
            if (run.mask()[index] == 0 &&
                distance_to_mask(run.mask(), x, y, 0, widest) > widest * widest) {
                outside_changed += static_cast<usize>(plain[index] != run.pixels()[index]);
            }
        }
    }
    CY_CHECK_EQ(outside_changed, usize{0});

    // A marked unit's own visible surface is untouched too: an outline never covers the material
    // it surrounds.
    usize surface_changed = 0;
    for (usize index = 0; index < kPixels; ++index) {
        if (run.mask()[index] != 0 && !hidden(run, index)) {
            surface_changed += static_cast<usize>(plain[index] != run.pixels()[index]);
        }
    }
    CY_CHECK_EQ(surface_changed, usize{0});
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

namespace {

/// A's outline alone at one width and colour: how wide it came out, and whether every pixel of it
/// is that colour exactly.
struct Ring {
    usize pixels = 0;
    i32 widest_squared = 0;
    usize off_colour = 0;
};

[[nodiscard]] Ring measure_ring(const std::vector<u32>& plain, const OutlineRun& run, u32 slot,
                                u32 colour) {
    Ring ring;
    const i32 reach = static_cast<i32>(kMaxOutlineWidth) + 2;
    for (i32 y = 0; y < kRows; ++y) {
        for (i32 x = 0; x < kColumns; ++x) {
            const usize index = index_of(x, y);
            if (run.mask()[index] != 0 || plain[index] == run.pixels()[index]) {
                continue;
            }
            const i32 distance = distance_to_mask(run.mask(), x, y, slot, reach);
            if (distance > reach * reach) {
                continue;
            }
            ++ring.pixels;
            ring.widest_squared = std::max(ring.widest_squared, distance);
            ring.off_colour += static_cast<usize>(run.pixels()[index] != colour);
        }
    }
    return ring;
}

}  // namespace

CY_TEST_CASE("(c) colour and width follow the API") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    std::vector<u32> plain;
    CY_REQUIRE(render_plain(fixture, plain));

    // A alone, selected: two pixels of gold, then five of blue.
    Ring rings[2];
    const u32 widths[2] = {2, 5};
    const HighlightColour colours[2] = {kGold, kTeamBlue};
    for (u32 which = 0; which < 2U; ++which) {
        RunOptions options;
        options.settings.selected_width = widths[which];
        const HighlightColour colour = colours[which];
        options.mark = [colour](HighlightSet& set) {
            CY_REQUIRE(set.select(kUnitA, colour).has_value());
        };
        OutlineRun run(fixture, options);
        CY_REQUIRE(run.render());
        rings[which] = measure_ring(plain, run, run.highlights().slot_of(kUnitA), colour.packed());
        std::fprintf(stderr, "(c) width %u: %zu ring pixels, widest %.2f px, %zu off colour\n",
                     widths[which], rings[which].pixels,
                     std::sqrt(static_cast<double>(rings[which].widest_squared)),
                     rings[which].off_colour);
        const i32 width = static_cast<i32>(widths[which]);
        // Exactly as wide as asked: some pixel at the full width, none beyond it.
        CY_CHECK_LE(rings[which].widest_squared, width * width);
        CY_CHECK_GT(rings[which].widest_squared, (width - 1) * (width - 1));
        // And exactly the colour asked for, byte for byte, including the alpha channel.
        CY_CHECK_GT(rings[which].pixels, usize{50});
        CY_CHECK_EQ(rings[which].off_colour, usize{0});
    }
    CY_CHECK_GT(rings[1].pixels, rings[0].pixels * 2U);

    // Hovered: a glow in the API's colour that falls off. The pixel beside C's silhouette is the
    // blend at `hovered_strength`, and the colour moves with the API.
    for (const HighlightColour colour : {kHoverWhite, HighlightColour{60, 255, 120, 255}}) {
        RunOptions options;
        options.mark = [colour](HighlightSet& set) {
            CY_REQUIRE(set.hover(kUnitC, colour).has_value());
        };
        OutlineRun run(fixture, options);
        CY_REQUIRE(run.render());
        const u32 slot = run.highlights().slot_of(kUnitC);
        usize adjacent = 0;
        usize adjacent_wrong = 0;
        usize outer = 0;
        usize outer_wrong = 0;
        for (i32 y = 0; y < kRows; ++y) {
            for (i32 x = 0; x < kColumns; ++x) {
                const usize index = index_of(x, y);
                if (run.mask()[index] != 0) {
                    continue;
                }
                const i32 distance = distance_to_mask(run.mask(), x, y, slot, 6);
                const f32 strength = run.settings().hovered_strength;
                if (distance == 1) {
                    ++adjacent;
                    const u32 expected = blend_outline(plain[index], colour.packed(), strength);
                    adjacent_wrong +=
                        static_cast<usize>(channel_delta(expected, run.pixels()[index]) > 1);
                } else if (distance == 25) {
                    // At the full width the glow is (1/5)^2 of its strength: faint, but there.
                    ++outer;
                    const u32 expected =
                        blend_outline(plain[index], colour.packed(), strength / 25.0F);
                    outer_wrong +=
                        static_cast<usize>(channel_delta(expected, run.pixels()[index]) > 1);
                }
            }
        }
        std::fprintf(stderr,
                     "(c) hovered %08X: %zu adjacent (%zu wrong), %zu at the width (%zu "
                     "wrong)\n",
                     colour.packed(), adjacent, adjacent_wrong, outer, outer_wrong);
        CY_CHECK_GT(adjacent, usize{50});
        CY_CHECK_EQ(adjacent_wrong, usize{0});
        CY_CHECK_GT(outer, usize{4});
        CY_CHECK_EQ(outer_wrong, usize{0});
    }
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}

CY_TEST_CASE("(d) occluded parts show the occluded style only where the unit is hidden") {
    DeviceFixture fixture;
    if (!fixture.has_gpu()) {
        fixture.report_skip();
        return;
    }
    std::vector<u32> plain;
    CY_REQUIRE(render_plain(fixture, plain));
    RunOptions options;
    options.mark = mark_field;
    OutlineRun run(fixture, options);
    CY_REQUIRE(run.render());
    save("selection-outlines-occluded.png", run.pixels());
    const HighlightSet& set = run.highlights();
    const u32 d_slot = set.slot_of(kUnitD);
    const PixelRect building = projected(run.scene(), kBuilding);

    // D is PARTLY hidden, and only where the building is; no other unit is hidden anywhere.
    usize d_hidden = 0;
    usize d_visible = 0;
    usize hidden_outside_building = 0;
    usize others_hidden = 0;
    usize fill_wrong = 0;
    for (i32 y = 0; y < kRows; ++y) {
        for (i32 x = 0; x < kColumns; ++x) {
            const usize index = index_of(x, y);
            const u32 slot = run.mask()[index];
            if (slot == 0) {
                continue;
            }
            const bool is_hidden = hidden(run, index);
            if (slot == d_slot) {
                d_hidden += static_cast<usize>(is_hidden);
                d_visible += static_cast<usize>(!is_hidden);
                hidden_outside_building +=
                    static_cast<usize>(is_hidden && !building.contains(x, y, 1));
            } else {
                others_hidden += static_cast<usize>(is_hidden);
            }
            // Tinted where hidden — the building's pixel moved toward D's colour — and untouched
            // where D is on screen.
            const bool changed = plain[index] != run.pixels()[index];
            fill_wrong += static_cast<usize>(changed != is_hidden);
        }
    }
    std::fprintf(stderr,
                 "(d) D: %zu hidden, %zu visible; %zu hidden off the building; %zu fill "
                 "pixels wrong\n",
                 d_hidden, d_visible, hidden_outside_building, fill_wrong);
    CY_CHECK_GT(d_hidden, usize{100});
    CY_CHECK_GT(d_visible, usize{100});
    CY_CHECK_EQ(hidden_outside_building, usize{0});
    CY_CHECK_EQ(others_hidden, usize{0});
    CY_CHECK_EQ(fill_wrong, usize{0});

    // The outline beside the hidden part is the occluded style — never the solid colour, and with
    // gaps — and beside the visible part it is solid.
    const std::vector<OutlineDecision> decisions = run.reference();
    const u32 red = kTeamRed.packed();
    usize occluded = 0;
    usize occluded_solid = 0;
    usize occluded_gaps = 0;
    usize visible = 0;
    usize visible_wrong = 0;
    for (i32 y = 0; y < kRows; ++y) {
        for (i32 x = 0; x < kColumns; ++x) {
            const usize index = index_of(x, y);
            if (run.mask()[index] != 0) {
                continue;
            }
            const OutlineDecision& decision = decisions[index];
            if (decision.slot == d_slot && decision.texel == OutlineTexel::OccludedOutline) {
                ++occluded;
                occluded_solid += static_cast<usize>(run.pixels()[index] == red);
            } else if (decision.slot == d_slot && decision.texel == OutlineTexel::Outline) {
                ++visible;
                visible_wrong += static_cast<usize>(run.pixels()[index] != red);
            } else if (decision.texel == OutlineTexel::None &&
                       distance_to_mask(run.mask(), x, y, d_slot, 2) <= 4 &&
                       building.contains(x, y)) {
                occluded_gaps += static_cast<usize>(plain[index] == run.pixels()[index]);
            }
        }
    }
    std::fprintf(stderr,
                 "(d) D's outline: %zu occluded (%zu solid, %zu gaps), %zu visible (%zu not "
                 "solid)\n",
                 occluded, occluded_solid, occluded_gaps, visible, visible_wrong);
    CY_CHECK_GT(occluded, usize{20});
    CY_CHECK_EQ(occluded_solid, usize{0});
    CY_CHECK_GT(occluded_gaps, usize{10});
    CY_CHECK_GT(visible, usize{20});
    CY_CHECK_EQ(visible_wrong, usize{0});

    // With the occluded style off, the hidden part draws nothing: no fill, and no outline whose
    // nearest marked pixel is hidden.
    RunOptions off = options;
    off.settings.show_occluded = false;
    OutlineRun without(fixture, off);
    CY_REQUIRE(without.render());
    const std::vector<OutlineDecision> hidden_off = without.reference();
    usize hidden_decisions = 0;
    usize drawn_elsewhere = 0;
    for (usize index = 0; index < kPixels; ++index) {
        const OutlineDecision& decision = hidden_off[index];
        hidden_decisions += static_cast<usize>(decision.texel == OutlineTexel::OccludedFill ||
                                               decision.texel == OutlineTexel::OccludedOutline);
        drawn_elsewhere += static_cast<usize>(decision.texel == OutlineTexel::None &&
                                              plain[index] != without.pixels()[index]);
    }
    CY_CHECK_EQ(hidden_decisions, usize{0});
    CY_CHECK_EQ(drawn_elsewhere, usize{0});
    CY_CHECK_LT(differing(plain, without.pixels()), differing(plain, run.pixels()));
    CY_CHECK_EQ(fixture.validation_errors(), 0U);
}
