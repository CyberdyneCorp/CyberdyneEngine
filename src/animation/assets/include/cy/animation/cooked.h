// SPDX-License-Identifier: MIT
#pragma once
// The cooked animation records the runtime reads: a skeleton, a clip and a compiled pose program.
// Issue #76 stage 2.
//
// ================================================================================================
// TWO OF THE THREE FORMATS ARE THE IMPORTER'S, READ HERE WITHOUT THE IMPORTER
// ================================================================================================
//
// `tools/import/` cooks a skeleton (`cy/import/fbx_skeleton.h`, step 7) and a clip
// (`cy/import/fbx_clip.h`, step 8), and until this file the only code that read either back was
// in that cook-time library — so a game that wanted a character had to link the importer, ufbx and
// all. These decoders read the SAME bytes into runtime objects, with nothing of `cy::import`:
//
//   skeleton   version 1, byte for byte the importer's record: joints in parent-before-child
//              order with their bind poses and bone levels, then the humanoid profile.
//   clip       version 1, byte for byte the importer's record: the header, the joint NAMES the
//              tracks index (so a binding can check it is the skeleton they mean), the tracks
//              and the 16-bit keys exactly as the codec stored them. Version 2 appends markers
//              and events, which an imported clip does not carry; a clip with neither is written
//              at version 1, so an importer's record round-trips byte for byte.
//   program    NEW: a `graph::pose::PoseProgram` as `compile_pose` produced it — instructions with
//              their dependency masks, states, transitions, clip references, masks, parameter
//              names, sync tables and the digest. It is written at cook time and read by
//              `graph::pose::assemble_pose_program`, which validates and copies and compiles
//              nothing, so a runtime that loads programs links no compiler.
//
// The keys are ADOPTED, not re-encoded: `Clip::adopt_compressed` takes the stored keys as they are,
// so a loaded clip samples exactly what the cook compressed.
//
// `encode_*` exists beside each decoder because the record must survive the trip — a cook writes
// it, the runtime reads it, and a tool that re-saves it must write the same bytes — and because a
// test of that is the only proof the two halves agree.

#include <cy/animation/clip.h>
#include <cy/animation/skeleton.h>
#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/graph/lower_pose.h>

namespace cy::animation {

inline constexpr u32 kCookedSkeletonVersion = 1;
inline constexpr u32 kCookedClipVersion = 1;
inline constexpr u32 kCookedClipEventsVersion = 2;
/// "CYPG" as it reads in a hex dump of the little-endian word.
inline constexpr u32 kCookedProgramMagic = 0x47505943U;
inline constexpr u32 kCookedProgramVersion = 1;

/// Read a cooked skeleton into `out`, which must be empty, and its humanoid profile. `out` is
/// finalized on success and ready for `AnimationRig::bind`.
[[nodiscard]] Status decode_skeleton(Span<const u8> payload, Skeleton& out,
                                     SkeletonProfile& humanoid) noexcept;
[[nodiscard]] Status encode_skeleton(const Skeleton& skeleton, const SkeletonProfile& humanoid,
                                     Array<u8>& out) noexcept;

/// Read a cooked clip into `out`, and the joint names its tracks index into `joints`.
[[nodiscard]] Status decode_clip(Span<const u8> payload, Clip& out, Array<Name>& joints) noexcept;
/// `joints` names what each track's joint index means: normally the skeleton's joint names, in
/// order.
[[nodiscard]] Status encode_clip(const Clip& clip, Span<const Name> joints,
                                 Array<u8>& out) noexcept;

/// Whether every joint track of `clip` means the joint `skeleton` has at that index, judged by the
/// names the cooked clip carries. `AnimationRig::bind` checks only counts; this is the check that
/// stops a clip cooked for one rig moving the wrong limbs of another. On a mismatch `out_joint` is
/// the first offending track's joint index.
[[nodiscard]] bool clip_matches_skeleton(const Clip& clip, Span<const Name> joints,
                                         const Skeleton& skeleton, u16& out_joint) noexcept;

[[nodiscard]] Expected<graph::pose::PoseProgram, Error> decode_program(
    Allocator& allocator, Span<const u8> payload) noexcept;
[[nodiscard]] Status encode_program(const graph::pose::PoseProgram& program,
                                    Array<u8>& out) noexcept;

}  // namespace cy::animation
