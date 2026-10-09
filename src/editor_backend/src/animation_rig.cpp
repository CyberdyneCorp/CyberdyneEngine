// SPDX-License-Identifier: MIT
// A graph baked into the rig a game loads. See cy/editor/animation_rig.h.

#include <cy/editor/animation_rig.h>

#include <cy/animation/cooked.h>
#include <cy/core/assets/cooked.h>

#include <algorithm>
#include <cstdio>
#include <initializer_list>

namespace cy::editor {
namespace {

namespace animation = cy::animation;

constexpr std::string_view kProgramFile = "program.cyasset";

[[nodiscard]] Status put(Array<char>& out, std::string_view text) noexcept {
    return out.append({text.data(), text.size()});
}

/// A value the manifest can hold: a quote or a line break would end it early.
[[nodiscard]] bool writable(std::string_view value) noexcept {
    return std::ranges::none_of(value, [](const char character) {
        return character == '"' || character == '\n' || character == '\r';
    });
}

[[nodiscard]] Status quoted(Array<char>& out, std::string_view value) noexcept {
    if (!writable(value)) {
        return fail(ErrorCode::InvalidArgument,
                    "a rig manifest value holds no quote and no line break");
    }
    if (Status opened = put(out, " \""); !opened) {
        return opened;
    }
    if (Status written = put(out, value); !written) {
        return written;
    }
    return put(out, "\"");
}

[[nodiscard]] Status entry(Array<char>& out, std::string_view key,
                           std::initializer_list<std::string_view> values) noexcept {
    if (Status keyed = put(out, key); !keyed) {
        return keyed;
    }
    for (const std::string_view value : values) {
        if (Status written = quoted(out, value); !written) {
            return written;
        }
    }
    return put(out, "\n");
}

[[nodiscard]] std::string_view id_text(AssetId id, char (&buffer)[AssetId::kTextLength + 1]) {
    if (id.is_nil()) {
        return {};
    }
    (void)id.format(buffer);
    return {buffer, AssetId::kTextLength};
}

/// One manifest line, split into its key and up to two quoted values.
struct Line {
    std::string_view key;
    std::string_view values[2];
    u32 count = 0;
};

[[nodiscard]] bool split(std::string_view text, Line& out) noexcept {
    const usize space = text.find(' ');
    out.key = text.substr(0, space);
    text = space == std::string_view::npos ? std::string_view{} : text.substr(space);
    while (!text.empty()) {
        if (text.size() < 3 || text[0] != ' ' || text[1] != '"' || out.count == 2) {
            return false;
        }
        const usize close = text.find('"', 2);
        if (close == std::string_view::npos) {
            return false;
        }
        out.values[out.count++] = text.substr(2, close - 2);
        text = text.substr(close + 1);
    }
    return true;
}

[[nodiscard]] Status refuse_line() noexcept {
    return fail(ErrorCode::InvalidArgument, "a rig manifest line is not cyrig 1");
}

[[nodiscard]] bool read_line_id(std::string_view text, AssetId& out) noexcept {
    if (text.empty()) {
        out = AssetId{};
        return true;
    }
    Expected<AssetId, Error> parsed = AssetId::parse(text);
    if (parsed) {
        out = *parsed;
    }
    return parsed.has_value();
}

[[nodiscard]] bool apply(const Line& line, AnimationRigManifest& out) noexcept {
    if (line.key == "clip" && line.count == 2) {
        return static_cast<bool>(out.clips.push_back(
            AnimationRigClip{Name::intern(line.values[0]), Name::intern(line.values[1])}));
    }
    if (line.count != 1) {
        return false;
    }
    const std::string_view value = line.values[0];
    if (line.key == "rig") {
        out.rig = Name::intern(value);
        return !value.empty();
    }
    if (line.key == "model") {
        out.model = Name::intern(value);
        return true;
    }
    if (line.key == "skeleton") {
        return read_line_id(value, out.skeleton) && !out.skeleton.is_nil();
    }
    if (line.key == "mesh") {
        return read_line_id(value, out.mesh);
    }
    if (line.key == "program") {
        out.program = Name::intern(value);
        return !value.empty();
    }
    return false;
}

/// A cooked record wrapped as a cooked asset of kind animation, as the file a bake writes.
[[nodiscard]] Status add_file(Allocator& allocator, AnimationBakeResult& out, std::string_view path,
                              const Array<u8>& record) noexcept {
    AnimationBakedFile file(allocator);
    if (Status named = put(file.path, path); !named) {
        return named;
    }
    if (Status wrapped = assets::write_cooked_asset(
            assets::AssetKind::Animation, assets::VariantKey{}, record.span(), file.bytes);
        !wrapped) {
        return wrapped;
    }
    return out.files.push_back(std::move(file));
}

[[nodiscard]] animation::Clip* find(Array<animation::Clip>& clips, Name name) noexcept {
    for (animation::Clip& clip : clips) {
        if (clip.name() == name) {
            return &clip;
        }
    }
    return nullptr;
}

}  // namespace

Status write_animation_rig(const AnimationRigManifest& manifest, Array<char>& out) noexcept {
    out.clear();
    char skeleton[AssetId::kTextLength + 1] = {};
    char mesh[AssetId::kTextLength + 1] = {};
    Status written = put(out, "cyrig 1\n");
    written = written ? entry(out, "rig", {manifest.rig.text()}) : written;
    written = written ? entry(out, "model", {manifest.model.text()}) : written;
    written = written ? entry(out, "skeleton", {id_text(manifest.skeleton, skeleton)}) : written;
    written = written ? entry(out, "mesh", {id_text(manifest.mesh, mesh)}) : written;
    written = written ? entry(out, "program", {manifest.program.text()}) : written;
    for (const AnimationRigClip& clip : manifest.clips) {
        written = written ? entry(out, "clip", {clip.name.text(), clip.path.text()}) : written;
    }
    return written;
}

Status read_animation_rig(std::string_view text, AnimationRigManifest& out) noexcept {
    // Everything the text does not say is unsaid: nothing survives from what `out` held before.
    out.rig = Name{};
    out.model = Name{};
    out.skeleton = AssetId{};
    out.mesh = AssetId{};
    out.program = Name{};
    out.clips.clear();
    bool versioned = false;
    while (!text.empty()) {
        const usize end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        if (line.empty()) {
            continue;
        }
        if (!versioned) {
            if (line != "cyrig 1") {
                return refuse_line();
            }
            versioned = true;
            continue;
        }
        Line parsed;
        if (!split(line, parsed) || !apply(parsed, out)) {
            return refuse_line();
        }
    }
    if (!versioned || out.rig.is_empty() || out.skeleton.is_nil() || out.program.is_empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "a rig manifest names its rig, its skeleton and its program");
    }
    return ok();
}

