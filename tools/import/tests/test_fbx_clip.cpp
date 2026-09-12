// FBX animation import, end to end in memory. M8.d.
//
// THE DOCUMENTS ARE BUILT HERE, in ASCII FBX 7.4, for the reason `test_fbx.cpp` gives: a binary
// fixture is a file somebody has to maintain and nobody can read in a review. It matters more here
// than anywhere else in this suite, because the files this work exists for — Mixamo character
// exports — are sixteen megabytes each and live outside the repository. A test that needed one of
// them would be a test that runs on one machine.
//
// WHAT THE ANIMATION-ONLY DOCUMENT IS A MODEL OF. A Mixamo animation export carries a skeleton, one
// animation stack, no mesh, no material, no skin and no blend shape. `animated_document` below is
// the smallest thing with that shape: one `LimbNode`, one stack, one curve. It is the shape that
// defeated the old `skipped-rig` predicate — which required a skin, a blend shape, or MORE THAN ONE
// stack — and so imported to a prefab in silence with the animation dropped.

#include <cy/core/math/scalar.h>
#include <cy/import/fbx.h>
#include <cy/import/fbx_clip.h>
#include <cy/import/model.h>
#include <cy/test/test.h>

#include <string>
#include <string_view>
#include <vector>

using namespace cy::import;
using cy::f32;
using cy::u8;
using cy::usize;

