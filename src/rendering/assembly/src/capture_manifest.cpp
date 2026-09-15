#include <cy/rendering/assembly/capture_manifest.h>

#include <cstdio>

namespace cy::rendering::assembly {
namespace {

/// Lower-case one ASCII byte. `std::tolower` takes a locale and this comparison must not.
[[nodiscard]] char lowered(char value) noexcept {
    return (value >= 'A' && value <= 'Z') ? static_cast<char>(value - 'A' + 'a') : value;
}

/// Case-insensitive substring search over ASCII. `needle` is already lower-case.
[[nodiscard]] bool contains(const char* haystack, const char* needle) noexcept {
    if (haystack == nullptr || needle == nullptr || *needle == '\0') {
        return false;
    }
    for (const char* start = haystack; *start != '\0'; ++start) {
        const char* left = start;
        const char* right = needle;
        while (*left != '\0' && *right != '\0' && lowered(*left) == *right) {
            ++left;
            ++right;
        }
        if (*right == '\0') {
            return true;
        }
    }
    return false;
}

}  // namespace

const char* capture_refusal_name(CaptureRefusal refusal) noexcept {
    switch (refusal) {
        case CaptureRefusal::None:
            return "None";
        case CaptureRefusal::FrameNotExecuted:
            return "FrameNotExecuted";
        case CaptureRefusal::ChainRefused:
            return "ChainRefused";
        case CaptureRefusal::ArbiterUnpinned:
            return "ArbiterUnpinned";
        case CaptureRefusal::NoPostChain:
            return "NoPostChain";
        case CaptureRefusal::Count:
            break;
    }
    return "?";
}

bool CaptureManifest::ran(PostStage stage) const noexcept {
    return position_of(stage) != stage_count;
}

u32 CaptureManifest::position_of(PostStage stage) const noexcept {
    for (u32 index = 0; index < stage_count; ++index) {
        if (stages[index].stage == stage) {
            return index;
        }
    }
    return stage_count;
}

CaptureRefusal capture_refusal(const AssemblyReport& report,
                               const CaptureProvenance& provenance) noexcept {
    if (!report.executed) {
        return CaptureRefusal::FrameNotExecuted;
    }
    if (report.post_refusal != PostChainRefusal::None) {
        return CaptureRefusal::ChainRefused;
    }
    if (provenance.purpose != CapturePurpose::Publication) {
        return CaptureRefusal::None;
    }
    if (!provenance.arbiter_pinned) {
        return CaptureRefusal::ArbiterUnpinned;
    }
    if (report.post_stages == 0) {
        return CaptureRefusal::NoPostChain;
    }
    return CaptureRefusal::None;
}

Expected<CaptureManifest, Error> capture_manifest(const AssemblyDescription& description,
                                                  const AssemblyReport& report,
                                                  const CaptureProvenance& provenance) noexcept {
    const CaptureRefusal refusal = capture_refusal(report, provenance);
    if (refusal != CaptureRefusal::None) {
        return fail(ErrorCode::InvalidArgument, capture_refusal_name(refusal));
    }

    CaptureManifest manifest;
    manifest.title = provenance.title != nullptr ? provenance.title : "";
    manifest.purpose = provenance.purpose;
    manifest.width = description.width;
    manifest.height = description.height;
    manifest.ev100 = provenance.ev100;
    manifest.tonemap = provenance.tonemap;
    manifest.arbiter_pinned = provenance.arbiter_pinned;

    // THE STAGE LIST IS COPIED, NEVER RE-DERIVED. `report.post_stage` is the array
    // `build_post_chain` filled, in the order it filled it. Rebuilding the chain here from
    // `description.post` would answer what the project asked for, which is the question this
    // manifest exists NOT to answer.
    manifest.stage_count = report.post_stages < kMaxPostStages ? report.post_stages : kMaxPostStages;
    for (u32 index = 0; index < manifest.stage_count; ++index) {
        const PostStage stage = report.post_stage[index];
        manifest.stages[index].stage = stage;
        manifest.stages[index].step = post_stage_step(stage);
        manifest.stages[index].space = post_stage_colour_space(stage);
        manifest.stages[index].quality = provenance.quality.level(stage);
    }

    manifest.prepass = report.prepass;
    manifest.velocity_written = report.prepass == PrepassMode::DepthNormalVelocity;
    manifest.temporal_frame = report.temporal_frame;
    manifest.temporal_invalidated = report.temporal_invalidated;
    manifest.jitter_pinned = report.jitter_pinned;
    manifest.jitter_index = report.jitter_index;
    manifest.passes_declared = report.passes_declared;
    manifest.draws = report.draws;
    return manifest;
}

Expected<usize, Error> write_capture_manifest(const CaptureManifest& manifest, char* out,
                                              usize capacity) noexcept {
    if (out == nullptr || capacity == 0) {
        return fail(ErrorCode::InvalidArgument, "no buffer to write into");
    }
    usize written = 0;
    // A single append helper, because every one of the appends below has the same two failure
    // modes — a negative return and a truncation — and a manifest that was silently truncated is a
    // stage list published as complete.
    const auto append = [&](const char* format, auto... args) noexcept -> bool {
        if (written >= capacity) {
            return false;
        }
        const int count = std::snprintf(out + written, capacity - written, format, args...);
        if (count < 0 || static_cast<usize>(count) >= capacity - written) {
            return false;
        }
        written += static_cast<usize>(count);
        return true;
    };

    bool fits = append("capture %s\n", manifest.title);
    fits = fits && append("purpose %s\n", manifest.purpose == CapturePurpose::Publication
                                              ? "publication"
                                              : "diagnostic");
    fits = fits && append("resolution %ux%u\n", manifest.width, manifest.height);
    fits = fits && append("exposure-ev100 %.3f\n", static_cast<double>(manifest.ev100));
    fits = fits && append("tonemap %s\n", tonemap_operator_name(manifest.tonemap));
    fits = fits && append("arbiter %s\n", manifest.arbiter_pinned ? "pinned" : "unpinned");
    fits = fits && append("prepass %s\n", prepass_mode_name(manifest.prepass));
    fits = fits && append("velocity %s\n", manifest.velocity_written ? "written" : "absent");
    fits = fits && append("temporal-frame %llu\n",
                          static_cast<unsigned long long>(manifest.temporal_frame));
    fits = fits && append("temporal-invalidated %s\n", manifest.temporal_invalidated ? "yes" : "no");
    fits = fits && append("jitter %s\n", manifest.jitter_pinned ? "pinned" : "free-running");
    fits = fits && append("jitter-index %u\n", manifest.jitter_index);
    fits = fits && append("passes %u\n", manifest.passes_declared);
    fits = fits && append("draws %u\n", manifest.draws);
    fits = fits && append("post-stages %u\n", manifest.stage_count);
    for (u32 index = 0; index < manifest.stage_count && fits; ++index) {
        const CaptureStage& stage = manifest.stages[index];
        fits = append("  %u step %u %s %s %s\n", index + 1, stage.step,
                      post_stage_name(stage.stage), colour_space_name(stage.space),
                      quality_level_name(stage.quality));
    }
    if (!fits) {
        return fail(ErrorCode::OutOfMemory,
                    "the manifest does not fit the buffer it was given");
    }
    return written;
}

const char* caption_alias(PostStage stage) noexcept {
    // THE PROSE A CAPTION ACTUALLY USES, lower-cased. A stage with no alias is one no caption
    // claims — nobody writes "this still went through output encoding" — and a claim that cannot be
    // made cannot be checked, which is the honest answer rather than a word invented so the table
    // looks complete.
    switch (stage) {
        case PostStage::AmbientOcclusion:
            return "ambient occlusion";
        case PostStage::SubsurfaceScattering:
            return "subsurface scattering";
        case PostStage::VolumetricFog:
            return "volumetric fog";
        case PostStage::ScreenSpaceReflections:
            return "screen-space reflections";
        case PostStage::TemporalAntiAliasing:
            return "temporal anti-aliasing";
        case PostStage::TemporalUpscaling:
            return "temporal upscaling";
        case PostStage::AutoExposureMeasurement:
            return "auto exposure";
        case PostStage::DepthOfField:
            return "depth of field";
        case PostStage::MotionBlur:
            return "motion blur";
        case PostStage::Bloom:
            return "bloom";
        case PostStage::ExposureApply:
            return "exposure";
        case PostStage::Tonemap:
            return "tone mapping";
        case PostStage::ColourGrading:
            return "colour grading";
        case PostStage::DisplaySpaceEffects:
            return "display-space effects";
        case PostStage::PostTonemapAntiAliasing:
            return "post-tonemap anti-aliasing";
        case PostStage::OutputEncoding:
        case PostStage::Count:
            break;
    }
    return nullptr;
}

CaptionCheck check_caption(const CaptureManifest& manifest, const char* caption) noexcept {
    CaptionCheck check;
    if (caption == nullptr) {
        check.diagnostic = "no caption";
        return check;
    }
    for (u32 index = 0; index < static_cast<u32>(PostStage::Count); ++index) {
        const auto stage = static_cast<PostStage>(index);
        const char* alias = caption_alias(stage);
        if (alias == nullptr || !contains(caption, alias)) {
            continue;
        }
        check.claimed += 1;
        if (manifest.ran(stage)) {
            check.confirmed += 1;
            continue;
        }
        if (check.missing == PostStage::Count) {
            check.missing = stage;
            check.diagnostic = post_stage_name(stage);
        }
    }
    // THE ONE CLAIM THAT IS ABOUT A CATEGORY RATHER THAN A STAGE. "Anti-aliasing" is what a caption
    // writes, and three different stages answer it — so a per-stage table alone would let the most
    // common caption in this project make no checkable claim at all. It is satisfied by ANY of the
    // three, and refused naming the temporal one, which is the stage a frame that meant to
    // anti-alias would have run.
    if (contains(caption, "anti-aliasing") || contains(caption, "antialiasing")) {
        check.claimed += 1;
        const bool any = manifest.ran(PostStage::TemporalAntiAliasing) ||
                         manifest.ran(PostStage::TemporalUpscaling) ||
                         manifest.ran(PostStage::PostTonemapAntiAliasing);
        if (any) {
            check.confirmed += 1;
        } else if (check.missing == PostStage::Count) {
            check.missing = PostStage::TemporalAntiAliasing;
            check.diagnostic =
                "the caption claims anti-aliasing and the frame ran no anti-aliasing stage";
        }
    }
    if (check.ok()) {
        check.diagnostic = "every stage the caption names is in the frame's own stage list";
    }
    return check;
}

}  // namespace cy::rendering::assembly
