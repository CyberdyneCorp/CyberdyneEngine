// The sixteen-byte instance, its derived identity, the canonical order a slot depends on, and the
// hierarchical cull. M10 task 2.4.

#include <cy/test/test.h>

#include <cy/core/memory/hash_map.h>
#include <cy/foliage/instance.h>

#include "fixtures.h"

namespace test = cy::foliage::test;
using cy::foliage::cluster_identity;
using cy::foliage::ClusterBounds;
using cy::foliage::ClusterBuilder;
using cy::foliage::ClusterCoord;
using cy::foliage::ClusterId;
using cy::foliage::ClusterStore;
using cy::foliage::CullView;
using cy::foliage::FoliageCluster;
using cy::foliage::FoliageInstance;
using cy::foliage::instance_identity;
using cy::foliage::InstanceFlags;
using cy::foliage::InstanceId;
using cy::foliage::species_id;
using cy::foliage::SpeciesBlock;
using cy::foliage::SpeciesLibrary;
using cy::foliage::VisibleCluster;

namespace {

[[nodiscard]] ClusterBounds bounds_at(cy::f64 x, cy::f64 z) noexcept {
    ClusterBounds bounds;
    bounds.min_x = x;
    bounds.min_z = z;
    bounds.max_x = x + 64.0;
    bounds.max_z = z + 64.0;
    bounds.min_y = 0.0F;
    bounds.max_y = 64.0F;
    return bounds;
}

}  // namespace

CY_TEST_CASE("a million instances cost tens of bytes each, not hundreds") {
    // `foliage` — "their per-instance memory SHALL be TENS OF BYTES rather than hundreds". The
    // `static_assert` in instance.h is the compile-time half; this is the whole-cluster number.
    CY_CHECK_EQ(sizeof(FoliageInstance), 16u);

    ClusterBuilder builder(test::allocator(), test::policy(), ClusterId{7}, ClusterCoord{0, 0, 0},
                           bounds_at(0.0, 0.0));
    for (cy::u32 index = 0; index < 2000; ++index) {
        const cy::f64 x = static_cast<cy::f64>(index % 50) * 1.2;
        const cy::f64 z = static_cast<cy::f64>(index) / 50.0 * 1.4;
        CY_REQUIRE(builder
                       .add(species_id(test::kPine), cy::world::WorldVec3d{x, 10.0, z}, 0.3F, 0.5F,
                            1, 200, 0.0F, 0.0F, InstanceFlags{})
                       .has_value());
    }
    auto cluster = builder.finish();
    CY_REQUIRE(cluster.has_value());
    CY_CHECK_EQ(cluster.value().size(), 2000u);
    // Per instance, including the blocks and the species table.
    const cy::f32 per_instance = builder.report().bytes_per_instance();
    CY_CHECK_LT(per_instance, 20.0F);
    // A million of them.
    CY_CHECK_LT(per_instance * 1'000'000.0F, 20e6F);
}