namespace {

/// FBX's own time unit: one second is this many KTime ticks in every 7.x file.
constexpr long long kKTimeSecond = 46186158000LL;

/// The three run-length-encoded attribute arrays one two-key curve segment needs.
///
/// `curved` picks FBX's cubic interpolation with user tangents (8 | 0x400) over its linear flag
/// (4), AND THE TWO ARE NOT INTERCHANGEABLE FOR THIS SUITE. ufbx's baker resamples only a cubic or
/// a constant segment, because a linear one is reproduced exactly by the two keys that bound it —
/// so a linearly keyed document bakes to the same keys at every sample rate, which is correct
/// behaviour and which is why the case that asserts the sample rate reaches the output keys its
/// document curved instead.
std::string curve_attributes(bool curved) {
    std::string text = "\t\tKeyAttrFlags: *1 {\n\t\t\ta: ";
    text += curved ? "1032" : "4";
    text += "\n\t\t}\n";
    // Four floats per run: the right slope, the next left slope, and the packed weights and
    // velocities that neither flag above asks for. Zero slopes make a cubic that eases out of one
    // end and into the other — smooth, and nowhere near the straight line between them.
    text +=
        "\t\tKeyAttrDataFloat: *4 {\n\t\t\ta: 0,0,0,0\n\t\t}\n"
        "\t\tKeyAttrRefCount: *1 {\n\t\t\ta: 2\n\t\t}\n";
    return text;
}

/// An animation-only ASCII FBX: one skeleton joint, one animation stack, one translation curve.
///
/// `end_value` is the curve's second key in the file's own centimetres, so 100 is one metre of
/// travel and 0 is a stack that holds still — which is what an exporter's `Take 001` looks like
/// beside the take that holds the motion.
std::string animated_document(std::string_view stack_name, double end_value,
                              bool second_stack = false, bool curved = false) {
    std::string text =
        "; FBX 7.4.0 project file\n"
        "FBXHeaderExtension:  {\n"
        "\tFBXHeaderVersion: 1003\n"
        "\tFBXVersion: 7400\n"
        "}\n"
        "GlobalSettings:  {\n"
        "\tVersion: 1000\n"
        "\tProperties70:  {\n"
        "\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",1\n"
        "\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
        "\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n"
        "\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n"
        "\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
        "\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
        // A centimetre file, which is what every FBX exporter writes by default and what makes the
        // metre conversion observable in a key value.
        "\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\",1\n"
        "\t}\n"
        "}\n"
        "Objects:  {\n"
        // The attribute is what makes the node a BONE rather than a group: `ufbx_node::bone` is
        // non-null only for a node with one, and that is the test every skeleton extractor applies.
        "\tNodeAttribute: 1100, \"NodeAttribute::\", \"LimbNode\" {\n"
        "\t\tTypeFlags: \"Skeleton\"\n"
        "\t}\n"
        "\tModel: 2000, \"Model::Root\", \"LimbNode\" {\n"
        "\t\tVersion: 232\n"
        "\t\tProperties70:  {\n"
        "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,0,0\n"
        "\t\t}\n"
        "\t}\n";
    text += "\tAnimationStack: 4000, \"AnimStack::";
    text += stack_name;
    text +=
        "\", \"\" {\n"
        "\t\tProperties70:  {\n"
        "\t\t\tP: \"LocalStart\", \"KTime\", \"Time\", \"\",0\n";
    text +=
        "\t\t\tP: \"LocalStop\", \"KTime\", \"Time\", \"\"," + std::to_string(kKTimeSecond) + "\n";
    text +=
        "\t\t}\n"
        "\t}\n"
        "\tAnimationLayer: 4100, \"AnimLayer::BaseLayer\", \"\" {\n"
        "\t}\n"
        "\tAnimationCurveNode: 4200, \"AnimCurveNode::T\", \"\" {\n"
        "\t\tProperties70:  {\n"
        "\t\t\tP: \"d|X\", \"Number\", \"\", \"A\",0\n"
        "\t\t\tP: \"d|Y\", \"Number\", \"\", \"A\",0\n"
        "\t\t\tP: \"d|Z\", \"Number\", \"\", \"A\",0\n"
        "\t\t}\n"
        "\t}\n"
        "\tAnimationCurve: 4300, \"AnimCurve::\", \"\" {\n"
        "\t\tDefault: 0\n"
        "\t\tKeyVer: 4009\n";
    text += "\t\tKeyTime: *2 {\n\t\t\ta: 0," + std::to_string(kKTimeSecond) + "\n\t\t}\n";
    text += "\t\tKeyValueFloat: *2 {\n\t\t\ta: 0," + std::to_string(end_value) + "\n\t\t}\n";
    // The flag and attribute arrays are run-length encoded against `KeyAttrRefCount`, so one run of
    // two keys is one flag, four tangent floats and a count of two — ufbx refuses a curve whose
    // five arrays disagree about how many keys they describe.
    text += curve_attributes(curved);
    text += "\t}\n";
    if (second_stack) {
        // A second stack over the SAME curve node, which is how a file ends up with two takes that
        // both animate the rig. It exists to make the sub-asset naming rule observable.
        text +=
            "\tAnimationStack: 5000, \"AnimStack::second\", \"\" {\n"
            "\t\tProperties70:  {\n"
            "\t\t\tP: \"LocalStart\", \"KTime\", \"Time\", \"\",0\n";
        text += "\t\t\tP: \"LocalStop\", \"KTime\", \"Time\", \"\"," +
                std::to_string(kKTimeSecond) + "\n";
        text +=
            "\t\t}\n"
            "\t}\n"
            "\tAnimationLayer: 5100, \"AnimLayer::SecondLayer\", \"\" {\n"
            "\t}\n"
            "\tAnimationCurveNode: 5200, \"AnimCurveNode::T\", \"\" {\n"
            "\t\tProperties70:  {\n"
            "\t\t\tP: \"d|X\", \"Number\", \"\", \"A\",0\n"
            "\t\t\tP: \"d|Y\", \"Number\", \"\", \"A\",0\n"
            "\t\t\tP: \"d|Z\", \"Number\", \"\", \"A\",0\n"
            "\t\t}\n"
            "\t}\n"
            "\tAnimationCurve: 5300, \"AnimCurve::\", \"\" {\n"
            "\t\tDefault: 0\n"
            "\t\tKeyVer: 4009\n";
        text += "\t\tKeyTime: *2 {\n\t\t\ta: 0," + std::to_string(kKTimeSecond) + "\n\t\t}\n";
        text += "\t\tKeyValueFloat: *2 {\n\t\t\ta: 0,-50\n\t\t}\n";
        text += curve_attributes(curved);
        text += "\t}\n";
    }
    text +=
        "}\n"
        "Connections:  {\n"
        "\tC: \"OO\",2000,0\n"
        "\tC: \"OO\",1100,2000\n"
        "\tC: \"OO\",4100,4000\n"
        "\tC: \"OO\",4200,4100\n"
        "\tC: \"OP\",4200,2000,\"Lcl Translation\"\n"
        "\tC: \"OP\",4300,4200,\"d|Z\"\n";
    if (second_stack) {
        text +=
            "\tC: \"OO\",5100,5000\n"
            "\tC: \"OO\",5200,5100\n"
            "\tC: \"OP\",5200,2000,\"Lcl Translation\"\n"
            "\tC: \"OP\",5300,5200,\"d|Z\"\n";
    }
    text += "}\n";
    return text;
}

ImportResult import_document(const std::string& text, const ImportOptions* options,
                             std::string_view source = "animations/idle.fbx") {
    FbxImporter importer;
    ImportRequest request;
    request.source = cy::assets::VirtualPath::normalise(source).value();
    request.bytes = cy::Span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size());
    request.options = options;
    ImportResult result;
    CY_REQUIRE(importer.import(request, result).has_value());
    return result;
}

