// The sort key, the total order over it, and the submission fingerprint. See
// cy/servers/render/sort.h.

#include <cy/servers/render/sort.h>

#include <cy/core/base/assert.h>

#include <algorithm>
#include <bit>

namespace cy::render {
namespace {

constexpr u32 kLayerShift = 64 - kSortLayerBits;
constexpr u32 kPipelineShift = kLayerShift - kSortPipelineBits;
constexpr u32 kMaterialShift = kPipelineShift - kSortMaterialBits;
constexpr u32 kMeshShift = kMaterialShift - kSortMeshBits;
static_assert(kMeshShift == kSortDepthBits,
              "the depth field occupies the low bits and nothing else");

constexpr u64 kDepthMask = (1ULL << kSortDepthBits) - 1ULL;

[[nodiscard]] constexpr u64 field(u64 value, u32 bits, u32 shift) noexcept {
    return (value & ((1ULL << bits) - 1ULL)) << shift;
}

/// Order by key, then by the stable id. Total, because two draws cannot share a stable id — see the
/// header comment for why that is the whole argument.
[[nodiscard]] bool draw_precedes(const DrawItem& a, const DrawItem& b) noexcept {
    if (a.key != b.key) {
        return a.key < b.key;
    }
    if (a.stable_id != b.stable_id) {
        return a.stable_id < b.stable_id;
    }
    // Reached only when a caller published two draws with one stable id, which the assertion in
    // sort_draws() reports in a development build. Ordering by surface keeps the comparison a
    // strict weak ordering in a shipping build rather than letting a self-comparison lie.
    return a.surface < b.surface;
}

}  // namespace

u32 quantise_depth(f32 view_depth) noexcept {
    if (!(view_depth > 0.0F)) {
        // Catches negatives and NaN in one test: a NaN fails every comparison, so `!(x > 0)` is
        // true for it, and a NaN depth sorting to the front is a visible artefact rather than a
        // random order.
        return 0;
    }
    const u32 bits = std::bit_cast<u32>(view_depth);
    return bits >> (32U - kSortDepthBits);
}

u64 make_sort_key(const DrawKeyInputs& inputs) noexcept {
    const u32 depth = quantise_depth(inputs.view_depth);
    // Transparent draws are composited, so they must arrive back to front: the same quantisation,
    // inverted. Decided from the layer rather than passed in — see the header.
    const bool back_to_front = (inputs.layer == SortLayer::Transparent);
    const u64 depth_field =
        back_to_front ? (kDepthMask - static_cast<u64>(depth)) : static_cast<u64>(depth);

    return field(static_cast<u64>(inputs.layer), kSortLayerBits, kLayerShift) |
           field(inputs.pipeline, kSortPipelineBits, kPipelineShift) |
           field(inputs.material, kSortMaterialBits, kMaterialShift) |
           field(inputs.mesh, kSortMeshBits, kMeshShift) | (depth_field & kDepthMask);
}

SortLayer sort_key_layer(u64 key) noexcept {
    const auto layer = static_cast<u8>(key >> kLayerShift);
    return (layer < kSortLayerCount) ? static_cast<SortLayer>(layer) : SortLayer::Overlay;
}

u32 sort_key_pipeline(u64 key) noexcept {
    return static_cast<u32>((key >> kPipelineShift) & ((1ULL << kSortPipelineBits) - 1ULL));
}

u32 sort_key_material(u64 key) noexcept {
    return static_cast<u32>((key >> kMaterialShift) & ((1ULL << kSortMaterialBits) - 1ULL));
}

u32 sort_key_mesh(u64 key) noexcept {
    return static_cast<u32>((key >> kMeshShift) & ((1ULL << kSortMeshBits) - 1ULL));
}

void sort_draws(Span<DrawItem> draws) noexcept {
#if defined(CY_DEVELOPMENT)
    // The duplicate-identity check. Quadratic, and therefore only over a small list: the point is
    // to catch the defect in a test and in a development frame with a handful of draws, not to pay
    // for it on a frame with fifty thousand. A larger frame is covered by the same check running in
    // the suites, which is where a producer's identity scheme is actually established.
    constexpr usize kIdentityCheckLimit = 512;
    if (draws.size() <= kIdentityCheckLimit) {
        for (usize i = 0; i + 1 < draws.size(); ++i) {
            for (usize j = i + 1; j < draws.size(); ++j) {
                CY_ASSERT_MSG(draws[i].stable_id != draws[j].stable_id ||
                                  draws[i].surface != draws[j].surface,
                              "two draws share a stable identity, so their relative order is the "
                              "order they were published in — which is what deterministic "
                              "submission forbids");
            }
        }
    }
#endif
    std::ranges::sort(draws, draw_precedes);
}

bool draws_are_ordered(Span<const DrawItem> draws) noexcept {
    for (usize index = 1; index < draws.size(); ++index) {
        if (draw_precedes(draws[index], draws[index - 1])) {
            return false;
        }
    }
    return true;
}

u64 submission_fingerprint(Span<const DrawItem> draws) noexcept {
    // FNV-1a over the four fields of every item, in order. A named, ordinary hash rather than a
    // clever one: the number only has to be stable across runs and sensitive to a reordering, and
    // an algorithm a reader recognises is worth more here than a few bits of avalanche.
    u64 hash = 1469598103934665603ULL;
    const auto fold = [&hash](u64 value) noexcept {
        for (u32 byte = 0; byte < 8; ++byte) {
            hash ^= (value >> (byte * 8U)) & 0xFFULL;
            hash *= 1099511628211ULL;
        }
    };
    for (const DrawItem& draw : draws) {
        fold(draw.key);
        fold(draw.stable_id);
        fold(draw.instance_slot);
        fold(draw.surface);
    }
    return hash;
}

}  // namespace cy::render
