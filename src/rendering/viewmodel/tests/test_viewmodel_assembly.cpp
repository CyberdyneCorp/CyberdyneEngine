// SPDX-License-Identifier: MIT
// The frame assembly's half of the first-person weapon view, with no device: the weapon view's own
// cull, its layers kept out of the world's, and the mark and shadow policy its draws carry.
//
// An integration suite rather than a unit one because each case initialises a whole
// `FrameAssembly` — the sky table, the temporal framework, the shadow cache — which costs a few
// milliseconds before the first cull.

#include <cy/core/math/matrix.h>
#include <cy/core/math/projection.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/rendering/assembly/frame_assembly.h>
#include <cy/rendering/viewmodel/viewmodel.h>
#include <cy/test/test.h>

using cy::f32;
using cy::u32;
using cy::Vec3;
using namespace cy::rendering;
using namespace cy::rendering::viewmodel;

namespace {

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::Renderer);
}

constexpr cy::render::LayerMask kWeaponLayer = 1U << 4U;
constexpr u32 kWidth = 320;
constexpr u32 kHeight = 180;
constexpr f32 kAspect = static_cast<f32>(kWidth) / static_cast<f32>(kHeight);

[[nodiscard]] ViewmodelSettings weapon_settings() {
    ViewmodelSettings settings;
    settings.layers = kWeaponLayer;
    settings.vertical_fov_radians = 0.9F;
    settings.near_plane = 0.01F;
    settings.far_plane = 10.0F;
    return settings;
}

[[nodiscard]] assembly::AssemblyDescription description() {
    assembly::AssemblyDescription out;
    out.width = kWidth;
    out.height = kHeight;
    out.near_plane = 0.1F;
    out.far_plane = 200.0F;
    out.clusters = ClusterGridConfig{16, 8, 16};
    out.material_capacity = 8;
    out.gpu_culling = false;
    return out;
}

[[nodiscard]] assembly::AssemblyView world_view(f32 fov) {
    assembly::AssemblyView view;
    view.fov_y_radians = fov;
    view.projection = cy::perspective_reversed_z(fov, kAspect, 0.1F, 200.0F);
    view.view = cy::look_at(Vec3{0.0F, 0.0F, 0.0F}, Vec3{0.0F, 0.0F, -1.0F});
    view.cull.frustum = cy::Frustum::from_view_projection(view.projection * view.view);
    view.cull.fov_y_radians = fov;
    return view;
}

/// A world wall three metres out, and a weapon on its own layer held 15 centimetres in front of the
/// eye — NEARER THAN THE WORLD'S NEAR PLANE, so the world's frustum cannot see it and only the
/// weapon view's can.
void fill(SpatialIndex& index) {
    SpatialEntry wall;
    wall.bounds = cy::Aabb::from_center_extents(Vec3{0.0F, 0.0F, -3.0F}, Vec3{2.0F, 2.0F, 0.1F});
    wall.stable_id = 1;
    wall.gpu_slot = 0;
    wall.radius = 3.0F;
    CY_REQUIRE(index.insert(wall).has_value());
    SpatialEntry weapon;
    weapon.bounds =
        cy::Aabb::from_center_extents(Vec3{0.02F, -0.01F, -0.06F}, Vec3{0.01F, 0.01F, 0.02F});
    weapon.stable_id = 2;
    weapon.gpu_slot = 1;
    weapon.layer_mask = kWeaponLayer;
    weapon.radius = 0.03F;
    CY_REQUIRE(index.insert(weapon).has_value());
}

}  // namespace

CY_TEST_CASE("the assembly culls the weapon view on its own and marks its draws") {
    SpatialIndex index(allocator());
    fill(index);
    assembly::FrameAssembly frame(allocator());
    CY_REQUIRE(frame.initialize(description()).has_value());

    // WITHOUT a weapon view the weapon is world geometry, and nearer than the world's near plane
    // the world's frustum does not see it.
    {
        assembly::AssemblyView view = world_view(1.2F);
        RenderGraph graph(allocator());
        assembly::AssemblyReport report;
        CY_REQUIRE(frame.assemble(index, view, assembly::FrameSinks{}, graph, report).has_value());
        CY_CHECK_EQ(report.draws, 1U);
        CY_CHECK_EQ(report.viewmodel_draws, 0U);
        for (const GpuDrawInstance& record : frame.draws().instances) {
            CY_CHECK_EQ(record.flags & cy::render::kInstanceViewmodel, 0U);
        }
    }
    // WITH one, the weapon is the weapon view's — seen through its own near plane — and the wall is
    // the world's, each in one list.
    for (const bool cast : {false, true}) {
        const ViewmodelSettings settings = [cast] {
            ViewmodelSettings made = weapon_settings();
            made.shadows.cast = cast;
            made.shadows.receive = !cast;
            return made;
        }();
        assembly::AssemblyView view = world_view(1.2F);
        view.viewmodel = assembly_view(settings, view.view, kAspect);
        RenderGraph graph(allocator());
        assembly::AssemblyReport report;
        CY_REQUIRE(frame.assemble(index, view, assembly::FrameSinks{}, graph, report).has_value());
        CY_CHECK_EQ(report.draws, 2U);
        CY_CHECK_EQ(report.viewmodel_visible, 1U);
        CY_CHECK_EQ(report.viewmodel_draws, 1U);
        u32 marked = 0;
        for (const GpuDrawInstance& record : frame.draws().instances) {
            const bool weapon = (record.flags & cy::render::kInstanceViewmodel) != 0U;
            CY_CHECK_EQ(weapon, record.instance_slot == 1U);
            if (!weapon) {
                // The world keeps its own policy untouched.
                CY_CHECK((record.flags & cy::render::kInstanceCastsShadow) != 0U);
                CY_CHECK((record.flags & cy::render::kInstanceReceivesShadow) != 0U);
                continue;
            }
            ++marked;
            CY_CHECK_EQ((record.flags & cy::render::kInstanceCastsShadow) != 0U, cast);
            CY_CHECK_EQ((record.flags & cy::render::kInstanceReceivesShadow) != 0U, !cast);
        }
        CY_CHECK_EQ(marked, 1U);
    }
}

CY_TEST_CASE("the weapon view's layers are not the world's, even where the world could see them") {
    SpatialIndex index(allocator());
    SpatialEntry crate;
    crate.bounds = cy::Aabb::from_center_extents(Vec3{0.0F, 0.0F, -2.0F}, Vec3{0.2F, 0.2F, 0.2F});
    crate.stable_id = 5;
    crate.layer_mask = kWeaponLayer | cy::render::kDefaultLayer;
    crate.radius = 0.4F;
    CY_REQUIRE(index.insert(crate).has_value());
    assembly::FrameAssembly frame(allocator());
    CY_REQUIRE(frame.initialize(description()).has_value());
    assembly::AssemblyView view = world_view(1.2F);
    view.viewmodel = assembly_view(weapon_settings(), view.view, kAspect);
    RenderGraph graph(allocator());
    assembly::AssemblyReport report;
    CY_REQUIRE(frame.assemble(index, view, assembly::FrameSinks{}, graph, report).has_value());
    // Drawn ONCE, by the weapon view: an instance on a weapon layer is the weapon view's.
    CY_CHECK_EQ(report.draws, 1U);
    CY_CHECK_EQ(report.viewmodel_draws, 1U);
}