const SubAsset* find(const ImportResult& result, std::string_view name) {
    for (const SubAsset& produced : result.assets()) {
        if (produced.view() == name) {
            return &produced;
        }
    }
    return nullptr;
}

const ImportDiagnostic* diagnostic(const ImportResult& result, std::string_view code) {
    for (const ImportDiagnostic& reported : result.diagnostics()) {
        if (std::string_view(reported.code) == code) {
            return &reported;
        }
    }
    return nullptr;
}

usize animation_count(const ImportResult& result) {
    usize count = 0;
    for (const SubAsset& produced : result.assets()) {
        const std::string_view name = produced.view();
        count += name.rfind("animation/", 0) == 0 ? 1U : 0U;
    }
    return count;
}

#if defined(CY_IMPORT_ANIMATION)
// Only the cases behind the guard read a clip back, because only a build with the codec authors
// one. Guarding the helper too is what lets this suite compile with `-D CY_ANIMATION=OFF` and go on
// asserting the thing that matters in that configuration: that nothing is dropped in silence.
CookedClip clip_of(const SubAsset& produced) {
    CookedClip clip;
    CY_REQUIRE(
        read_cooked_clip(cy::Span<const u8>(produced.payload.data(), produced.payload.size()), clip)
            .has_value());
    return clip;
}
#endif  // CY_IMPORT_ANIMATION

}  // namespace

CY_TEST_CASE("fbx: an animation-only file is never imported in silence") {
    // THE REGRESSION. The predicate this replaces was
    //
    //     skin_deformers.count != 0 || anim_stacks.count > 1 || blend_deformers.count != 0
    //
    // and the document below satisfies none of its three disjuncts: one stack, no skin, no blend
    // shape. Before M8.d it therefore produced a prefab, no clip, and NO DIAGNOSTIC — the animation
    // an artist exported was dropped and nobody was told. The invariant asserted here is the one
    // that has to hold however the importer grows: either the animation comes through, or something
    // in the report names it. Never neither.
    const ImportResult result = import_document(animated_document("mixamo.com", 100.0), nullptr);
    CY_CHECK(!result.has_errors());

    const bool imported = animation_count(result) != 0;
    const bool named = diagnostic(result, "skipped-rig") != nullptr ||
                       diagnostic(result, "constant-animation-stack") != nullptr ||
                       diagnostic(result, "animation-runtime-absent") != nullptr;
    const bool never_silent = imported || named;
    CY_CHECK(never_silent);

    // And in this build it is the first of the two: the clip is produced.
    if (kFbxClipsAvailable) {
        CY_CHECK(imported);
        const SubAsset* produced = find(result, "animation/idle");
        CY_REQUIRE(produced != nullptr);
        CY_CHECK(produced->kind == cy::assets::AssetKind::Animation);
        CY_CHECK(!produced->primary);
        // The prefab is still the one primary sub-asset: a clip is part of the source, not the
        // thing the source resolves to.
        CY_REQUIRE(find(result, "prefab") != nullptr);
        CY_CHECK(find(result, "prefab")->primary);
    }
}

