#include <cy/rendering/post/quality.h>

#include <cy/core/math/scalar.h>

namespace cy::rendering {
namespace {

void declare(PostQualityTable& table, PostStage stage, QualityLevel level, f32 resolution_scale,
             u32 samples, u32 iterations, f32 cost_ms) noexcept {
    EffectQuality& entry = table.entries[static_cast<usize>(stage)][static_cast<usize>(level)];
    entry.resolution_scale = resolution_scale;
    entry.samples = samples;
    entry.iterations = iterations;
    entry.cost_ms = cost_ms;
}

/// The expensive stages: the ones whose cost a preset actually moves. The cheap ones are declared
/// with one line each below, at the same cost on every level, because a stage whose cost does not
/// vary should not pretend to have a ladder.
void declare_variable_stages(PostQualityTable& table) noexcept {
    declare(table, PostStage::AmbientOcclusion, QualityLevel::Low, 0.5F, 4, 1, 0.35F);
    declare(table, PostStage::AmbientOcclusion, QualityLevel::Medium, 0.5F, 8, 2, 0.60F);
    declare(table, PostStage::AmbientOcclusion, QualityLevel::High, 1.0F, 12, 2, 1.20F);
    declare(table, PostStage::AmbientOcclusion, QualityLevel::Ultra, 1.0F, 24, 3, 2.10F);

    declare(table, PostStage::SubsurfaceScattering, QualityLevel::Low, 0.5F, 8, 1, 0.25F);
    declare(table, PostStage::SubsurfaceScattering, QualityLevel::Medium, 1.0F, 12, 1, 0.45F);
    declare(table, PostStage::SubsurfaceScattering, QualityLevel::High, 1.0F, 16, 2, 0.70F);
    declare(table, PostStage::SubsurfaceScattering, QualityLevel::Ultra, 1.0F, 24, 2, 1.00F);

    declare(table, PostStage::VolumetricFog, QualityLevel::Low, 0.5F, 1, 32, 0.40F);
    declare(table, PostStage::VolumetricFog, QualityLevel::Medium, 0.5F, 1, 64, 0.75F);
    declare(table, PostStage::VolumetricFog, QualityLevel::High, 1.0F, 2, 64, 1.40F);
    declare(table, PostStage::VolumetricFog, QualityLevel::Ultra, 1.0F, 4, 128, 2.60F);

    declare(table, PostStage::ScreenSpaceReflections, QualityLevel::Low, 0.5F, 1, 16, 0.45F);
    declare(table, PostStage::ScreenSpaceReflections, QualityLevel::Medium, 0.5F, 2, 32, 0.85F);
    declare(table, PostStage::ScreenSpaceReflections, QualityLevel::High, 1.0F, 2, 48, 1.70F);
    declare(table, PostStage::ScreenSpaceReflections, QualityLevel::Ultra, 1.0F, 4, 64, 3.00F);

    declare(table, PostStage::DepthOfField, QualityLevel::Low, 0.5F, 8, 1, 0.30F);
    declare(table, PostStage::DepthOfField, QualityLevel::Medium, 0.5F, 16, 1, 0.55F);
    declare(table, PostStage::DepthOfField, QualityLevel::High, 1.0F, 24, 2, 1.10F);
    declare(table, PostStage::DepthOfField, QualityLevel::Ultra, 1.0F, 48, 2, 1.90F);

    declare(table, PostStage::MotionBlur, QualityLevel::Low, 0.5F, 4, 1, 0.20F);
    declare(table, PostStage::MotionBlur, QualityLevel::Medium, 1.0F, 8, 1, 0.40F);
    declare(table, PostStage::MotionBlur, QualityLevel::High, 1.0F, 12, 1, 0.60F);
    declare(table, PostStage::MotionBlur, QualityLevel::Ultra, 1.0F, 20, 1, 0.95F);

    declare(table, PostStage::Bloom, QualityLevel::Low, 0.5F, 1, 4, 0.15F);
    declare(table, PostStage::Bloom, QualityLevel::Medium, 0.5F, 1, 5, 0.25F);
    declare(table, PostStage::Bloom, QualityLevel::High, 0.5F, 1, 6, 0.35F);
    declare(table, PostStage::Bloom, QualityLevel::Ultra, 1.0F, 1, 7, 0.60F);

    declare(table, PostStage::TemporalAntiAliasing, QualityLevel::Low, 1.0F, 4, 1, 0.25F);
    declare(table, PostStage::TemporalAntiAliasing, QualityLevel::Medium, 1.0F, 8, 1, 0.35F);
    declare(table, PostStage::TemporalAntiAliasing, QualityLevel::High, 1.0F, 9, 1, 0.45F);
    declare(table, PostStage::TemporalAntiAliasing, QualityLevel::Ultra, 1.0F, 16, 1, 0.65F);

    declare(table, PostStage::TemporalUpscaling, QualityLevel::Low, 0.5F, 8, 1, 0.55F);
    declare(table, PostStage::TemporalUpscaling, QualityLevel::Medium, 0.59F, 9, 1, 0.70F);
    declare(table, PostStage::TemporalUpscaling, QualityLevel::High, 0.67F, 12, 1, 0.85F);
    declare(table, PostStage::TemporalUpscaling, QualityLevel::Ultra, 0.77F, 16, 1, 1.05F);

    declare(table, PostStage::PostTonemapAntiAliasing, QualityLevel::Low, 1.0F, 1, 1, 0.08F);
    declare(table, PostStage::PostTonemapAntiAliasing, QualityLevel::Medium, 1.0F, 1, 1, 0.12F);
    declare(table, PostStage::PostTonemapAntiAliasing, QualityLevel::High, 1.0F, 2, 2, 0.20F);
    declare(table, PostStage::PostTonemapAntiAliasing, QualityLevel::Ultra, 1.0F, 4, 2, 0.30F);
}

/// The stages whose cost is a lookup or a multiply: the same at every level, declared once each so
/// that the sum against a frame budget is complete rather than nearly complete.
void declare_fixed_stages(PostQualityTable& table) noexcept {
    struct Fixed {
        PostStage stage;
        f32 cost_ms;
    };
    const Fixed fixed[] = {
        {PostStage::AutoExposureMeasurement, 0.06F},
        {PostStage::ExposureApply, 0.02F},
        {PostStage::Tonemap, 0.05F},
        {PostStage::ColourGrading, 0.05F},
        {PostStage::DisplaySpaceEffects, 0.06F},
        {PostStage::OutputEncoding, 0.03F},
    };
    for (const Fixed& entry : fixed) {
        for (u32 level = 1; level < kQualityLevelCount; ++level) {
            declare(table, entry.stage, static_cast<QualityLevel>(level), 1.0F, 1, 1,
                    entry.cost_ms);
        }
    }
}

[[nodiscard]] PostQualityTable build_default_table() noexcept {
    PostQualityTable table;
    declare_variable_stages(table);
    declare_fixed_stages(table);
    return table;
}

}  // namespace

const char* quality_level_name(QualityLevel level) noexcept {
    switch (level) {
        case QualityLevel::Off:
            return "Off";
        case QualityLevel::Low:
            return "Low";
        case QualityLevel::Medium:
            return "Medium";
        case QualityLevel::High:
            return "High";
        case QualityLevel::Ultra:
            return "Ultra";
        case QualityLevel::Count:
            break;
    }
    return "Unknown";
}

const EffectQuality& PostQualityTable::at(PostStage stage, QualityLevel level) const noexcept {
    const auto stage_index =
        math::min(static_cast<usize>(stage), static_cast<usize>(PostStage::Count) - 1U);
    const auto level_index =
        math::min(static_cast<usize>(level), static_cast<usize>(kQualityLevelCount) - 1U);
    return entries[stage_index][level_index];
}

const PostQualityTable& default_post_quality_table() noexcept {
    static const PostQualityTable table = build_default_table();
    return table;
}

QualityLevel PostQualityPreset::level(PostStage stage) const noexcept {
    const auto index = static_cast<usize>(stage);
    return index < static_cast<usize>(PostStage::Count) ? levels[index] : QualityLevel::Off;
}

void PostQualityPreset::set(PostStage stage, QualityLevel value) noexcept {
    const auto index = static_cast<usize>(stage);
    if (index < static_cast<usize>(PostStage::Count)) {
        levels[index] = value;
    }
}

PostQualityPreset post_quality_preset(QualityLevel level) noexcept {
    PostQualityPreset preset;
    for (QualityLevel& entry : preset.levels) {
        entry = level;
    }
    return preset;
}

f32 scaled_cost_ms(const EffectQuality& quality, u32 width, u32 height) noexcept {
    const u64 pixels = static_cast<u64>(width) * static_cast<u64>(height);
    const f32 ratio = static_cast<f32>(pixels) / static_cast<f32>(kReferencePixels);
    // The effect's own resolution scale is already in the declared cost; what varies here is the
    // output resolution the whole chain runs at.
    return quality.cost_ms * ratio;
}

f32 preset_cost_ms(Span<const PostStage> chain, const PostQualityPreset& preset,
                   const PostQualityTable& table, u32 width, u32 height) noexcept {
    f32 total = 0.0F;
    for (const PostStage stage : chain) {
        total += scaled_cost_ms(table.at(stage, preset.level(stage)), width, height);
    }
    return total;
}

bool fit_preset_to_budget(Span<const PostStage> chain, const PostQualityTable& table, u32 width,
                          u32 height, f32 budget_ms, PostQualityPreset& preset) noexcept {
    if (preset_cost_ms(chain, preset, table, width, height) <= budget_ms) {
        return true;
    }
    // Reverse chain order: the last stage of the chain gives ground first. That keeps this the same
    // shape as `residency`'s declared reduction order and the shadow budget's use of it, rather
    // than a second way of deciding what to give up.
    for (usize step = chain.size(); step > 0; --step) {
        const PostStage stage = chain[step - 1];
        while (preset.level(stage) > QualityLevel::Low) {
            preset.set(stage, static_cast<QualityLevel>(static_cast<u8>(preset.level(stage)) - 1U));
            if (preset_cost_ms(chain, preset, table, width, height) <= budget_ms) {
                return true;
            }
        }
    }
    return false;
}

}  // namespace cy::rendering
