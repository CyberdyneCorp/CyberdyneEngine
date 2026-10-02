// SPDX-License-Identifier: MIT
// `animation` as a graph node: imported rigs and clips in, a character's cooked skeleton, clips and
// compiled locomotion program out. Issue #76 stage 2.
//
// ================================================================================================
// WHAT THE NODE IS
// ================================================================================================
//
// An `import` node turns one source file into a bundle; for an animated FBX the bundle holds that
// file's skeleton record and its clip. A character is several files — a Mixamo character is one
// export per motion — so the step that makes a CHARACTER out of them belongs to no single import.
// This node is that step, over the bundles its upstream import nodes produced:
//
//     node "animation:hero" cook "animation" 1
//       source "characters/hero.cyanim"
//       upstream "import:idle"
//       upstream "import:walk"
//       upstream "import:run"
//       upstream "import:die"
//       output "derived/hero.skeleton"
//       output "derived/hero.program"
//       output "derived/hero.idle.clip"
//       output "derived/hero.walk.clip"
//       output "derived/hero.run.clip"
//       output "derived/hero.die.clip"
//
// and the description it reads, `cyanim 1`:
//
//     cyanim 1
//     name "locomotion"
//     rig "derived/walk.bundle"
//     clip "idle" "derived/idle.bundle"
//     clip "walk" "derived/walk.bundle"
//     clip "run" "derived/run.bundle"
//     clip "die" "derived/die.bundle" hold
//     blend "walk_to_run" 0.15
//
// `rig` names the bundle whose skeleton is the character's; each `clip` names the program's clip
// and the bundle it comes from, with `hold` for a clip that stops on its last frame; `blend`
// overrides one of the five locomotion blend durations. The work is `cook_locomotion_set`
// (`cy/import/animation_cook.h`): clips on the character's rig are used as they are, clips on
// another are retargeted and baked, and the machine is compiled — here, at cook time, so the
// runtime loads a program and never compiles one.
//
// THE OUTPUTS are matched by suffix: `.skeleton`, `.program`, and `.<clip>.clip` for each clip. A
// declared output with no match, or a clip with no declared output, is refused rather than
// guessed at.
//
// THE KEY. The description is a declared source and every bundle an upstream output, so the node's
// key covers the description's content, every upstream's output digest and
// `kAnimationProducerVersion`: re-exporting one motion re-imports it and re-cooks the character.

#include "animation_producer.h"

#include "text.h"

#include <cy/build/content_producers.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/import/animation_cook.h>
#include <cy/import/pipeline.h>

#include <cerrno>
#include <cstdlib>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cy::build {
namespace {

struct ClipLine {
    std::string name;
    std::string bundle;
    bool looping = true;
};

struct Description {
    std::string name = "locomotion";
    std::string rig;
    std::vector<ClipLine> clips;
    import::LocomotionBlends blends;
};

[[nodiscard]] Status refuse(NodeContext& context, const char* code, const std::string& message) {
    context.diagnose(Severity::Error, code, message, context.node().name);
    return fail(ErrorCode::InvalidArgument, "an animation node's description is malformed");
}

[[nodiscard]] Expected<f32, Error> seconds(std::string_view text) {
    const std::string owned(text);
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(owned.c_str(), &end);
    if (owned.empty() || errno != 0 || end == nullptr || *end != '\0' || !(value > 0.0)) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a blend duration is a positive number of seconds"});
    }
    return static_cast<f32>(value);
}

[[nodiscard]] f32* blend_named(import::LocomotionBlends& blends, std::string_view name) noexcept {
    if (name == "idle_to_walk") {
        return &blends.idle_to_walk;
    }
    if (name == "walk_to_run") {
        return &blends.walk_to_run;
    }
    if (name == "run_to_walk") {
        return &blends.run_to_walk;
    }
    if (name == "walk_to_idle") {
        return &blends.walk_to_idle;
    }
    if (name == "to_die") {
        return &blends.to_die;
    }
    return nullptr;
}

[[nodiscard]] Status parse_line(NodeContext& context, const text::Line& line, Description& out) {
    const std::string_view key = line.word(0);
    if (key == "name" && line.words.size() == 2) {
        out.name = line.word(1);
        return ok();
    }
    if (key == "rig" && line.words.size() == 2) {
        out.rig = line.word(1);
        return ok();
    }
    if (key == "clip" && (line.words.size() == 3 || line.words.size() == 4)) {
        const bool hold = line.words.size() == 4 && line.word(3) == "hold";
        if (line.words.size() == 4 && !hold) {
            return refuse(context, "animation-description",
                          "line " + std::to_string(line.number) + ": a clip's only flag is 'hold'");
        }
        out.clips.push_back(ClipLine{std::string(line.word(1)), std::string(line.word(2)), !hold});
        return ok();
    }
    if (key == "blend" && line.words.size() == 3) {
        f32* slot = blend_named(out.blends, line.word(1));
        const Expected<f32, Error> value = seconds(line.word(2));
        if (slot == nullptr || !value) {
            return refuse(context, "animation-description",
                          "line " + std::to_string(line.number) +
                              ": a blend names one of idle_to_walk, walk_to_run, run_to_walk, "
                              "walk_to_idle, to_die and a positive duration");
        }
        *slot = *value;
        return ok();
    }
    return refuse(
        context, "animation-description",
        "line " + std::to_string(line.number) + ": unknown record '" + std::string(key) + "'");
}

