// SPDX-License-Identifier: MIT
// Cooked animation assets through the asset system. See cy/animation/library.h.

#include <cy/animation/library.h>

#include <cy/animation/cooked.h>
#include <cy/core/base/diagnostic_sink.h>

#include <cstdarg>
#include <cstdio>
#include <utility>

namespace cy::animation {

struct AnimationLibrary::LoadedSkeleton {
    explicit LoadedSkeleton(Allocator& allocator) noexcept : skeleton(allocator) {}
    AssetId id;
    Ref<assets::AssetData> data;
    Skeleton skeleton;
    SkeletonProfile humanoid;
};

struct AnimationLibrary::LoadedClip {
    explicit LoadedClip(Allocator& allocator) noexcept : clip(allocator), joints(allocator) {}
    AssetId id;
    Ref<assets::AssetData> data;
    Clip clip;
    /// The joint names the cooked clip's tracks index, which is what a binding checks.
    Array<Name> joints;
};

struct AnimationLibrary::LoadedProgram {
    explicit LoadedProgram(Allocator& allocator) noexcept : program(allocator) {}
    AssetId id;
    Ref<assets::AssetData> data;
    graph::pose::PoseProgram program;
};

struct AnimationLibrary::BoundRig {
    explicit BoundRig(Allocator& allocator) noexcept
        : rig(allocator), table(allocator), clips(allocator) {}
    AnimationRig rig;
    Array<const Clip*> table;
    const LoadedSkeleton* skeleton = nullptr;
    const LoadedProgram* program = nullptr;
    /// The loaded clips the table points into, so a reload knows which rigs to check and rebind.
    Array<LoadedClip*> clips;
};

namespace {

template <class T>
[[nodiscard]] T* find_loaded(const Array<UniquePtr<T>>& table, AssetId id) noexcept {
    for (const UniquePtr<T>& entry : table) {
        if (entry->id == id) {
            return entry.get();
        }
    }
    return nullptr;
}

}  // namespace

AnimationLibrary::AnimationLibrary(Allocator& allocator, assets::AssetSystem& assets) noexcept
    : allocator_(&allocator),
      assets_(&assets),
      skeletons_(allocator),
      clips_(allocator),
      programs_(allocator),
      rigs_(allocator) {}

AnimationLibrary::~AnimationLibrary() {
    if (watching_) {
        assets_->remove_reload_observer(&AnimationLibrary::on_reload, this);
    }
    // The rigs point into the loaded objects, so they go first.
    rigs_.clear();
    clips_.clear();
    programs_.clear();
    skeletons_.clear();
}

Error AnimationLibrary::refuse(ErrorCode code, const char* format, ...) noexcept {
    va_list arguments;
    va_start(arguments, format);
    (void)std::vsnprintf(refusal_, sizeof(refusal_), format, arguments);
    va_end(arguments);
    emit_diagnostic(DiagnosticSeverity::Error, "animation", refusal_);
    ++stats_.refusals;
    return Error{code, refusal_, 0};
}

Expected<Ref<assets::AssetData>, Error> AnimationLibrary::load(AssetId id) noexcept {
    char text[AssetId::kTextLength + 1] = {};
    (void)id.format(text);
    Expected<Ref<assets::AssetData>, Error> loaded = assets_->load(id);
    if (!loaded) {
        return make_unexpected(refuse(loaded.error().code, "animation asset %s did not load: %s",
                                      text, loaded.error().message));
    }
    if ((*loaded)->is_placeholder()) {
        return make_unexpected(refuse(ErrorCode::NotFound,
                                      "animation asset %s is missing; the asset system served a "
                                      "placeholder",
                                      text));
    }
    return loaded;
}

AnimationLibrary::LoadedSkeleton* AnimationLibrary::find_skeleton(AssetId id) const noexcept {
    return find_loaded(skeletons_, id);
}

AnimationLibrary::LoadedClip* AnimationLibrary::find_clip(AssetId id) const noexcept {
    return find_loaded(clips_, id);
}

AnimationLibrary::LoadedProgram* AnimationLibrary::find_program(AssetId id) const noexcept {
    return find_loaded(programs_, id);
}

Expected<const Skeleton*, Error> AnimationLibrary::skeleton(AssetId id) noexcept {
    if (const LoadedSkeleton* found = find_skeleton(id); found != nullptr) {
        return &found->skeleton;
    }
    Expected<Ref<assets::AssetData>, Error> data = load(id);
    if (!data) {
        return make_unexpected(data.error());
    }
    Expected<UniquePtr<LoadedSkeleton>, Error> entry =
        make_unique<LoadedSkeleton>(*allocator_, *allocator_);
    if (!entry) {
        return make_unexpected(entry.error());
    }
    (*entry)->id = id;
    (*entry)->data = std::move(*data);
    if (Status decoded =
            decode_skeleton((*entry)->data->bytes(), (*entry)->skeleton, (*entry)->humanoid);
        !decoded) {
        char text[AssetId::kTextLength + 1] = {};
        (void)id.format(text);
        return make_unexpected(refuse(decoded.error().code,
                                      "skeleton %s is not a cooked skeleton: %s", text,
                                      decoded.error().message));
    }
    const Skeleton* out = &(*entry)->skeleton;
    if (Status pushed = skeletons_.push_back(std::move(*entry)); !pushed) {
        return make_unexpected(pushed.error());
    }
    ++stats_.skeletons;
    return out;
}

const SkeletonProfile* AnimationLibrary::humanoid(AssetId id) const noexcept {
    const LoadedSkeleton* found = find_skeleton(id);
    return found == nullptr ? nullptr : &found->humanoid;
}

Expected<AnimationLibrary::LoadedClip*, Error> AnimationLibrary::load_clip(AssetId id) noexcept {
    if (LoadedClip* found = find_clip(id); found != nullptr) {
        return found;
    }
    Expected<Ref<assets::AssetData>, Error> data = load(id);
    if (!data) {
        return make_unexpected(data.error());
    }
    Expected<UniquePtr<LoadedClip>, Error> entry =
        make_unique<LoadedClip>(*allocator_, *allocator_);
    if (!entry) {
        return make_unexpected(entry.error());
    }
    (*entry)->id = id;
    (*entry)->data = std::move(*data);
    if (Status decoded = decode_clip((*entry)->data->bytes(), (*entry)->clip, (*entry)->joints);
        !decoded) {
        char text[AssetId::kTextLength + 1] = {};
        (void)id.format(text);
        return make_unexpected(refuse(decoded.error().code, "clip %s is not a cooked clip: %s",
                                      text, decoded.error().message));
    }
    LoadedClip* out = entry->get();
    if (Status pushed = clips_.push_back(std::move(*entry)); !pushed) {
        return make_unexpected(pushed.error());
    }
    ++stats_.clips;
    return out;
}

Expected<const Clip*, Error> AnimationLibrary::clip(AssetId id) noexcept {
    Expected<LoadedClip*, Error> loaded = load_clip(id);
    if (!loaded) {
        return make_unexpected(loaded.error());
    }
    return &(*loaded)->clip;
}

Expected<const graph::pose::PoseProgram*, Error> AnimationLibrary::program(AssetId id) noexcept {
    if (const LoadedProgram* found = find_program(id); found != nullptr) {
        return &found->program;
    }
    Expected<Ref<assets::AssetData>, Error> data = load(id);
    if (!data) {
        return make_unexpected(data.error());
    }
    Expected<graph::pose::PoseProgram, Error> decoded =
        decode_program(*allocator_, (*data)->bytes());
    if (!decoded) {
        char text[AssetId::kTextLength + 1] = {};
        (void)id.format(text);
        return make_unexpected(refuse(decoded.error().code,
                                      "program %s is not a cooked pose program: %s", text,
                                      decoded.error().message));
    }
    Expected<UniquePtr<LoadedProgram>, Error> entry =
        make_unique<LoadedProgram>(*allocator_, *allocator_);
    if (!entry) {
        return make_unexpected(entry.error());
    }
    (*entry)->id = id;
    (*entry)->data = std::move(*data);
    (*entry)->program = std::move(*decoded);
    const graph::pose::PoseProgram* out = &(*entry)->program;
    if (Status pushed = programs_.push_back(std::move(*entry)); !pushed) {
        return make_unexpected(pushed.error());
    }
    ++stats_.programs;
    return out;
}

Status AnimationLibrary::bind_table(BoundRig& rig, Span<LoadedClip* const> available) noexcept {
    const graph::pose::PoseProgram& program = rig.program->program;
    const Skeleton& skeleton = rig.skeleton->skeleton;
    if (Status sized = rig.table.resize(program.clips().size()); !sized) {
        return sized;
    }
    rig.clips.clear();
    for (usize index = 0; index < program.clips().size(); ++index) {
        const Name wanted = program.clips()[index].name;
        LoadedClip* match = nullptr;
        for (LoadedClip* candidate : available) {
            if (candidate->clip.name() == wanted) {
                match = candidate;
                break;
            }
        }
        // A null entry is legal to `AnimationRig::bind` and samples the reference pose with nothing
        // reporting it. That is the silent failure this refuses.
        if (match == nullptr) {
            return Status{make_unexpected(
                refuse(ErrorCode::NotFound,
                       "program '%s' names clip '%s', and no clip of that name was given",
                       program.name().c_str(), wanted.c_str()))};
        }
        u16 joint = kInvalidJoint;
        if (!clip_matches_skeleton(match->clip, match->joints.span(), skeleton, joint)) {
            const char* meant = joint < match->joints.size() ? match->joints[joint].c_str() : "?";
            return Status{make_unexpected(refuse(
                ErrorCode::InvalidArgument,
                "clip '%s' was cooked for another rig: its track for joint %u means '%s', which "
                "skeleton '%s' does not have there",
                wanted.c_str(), static_cast<u32>(joint), meant, skeleton.name().c_str()))};
        }
        rig.table[index] = &match->clip;
        if (Status pushed = rig.clips.push_back(match); !pushed) {
            return pushed;
        }
    }
    return rig.rig.bind(skeleton, program, rig.table.span());
}

Expected<const AnimationRig*, Error> AnimationLibrary::rig(const RigAssets& wanted) noexcept {
    if (Expected<const Skeleton*, Error> loaded = skeleton(wanted.skeleton); !loaded) {
        return make_unexpected(loaded.error());
    }
    if (Expected<const graph::pose::PoseProgram*, Error> loaded = program(wanted.program);
        !loaded) {
        return make_unexpected(loaded.error());
    }
    Array<LoadedClip*> available(*allocator_);
    for (const AssetId id : wanted.clips) {
        Expected<LoadedClip*, Error> loaded = load_clip(id);
        if (!loaded) {
            return make_unexpected(loaded.error());
        }
        if (Status pushed = available.push_back(*loaded); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    Expected<UniquePtr<BoundRig>, Error> entry = make_unique<BoundRig>(*allocator_, *allocator_);
    if (!entry) {
        return make_unexpected(entry.error());
    }
    (*entry)->skeleton = find_skeleton(wanted.skeleton);
    (*entry)->program = find_program(wanted.program);
    if (Status bound = bind_table(**entry, available.span()); !bound) {
        return make_unexpected(bound.error());
    }
    const AnimationRig* out = &(*entry)->rig;
    if (Status pushed = rigs_.push_back(std::move(*entry)); !pushed) {
        return make_unexpected(pushed.error());
    }
    ++stats_.rigs;
    return out;
}

// --- Hot reload ----------------------------------------------------------------------------------

Status AnimationLibrary::watch() noexcept {
    if (watching_) {
        return ok();
    }
    if (Status added = assets_->add_reload_observer(&AnimationLibrary::on_reload, this); !added) {
        return added;
    }
    watching_ = true;
    return ok();
}

Status AnimationLibrary::reload_clip(LoadedClip& loaded) noexcept {
    // Decoded beside the live clip first: nothing the rigs point at changes until the new clip has
    // decoded and matched every skeleton it is bound to.
    Clip fresh(*allocator_);
    Array<Name> joints(*allocator_);
    if (Status decoded = decode_clip(loaded.data->bytes(), fresh, joints); !decoded) {
        return Status{make_unexpected(refuse(decoded.error().code,
                                             "a reload of clip '%s' did not decode, and the "
                                             "working clip is kept: %s",
                                             loaded.clip.name().c_str(), decoded.error().message))};
    }
    if (fresh.name() != loaded.clip.name()) {
        return Status{make_unexpected(
            refuse(ErrorCode::InvalidArgument,
                   "a reload renamed clip '%s' to '%s'; programs bind clips by name, so the "
                   "working clip is kept",
                   loaded.clip.name().c_str(), fresh.name().c_str()))};
    }
    for (const UniquePtr<BoundRig>& rig : rigs_) {
        bool uses = false;
        for (const LoadedClip* clip : rig->clips) {
            uses = uses || clip == &loaded;
        }
        u16 joint = kInvalidJoint;
        if (uses && !clip_matches_skeleton(fresh, joints.span(), rig->skeleton->skeleton, joint)) {
            return Status{make_unexpected(
                refuse(ErrorCode::InvalidArgument,
                       "a reload of clip '%s' no longer matches skeleton '%s' at joint %u, and the "
                       "working clip is kept",
                       fresh.name().c_str(), rig->skeleton->skeleton.name().c_str(),
                       static_cast<u32>(joint)))};
        }
    }
    // THE SWAP, INTO THE SAME OBJECT. Every rig table and every cursor keeps its pointer; a cursor
    // sized for the old track count restarts on its next sample.
    loaded.clip = std::move(fresh);
    loaded.joints = std::move(joints);
    for (const UniquePtr<BoundRig>& rig : rigs_) {
        bool uses = false;
        for (const LoadedClip* clip : rig->clips) {
            uses = uses || clip == &loaded;
        }
        // Rebound so a changed duration reaches the time-parameter table the clocks wrap by.
        if (uses) {
            if (Status bound = rig->rig.bind(rig->skeleton->skeleton, rig->program->program,
                                             rig->table.span());
                !bound) {
                return bound;
            }
        }
    }
    return ok();
}

void AnimationLibrary::on_reload(void* user, const assets::ReloadEvent& event) noexcept {
    auto* library = static_cast<AnimationLibrary*>(user);
    if (LoadedClip* clip = library->find_clip(event.id); clip != nullptr) {
        if (library->reload_clip(*clip)) {
            ++library->stats_.reloads_applied;
        } else {
            ++library->stats_.reloads_refused;
        }
        return;
    }
    if (library->find_skeleton(event.id) != nullptr || library->find_program(event.id) != nullptr) {
        char text[AssetId::kTextLength + 1] = {};
        (void)event.id.format(text);
        (void)library->refuse(ErrorCode::Unsupported,
                              "asset %s is a skeleton or a program, which live instances are laid "
                              "out from; it is not swapped under them — rebuild the rig to use it",
                              text);
        ++library->stats_.reloads_refused;
    }
}

}  // namespace cy::animation
