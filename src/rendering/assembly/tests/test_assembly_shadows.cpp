// The shadow half of the assembled frame, judged THROUGH the frame. M11.c task 3.5.
//
// ================================================================================================
// WHY THESE CASES ARE HERE AND NOT IN `unit.render_shadows`
// ================================================================================================
//
// Task 3.5 put three mechanisms into `FrameAssembly::request_shadow_pages`: a shadow mode selected
// per light against the renderer profile, the fallback chain walked for every page the frame asks
// for, and `invalidate_light` called when a shadow-casting light moves. `unit.render_shadows`
// checks `select_shadow_mode` as a pure function, and until these cases NOTHING read
// `AssemblyReport::shadow_modes`, `shadow_substitutions`, `shadow_lights_moved` or
// `shadow_pages_invalidated` — so a frame that stopped calling any of the three would have kept
// every suite green. The row's sentence is about the ASSEMBLED frame, so that is where it is asked.
//
// Each case was proved red by removing the call it names from `frame_assembly.cpp`.

#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/math/quat.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/assembly/frame_assembly.h>
#include <cy/test/test.h>

using cy::f32;
using cy::u32;
using cy::u64;
using cy::Vec3;
using namespace cy::rendering;
using namespace cy::rendering::assembly;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

constexpr u32 kWidth = 320;
constexpr u32 kHeight = 180;
/// One page per clip level per shadow-casting light: `kShadowLevels` in `frame_assembly.cpp`.
constexpr u32 kLevels = 3;

[[nodiscard]] AssemblyDescription make_description() noexcept {
    AssemblyDescription description;
    description.width = kWidth;
    description.height = kHeight;
    description.near_plane = 0.1F;
    description.far_plane = 200.0F;
    description.clusters = ClusterGridConfig{32, 16, 32};
    description.shadows.slots = 64;
    description.material_capacity = 8;
    description.gpu_culling = false;
    return description;
}

[[nodiscard]] AssemblyView make_view(cy::Span<const cy::render::LightDescription> lights) noexcept {
    AssemblyView view;
    view.fov_y_radians = 1.0471975512F;
    view.projection = cy::perspective_reversed_z(
        view.fov_y_radians, static_cast<f32>(kWidth) / static_cast<f32>(kHeight), 0.1F, 200.0F);
    view.view =
        cy::look_at(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F}, Vec3{0.0F, 1.0F, 0.0F});
    view.cull.frustum = cy::Frustum::from_view_projection(view.projection * view.view);
    view.cull.camera_position = Vec3{0.0F, 0.0F, 0.0F};
    view.cull.camera_forward = Vec3{0.0F, 0.0F, -1.0F};
    view.cull.fov_y_radians = view.fov_y_radians;
    view.lights = lights;
    return view;
}

[[nodiscard]] cy::render::LightDescription sun(u64 id, f32 pitch_radians) noexcept {
    cy::render::LightDescription light;
    light.kind = cy::render::LightKind::Directional;
    light.transform = cy::Transform::from_rotation(
        cy::Quat::from_axis_angle(Vec3{1.0F, 0.0F, 0.0F}, pitch_radians));
    light.intensity = 100000.0F;
    light.stable_id = id;
    return light;
}

[[nodiscard]] cy::render::LightDescription point_light(Vec3 at, u64 id) noexcept {
    cy::render::LightDescription light;
    light.kind = cy::render::LightKind::Point;
    light.transform = cy::Transform::from_translation(at);
    light.intensity = 1000.0F;
    light.range = 20.0F;
    light.stable_id = id;
    return light;
}

/// One visible instance, so every frame below is a frame that draws something.
struct OneInstance {
    OneInstance() noexcept : index(allocator()) {
        SpatialEntry entry;
        entry.bounds =
            cy::Aabb::from_center_extents(Vec3{0.0F, 0.0F, -6.0F}, Vec3{0.5F, 0.5F, 0.5F});
        entry.stable_id = 7;
        entry.radius = 0.9F;
        ok = index.insert(entry).has_value();
    }
    SpatialIndex index;
    bool ok = false;
};