#if defined(CY_IMPORT_ANIMATION)

CY_TEST_CASE("fbx: the clip carries the file's motion, converted to seconds and metres") {
    const ImportResult result = import_document(animated_document("mixamo.com", 100.0), nullptr);
    const SubAsset* produced = find(result, "animation/idle");
    CY_REQUIRE(produced != nullptr);
    const CookedClip clip = clip_of(*produced);

    // The stack spans one second of KTime, and the clip's timeline is in seconds.
    CY_CHECK(cy::math::nearly_equal(clip.duration, 1.0f, 1e-3f));
    CY_CHECK(clip.sample_rate_hint > 0.0f);

    // One joint, named as the source names it, so a loader can rebind by name rather than trusting
    // that two rigs number their joints alike.
    CY_REQUIRE(clip.joint_names.size() == 1);
    CY_CHECK(clip.joint_names[0] == "Root");

    // A translation track (`TrackKind::Translation` is 0) on joint 0, and it is NOT constant —
    // the whole point of the file is that it moves.
    usize translation_tracks = 0;
    for (const CookedClipTrack& track : clip.tracks) {
        if (track.kind == 0 && track.joint == 0) {
            ++translation_tracks;
            CY_CHECK(!track.constant);
            // A centimetre file travelling 100 units is one METRE of engine travel, and the
            // quantisation range the codec derived is what carries it.
            CY_CHECK(cy::math::nearly_equal(track.range_max.z, 1.0f, 1e-3f));
            CY_CHECK(cy::math::nearly_equal(track.range_min.z, 0.0f, 1e-3f));
        }
    }
    CY_CHECK(translation_tracks == 1);
    CY_CHECK(!clip.keys.empty());

    // Every track's key run lies inside the key array, which is the invariant a reader of this
    // record depends on and the one a bad writer would break.
    for (const CookedClipTrack& track : clip.tracks) {
        CY_CHECK(static_cast<usize>(track.first_key) + track.key_count <= clip.keys.size());
    }

    // The compression report reaches the caller, measured rather than estimated.
    const ImportDiagnostic* measured = diagnostic(result, "imported-animation");
    CY_REQUIRE(measured != nullptr);
    CY_CHECK(measured->severity == ImportSeverity::Info);
}

CY_TEST_CASE("fbx: the clip is named from the source, and carries the stack's own name") {
    // `importer.h`: a sub-asset's name must come from "something an artist controls". A stack name
    // is not enough on its own — every Mixamo export calls its stack `mixamo.com` — so a file that
    // yields one clip takes the source's stem, which is what the artist typed when they exported.
    const ImportResult walk =
        import_document(animated_document("mixamo.com", 100.0), nullptr, "animations/Walking.fbx");
    CY_REQUIRE(find(walk, "animation/Walking") != nullptr);
    // The CLIP's own name is still the stack's, because that is what the file says it is.
    CY_CHECK(clip_of(*find(walk, "animation/Walking")).name == "mixamo.com");

    // A file with two moving stacks names each after its own stack, because a file with several is
    // one an artist named them inside.
    const ImportResult two =
        import_document(animated_document("first", 100.0, true), nullptr, "animations/takes.fbx");
    CY_CHECK(animation_count(two) == 2);
    CY_CHECK(find(two, "animation/first") != nullptr);
    CY_CHECK(find(two, "animation/second") != nullptr);
    CY_CHECK(find(two, "animation/takes") == nullptr);
}

