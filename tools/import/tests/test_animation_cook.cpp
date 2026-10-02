// SPDX-License-Identifier: MIT
// The cooked animation records across the cook-to-runtime seam, and the character cook. Issue #76
// stage 2.
//
// Two claims that need both halves in one binary, which is why this suite is here and not in
// src/animation/tests/: the runtime may not link the importer, and the importer's suites are where
// an imported record is made.
//
//   * the records the IMPORTER writes are the records the RUNTIME reads: a skeleton or clip the
//     importer cooked decodes with `cy/animation/cooked.h` and encodes back to the same bytes;
//   * `cook_locomotion_set` turns imported sources into a skeleton, four clips and a program that
//     load and bind, using a clip as it is when its rig is the character's and retargeting it when
//     its rest pose is not.
//
// Behind CY_ANIMATION (tests/CMakeLists.txt), because both halves are the animation runtime.

#include <cy/animation/cooked.h>
#include <cy/animation/evaluate.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/import/animation_cook.h>
#include <cy/import/fbx_skeleton.h>
#include <cy/test/test.h>

#include "fbx_clip_documents.h"

#include <cstring>
#include <string_view>

using namespace cy;
using namespace cy::import;
using namespace cy::import::testing;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Animation);
}

const SubAsset* prefixed(const ImportResult& result, std::string_view prefix) {
    for (const SubAsset& produced : result.assets()) {
        if (produced.view().starts_with(prefix)) {
            return &produced;
        }
    }
    return nullptr;
}

Span<const u8> payload(const SubAsset& produced) {
    return {produced.payload.data(), produced.payload.size()};
}

bool same(Span<const u8> a, const Array<u8>& b) {
    return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size()) == 0;
}

}  // namespace

CY_TEST_CASE(
    "cooked records: the importer's skeleton and clip read back in the runtime and re-save byte "
    "for byte") {
    const ImportResult imported = import_document(animated_document("walk", 100.0), nullptr);
    const SubAsset* skeleton_record = prefixed(imported, kSkeletonSubAssetPrefix);
    const SubAsset* clip_record = prefixed(imported, "animation/");
    CY_REQUIRE(skeleton_record != nullptr);
    CY_REQUIRE(clip_record != nullptr);

    animation::Skeleton skeleton(allocator());
    animation::SkeletonProfile humanoid;
    CY_REQUIRE(
        animation::decode_skeleton(payload(*skeleton_record), skeleton, humanoid).has_value());
    Array<u8> skeleton_again(allocator());
    CY_REQUIRE(animation::encode_skeleton(skeleton, humanoid, skeleton_again).has_value());
    CY_CHECK(same(payload(*skeleton_record), skeleton_again));

    animation::Clip clip(allocator());
    Array<Name> joints(allocator());
    CY_REQUIRE(animation::decode_clip(payload(*clip_record), clip, joints).has_value());
    Array<u8> clip_again(allocator());
    CY_REQUIRE(animation::encode_clip(clip, joints.span(), clip_again).has_value());
    CY_CHECK(same(payload(*clip_record), clip_again));

    // The importer's own reader agrees with the runtime's about what the record holds.
    CookedClip inspected;
    CY_REQUIRE(read_cooked_clip(payload(*clip_record), inspected).has_value());
    CY_CHECK_EQ(inspected.tracks.size(), static_cast<usize>(clip.track_count()));
    CY_CHECK_EQ(inspected.keys.size(), clip.keys().size());
    CY_CHECK_EQ(inspected.duration, clip.duration());
    u16 mismatch = animation::kInvalidJoint;
    CY_CHECK(animation::clip_matches_skeleton(clip, joints.span(), skeleton, mismatch));
}

CY_TEST_CASE("animation cook: four imports become a skeleton, four clips and a program that bind") {
    // Three sources share the rig's rest pose and one does not: its root stands 20 cm higher, so
    // it is congruent and must be retargeted rather than played as it is.
    const ImportResult idle =
        import_document(animated_document("idle", 10.0), nullptr, "animations/idle.fbx");
    const ImportResult walk =
        import_document(animated_document("walk", 100.0), nullptr, "animations/walk.fbx");
    const ImportResult run =
        import_document(animated_document("run", 200.0), nullptr, "animations/run.fbx");
    const ImportResult die = import_document(animated_document("die", 50.0, false, false, 20.0),
                                             nullptr, "animations/die.fbx");

    AnimationCookSpec spec;
    spec.rig = &walk;
    spec.clips = {
        {&idle, "idle", true}, {&walk, "walk", true}, {&run, "run", true}, {&die, "die", false}};
    CookedAnimationSet set;
    CY_REQUIRE(cook_locomotion_set(spec, set).has_value());
    CY_REQUIRE_EQ(set.clips.size(), 4U);
    CY_CHECK_FALSE(set.clips[0].retargeted);
    CY_CHECK_FALSE(set.clips[1].retargeted);
    CY_CHECK(set.clips[3].retargeted);
    CY_CHECK_GT(set.clips[3].rest_difference_metres, 0.1F);

    animation::Skeleton skeleton(allocator());
    animation::SkeletonProfile humanoid;
    CY_REQUIRE(animation::decode_skeleton(set.skeleton.span(), skeleton, humanoid).has_value());
    Expected<graph::pose::PoseProgram, Error> program =
        animation::decode_program(allocator(), set.program.span());
    CY_REQUIRE(program.has_value());

    // Every clip carries the CHARACTER's joint names, and the death is cooked to hold.
    animation::Clip clips[4] = {animation::Clip(allocator()), animation::Clip(allocator()),
                                animation::Clip(allocator()), animation::Clip(allocator())};
    const animation::Clip* table[4] = {};
    for (usize index = 0; index < 4; ++index) {
        Array<Name> joints(allocator());
        CY_REQUIRE(animation::decode_clip(set.clips[index].bytes.span(), clips[index], joints)
                       .has_value());
        u16 mismatch = animation::kInvalidJoint;
        CY_CHECK(animation::clip_matches_skeleton(clips[index], joints.span(), skeleton, mismatch));
    }
    CY_CHECK(clips[3].loop_mode() == animation::LoopMode::None);
    for (usize slot = 0; slot < program->clips().size(); ++slot) {
        for (const animation::Clip& clip : clips) {
            if (clip.name() == program->clips()[slot].name) {
                table[slot] = &clip;
            }
        }
        CY_CHECK(table[slot] != nullptr);
    }
    animation::AnimationRig rig(allocator());
    CY_REQUIRE(
        rig.bind(skeleton, *program, Span<const animation::Clip* const>(table, 4)).has_value());

    // A source with no clip is refused rather than cooked into a character that stands still.
    AnimationCookSpec missing = spec;
    const ImportResult still =
        import_document(animated_document("still", 0.0), nullptr, "animations/still.fbx");
    missing.clips[0].import = &still;
    CookedAnimationSet refused;
    CY_CHECK_FALSE(cook_locomotion_set(missing, refused).has_value());
}