[[nodiscard]] u32 count_of(const ShadowModeLedger& ledger, ShadowMode mode) noexcept {
    return ledger.counts[static_cast<cy::usize>(mode)];
}

[[nodiscard]] u32 count_of(const SubstitutionLedger& ledger, ShadowSubstitution rung) noexcept {
    return ledger.counts[static_cast<cy::usize>(rung)];
}

}  // namespace

CY_TEST_CASE("the assembled frame selects each light's shadow mode against the profile") {
    FrameAssembly assembly(allocator());
    CY_REQUIRE(assembly.initialize(make_description()).has_value());
    OneInstance scene;
    CY_REQUIRE(scene.ok);

    // Four lights, four declarations, on a profile with NO virtual pages and NO trace: the
    // constrained profile the requirement's fallback sentence is written for.
    cy::render::LightDescription lights[] = {sun(1, -0.8F), point_light(Vec3{0.0F, 0.0F, -6.0F}, 2),
                                             point_light(Vec3{2.0F, 0.0F, -8.0F}, 3),
                                             point_light(Vec3{-2.0F, 0.0F, -8.0F}, 4)};
    lights[3].casts_shadow = false;
    const ShadowMode declared[] = {ShadowMode::RayTraced, ShadowMode::Virtual,
                                   ShadowMode::Conventional, ShadowMode::Virtual};
    AssemblyView view = make_view({lights, 4});
    view.shadow_modes = {declared, 4};
    view.shadow_profile.virtual_pages = false;
    view.shadow_profile.traced = false;

    RenderGraph graph(allocator());
    AssemblyReport report;
    CY_REQUIRE(assembly.assemble(scene.index, view, FrameSinks{}, graph, report).has_value());

    // EVERY light is selected, the non-caster included: `None` is one of the six modes.
    CY_CHECK_EQ(report.shadow_modes.lights, 4U);
    // The traced sun and the virtual point light both KEEP a shadow, conventionally — "fall back
    // to its conventional mode with a diagnostic, not lose its shadow" — and the light that
    // declared Conventional got it outright.
    CY_CHECK_EQ(count_of(report.shadow_modes, ShadowMode::Conventional), 3U);
    CY_CHECK_EQ(count_of(report.shadow_modes, ShadowMode::None), 1U);
    CY_CHECK_EQ(count_of(report.shadow_modes, ShadowMode::Virtual), 0U);
    CY_CHECK_EQ(count_of(report.shadow_modes, ShadowMode::RayTraced), 0U);
    // Two lights did not get what they declared. The non-caster is NOT a degradation.
    CY_CHECK_EQ(report.shadow_modes.degraded, 2U);
    // Three lights still cast, so three lights' pages are asked for.
    CY_CHECK_EQ(report.shadow_pages_requested, 3U * kLevels);

    // The same lights on the full profile: every declaration stands except the traced one, which
    // has no trace this frame and is the only light degraded — onto the paged path.
    view.shadow_profile.virtual_pages = true;
    RenderGraph again(allocator());
    AssemblyReport full;
    CY_REQUIRE(assembly.assemble(scene.index, view, FrameSinks{}, again, full).has_value());
    CY_CHECK_EQ(count_of(full.shadow_modes, ShadowMode::Virtual), 2U);
    CY_CHECK_EQ(count_of(full.shadow_modes, ShadowMode::Conventional), 1U);
    CY_CHECK_EQ(full.shadow_modes.degraded, 1U);
}

