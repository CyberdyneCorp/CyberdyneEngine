#include <cy/vfx/gpu_layout.h>

namespace cy::vfx {
namespace {

/// How many components of an attribute at this precision share one 32-bit word. The same table
/// `emit_slang.cpp` uses; it is duplicated there only because that file's copy is `static` inside
/// an anonymous namespace and predates this header, and the two are checked against each other by
/// `unit.vfx_values`.
[[nodiscard]] u32 components_per_word(Precision precision) noexcept {
    switch (precision) {
        case Precision::Float16:
        case Precision::Snorm16:
            return 2;
        case Precision::Unorm8:
            return 4;
        case Precision::Float32:
        case Precision::Auto:
            break;
    }
    return 1;
}

}  // namespace

u32 gpu_words_per_particle(const AttributeSlot& slot) noexcept {
    const u32 per_word = components_per_word(slot.precision);
    return (slot.components + per_word - 1U) / per_word;
}

u32 gpu_array_base_words(const AttributeLayout& layout, const AttributeSlot& slot,
                         u32 capacity) noexcept {
    u32 base = 0;
    for (const AttributeSlot& candidate : layout.slots()) {
        if (candidate.elided) {
            continue;
        }
        if (candidate.name == slot.name) {
            return base;
        }
        base += gpu_words_per_particle(candidate) * capacity;
    }
    return base;
}

u32 gpu_round_up_pow2(u32 value) noexcept {
    u32 rounded = 1;
    while (rounded < value) {
        rounded <<= 1U;
    }
    return rounded;
}

u32 gpu_sort_passes(u32 n) noexcept {
    // log2(n) * (log2(n) + 1) / 2 — the same double loop `vfx_sort` runs, counted rather than
    // estimated, so `kGpuCountSortPasses` is what the dispatch actually did.
    u32 stages = 0;
    for (u32 k = 2; k <= n; k <<= 1U) {
        for (u32 j = k >> 1U; j > 0; j >>= 1U) {
            ++stages;
        }
    }
    return stages;
}

u64 gpu_block_words(const AttributeLayout& layout, u32 capacity) noexcept {
    u64 words = 0;
    for (const AttributeSlot& slot : layout.slots()) {
        if (slot.elided) {
            continue;
        }
        words += static_cast<u64>(gpu_words_per_particle(slot)) * static_cast<u64>(capacity);
    }
    return words;
}

}  // namespace cy::vfx
