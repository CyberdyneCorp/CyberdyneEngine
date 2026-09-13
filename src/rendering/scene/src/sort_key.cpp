// Sort key construction and the ordering itself. Task 4.1.5.

#include <cy/rendering/scene/sort_key.h>

#include <algorithm>

namespace cy::rendering {
namespace {

constexpr u32 kDepthBits = 24;
constexpr u32 kDepthMax = (1U << kDepthBits) - 1U;
constexpr u32 kSurfaceBits = 5;
constexpr u32 kSurfaceMax = (1U << kSurfaceBits) - 1U;

/// Quantise reversed-Z clip depth to `kDepthBits`, ascending in the requested direction.
///
/// `near_first` is the opaque ordering: the near plane is depth 1 under `cy::DepthConvention`, so
/// front-to-back is *descending* depth and the field is the complement. Getting this backwards
/// draws the scene back to front, which early-Z hides completely — the frame is correct and slower,
/// and nothing reports it. That is why the direction is a named parameter rather than a sign
/// someone reads off a subtraction.
[[nodiscard]] u64 quantise_depth(f32 depth, bool near_first) noexcept {
    const f32 clamped = depth < 0.0f ? 0.0f : (depth > 1.0f ? 1.0f : depth);
    const f32 ordered = near_first ? (1.0f - clamped) : clamped;
    return static_cast<u64>(ordered * static_cast<f32>(kDepthMax));
}

}  // namespace

DrawSortKey make_sort_key(const DrawSortInput& input) noexcept {
    const u64 layer = static_cast<u64>(input.layer) & 0x7U;
    const u64 program = sort_bucket(stable_identity(input.program_identity));
    const u64 mesh = sort_bucket(stable_identity(input.mesh_identity));
    const u64 surface = input.surface > kSurfaceMax ? kSurfaceMax : input.surface;

    const bool transparent = input.layer == SortLayer::Transparent;
    const u64 depth = quantise_depth(input.depth, !transparent);

    DrawSortKey key;
    if (transparent) {
        key.primary = (layer << 61U) | (depth << 37U) | (program << 21U) | (mesh << 5U) | surface;
    } else {
        key.primary = (layer << 61U) | (program << 45U) | (mesh << 29U) | (depth << 5U) | surface;
    }

    // The full identities, not the folds. This is what makes the order total across a bucket
    // collision, and it is why a fold losing 48 bits costs batching and never determinism.
    u64 tiebreak = stable_identity(input.instance_identity);
    tiebreak = hash_combine(tiebreak, stable_identity(input.program_identity));
    tiebreak = hash_combine(tiebreak, stable_identity(input.mesh_identity));
    key.tiebreak = hash_combine(tiebreak, hash_integer(surface, kSortSeed));
    return key;
}

void sort_draws(Span<DrawItem> draws) noexcept {
    // std::sort rather than std::stable_sort on purpose. A stable sort would hide an incomplete
    // ordering by falling back to input order — which is publication order — so the frame would
    // reproduce until the day something republished in a different sequence. With a total order the
    // two sorts give identical results, and with an incomplete one only this spelling reports it.
    std::sort(draws.begin(), draws.end(),
              [](const DrawItem& a, const DrawItem& b) noexcept { return a.key < b.key; });
}

usize first_duplicate_key(Span<const DrawItem> draws) noexcept {
    for (usize i = 1; i < draws.size(); ++i) {
        if (draws[i].key == draws[i - 1].key) {
            return i;
        }
    }
    return draws.size();
}

}  // namespace cy::rendering
