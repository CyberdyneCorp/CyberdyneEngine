#pragma once
// WHAT THE FRAME ACTUALLY RAN, EMITTED BY THE FRAME. M11.c task 3.2.
//
// ================================================================================================
// THE DEFECT THIS CLOSES, AND IT IS A DEFECT IN A CAPTION RATHER THAN IN A SHADER
// ================================================================================================
//
// `rendering-post-processing` fixes the chain's order and its colour space and every requirement
// under it is about a stage. NONE OF IT CONSTRAINS WHAT A PUBLISHED PICTURE ACTUALLY RAN. The
// largest capture this project has published — `docs/design/images/m10-world.png` — comes out of a
// target that links neither the post chain nor the assembled frame, so a caption claiming tone
// mapping and anti-aliasing would have been wrong with NOTHING IN THE TREE ABLE TO SAY SO.
//
// A caption is the one part of an artefact nobody can re-run. So the frame emits its own stage
// list, and the caption is checked against it:
//
//   > A frame that is captured for publication SHALL emit the list of post stages it actually
//   > executed, with their order and their quality level, produced by the frame rather than
//   > written by hand.
//
// ================================================================================================
// WHY IT IS BUILT FROM `AssemblyReport` AND NOT FROM `AssemblyDescription`
// ================================================================================================
//
// The description is what the project ASKED FOR and the report is what the frame DID. They differ
// in exactly the case this exists for: a chain that refused (`PostChainRefusal`), a temporal stage
// the frame had no motion vectors for, a stage a quality preset turned off. `capture_manifest()`
// reads `AssemblyReport::post_stage[]` — the array `build_post_chain` wrote, in the order it wrote
// it — and the description only for the things a report cannot carry: the resolution the frame was
// declared at and the quality preset the stages ran at.
//
// AND IT REFUSES A FRAME THAT DID NOT RUN. `AssemblyReport::executed` is false until
// `FrameAssembly::execute` has returned, so a manifest cannot be produced from a frame that was
// only assembled — which would be a stage list for a picture that does not exist.
//
// ================================================================================================
// THE PINNED REFUSAL, WHICH IS A REFUSAL AND NOT A WARNING
// ================================================================================================
//
// > A capture intended for publication SHALL be taken with the budget arbiter pinned, so the
// > published frame is not one the arbiter degraded mid-capture and then described as authored
// > quality.
//
// `CaptureProvenance::arbiter_pinned` is READ OFF `ArbiterReport::pinned` by the capturing caller —
// this module does not link `cy::rendering-arbiter` and must not: the assembly "chooses no quality
// level, owns no configuration asset and reads no project settings". What it can do is refuse to
// produce a publication manifest for an unpinned frame, which is what makes the rule a mechanism.
// A caller that wants the numbers without the claim asks for `CapturePurpose::Diagnostic`.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/rendering/assembly/frame_assembly.h>
#include <cy/rendering/post/chain.h>
#include <cy/rendering/post/quality.h>
#include <cy/rendering/post/tonemap.h>

namespace cy::rendering::assembly {

/// What the capture is for. The two differ in ONE way and it is the point of the distinction: a
/// publication manifest may not be produced for an unpinned frame.
enum class CapturePurpose : u8 {
    /// A still or a video published as evidence of what the engine produces.
    Publication = 0,
    /// A capture read by a developer. Every number is the same; nothing is refused.
    Diagnostic,
    Count,
};

/// Why a manifest could not be produced. `None` is a manifest that was.
enum class CaptureRefusal : u8 {
    None = 0,
    /// The frame was assembled and never executed, so there is no picture this describes.
    FrameNotExecuted,
    /// The post chain refused, so the frame ran a chain nobody asked for. `AssemblyReport` carries
    /// which refusal.
    ChainRefused,
    /// A publication capture taken while the budget arbiter was free to degrade the frame.
    ArbiterUnpinned,
    /// The frame ran no post stages at all. A picture that went through no chain is a legitimate
    /// capture and is NOT a publication one — it is the "with none" half of a before/after pair,
    /// and it asks for `CapturePurpose::Diagnostic` by name.
    NoPostChain,
    Count,
};

[[nodiscard]] const char* capture_refusal_name(CaptureRefusal refusal) noexcept;

/// The facts about the capture that no report carries, supplied by the caller that took it.
///
/// Everything here is READ from something rather than typed: `arbiter_pinned` from
/// `ArbiterReport::pinned`, `ev100` from the exposure state the frame applied, `tonemap` from the
/// tonemap settings it was configured with. The distinction that matters is that none of it is the
/// STAGE LIST, which is the one thing a caption is tempted to invent.
struct CaptureProvenance {
    /// Names the capture in the manifest. Never null; must outlive the manifest.
    const char* title = "";
    /// What the arbiter reported this frame. A publication capture is refused when it is false.
    bool arbiter_pinned = false;
    CapturePurpose purpose = CapturePurpose::Publication;
    /// The exposure the frame applied, in EV100, and the operator that tonemapped it.
    f32 ev100 = 0.0F;
    TonemapOperator tonemap = default_tonemap_operator();
    /// The quality preset the stages ran at. One level per stage; `post_quality_preset()` builds
    /// the three named ones.
    PostQualityPreset quality;
};

/// One stage of the chain, as the frame ran it.
struct CaptureStage {
    PostStage stage = PostStage::Count;
    /// The specification's own step number, 1 to 15.
    u32 step = 0;
    ColourSpace space = ColourSpace::SceneReferred;
    QualityLevel quality = QualityLevel::Off;
};

/// What one captured frame ran. Produced by `capture_manifest()` and by nothing else.
struct CaptureManifest {
    const char* title = "";
    CapturePurpose purpose = CapturePurpose::Publication;
    u32 width = 0;
    u32 height = 0;
    f32 ev100 = 0.0F;
    TonemapOperator tonemap = default_tonemap_operator();
    bool arbiter_pinned = false;

