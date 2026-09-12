// Stable generated identity. M10 tasks.md 4.2, whose exit criterion this suite is.
//
// `procedural-content-generation` — "Stable generated identity": "Every generated output SHALL
// carry a stable identity derived from generator, region, and a stable per-output key. Identity
// SHALL NOT derive from iteration order, array index, insertion order, or worker scheduling", and
// its second scenario: "WHEN generation runs with different worker counts THEN generated identities
// SHALL be unchanged."
//
// ================================================================================================
// THE COLLISION SWEEP IS HERE BECAUSE THE SPIKE FOUND THE DEFECT IN ITSELF
// ================================================================================================
//
// design.md §1.6: `Derived` was first `fold_multiply(region + 1, slot + 1)`, which for small
// operands is plain `a * b` — the product's high half is zero, so there is nothing to fold — and
// `a * b` is not injective, so (region 1, slot 5) and (region 5, slot 1) were ONE instance. It
// reported 93 of 455 overrides mis-bound under a scheme that CANNOT mis-bind. **An identity scheme
// is only as stable as the function deriving it**, so this suite counts the collisions rather than
// asserting the derivation is good.
//
// HOW IT WAS SHOWN TO BE ABLE TO FAIL: the spike's original defect was put back — `region_key()`
// reduced to a small packed ordinal and `derive_identity()` to
// `fold_multiply(region + 1, slot + 1)` over it — and "derived identities do not collide over a
// world of regions and slots" went red at **35 211 collisions of 65 536**, with the region-key and
// input-sensitivity cases red beside it. Both were then restored. The two halves of the mutation
// are needed together: `fold_multiply` over a LARGE operand folds and stays injective, and the
// defect is specifically that a small `a * b` does not — which is the whole content of "an identity
// scheme is only as stable as the function deriving it". Reported in this milestone's
// `verified_failing`.

#include <cy/test/test.h>

#include <cy/core/memory/hash_map.h>
#include <cy/pcg/identity.h>

#include "fixtures.h"

using cy::pcg::GeneratedId;
using cy::pcg::IdentitySource;
using cy::pcg::NodeIdentity;
using cy::pcg::RegionKey;
namespace test = cy::pcg::test;

namespace {

constexpr cy::u64 kSeed = 0x5eed'0000'1234'abcdULL;
constexpr cy::i32 kRegions = 64;  // 64 x 64 regions
constexpr cy::u32 kSlots = 16;    // x 16 candidate slots = 65 536 identities

}  // namespace

CY_TEST_CASE("derived identities do not collide over a world of regions and slots") {
    const NodeIdentity node = cy::pcg::node_identity("forest.scatter");
    cy::HashMap<cy::u64, cy::u32> seen(test::allocator());
    CY_REQUIRE(seen.reserve(static_cast<cy::usize>(kRegions) * kRegions * kSlots));

    cy::u32 collisions = 0;
    for (cy::i32 z = 0; z < kRegions; ++z) {
        for (cy::i32 x = 0; x < kRegions; ++x) {
            const RegionKey key = cy::pcg::region_key(x, z, 0);
            for (cy::u32 slot = 0; slot < kSlots; ++slot) {
                const GeneratedId id = cy::pcg::derive_identity(kSeed, node, key, slot);
                if (seen.contains(id.value)) {
                    ++collisions;
                    continue;
                }
                CY_REQUIRE(seen.insert(id.value, 1).has_value());
            }
        }
    }
    // ZERO, not "few". A 64-bit draw over 65 536 subjects has a birthday probability below 2^-33,
    // so a single collision here is a defect in the derivation rather than bad luck — which is
    // exactly what the spike's own `a * b` was, and exactly how it looked.
    CY_CHECK_EQ(collisions, 0u);
    CY_CHECK_EQ(seen.size(), static_cast<cy::usize>(kRegions) * kRegions * kSlots);
}

