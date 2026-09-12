// The half of CyberFoliage that names the renderer: the surface class mapping, the GPU scene's own
// instance record, and the arbiter adapter. See cook.h for why these three and nothing else.

#include <cy/foliage/cook.h>

#include <cy/core/math/quat.h>
#include <cy/core/math/scalar.h>

namespace cy::foliage {

rendering::vg::SurfaceClass to_surface_class(FoliageSurface surface) noexcept {
    switch (surface) {
        case FoliageSurface::Foliage:
            return rendering::vg::SurfaceClass::Foliage;
        case FoliageSurface::Aggregate:
            return rendering::vg::SurfaceClass::Aggregate;
        case FoliageSurface::Thin:
            return rendering::vg::SurfaceClass::Thin;
    }
    // Unreachable for every declared species — `FoliageSurface` has three values and each has a
    // case above — and the fallback is `Foliage` rather than `Solid`, so even a value cast in from
    // outside the enumeration cannot make the geometry system treat a plant as solid.
    return rendering::vg::SurfaceClass::Foliage;
}

rendering::vg::Importance to_geometry_importance(GameplayImportance importance) noexcept {
    switch (importance) {
        case GameplayImportance::Cover:
            return rendering::vg::Importance::Gameplay;
        case GameplayImportance::Relevant:
            return rendering::vg::Importance::Normal;
        case GameplayImportance::Decorative:
            return rendering::vg::Importance::Background;
        case GameplayImportance::kCount:
            break;
    }
    return rendering::vg::Importance::Normal;
}

AssetTable::AssetTable(Allocator& allocator) noexcept : bindings_(allocator) {}

Status AssetTable::bind(const AssetBinding& binding) noexcept {
    for (AssetBinding& existing : bindings_) {
        if (existing.species == binding.species && existing.tier == binding.tier) {
            existing = binding;
            return ok();
        }
    }
    return bindings_.push_back(binding);
}

const AssetBinding* AssetTable::find(SpeciesId species, DetailTier tier) const noexcept {
    const AssetBinding* exact = nullptr;
    const AssetBinding* coarsest = nullptr;
    for (const AssetBinding& binding : bindings_) {
        if (!(binding.species == species)) {
            continue;
        }
        if (binding.tier == tier) {
            exact = &binding;
        }
        // The fallback: the coarsest tier this species IS bound at. A world that cooked no
        // impostors draws aggregates rather than nothing.
        if (coarsest == nullptr ||
            static_cast<u32>(binding.tier) > static_cast<u32>(coarsest->tier)) {
            coarsest = &binding;
        }
    }
    return exact != nullptr ? exact : coarsest;
}

Expected<PublishReport, Error> build_instances(
    const SpeciesLibrary& library, const ClusterStore& clusters, const AssetTable& assets,
    Span<const VisibleCluster> visible, const FoliageBudget& budget,
    Array<rendering::vg::GeometryInstance>& out) noexcept {
    PublishReport report;
    for (const VisibleCluster& entry : visible) {
        const FoliageCluster* cluster = clusters.find(entry.cluster);
        if (cluster == nullptr) {
            continue;
        }
        ++report.clusters;
        if (static_cast<u32>(entry.tier) < kDetailTierCount) {
            ++report.by_tier[static_cast<u32>(entry.tier)];
        }
        for (const SpeciesBlock& block : cluster->blocks()) {
            const SpeciesDeclaration* species = library.find(block.species);
            if (species == nullptr) {
                continue;
            }
            const DetailTier tier = resolve_tier(*species, entry.tier);
            const AssetBinding* binding = assets.find(block.species, tier);
            if (binding == nullptr) {
                report.unbound += block.count;
                continue;
            }
            // The budget's own fraction, per species. A protected species gets 1 whatever the
            // position is — `FoliageBudget::fraction_for()` answers before it looks at the class,
            // which is where "gameplay-relevant cover persists" actually lives.
            const f32 fraction = budget.fraction_for(*species);
            const auto allowed =
                static_cast<u32>(static_cast<f32>(block.count) * math::clamp(fraction, 0.0F, 1.0F));
            u32 published = 0;
            for (u32 slot = block.first; slot < block.first + block.count; ++slot) {
                const FoliageInstance* instance = cluster->at(slot);
                if (instance == nullptr) {
                    continue;
                }
                if (!instance->flags.drawable()) {
                    // The suppression the specification asks for, counted rather than asserted.
                    ++report.suppressed;
                    continue;
                }
                if (published >= allowed) {
                    ++report.budget_dropped;
                    continue;
                }
                const world::WorldVec3d position = cluster->bounds().decode(*instance);
                rendering::vg::GeometryInstance published_instance;
                // Camera-relative is the renderer's own business: `GeometryInstance::translation`
                // is f32 and the caller rebases it against its view's origin, exactly as it does
                // for every other instance in the scene. Handing it absolute metres here would be
                // this module inventing a second coordinate convention.
                published_instance.translation =
                    Vec3{static_cast<f32>(position.x), static_cast<f32>(position.y),
                         static_cast<f32>(position.z)};
                published_instance.scale =
                    species->scale_min + ((species->scale_max - species->scale_min) *
                                          (static_cast<f32>(instance->scale) * (1.0F / 65535.0F)));
                const f32 yaw = static_cast<f32>(instance->yaw) * (math::kTwoPi / 65536.0F);
                published_instance.rotation = Quat::from_euler_yxz(Vec3{0.0F, yaw, 0.0F});
                published_instance.asset = binding->asset;
                published_instance.material_offset = binding->material_offset;
                published_instance.importance = to_geometry_importance(species->importance);
                if (Status pushed = out.push_back(published_instance); !pushed) {
                    return make_unexpected(pushed.error());
                }
                ++published;
                ++report.instances;
            }
        }
    }
    return report;
}

rendering::SubsystemDeclaration declare_to_arbiter(const FoliageLadder& ladder,
                                                   u32 reduction_order) noexcept {
    rendering::SubsystemDeclaration declaration;
    // Geometry, because foliage IS geometry. `rendering-architecture`'s subsystem list is that
    // capability's to change, and adding an eighth member from inside a foliage module would be a
    // change to a specification this milestone is not amending — budget.h says so at length.
    declaration.subsystem = rendering::BudgetSubsystem::Geometry;
    declaration.reduction_order = reduction_order;
    declaration.ladder.positions =
        static_cast<u8>(math::min<u32>(ladder.positions, rendering::kMaxLadderPositions));
    for (u8 position = 0; position < declaration.ladder.positions; ++position) {
        declaration.ladder.relative_cost[position] = ladder.cost_at(position);
    }
    return declaration;
}

u8 position_for_allocation(const FoliageLadder& ladder, f32 relative_allocation) noexcept {
    const u8 last = ladder.last_position();
    for (u8 position = 0; position <= last; ++position) {
        if (ladder.cost_at(position) <= relative_allocation) {
            return position;
        }
    }
    // Nothing fits. The last position is the least foliage can cost, and reporting it is what lets
    // the arbiter reach for resolution scale — its own last lever — rather than wait on this one.
    return last;
}

}  // namespace cy::foliage
