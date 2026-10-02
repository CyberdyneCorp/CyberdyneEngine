// SPDX-License-Identifier: MIT
#pragma once
// The ASCII FBX documents the clip suites build: an animation-only rig of one joint, one stack and
// one curve. Shared by test_fbx_clip.cpp and test_animation_cook.cpp; see the former's header for
// why the documents are built in code rather than kept as fixtures.

#include <cy/import/fbx.h>
#include <cy/import/fbx_clip.h>
#include <cy/test/test.h>

#include <string>
#include <string_view>

namespace cy::import::testing {

/// FBX's own time unit: one second is this many KTime ticks in every 7.x file.
inline constexpr long long kKTimeSecond = 46186158000LL;

/// The three run-length-encoded attribute arrays one two-key curve segment needs.
///
/// `curved` picks FBX's cubic interpolation with user tangents (8 | 0x400) over its linear flag
/// (4), AND THE TWO ARE NOT INTERCHANGEABLE FOR THIS SUITE. ufbx's baker resamples only a cubic or
/// a constant segment, because a linear one is reproduced exactly by the two keys that bound it —
/// so a linearly keyed document bakes to the same keys at every sample rate, which is correct
/// behaviour and which is why the case that asserts the sample rate reaches the output keys its
/// document curved instead.
inline std::string curve_attributes(bool curved) {
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
inline std::string animated_document(std::string_view stack_name, double end_value,
                                     bool second_stack = false, bool curved = false,
                                     double rest_y = 0.0) {
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
        "\t\t\tP: \"Lcl Translation\", \"Lcl Translation\", \"\", \"A\",0,";
    // The joint's rest height, in the file's centimetres: a rig that differs from another only
    // here is congruent with it and does not share its rest pose, which is the case a retarget is
    // for.
    text += rest_y == 0.0 ? std::string("0") : std::to_string(rest_y);
    text +=
        ",0\n"
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

inline ImportResult import_document(const std::string& text, const ImportOptions* options,
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

}  // namespace cy::import::testing