CY_TEST_CASE("a region key separates negative coordinates from their unsigned twins") {
    // A bit-PACKED key is injective only while the coordinate fits the bits allotted to it, and a
    // world generated at a negative coordinate is exactly where a packing quietly stops being one.
    CY_CHECK_NE(cy::pcg::region_key(-1, 0, 0).value, cy::pcg::region_key(0, -1, 0).value);
    CY_CHECK_NE(cy::pcg::region_key(-1, 0, 0).value, cy::pcg::region_key(1, 0, 0).value);
    CY_CHECK_NE(cy::pcg::region_key(3, 5, 0).value, cy::pcg::region_key(5, 3, 0).value);
    // The level is part of the subject: macro generation and local generation of one square are
    // different regions.
    CY_CHECK_NE(cy::pcg::region_key(3, 5, 0).value, cy::pcg::region_key(3, 5, 1).value);
    CY_CHECK_EQ(cy::pcg::region_key(3, 5, 0).value, cy::pcg::region_key(3, 5, 0).value);
}

CY_TEST_CASE("identity is a pure function of seed, node, region and slot") {
    const NodeIdentity scatter = cy::pcg::node_identity("forest.scatter");
    const NodeIdentity other = cy::pcg::node_identity("forest.rocks");
    const RegionKey key = cy::pcg::region_key(7, 11, 0);

    // Called twice, in any order, from any thread, at any tick: the same number. There is no cursor
    // anywhere in the derivation, so "generated identities SHALL be unchanged with different worker
    // counts" is arithmetic rather than a discipline.
    CY_CHECK_EQ(cy::pcg::derive_identity(kSeed, scatter, key, 3).value,
                cy::pcg::derive_identity(kSeed, scatter, key, 3).value);

    // Every one of the four inputs moves it.
    CY_CHECK_NE(cy::pcg::derive_identity(kSeed, scatter, key, 3).value,
                cy::pcg::derive_identity(kSeed + 1, scatter, key, 3).value);
    CY_CHECK_NE(cy::pcg::derive_identity(kSeed, scatter, key, 3).value,
                cy::pcg::derive_identity(kSeed, other, key, 3).value);
    CY_CHECK_NE(cy::pcg::derive_identity(kSeed, scatter, key, 3).value,
                cy::pcg::derive_identity(kSeed, scatter, cy::pcg::region_key(7, 12, 0), 3).value);
    CY_CHECK_NE(cy::pcg::derive_identity(kSeed, scatter, key, 3).value,
                cy::pcg::derive_identity(kSeed, scatter, key, 4).value);
}

CY_TEST_CASE("a node's identity comes from its name, not from its position in the graph") {
    // Inserting a node upstream must not renumber every instance downstream of it — the graph-level
    // version of the same mistake a traversal counter makes at the instance level.
    CY_CHECK_EQ(cy::pcg::node_identity("forest.scatter").value,
                cy::pcg::node_identity("forest.scatter").value);
    CY_CHECK_NE(cy::pcg::node_identity("forest.scatter").value,
                cy::pcg::node_identity("forest.scatter2").value);
    CY_CHECK(cy::pcg::node_identity("forest.scatter").is_valid());
}

CY_TEST_CASE("the generation stream is separate from the identity derivation") {
    // `simulation-and-determinism`: "consuming randomness in one does not shift another's
    // sequence". A placement rule that begins drawing one more value per candidate must not
    // renumber a world's instances, and it cannot when the two descend from different roots.
    const NodeIdentity node = cy::pcg::node_identity("forest.scatter");
    const RegionKey key = cy::pcg::region_key(2, 2, 0);
    const cy::determinism::RandomStream stream = cy::pcg::generation_stream(kSeed, node, key);
    const GeneratedId identity = cy::pcg::derive_identity(kSeed, node, key, 0);
    CY_CHECK_NE(stream.draw(cy::pcg::generation_point(), 0, 0), identity.value);
    // And the generation stream is AUTHORITATIVE: a generated world is state a save, a replay and a
    // peer all have to agree on.
    CY_CHECK(stream.authoritative());
}

CY_TEST_CASE("every identity source has a name, so a refusal can print the one it refused") {
    CY_CHECK(test::same_text(cy::pcg::identity_source_name(IdentitySource::Derived), "derived"));
    CY_CHECK(test::same_text(cy::pcg::identity_source_name(IdentitySource::TraversalCounter),
                             "traversal-counter"));
    CY_CHECK(test::same_text(cy::pcg::identity_source_name(IdentitySource::SurvivorRank),
                             "survivor-rank"));
}