CY_TEST_CASE("fbx: a stack that animates nothing produces no clip and says which one") {
    // A character export carries a take holding only exporter bookkeeping beside the take holding
    // the motion; taking the first stack cooks the flat one and drops the real one. The rule is
    // measured rather than matched on a name: a stack every track of which collapsed to one key
    // animates nothing.
    const ImportResult result =
        import_document(animated_document("Take 001", 0.0), nullptr, "animations/still.fbx");
    CY_CHECK(animation_count(result) == 0);

    const ImportDiagnostic* named = diagnostic(result, "constant-animation-stack");
    CY_REQUIRE(named != nullptr);
    CY_CHECK(named->severity == ImportSeverity::Info);
    CY_CHECK(std::string_view(named->subject) == "Take 001");

    // And it is NOT reported as a skipped rig: the stack was read, understood and found to hold
    // nothing, which is a different statement from "this build cannot import animation".
    CY_CHECK(diagnostic(result, "skipped-rig") == nullptr);
}

CY_TEST_CASE("fbx: turning animation off names the drop, in a sentence that survives the cap") {
    ImportOptions options;
    const OptionsSchema schema = fbx_options();
    CY_REQUIRE(options.set(schema, "import-animations", OptionValue::of_bool(false)).has_value());

    const ImportResult result = import_document(animated_document("mixamo.com", 100.0), &options);
    CY_CHECK(animation_count(result) == 0);

    const ImportDiagnostic* skipped = diagnostic(result, "skipped-rig");
    CY_REQUIRE(skipped != nullptr);
    const std::string_view detail(skipped->detail);
    CY_CHECK(detail.find("animation") != std::string_view::npos);
    // The sentence this replaces was 230 bytes against a 192-byte capacity, so its last clause had
    // never reached a report. Asserting the END of the sentence is what proves the whole of it
    // fits.
    CY_CHECK(detail.find("came through") != std::string_view::npos);
    CY_CHECK(detail.size() < ImportDiagnostic::kDetailCapacity);
}

CY_TEST_CASE("fbx: the compression tolerances are declared options, and they change the output") {
    const OptionsSchema schema = fbx_options();
    static constexpr std::string_view kDeclared[] = {"import-animations",
                                                     "animation-sample-rate",
                                                     "animation-key-reduction",
                                                     "animation-translation-tolerance-mm",
                                                     "animation-rotation-tolerance-degrees",
                                                     "animation-root-motion"};
    for (const std::string_view name : kDeclared) {
        CY_CHECK(schema.find(name) != nullptr);
    }
    // A setting that changes the cooked bytes and is not in the schema cannot reach the derivation
    // key, which `options.h` calls the one defect a cook cache cannot survive. This is the half of
    // that claim a test can make: the option is declared AND it moves the output.
    //
    // THE DOCUMENT IS KEYED CURVED ON PURPOSE, and the reason is a real property of the baker
    // rather than a convenience. ufbx resamples a cubic or a constant segment and leaves a LINEAR
    // one alone, because two keys and a straight line between them already say everything a sample
    // could: a linearly keyed file therefore bakes identically at every rate, and a case asserting
    // otherwise would be asserting something false about a correct importer.
    ImportOptions coarse;
    CY_REQUIRE(
        coarse.set(schema, "animation-sample-rate", OptionValue::of_float(10.0)).has_value());

    const std::string document = animated_document("mixamo.com", 100.0, false, true);
    const ImportResult standard = import_document(document, nullptr);
    const ImportResult resampled = import_document(document, &coarse);
    const CookedClip left = clip_of(*find(standard, "animation/idle"));
    const CookedClip right = clip_of(*find(resampled, "animation/idle"));
    // Sampling a curve three times as often and fitting it to the same tolerance keeps more keys,
    // so the rate reaches the KEYS and not merely the hint stored beside them.
    CY_CHECK(left.keys.size() > right.keys.size());
    CY_CHECK(cy::math::nearly_equal(left.sample_rate_hint, 30.0f, 1e-3f));
    CY_CHECK(cy::math::nearly_equal(right.sample_rate_hint, 10.0f, 1e-3f));

    // Key reduction is the baker's own decimation, ahead of the engine's error-bounded fit. Turning
    // it off imports every sampled frame, which is the way to tell a baking artefact from a fitting
    // one — so it too must move the output rather than merely being accepted.
    ImportOptions unreduced;
    CY_REQUIRE(
        unreduced.set(schema, "animation-key-reduction", OptionValue::of_bool(false)).has_value());
    const ImportResult every_frame = import_document(document, &unreduced);
    CY_CHECK(clip_of(*find(every_frame, "animation/idle")).keys.size() >= left.keys.size());
}