Status AnimationRigBaker::bake(const AnimationBakeRequest& request,
                               AnimationBakeResult& out) noexcept {
    out.baked = false;
    out.files.clear();
    AnimationCharacter character(*allocator_);
    if (Status loaded = character.load(request.character, *source_); !loaded) {
        return loaded;
    }
    AnimationCompilation compilation(*allocator_);
    if (Status compiled = compile_animation_graph(&character, request.source, compilation);
        !compiled) {
        return compiled;
    }
    for (const graph::Diagnostic& diagnostic : compilation.sink.entries()) {
        out.sink.report(diagnostic);
    }
    if (!compilation.compiled) {
        return ok();
    }
    Array<animation::Clip> clips(*allocator_);
    if (Status built = character.build_clips(compilation.events.span(), clips); !built) {
        return built;
    }
    AnimationRigManifest manifest(*allocator_);
    manifest.rig = Name::intern(request.rig);
    manifest.model = Name::intern(request.character.model);
    manifest.skeleton = request.character.skeleton;
    manifest.mesh = request.character.mesh;
    manifest.program = Name::intern(kProgramFile);
    Array<u8> record(*allocator_);
    // The compiler keeps one `ClipRef` per clip name, so a clip two nodes sample is baked once.
    for (const graph::pose::ClipRef& reference : compilation.program.clips()) {
        animation::Clip* clip = find(clips, reference.name);
        if (clip == nullptr) {
            return fail(ErrorCode::NotFound, "the program names a clip the character lacks");
        }
        clip->set_loop_mode(reference.looping ? animation::LoopMode::Loop
                                              : animation::LoopMode::None);
        record.clear();
        if (Status encoded = animation::encode_clip(*clip, character.joint_names(), record);
            !encoded) {
            return encoded;
        }
        char path[32] = {};
        (void)std::snprintf(path, sizeof(path), "clips/%u.cyasset",
                            static_cast<u32>(manifest.clips.size()));
        if (Status added = add_file(*allocator_, out, path, record); !added) {
            return added;
        }
        if (Status listed =
                manifest.clips.push_back(AnimationRigClip{reference.name, Name::intern(path)});
            !listed) {
            return listed;
        }
    }
    record.clear();
    if (Status encoded = animation::encode_program(compilation.program, record); !encoded) {
        return encoded;
    }
    if (Status added = add_file(*allocator_, out, kProgramFile, record); !added) {
        return added;
    }
    Array<char> text(*allocator_);
    if (Status written = write_animation_rig(manifest, text); !written) {
        return written;
    }
    AnimationBakedFile file(*allocator_);
    if (Status named = put(file.path, kAnimationRigManifest); !named) {
        return named;
    }
    if (Status copied = file.bytes.append({reinterpret_cast<const u8*>(text.data()), text.size()});
        !copied) {
        return copied;
    }
    if (Status added = out.files.push_back(std::move(file)); !added) {
        return added;
    }
    out.baked = true;
    return ok();
}

}  // namespace cy::editor
