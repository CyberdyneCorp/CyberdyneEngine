#pragma once
// Quality levels and declared cost, so a preset can be assembled against a frame budget. Task 8.4.
//
// `rendering-post-processing` — "Performance and quality scaling": "Every effect SHALL expose
// quality levels (off, low, medium, high, ultra) that map to concrete parameters (resolution scale,
// sample counts, iteration counts), and SHALL declare its typical GPU cost so a quality preset can
// be assembled against a frame budget."
//
// THE DECLARED COST IS THE POINT, AND IT IS THE SAME MECHANISM AS THE SHADOW BUDGET'S. M7
// `design.md` §2.10: "a subsystem must declare what each ladder position costs relative to position
// 0. An arbiter allocating milliseconds over a ladder it cannot price is choosing blind." A post
// chain whose stages cannot be priced is a chain the renderer budget arbiter cannot reason about,
// and the arbiter is what makes six unbounded systems into one frame.
//
// The numbers below are DECLARED, at a stated reference resolution, and they are the engine's
// defaults rather than measurements of this machine — there is no device in this module. A project
// that measures its own replaces them; `PostQualityTable` exists so that replacing them is one
// object and not an edit to every call site.

#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/rendering/post/chain.h>

namespace cy::rendering {

/// The five levels, in the order the requirement names them. `Off` is a level and not the absence
/// of one: a stage at `Off` is absent from the chain, which is what `chain.h` already arranges.
enum class QualityLevel : u8 {
    Off = 0,
    Low,
    Medium,
    High,
    Ultra,
    Count,
};

[[nodiscard]] const char* quality_level_name(QualityLevel level) noexcept;

inline constexpr u32 kQualityLevelCount = static_cast<u32>(QualityLevel::Count);

/// The concrete parameters a level maps to. Not every field means something to every stage — a
/// tonemap has no sample count — and a field a stage ignores reads zero rather than a plausible
/// number, so that a stage which started reading it fails loudly.
struct EffectQuality {
    /// Fraction of the render resolution the effect is computed at.
    f32 resolution_scale = 1.0F;
    /// Samples per pixel, taps, or directions, depending on the effect.
    u32 samples = 0;
    /// Iterations, mip levels, or passes.
    u32 iterations = 0;
    /// Declared cost in milliseconds at `kReferencePixels`. Scaled by
    /// `scaled_cost_ms()` for other resolutions.
    f32 cost_ms = 0.0F;
};

/// The resolution the declared costs are stated at: 1920 x 1080. Stating it is what lets a cost be
/// scaled honestly rather than compared against a number nobody wrote down.
inline constexpr u64 kReferencePixels = 1920ULL * 1080ULL;

/// The whole table: one `EffectQuality` per stage per level.
struct PostQualityTable {
    EffectQuality entries[static_cast<usize>(PostStage::Count)][kQualityLevelCount] = {};

    [[nodiscard]] const EffectQuality& at(PostStage stage, QualityLevel level) const noexcept;
};

/// The engine's declared defaults.
[[nodiscard]] const PostQualityTable& default_post_quality_table() noexcept;

/// A preset: one level per stage.
struct PostQualityPreset {
    QualityLevel levels[static_cast<usize>(PostStage::Count)] = {};

    [[nodiscard]] QualityLevel level(PostStage stage) const noexcept;
    void set(PostStage stage, QualityLevel value) noexcept;
};

/// The three named presets a project starts from. `Medium` is what "WHEN a project selects the
/// 'medium' preset" refers to.
[[nodiscard]] PostQualityPreset post_quality_preset(QualityLevel level) noexcept;

/// The summed declared cost of the stages actually in `chain`, at this resolution. The number the
/// requirement asks to be reportable against the target frame time.
[[nodiscard]] f32 preset_cost_ms(Span<const PostStage> chain, const PostQualityPreset& preset,
                                 const PostQualityTable& table, u32 width, u32 height) noexcept;

/// One stage's declared cost at this resolution.
[[nodiscard]] f32 scaled_cost_ms(const EffectQuality& quality, u32 width, u32 height) noexcept;

/// Lower every stage's level until the summed cost fits `budget_ms`, in the chain's own reverse
/// order — the last stage of the chain is the first to give ground, which matches `residency`'s
/// meaning of a declared reduction order and the shadow budget's use of it.
///
/// Returns true when it fits. False means every stage is at `Low` and the budget still does not
/// hold, which is an honest answer and not a failure to try.
[[nodiscard]] bool fit_preset_to_budget(Span<const PostStage> chain, const PostQualityTable& table,
                                        u32 width, u32 height, f32 budget_ms,
                                        PostQualityPreset& preset) noexcept;

}  // namespace cy::rendering
