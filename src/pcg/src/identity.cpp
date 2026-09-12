// Stable generated identity. See include/cy/pcg/identity.h.

#include <cy/pcg/identity.h>

namespace cy::pcg {

using determinism::RandomSource;
using determinism::RandomStream;
using determinism::stream_id;
using determinism::StreamId;
using determinism::substream;

namespace {

/// The root of every identity in this module. A literal, so the number is a compile-time constant
/// and is the same on every machine in every process forever.
constexpr const char* kIdentityStream = "pcg.identity";

/// The root every generation draw descends from. Separate from the identity root, because
/// `simulation-and-determinism` requires that "consuming randomness in one does not shift another's
/// sequence" — a placement rule that begins drawing one more value per candidate must not renumber
/// a world's instances, and it cannot when the two are different streams.
constexpr const char* kGenerationStream = "pcg.generation";

}  // namespace

const char* identity_source_name(IdentitySource source) noexcept {
    switch (source) {
        case IdentitySource::Derived:
            return "derived";
        case IdentitySource::TraversalCounter:
            return "traversal-counter";
        case IdentitySource::SurvivorRank:
            return "survivor-rank";
    }
    return "unknown";
}

RegionKey region_key(i32 x, i32 z, u8 level) noexcept {
    // Two `substream` levels rather than a bit-packing. A packed key is injective only while the
    // coordinate fits the bits allotted to it, and a world generated at level 2 with negative
    // coordinates is exactly where a packing quietly stops being one. The casts go through u32 so a
    // negative coordinate is its two's-complement pattern rather than a sign-extended one, which
    // keeps (x = -1) and (x = 0xFFFFFFFF) from being the same subject at different widths.
    const StreamId root = stream_id("pcg.region");
    const StreamId with_x = substream(root, static_cast<u64>(static_cast<u32>(x)));
    const StreamId with_z = substream(with_x, static_cast<u64>(static_cast<u32>(z)));
    return RegionKey{substream(with_z, static_cast<u64>(level)).value};
}

GeneratedId derive_identity(u64 seed, NodeIdentity node, RegionKey region, u32 slot) noexcept {
    // SUBSTREAM THEN DRAW. Not `fold_multiply(region + 1, slot + 1)`: for small operands that is
    // plain `a * b` — the product's high half is zero, so nothing folds — and `a * b` is not
    // injective, so (region 1, slot 5) and (region 5, slot 1) would be one instance. The spike
    // found that defect in itself and reported 93 of 455 overrides mis-bound under a scheme that
    // cannot mis-bind; design.md §1.6 records it. Three mixing levels here, and
    // `tests/test_identity.cpp` measures the collision count rather than assuming it.
    const StreamId root = stream_id(kIdentityStream);
    const StreamId per_node = substream(root, node.value);
    const StreamId per_region = substream(per_node, region.value);
    const RandomSource source(seed);
    const RandomStream stream = source.stream(per_region);
    return GeneratedId{stream.draw(generation_point(), 0, static_cast<u64>(slot))};
}

determinism::RandomStream generation_stream(u64 seed, NodeIdentity node,
                                            RegionKey region) noexcept {
    const StreamId root = stream_id(kGenerationStream);
    const StreamId per_node = substream(root, node.value);
    const StreamId per_region = substream(per_node, region.value);
    const RandomSource source(seed);
    // Authoritative: a generated world is state a save, a replay and a peer all have to agree on.
    // A generator that is decorative declares a `Presentation` determinism level at the GRAPH, not
    // by drawing from a presentation stream — see the module README's note on determinism levels.
    return source.stream(per_region, determinism::StreamPurpose::Authoritative);
}

}  // namespace cy::pcg