    /// The chain, in the order the frame ran it.
    u32 stage_count = 0;
    CaptureStage stages[kMaxPostStages] = {};

    /// The frame's structure, which is the half of "did it go through anti-aliasing" a stage list
    /// alone cannot answer: a temporal stage in a frame whose prepass produced no velocity is a
    /// stage reading a target that was never allocated.
    PrepassMode prepass = PrepassMode::DepthOnly;
    bool velocity_written = false;
    u64 temporal_frame = 0;
    bool temporal_invalidated = false;
    /// Whether the jitter sequence was PINNED, and where it started. `temporal-rendering`'s
    /// determinism requirement is what makes a capture reproducible, and it is the one a beauty
    /// shot is most tempted to skip — so it is published rather than assumed.
    bool jitter_pinned = false;
    u32 jitter_index = 0;
    /// Passes the frame declared, and draws it issued. What stops a manifest listing sixteen
    /// stages over a frame with nothing in it.
    u32 passes_declared = 0;
    u32 draws = 0;

    [[nodiscard]] bool ran(PostStage stage) const noexcept;
    /// Where in the chain a stage ran, or `stage_count` when it did not.
    [[nodiscard]] u32 position_of(PostStage stage) const noexcept;
};

/// Build the manifest from what the frame reported. The stage list is copied out of
/// `AssemblyReport::post_stage[]`; nothing here re-derives it from the configuration.
[[nodiscard]] Expected<CaptureManifest, Error> capture_manifest(
    const AssemblyDescription& description, const AssemblyReport& report,
    const CaptureProvenance& provenance) noexcept;

/// Why `capture_manifest` would refuse, without building one. Exposed so a capture recipe can say
/// what is wrong before it spends a frame.
[[nodiscard]] CaptureRefusal capture_refusal(const AssemblyReport& report,
                                             const CaptureProvenance& provenance) noexcept;

/// Write the manifest as the text published beside the picture. Returns the number of bytes
/// written, or an error when `capacity` is too small — truncating a provenance statement is how
/// half a stage list gets published as a whole one.
[[nodiscard]] Expected<usize, Error> write_capture_manifest(const CaptureManifest& manifest,
                                                            char* out, usize capacity) noexcept;

/// What a caption claims, checked against what the frame ran.
///
/// A caption is English, so the match is over the PROSE a caption uses — "tone mapping", "temporal
/// anti-aliasing", "depth of field" — and not over the enumerator spelling. `caption_aliases()`
/// publishes the table so a reader can see exactly which words are load-bearing.
struct CaptionCheck {
    /// The first stage the caption names that the frame did not run, or `PostStage::Count`.
    PostStage missing = PostStage::Count;
    /// How many of the caption's claims the manifest confirmed.
    u32 confirmed = 0;
    /// How many claims it made in total.
    u32 claimed = 0;
    /// Never null. Names the stage when there is one.
    const char* diagnostic = "";

    [[nodiscard]] constexpr bool ok() const noexcept { return missing == PostStage::Count; }
};

/// The prose a caption uses for one stage, lower-cased, or null when the stage has none — a
/// caption that says "output encoding" is not a caption anyone writes.
[[nodiscard]] const char* caption_alias(PostStage stage) noexcept;

/// Check one caption. Case-insensitive, substring, over `caption_alias()`.
[[nodiscard]] CaptionCheck check_caption(const CaptureManifest& manifest,
                                         const char* caption) noexcept;

}  // namespace cy::rendering::assembly