CY_TEST_CASE("a position survives the cluster's quantisation to within a millimetre") {
    const ClusterBounds bounds = bounds_at(1'000'000.0, -250'000.0);
    // A cluster a thousand kilometres out: the whole point of quantising against the CLUSTER is
    // that the precision does not depend on where the cluster is.
    for (cy::u32 index = 0; index < 64; ++index) {
        const cy::f64 fraction = static_cast<cy::f64>(index) / 63.0;
        const cy::world::WorldVec3d at{bounds.min_x + (fraction * 64.0), 10.0 + (fraction * 40.0),
                                       bounds.min_z + ((1.0 - fraction) * 64.0)};
        FoliageInstance instance;
        bounds.encode(at, instance);
        const cy::world::WorldVec3d back = bounds.decode(instance);
        const cy::f64 error_x = back.x > at.x ? back.x - at.x : at.x - back.x;
        const cy::f64 error_y = back.y > at.y ? back.y - at.y : at.y - back.y;
        const cy::f64 error_z = back.z > at.z ? back.z - at.z : at.z - back.z;
        CY_CHECK_LT(error_x, 0.002);
        CY_CHECK_LT(error_y, 0.002);
        CY_CHECK_LT(error_z, 0.002);
    }
}

CY_TEST_CASE("identity is derived and is injective over clusters and slots") {
    // The M10 spike's own defect, held as a regression: `fold_multiply(region + 1, slot + 1)` is
    // `a * b` for small operands and `a * b` is not injective — region 1 slot 5 and region 5 slot 1
    // collided. Substream-then-draw does not, and this is the check that says so.
    cy::HashMap<cy::u64, cy::u64> seen(test::allocator());
    cy::u32 collisions = 0;
    for (cy::u32 cluster = 1; cluster <= 64; ++cluster) {
        for (cy::u32 slot = 0; slot < 64; ++slot) {
            const InstanceId identity = instance_identity(99, ClusterId{cluster}, slot);
            CY_REQUIRE(identity.is_valid());
            if (seen.contains(identity.value)) {
                ++collisions;
            } else {
                CY_REQUIRE(seen.insert(identity.value, 1).has_value());
            }
        }
    }
    CY_CHECK_EQ(collisions, 0u);
    CY_CHECK_EQ(seen.size(), 64u * 64u);

    // And it is a function of the seed too: the same cluster and slot under two seeds are two
    // instances, which is what makes two worlds' saves unable to bind to each other.
    CY_CHECK_NE(instance_identity(1, ClusterId{5}, 3).value,
                instance_identity(2, ClusterId{5}, 3).value);
}

CY_TEST_CASE("the rule graph version renames every cluster") {
    // "Changing a rule graph regenerates the region" as arithmetic rather than as a procedure
    // somebody runs: the version participates in the cluster's identity.
    const ClusterCoord coord{3, -7, 0};
    CY_CHECK_NE(cluster_identity(42, coord, 1).value, cluster_identity(42, coord, 2).value);
    CY_CHECK_EQ(cluster_identity(42, coord, 1).value, cluster_identity(42, coord, 1).value);
    CY_CHECK_NE(cluster_identity(42, coord, 1).value,
                cluster_identity(42, ClusterCoord{-7, 3, 0}, 1).value);
}

CY_TEST_CASE("a cluster's slots do not depend on the order placement accepted its instances") {
    // THE PROPERTY IDENTITY RESTS ON. Two builders given the same set in opposite orders must
    // produce byte-identical clusters, because a slot is what an identity is derived from and an
    // order-dependent slot would make every identity depend on a traversal.
    struct Placement {
        cy::f64 x;
        cy::f64 z;
        cy::u8 variation;
    };
    Placement placements[40];
    for (cy::u32 index = 0; index < 40; ++index) {
        placements[index] = Placement{static_cast<cy::f64>((index * 7) % 40) * 1.5,
                                      static_cast<cy::f64>((index * 13) % 40) * 1.3,
                                      static_cast<cy::u8>(index % 4)};
    }

    const ClusterBounds bounds = bounds_at(0.0, 0.0);
    ClusterBuilder forward(test::allocator(), test::policy(), ClusterId{11}, ClusterCoord{0, 0, 0},
                           bounds);
    for (const Placement& placement : placements) {
        CY_REQUIRE(forward
                       .add(species_id(test::kPine),
                            cy::world::WorldVec3d{placement.x, 5.0, placement.z}, 0.5F, 0.25F,
                            placement.variation, 100, 0.0F, 0.0F, InstanceFlags{})
                       .has_value());
    }
    ClusterBuilder backward(test::allocator(), test::policy(), ClusterId{11}, ClusterCoord{0, 0, 0},
                            bounds);
    for (cy::u32 index = 40; index > 0; --index) {
        const Placement& placement = placements[index - 1];
        CY_REQUIRE(backward
                       .add(species_id(test::kPine),
                            cy::world::WorldVec3d{placement.x, 5.0, placement.z}, 0.5F, 0.25F,
                            placement.variation, 100, 0.0F, 0.0F, InstanceFlags{})
                       .has_value());
    }

    auto a = forward.finish();
    auto b = backward.finish();
    CY_REQUIRE(a.has_value());
    CY_REQUIRE(b.has_value());
    CY_REQUIRE_EQ(a.value().size(), b.value().size());
    cy::u32 differences = 0;
    for (cy::u32 slot = 0; slot < static_cast<cy::u32>(a.value().size()); ++slot) {
        if (!(*a.value().at(slot) == *b.value().at(slot))) {
            ++differences;
        }
        // And therefore the identities agree slot for slot.
        CY_CHECK_EQ(a.value().identity_at(5, slot).value, b.value().identity_at(5, slot).value);
    }
    CY_CHECK_EQ(differences, 0u);
}

CY_TEST_CASE("a cluster's species blocks are contiguous and track their drawable count") {
    ClusterBuilder builder(test::allocator(), test::policy(), ClusterId{3}, ClusterCoord{0, 0, 0},
                           bounds_at(0.0, 0.0));
    for (cy::u32 index = 0; index < 30; ++index) {
        const cy::foliage::SpeciesId rotation[3] = {
            species_id(test::kPine), species_id(test::kFern), species_id(test::kBoulder)};
        const cy::foliage::SpeciesId species = rotation[index % 3];
        CY_REQUIRE(builder
                       .add(species,
                            cy::world::WorldVec3d{static_cast<cy::f64>(index) * 2.0, 4.0,
                                                  static_cast<cy::f64>(index) * 1.1},
                            0.0F, 0.5F, 0, 128, 0.0F, 0.0F, InstanceFlags{})
                       .has_value());
    }
    auto built = builder.finish();
    CY_REQUIRE(built.has_value());
    FoliageCluster& cluster = built.value();
    CY_CHECK_EQ(cluster.blocks().size(), 3u);

    cy::u32 covered = 0;
    for (const SpeciesBlock& block : cluster.blocks()) {
        CY_CHECK_EQ(block.first, covered);
        CY_CHECK_EQ(block.drawable, block.count);
        covered += block.count;
        for (cy::u32 slot = block.first; slot < block.first + block.count; ++slot) {
            CY_CHECK(cluster.species_at(cluster.at(slot)->species_slot) == block.species);
        }
    }
    CY_CHECK_EQ(covered, 30u);

    // Suppressing one instance drops exactly one from its block's drawable count and leaves every
    // other slot — and therefore every other identity — where it was.
    const InstanceId before = cluster.identity_at(9, 20);
    InstanceFlags promoted;
    promoted.set(InstanceFlags::kPromoted);
    CY_REQUIRE(cluster.set_flags(0, promoted).has_value());
    CY_CHECK_EQ(cluster.blocks()[0].drawable, cluster.blocks()[0].count - 1);
    CY_CHECK_EQ(cluster.identity_at(9, 20).value, before.value);
}

CY_TEST_CASE("clusters are culled before instances, so most instances are rejected in bulk") {
    SpeciesLibrary library(test::allocator());
    CY_REQUIRE(test::declare_all(library).has_value());
    ClusterStore store(test::allocator());

    // Sixteen clusters in a row, a hundred instances each. The view looks along +X from the origin
    // and reaches 200 m, so most of the row is beyond it.
    for (cy::i32 index = 0; index < 16; ++index) {
        const cy::f64 x = static_cast<cy::f64>(index) * 64.0;
        ClusterBuilder builder(test::allocator(), test::policy(),
                               ClusterId{static_cast<cy::u64>(index + 1)},
                               ClusterCoord{index, 0, 0}, bounds_at(x, 0.0));
        for (cy::u32 instance = 0; instance < 100; ++instance) {
            CY_REQUIRE(
                builder
                    .add(species_id(test::kPine),
                         cy::world::WorldVec3d{x + (static_cast<cy::f64>(instance % 10) * 6.0), 3.0,
                                               static_cast<cy::f64>(instance) / 10.0 * 6.0},
                         0.0F, 0.5F, 0, 128, 0.0F, 0.0F, InstanceFlags{})
                    .has_value());
        }
        auto built = builder.finish();
        CY_REQUIRE(built.has_value());
        CY_REQUIRE(store.insert(static_cast<FoliageCluster&&>(built.value())).has_value());
    }
    CY_CHECK_EQ(store.instance_count(), 1600u);

    CullView view;
    view.eye = cy::world::WorldVec3d{0.0, 20.0, 32.0};
    view.max_distance_metres = 200.0F;
    view.pixels_per_metre = 1200.0F;
    cy::Array<VisibleCluster> visible(test::allocator());
    auto result = store.cull(library, view, visible);
    CY_REQUIRE(result.has_value());

    CY_CHECK_EQ(result.value().clusters_tested, 16u);
    CY_CHECK_LT(result.value().clusters_visible, 8u);
    // The measurement the requirement is about: the instances inside rejected clusters were never
    // examined individually.
    CY_CHECK_GT(result.value().instances_rejected_in_bulk, result.value().instances_visible);
    CY_CHECK_EQ(result.value().instances_rejected_in_bulk + result.value().instances_visible,
                1600u);
    CY_CHECK_EQ(visible.size(), result.value().clusters_visible);

    // And a view that reaches the whole row walks the detail ladder: the near cluster draws at full
    // detail and the far one does not. A separate view, because the bulk-rejection measurement
    // above needs a SHORT one and a tier ladder needs a long one.
    CullView far_view = view;
    far_view.max_distance_metres = 8192.0F;
    far_view.pixels_per_metre = 200.0F;
    cy::Array<VisibleCluster> reached(test::allocator());
    auto everything = store.cull(library, far_view, reached);
    CY_REQUIRE(everything.has_value());
    CY_CHECK_EQ(everything.value().clusters_visible, 16u);
    CY_CHECK_EQ(reached[0].tier, cy::foliage::DetailTier::Detailed);
    CY_CHECK_GT(static_cast<cy::u32>(reached[reached.size() - 1].tier),
                static_cast<cy::u32>(reached[0].tier));
}

CY_TEST_CASE("a cluster is independently evictable and the store's index survives it") {
    ClusterStore store(test::allocator());
    for (cy::i32 index = 0; index < 5; ++index) {
        ClusterBuilder builder(
            test::allocator(), test::policy(), ClusterId{static_cast<cy::u64>(index + 1)},
            ClusterCoord{index, 0, 0}, bounds_at(static_cast<cy::f64>(index) * 64.0, 0.0));
        CY_REQUIRE(builder
                       .add(species_id(test::kFern), cy::world::WorldVec3d{1.0, 1.0, 1.0}, 0.0F,
                            0.5F, 0, 10, 0.0F, 0.0F, InstanceFlags{})
                       .has_value());
        auto built = builder.finish();
        CY_REQUIRE(built.has_value());
        CY_REQUIRE(store.insert(static_cast<FoliageCluster&&>(built.value())).has_value());
    }
    CY_CHECK_EQ(store.size(), 5u);
    CY_CHECK(store.evict(ClusterId{2}));
    CY_CHECK_FALSE(store.evict(ClusterId{2}));
    CY_CHECK_EQ(store.size(), 4u);
    CY_CHECK(store.find(ClusterId{2}) == nullptr);
    // The swap-remove moved another cluster; its index entry has to still be right.
    for (cy::u64 id : {1ULL, 3ULL, 4ULL, 5ULL}) {
        const FoliageCluster* found = store.find(ClusterId{id});
        CY_REQUIRE(found != nullptr);
        CY_CHECK_EQ(found->id().value, id);
    }
}
