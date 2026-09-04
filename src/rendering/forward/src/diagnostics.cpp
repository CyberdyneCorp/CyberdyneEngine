#include <cy/rendering/forward/diagnostics.h>

#include <algorithm>

namespace cy::rendering {

SortKeyDistribution analyse_sort_keys(Span<const render::DrawItem> draws) noexcept {
    SortKeyDistribution distribution;
    distribution.draws = static_cast<u32>(draws.size());
    distribution.keys_non_decreasing = true;
    if (draws.empty()) {
        return distribution;
    }

    u64 previous_key = 0;
    u32 previous_pipeline = 0;
    u32 previous_material = 0;
    bool first = true;
    for (const render::DrawItem& draw : draws) {
        const auto layer = static_cast<u32>(render::sort_key_layer(draw.key));
        if (layer < render::kSortLayerCount) {
            ++distribution.layer_draws[layer];
        }
        const u32 pipeline = render::sort_key_pipeline(draw.key);
        const u32 material = render::sort_key_material(draw.key);
        if (first || pipeline != previous_pipeline) {
            ++distribution.pipeline_runs;
        }
        if (first || material != previous_material || pipeline != previous_pipeline) {
            ++distribution.material_runs;
        }
        if (!first && draw.key < previous_key) {
            distribution.keys_non_decreasing = false;
        }
        previous_key = draw.key;
        previous_pipeline = pipeline;
        previous_material = material;
        first = false;
    }
    return distribution;
}

void FrameDiagnostics::clear() noexcept {
    clusters = ClusterStatistics{};
    cull = CullStatistics{};
    shadows = ShadowAtlasStatistics{};
    sort = SortKeyDistribution{};
    pipelines = PipelineCacheDiagnostics{};
    passes.clear();
    prepass_mode = PrepassMode::DepthOnly;
}

u32 FrameDiagnostics::total_draw_calls() const noexcept {
    u32 total = 0;
    for (const PassDiagnostics& pass : passes.span()) {
        total += pass.draw_calls;
    }
    return total;
}

u64 FrameDiagnostics::total_triangles() const noexcept {
    u64 total = 0;
    for (const PassDiagnostics& pass : passes.span()) {
        total += pass.triangles;
    }
    return total;
}

u64 FrameDiagnostics::total_gpu_nanoseconds() const noexcept {
    u64 total = 0;
    for (const PassDiagnostics& pass : passes.span()) {
        if (pass.gpu_measured) {
            total += pass.gpu_nanoseconds;
        }
    }
    return total;
}

bool FrameDiagnostics::any_gpu_measured() const noexcept {
    const Span<const PassDiagnostics> recorded = passes.span();
    return std::ranges::any_of(recorded,
                               [](const PassDiagnostics& pass) { return pass.gpu_measured; });
}

Status record_pass(FrameDiagnostics& diagnostics, const PassDiagnostics& pass) noexcept {
    for (const PassDiagnostics& existing : diagnostics.passes.span()) {
        if (existing.kind == pass.kind) {
            return fail(ErrorCode::AlreadyExists,
                        "frame diagnostics: this pass has already been recorded");
        }
    }
    return diagnostics.passes.push_back(pass);
}

void accumulate_into(render::FrameStatistics& frame, const FrameDiagnostics& diagnostics) noexcept {
    frame.instances_visible += diagnostics.cull.visible;
    frame.passes += static_cast<u32>(diagnostics.passes.size());
    for (const PassDiagnostics& pass : diagnostics.passes.span()) {
        frame.draw_calls += pass.draw_calls;
        frame.draws_merged += pass.draws_merged;
        frame.triangles += pass.triangles;
        frame.add_pass_timing(pass.name, pass.gpu_nanoseconds, pass.gpu_measured);
    }
}

}  // namespace cy::rendering