CY_TEST_CASE("the Approximation rung is reached only when the caller says a trace is available") {
    OneInstance scene;
    CY_REQUIRE(scene.ok);
    const cy::render::LightDescription lights[] = {sun(1, -0.8F),
                                                   point_light(Vec3{0.0F, 0.0F, -6.0F}, 2)};
    const ShadowMode declared[] = {ShadowMode::RayTraced, ShadowMode::Virtual};

    // A FIRST frame, so no page has ever been rendered: every lookup falls through the requested,
    // coarser and stale rungs, and what it lands on is the question.
    const auto first_frame = [&](bool traced, AssemblyReport& report) {
        FrameAssembly assembly(allocator());
        CY_REQUIRE(assembly.initialize(make_description()).has_value());
        AssemblyView view = make_view({lights, 2});
        view.shadow_modes = {declared, 2};
        view.shadow_profile.traced = traced;
        RenderGraph graph(allocator());
        CY_REQUIRE(assembly.assemble(scene.index, view, FrameSinks{}, graph, report).has_value());
    };

    AssemblyReport untraced;
    first_frame(false, untraced);
    // The chain is walked for every page the frame asked for — a number, not a function nothing
    // calls — and with no trace the walk ends UNSHADOWED, counted loudly.
    CY_CHECK_EQ(untraced.shadow_substitutions.total(), 2U * kLevels);
    CY_CHECK_EQ(count_of(untraced.shadow_substitutions, ShadowSubstitution::Approximation), 0U);
    CY_CHECK_EQ(count_of(untraced.shadow_substitutions, ShadowSubstitution::Unshadowed),
                2U * kLevels);
    CY_CHECK_EQ(count_of(untraced.shadow_modes, ShadowMode::RayTraced), 0U);

    AssemblyReport traced;
    first_frame(true, traced);
    // The caller says a trace is available: the sun keeps the mode it declared, and every page
    // that has nothing resident lands on the approximation instead of losing its shadow.
    CY_CHECK_EQ(count_of(traced.shadow_modes, ShadowMode::RayTraced), 1U);
    CY_CHECK_EQ(traced.shadow_modes.degraded, 0U);
    CY_CHECK_EQ(traced.shadow_substitutions.total(), 2U * kLevels);
    CY_CHECK_EQ(count_of(traced.shadow_substitutions, ShadowSubstitution::Approximation),
                2U * kLevels);
    CY_CHECK_EQ(count_of(traced.shadow_substitutions, ShadowSubstitution::Unshadowed), 0U);
}

CY_TEST_CASE("a shadow-casting light that moves dirties its pages, and one that stays does not") {
    FrameAssembly assembly(allocator());
    CY_REQUIRE(assembly.initialize(make_description()).has_value());
    OneInstance scene;
    CY_REQUIRE(scene.ok);

    cy::render::LightDescription lights[] = {sun(1, -0.8F)};
    const AssemblyView view = make_view({lights, 1});

    RenderGraph first(allocator());
    AssemblyReport a;
    CY_REQUIRE(assembly.assemble(scene.index, view, FrameSinks{}, first, a).has_value());
    CY_CHECK_EQ(a.shadow_lights_moved, 0U);  // first sight is not a move
    for (cy::u8 level = 0; level < kLevels; ++level) {
        VirtualPage page;
        page.light_slot = 0;
        page.level = level;
        assembly.shadows().record_render(page, 0.02F);
    }

    // The sun has not moved: every page is a hit and the receiver gets the page it asked for.
    RenderGraph second(allocator());
    AssemblyReport b;
    CY_REQUIRE(assembly.assemble(scene.index, view, FrameSinks{}, second, b).has_value());
    CY_CHECK_EQ(b.shadow_lights_moved, 0U);
    CY_CHECK_EQ(b.shadow_pages_invalidated, 0U);
    CY_CHECK_EQ(b.shadow_pages_resident, kLevels);
    CY_CHECK_EQ(count_of(b.shadow_substitutions, ShadowSubstitution::Requested), kLevels);

    // The sun moves, as it does over a day. Its pages are dirtied and ATTRIBUTED to it, none is a
    // hit any more, and a receiver samples the previous contents rather than a shadow left where
    // the sun was without anyone knowing.
    lights[0] = sun(1, -0.6F);
    const AssemblyView moved = make_view({lights, 1});
    RenderGraph third(allocator());
    AssemblyReport c;
    CY_REQUIRE(assembly.assemble(scene.index, moved, FrameSinks{}, third, c).has_value());
    CY_CHECK_EQ(c.shadow_lights_moved, 1U);
    CY_CHECK_EQ(c.shadow_pages_invalidated, kLevels);
    CY_CHECK_EQ(c.shadow_pages_resident, 0U);
    CY_CHECK_EQ(count_of(c.shadow_substitutions, ShadowSubstitution::Requested), 0U);
    CY_CHECK_EQ(count_of(c.shadow_substitutions, ShadowSubstitution::StalePage), kLevels);
}
