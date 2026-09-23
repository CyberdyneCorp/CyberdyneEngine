// THE EMBEDDED MODULES AGREE WITH THE BLOCK THEY READ. M11.c task 6.3.
//
// ================================================================================================
// THE DEFECT THIS SUITE EXISTS FOR
// ================================================================================================
//
// `particle_spirv.h` was compiled on 2026-09-10 against `cy/frame.slang` as it was then. On
// 2026-09-20 the temporal anti-aliasing work inserted `previousRelativeToClipRow0..3` into the
// per-view block BEFORE `relativeToViewRow0..3`, and the C++ half, `FrameViewData`, moved with it —
// its static assertions put `relative_to_view` at 128. The embedded particle module still read it
// at
// 64. So every sprite's billboard basis was read out of LAST FRAME'S CLIP MATRIX: a mote on the
// first frame of a view was one size and on every frame after it another, `render.vfx`'s shot
// rendered twice came back 39 524 texels apart, and `m11c:vfx-in-the-shot` went red on four
// assertions.
//
// Nothing compared the two because nothing COULD without a Slang compiler, and the modules are
// checked in precisely so a build needs none. But a SPIR-V module carries its own layout: every
// member of the block has an `OpMemberName` and an `Offset` decoration. This suite reads them out
// of the bytes the renderer actually hands the driver and compares every one against `offsetof`.
//
// Regression: with the stale module back, `relativeToViewRow0` is at 64 against 128 and the case is
// red on every row that moved.

#include <cy/rendering/particles/particle_renderer.h>
#include <cy/rendering/pipeline/frame_pipelines.h>
#include <cy/test/test.h>

#include "particle_spirv.h"
#include "strip_spirv.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

using namespace cy;
using cy::rendering::pipeline::FrameViewData;

namespace {

constexpr u32 kSpirvMagic = 0x07230203U;
constexpr u32 kOpMemberName = 6;
constexpr u32 kOpMemberDecorate = 72;
constexpr u32 kDecorationOffset = 35;
constexpr u32 kHeaderWords = 5;
constexpr u32 kNotFound = ~0U;

/// Where `cy/frame.slang`'s block puts each member, according to its C++ half. The first row of
/// each matrix stands for all four: the rows are one float4 apart and a module that has the first
/// right and a later one wrong is not a module any compiler produces.
struct Member {
    const char* name;
    u32 offset;
};

constexpr Member kFrameMembers[] = {
    {"relativeToClipRow0", offsetof(FrameViewData, relative_to_clip)},
    {"previousRelativeToClipRow0", offsetof(FrameViewData, previous_relative_to_clip)},
    {"relativeToViewRow0", offsetof(FrameViewData, relative_to_view)},
    {"relativeToViewRow1", offsetof(FrameViewData, relative_to_view) + 16U},
    {"ambientAndOcclusion", offsetof(FrameViewData, ambient_and_occlusion)},
    {"extentAndInverse", offsetof(FrameViewData, extent_and_inverse)},
    {"clusterGrid", offsetof(FrameViewData, cluster_dimensions)},
    {"counts", offsetof(FrameViewData, counts)},
    {"materialOffsets", offsetof(FrameViewData, material_offsets)},
    {"temporalFeedback", offsetof(FrameViewData, temporal_feedback)},
    {"temporalJitter", offsetof(FrameViewData, temporal_jitter)},
    {"materialTextures", offsetof(FrameViewData, material_textures)},
};

/// The `Offset` decoration a module carries for the member called `name`, or `kNotFound`.
///
/// Two passes over the instruction stream: find the (struct, member) the name belongs to, then the
/// decoration on that pair. A name two structs share would be ambiguous, and `relativeToViewRow0`
/// belongs to exactly one — `CyFrameData`.
[[nodiscard]] u32 member_offset(Span<const u32> words, const char* name) noexcept {
    if (words.size() < kHeaderWords || words[0] != kSpirvMagic) {
        return kNotFound;
    }
    u32 target = kNotFound;
    u32 member = kNotFound;
    for (usize at = kHeaderWords; at < words.size();) {
        const u32 opcode = words[at] & 0xFFFFU;
        const u32 count = words[at] >> 16U;
        if (count == 0 || at + count > words.size()) {
            return kNotFound;
        }
        if (opcode == kOpMemberName && count > 3) {
            const auto* text = reinterpret_cast<const char*>(&words[at + 3]);
            const usize room = (count - 3U) * sizeof(u32);
            if (std::strncmp(text, name, room) == 0 && std::strlen(name) < room) {
                target = words[at + 1];
                member = words[at + 2];
            }
        }
        at += count;
    }
    if (target == kNotFound) {
        return kNotFound;
    }
    for (usize at = kHeaderWords; at < words.size();) {
        const u32 opcode = words[at] & 0xFFFFU;
        const u32 count = words[at] >> 16U;
        if (opcode == kOpMemberDecorate && count >= 5 && words[at + 1] == target &&
            words[at + 2] == member && words[at + 3] == kDecorationOffset) {
            return words[at + 4];
        }
        at += count;
    }
    return kNotFound;
}

void check_module(const char* module, Span<const u32> words) {
    for (const Member& expected : kFrameMembers) {
        const u32 found = member_offset(words, expected.name);
        if (found != expected.offset) {
            std::fprintf(stderr, "%s: `%s` is at %d in the module and at %u in FrameViewData\n",
                         module, expected.name, found == kNotFound ? -1 : static_cast<int>(found),
                         expected.offset);
        }
        CY_CHECK_EQ(found, expected.offset);
    }
}

}  // namespace

CY_TEST_CASE(
    "the embedded particle and strip modules read the per-view block where C++ writes it") {
    // The vertex stages are the ones that read the block; the fragment stages import it and read
    // nothing from it, so their modules carry no layout to compare.
    check_module("particle vertex",
                 Span<const u32>(rendering::particles::kParticleVertexSpirv,
                                 sizeof(rendering::particles::kParticleVertexSpirv) / sizeof(u32)));
    check_module("strip vertex",
                 Span<const u32>(rendering::particles::kStripVertexSpirv,
                                 sizeof(rendering::particles::kStripVertexSpirv) / sizeof(u32)));
}

CY_TEST_CASE("the layout reader finds what it is asked for and refuses what is not SPIR-V") {
    // The negative controls for the reader itself: a check that returned the expected offset for a
    // name that is not there would pass on any module at all.
    const Span<const u32> particle(
        rendering::particles::kParticleVertexSpirv,
        sizeof(rendering::particles::kParticleVertexSpirv) / sizeof(u32));
    CY_CHECK_EQ(member_offset(particle, "noSuchMemberAnywhere"), kNotFound);
    const u32 not_spirv[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    CY_CHECK_EQ(member_offset(Span<const u32>(not_spirv, 8), "relativeToViewRow0"), kNotFound);
    CY_CHECK_EQ(member_offset(particle, "relativeToClipRow0"), 0U);
}
