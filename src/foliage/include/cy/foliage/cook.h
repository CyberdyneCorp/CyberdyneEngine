#pragma once
// The half of CyberFoliage that names the renderer. M10 task 2.4.
//
// `cy::foliage` — every other header in this directory — is the runtime and query side: clusters,
// placement, exceptions, promotion, wind, interaction, ground cover, the budget. It names no
// device, no pipeline, no material compiler and no `rendering::` type at all.
//
// `cy::foliage-render` is this file, and it is three things:
//
//   1. THE SURFACE CLASS MAPPING. `foliage` — "Foliage assets SHALL DECLARE THEIR SURFACE CLASS
//      (see `virtual-geometry`) so that the geometry system applies appropriate simplification,
//      culling, and rasterisation policy". `FoliageSurface` (species.h) carries the three classes a
//      plant may be; this is the one function that turns one into a `rendering::vg::SurfaceClass`,
//      and `test_cook.cpp` holds that it is total and that no input yields `Solid`.
//   2. PUBLISHING INTO THE GPU SCENE. "Foliage SHALL publish into the GPU scene AS INSTANCES (see
//      `rendering-architecture`), and SHALL be culled, shaded, and shadowed BY THE SAME PASSES as
//      other geometry." `build_instances()` produces `rendering::vg::GeometryInstance` — the
//      engine's own instance record, the one `GpuScene::set_instances()` takes — and nothing else.
//      There is no foliage pass, no foliage draw path and no foliage shadow path, and the way that
//      is made checkable is that this file is the only one in the module a renderer type could hide
//      in.
//   3. THE ARBITER ADAPTER. budget.h explains why there is no `BudgetSubsystem::Foliage`; this is
//      where foliage's own priced ladder becomes a `rendering::QualityLadder` under
//      `BudgetSubsystem::Geometry`.
//
// src/terrain/'s `cy::terrain-cook` and src/ml/'s `cy::ml-cook` are the precedents and they state
// the split the same way: a dedicated server that answers foliage queries, and a cooker that never
// opens a device, should not link a cluster builder between them.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/foliage/budget.h>
#include <cy/foliage/instance.h>
#include <cy/foliage/species.h>
#include <cy/rendering/arbiter/subsystem.h>
#include <cy/rendering/virtual_geometry/cluster.h>
#include <cy/rendering/virtual_geometry/traversal.h>

namespace cy::foliage {

/// The one mapping. Total by construction — `FoliageSurface` has three values and every one of them
/// has a case — and `Solid` is not reachable from any of them.
[[nodiscard]] rendering::vg::SurfaceClass to_surface_class(FoliageSurface surface) noexcept;

/// The geometry importance a species' gameplay importance implies.
///
/// `virtual-geometry`'s `Importance` scales a threshold; `foliage`'s `GameplayImportance` protects
/// a species from budget pressure. They are different axes and this is the deliberate translation
/// between them: cover that matters tactically gets `Gameplay` so the geometry system keeps its
/// detail too, and decorative planting gets `Background` so it coarsens first.
[[nodiscard]] rendering::vg::Importance to_geometry_importance(
    GameplayImportance importance) noexcept;

/// Which asset a (species, tier) pair draws from. The caller owns the asset table — this module
/// cooks no geometry and loads nothing — so publishing needs a mapping from the pair to an index in
/// the GPU scene's asset list, and this is its shape.
struct AssetBinding {
    SpeciesId species;
    DetailTier tier = DetailTier::Detailed;
    /// Index into `rendering::vg::GpuScene::assets`.
    u32 asset = 0;
    /// Added to each cluster's material index, so one asset's instances can use different material
    /// tables without recooking. `rendering::vg::GeometryInstance::material_offset`'s own meaning.
    u32 material_offset = 0;
};

/// The bindings of a world, looked up by (species, tier).
class AssetTable {
public:
    explicit AssetTable(Allocator& allocator) noexcept;

    [[nodiscard]] Status bind(const AssetBinding& binding) noexcept;
    /// The binding for a pair, or null. A species with no binding at the wanted tier falls back to
    /// the coarsest tier it IS bound at, so a world that cooked no impostors draws aggregates
    /// rather than nothing.
    [[nodiscard]] const AssetBinding* find(SpeciesId species, DetailTier tier) const noexcept;
    [[nodiscard]] usize size() const noexcept { return bindings_.size(); }

private:
    Array<AssetBinding> bindings_;
};

/// What one publish produced.
struct PublishReport {
    u32 clusters = 0;
    u32 instances = 0;
    /// Instances not published because they are promoted or removed. The suppression the
    /// specification asks for — "the GPU instance SHALL be suppressed so it is not drawn twice" —
    /// counted rather than asserted.
    u32 suppressed = 0;
    /// Instances the budget's per-class fraction dropped.
    u32 budget_dropped = 0;
    /// Instances with no asset bound at any tier. A cooking gap, reported rather than silent.
    u32 unbound = 0;
    u32 by_tier[kDetailTierCount] = {};
};

/// Publish visible clusters into the GPU scene's instance form.
///
/// Every instance produced is a `rendering::vg::GeometryInstance` — the same record a static mesh
/// publishes — which is what makes "culled, shaded, and shadowed by the SAME PASSES" a build fact
/// rather than a promise: there is no other record this function could produce.
[[nodiscard]] Expected<PublishReport, Error> build_instances(
    const SpeciesLibrary& library, const ClusterStore& clusters, const AssetTable& assets,
    Span<const VisibleCluster> visible, const FoliageBudget& budget,
    Array<rendering::vg::GeometryInstance>& out) noexcept;

/// Foliage's declaration to the renderer's budget arbiter.
///
/// `BudgetSubsystem::Geometry`, because foliage IS geometry and `rendering-architecture`'s list of
/// subsystems is that capability's to change — see budget.h's header note.
[[nodiscard]] rendering::SubsystemDeclaration declare_to_arbiter(const FoliageLadder& ladder,
                                                                 u32 reduction_order) noexcept;

/// The ladder position an arbiter allocation implies, given foliage's declared prices. The inverse
/// of `FoliageLadder::cost_at()`: the coarsest position whose relative cost is still within the
/// allocation, or the last position when nothing fits.
[[nodiscard]] u8 position_for_allocation(const FoliageLadder& ladder,
                                         f32 relative_allocation) noexcept;

}  // namespace cy::foliage
