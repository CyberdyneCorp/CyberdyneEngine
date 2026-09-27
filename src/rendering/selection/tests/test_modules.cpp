// SPDX-License-Identifier: MIT
// The embedded mask module reads the frame's blocks where C++ writes them.
// `unit.rendering_selection`.
//
// `selection_spirv.h` is compiled by hand and checked in, and its mask vertex stage imports
// `cy/frame.slang`: it reads the per-view rows, the draw records and the instance rows. When the
// frame's blocks move and the module is not regenerated, the mask draws every marked unit in the
// wrong place — and the outline is drawn around where the unit is not. The particle module had that
// defect once (src/rendering/particles/tests/test_particle_modules.cpp says how); this is the same
// check for this module, read out of the bytes the renderer hands the driver, with no compiler.

#include <cy/rendering/pipeline/frame_pipelines.h>
#include <cy/test/test.h>

#include "selection_spirv.h"

#include <cstddef>
#include <cstdio>
#include <cstring>

using namespace cy;
using cy::rendering::pipeline::FrameViewData;
using cy::rendering::pipeline::InstanceTransform;

namespace {

constexpr u32 kSpirvMagic = 0x07230203U;
constexpr u32 kOpMemberName = 6;
constexpr u32 kOpMemberDecorate = 72;
constexpr u32 kDecorationOffset = 35;
constexpr u32 kHeaderWords = 5;
constexpr u32 kNotFound = ~0U;

struct MemberId {
    u32 target = kNotFound;
    u32 member = kNotFound;
};

/// The (struct, member) an `OpMemberName` gives `name`, or `kNotFound` in both.
[[nodiscard]] MemberId find_member(Span<const u32> words, const char* name) noexcept {
    MemberId found;
    for (usize at = kHeaderWords; at < words.size();) {
        const u32 opcode = words[at] & 0xFFFFU;
        const u32 count = words[at] >> 16U;
        if (count == 0 || at + count > words.size()) {
            return {};
        }
        const auto* text = reinterpret_cast<const char*>(&words[at + 3]);
        const usize room = count > 3 ? (count - 3U) * sizeof(u32) : 0U;
        if (opcode == kOpMemberName && room > 0 && std::strncmp(text, name, room) == 0 &&
            std::strlen(name) < room) {
            found = MemberId{words[at + 1], words[at + 2]};
        }
        at += count;
    }
    return found;
}

/// The `Offset` decoration a module carries for the member called `name`, or `kNotFound`: the
/// (struct, member) the name belongs to, then the decoration on that pair.
[[nodiscard]] u32 member_offset(Span<const u32> words, const char* name) noexcept {
    if (words.size() < kHeaderWords || words[0] != kSpirvMagic) {
        return kNotFound;
    }
    const MemberId id = find_member(words, name);
    for (usize at = kHeaderWords; id.target != kNotFound && at < words.size();) {
        const u32 opcode = words[at] & 0xFFFFU;
        const u32 count = words[at] >> 16U;
        if (opcode == kOpMemberDecorate && count >= 5 && words[at + 1] == id.target &&
            words[at + 2] == id.member && words[at + 3] == kDecorationOffset) {
            return words[at + 4];
        }
        at += count;
    }
    return kNotFound;
}

}  // namespace

CY_TEST_CASE("the embedded mask module reads the frame's blocks where C++ writes them") {
    const Span<const u32> mask(rendering::selection::kOutlineMaskVertexSpirv,
                               sizeof(rendering::selection::kOutlineMaskVertexSpirv) / sizeof(u32));
    struct Member {
        const char* name;
        u32 offset;
    };
    const Member members[] = {
        {"relativeToClipRow0", static_cast<u32>(offsetof(FrameViewData, relative_to_clip))},
        {"relativeToClipRow3", static_cast<u32>(offsetof(FrameViewData, relative_to_clip)) + 48U},
        {"instanceSlot", 0U},
        {"row0", static_cast<u32>(offsetof(InstanceTransform, row0))},
        {"row1", static_cast<u32>(offsetof(InstanceTransform, row1))},
        {"row2", static_cast<u32>(offsetof(InstanceTransform, row2))},
    };
    for (const Member& expected : members) {
        const u32 found = member_offset(mask, expected.name);
        if (found != expected.offset) {
            std::fprintf(stderr, "mask vertex: `%s` is at %d in the module and at %u in C++\n",
                         expected.name, found == kNotFound ? -1 : static_cast<int>(found),
                         expected.offset);
        }
        CY_CHECK_EQ(found, expected.offset);
    }
    // The reader's negative control: a name that is not there is not found.
    CY_CHECK_EQ(member_offset(mask, "noSuchMemberAnywhere"), kNotFound);
}