CY_TEST_CASE("fbx: the root motion joint is opt-in, and names a joint that actually moves") {
    // Off by default, because `Clip::sample` still writes the designated joint's translation into
    // the pose as well as `root_delta` reporting it — so a clip that designated one would be
    // applied twice by today's runtime. `0xFFFF` is `cy::animation::kInvalidJoint`.
    const ImportResult without = import_document(animated_document("mixamo.com", 100.0), nullptr);
    CY_CHECK(clip_of(*find(without, "animation/idle")).root_motion_joint == 0xFFFFU);

    ImportOptions options;
    const OptionsSchema schema = fbx_options();
    CY_REQUIRE(
        options.set(schema, "animation-root-motion", OptionValue::of_enumeration("root-joint"))
            .has_value());
    const ImportResult with = import_document(animated_document("mixamo.com", 100.0), &options);
    CY_CHECK(clip_of(*find(with, "animation/idle")).root_motion_joint == 0U);
}

CY_TEST_CASE("fbx: two imports of one animated file produce byte-identical clips") {
    // The property the content-addressed cook cache rests on, asserted on the bytes so a failure
    // says which sub-asset moved. It is worth asserting HERE and not only in `test_fbx.cpp`,
    // because a clip's bytes pass through a quantiser, a curve fitter and a third-party baker —
    // three places a non-deterministic ordering could hide.
    const std::string document = animated_document("mixamo.com", 100.0, true);
    const ImportResult first = import_document(document, nullptr);
    const ImportResult second = import_document(document, nullptr);

    CY_REQUIRE(first.assets().size() == second.assets().size());
    for (usize index = 0; index < first.assets().size(); ++index) {
        const SubAsset& left = first.assets()[index];
        const SubAsset& right = second.assets()[index];
        CY_CHECK(left.view() == right.view());
        CY_REQUIRE(left.payload.size() == right.payload.size());
        for (usize at = 0; at < left.payload.size(); ++at) {
            CY_REQUIRE(left.payload[at] == right.payload[at]);
        }
    }
}

CY_TEST_CASE("fbx: a truncated clip record is refused rather than read off the end") {
    const ImportResult result = import_document(animated_document("mixamo.com", 100.0), nullptr);
    const SubAsset* produced = find(result, "animation/idle");
    CY_REQUIRE(produced != nullptr);

    // Every prefix of a valid record is either refused or read as a shorter valid one; none of them
    // may read past the buffer. Run under the sanitizers, this case is what says so.
    for (usize length = 0; length < produced->payload.size(); ++length) {
        CookedClip clip;
        (void)read_cooked_clip(cy::Span<const u8>(produced->payload.data(), length), clip);
    }
    CookedClip whole;
    CY_CHECK(read_cooked_clip(
                 cy::Span<const u8>(produced->payload.data(), produced->payload.size()), whole)
                 .has_value());
}

#endif  // CY_IMPORT_ANIMATION