[[nodiscard]] Status parse(NodeContext& context, std::string_view document, Description& out) {
    auto lines = text::read(document);
    if (!lines) {
        return refuse(context, "animation-description", lines.error().message);
    }
    if (lines->empty() || lines->front().word(0) != "cyanim" || lines->front().word(1) != "1") {
        return refuse(context, "animation-description",
                      "the description does not begin 'cyanim 1'");
    }
    for (usize index = 1; index < lines->size(); ++index) {
        if (Status parsed = parse_line(context, (*lines)[index], out); !parsed) {
            return parsed;
        }
    }
    if (out.rig.empty()) {
        return refuse(context, "animation-description", "the description names no rig bundle");
    }
    return ok();
}

/// The bundles the description names, each decoded once.
struct Bundles {
    std::vector<std::string> names;
    std::vector<std::unique_ptr<import::ImportResult>> results;

    [[nodiscard]] Expected<const import::ImportResult*, Error> get(NodeContext& context,
                                                                   const std::string& name) {
        for (usize index = 0; index < names.size(); ++index) {
            if (names[index] == name) {
                return results[index].get();
            }
        }
        Array<u8> bytes(default_allocator());
        if (Status read = context.read(name, bytes); !read) {
            return make_unexpected(read.error());
        }
        auto decoded = std::make_unique<import::ImportResult>();
        if (Status unpacked = import::decode_import_bundle(bytes.span(), *decoded); !unpacked) {
            return make_unexpected(unpacked.error());
        }
        names.push_back(name);
        results.push_back(std::move(decoded));
        return results.back().get();
    }
};

[[nodiscard]] bool ends_with(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

/// The declared output a cooked record goes to, by suffix, or null.
[[nodiscard]] const std::string* output_for(const NodeDesc& node, const std::string& suffix) {
    for (const std::string& output : node.outputs) {
        if (ends_with(output, suffix)) {
            return &output;
        }
    }
    return nullptr;
}

[[nodiscard]] Status write_outputs(NodeContext& context, const import::CookedAnimationSet& set) {
    const NodeDesc& node = context.node();
    usize written = 0;
    const auto write = [&](const std::string& suffix, const Array<u8>& bytes) -> Status {
        const std::string* output = output_for(node, suffix);
        if (output == nullptr) {
            return refuse(context, "animation-output",
                          "the node declares no output ending in '" + suffix + "'");
        }
        ++written;
        return context.write(*output, bytes.data(), bytes.size());
    };
    if (Status done = write(".skeleton", set.skeleton); !done) {
        return done;
    }
    if (Status done = write(".program", set.program); !done) {
        return done;
    }
    for (const import::CookedAnimationClip& clip : set.clips) {
        if (Status done = write("." + clip.name + ".clip", clip.bytes); !done) {
            return done;
        }
    }
    if (written != node.outputs.size()) {
        return refuse(context, "animation-output",
                      "the node declares an output no cooked record matches");
    }
    return ok();
}

}  // namespace

Status produce_animation(NodeContext& context) {
    const NodeDesc& node = context.node();
    if (node.sources.size() != 1) {
        return refuse(context, "animation-description",
                      "an animation node declares exactly one `cyanim` description");
    }
    Array<u8> document(default_allocator());
    if (Status read = context.read(node.sources.front(), document); !read) {
        return read;
    }
    Description description;
    if (Status parsed =
            parse(context,
                  std::string_view(reinterpret_cast<const char*>(document.data()), document.size()),
                  description);
        !parsed) {
        return parsed;
    }

    Bundles bundles;
    import::AnimationCookSpec spec;
    spec.name = description.name;
    spec.blends = description.blends;
    Expected<const import::ImportResult*, Error> rig = bundles.get(context, description.rig);
    if (!rig) {
        return Status{make_unexpected(rig.error())};
    }
    spec.rig = *rig;
    for (const ClipLine& line : description.clips) {
        Expected<const import::ImportResult*, Error> source = bundles.get(context, line.bundle);
        if (!source) {
            return Status{make_unexpected(source.error())};
        }
        spec.clips.push_back(import::AnimationClipSource{*source, line.name, line.looping});
    }

    import::CookedAnimationSet set;
    if (Status cooked = import::cook_locomotion_set(spec, set); !cooked) {
        context.diagnose(Severity::Error, "animation-cook", cooked.error().message, node.name);
        return cooked;
    }
    return write_outputs(context, set);
}

}  // namespace cy::build
