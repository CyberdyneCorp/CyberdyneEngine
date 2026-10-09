// SPDX-License-Identifier: MIT
#include "play_animation.h"

#include "animation_assets.h"

#include <cy/animation/animation_system.h>
#include <cy/animation/library.h>
#include <cy/core/assets/asset_system.h>
#include <cy/core/assets/file.h>
#include <cy/core/assets/package.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/jobs/async.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/memory/ownership.h>
#include <cy/editor/animation_rig.h>
#include <cy/game_backend/animation_backend.h>

#include <algorithm>
#include <filesystem>
#include <system_error>

namespace cy::sample::editor_window {
namespace {

/// A path the manifest names inside its rig's directory: relative, and never leaving it.
[[nodiscard]] bool inside(std::string_view path) noexcept {
    return !path.empty() && path.front() != '/' && path.find("..") == std::string_view::npos &&
           path.find('\\') == std::string_view::npos && path.find(':') == std::string_view::npos;
}

}  // namespace

/// Everything one Play's animation owns, built at `start` and dropped at `stop`, in that order.
struct PlayAnimation::Loaded {
    Loaded() noexcept = default;

    ~Loaded() {
        adapter.reset();
        system.reset();
        library.reset();
        if (started) {
            assets.shutdown();
            async.stop();
            workers.shutdown();
        }
    }

    Loaded(const Loaded&) = delete;
    Loaded& operator=(const Loaded&) = delete;
    Loaded(Loaded&&) = delete;
    Loaded& operator=(Loaded&&) = delete;

    [[nodiscard]] Status store(AssetId id, const Array<u8>& bytes) noexcept {
        Expected<assets::VirtualPath, Error> path = assets::package_entry_path(id, {});
        if (!path) {
            return make_unexpected(path.error());
        }
        return files.write(*path, bytes.data(), bytes.size());
    }

    jobs::JobSystem workers;
    jobs::AsyncService async;
    assets::VirtualFileSystem files;
    assets::AssetSystem assets;
    std::unique_ptr<animation::AnimationLibrary> library;
    std::unique_ptr<animation::AnimationSystem> system;
    std::unique_ptr<game_backend::AnimationAdapter> adapter;
    bool started = false;
};

PlayAnimation::PlayAnimation(Allocator& allocator, const char* project) noexcept
    : allocator_(&allocator), project_(project != nullptr ? project : "") {}

PlayAnimation::~PlayAnimation() {
    stop();
}

void PlayAnimation::stop() noexcept {
    loaded_.reset();
    rigs_.clear();
}

abi::game::AnimationBackend* PlayAnimation::backend() noexcept {
    return loaded_ != nullptr ? loaded_->adapter.get() : nullptr;
}

Status PlayAnimation::start(gameplay::PlaySession& play) noexcept {
    stop();
    problems_.clear();
    if (play.world() == nullptr) {
        return fail(ErrorCode::InvalidArgument, "Play's animation needs a running session");
    }
    auto loaded = std::make_unique<Loaded>();
    jobs::JobSystemConfig config;
    config.worker_count = 1;
    if (Status started = loaded->workers.start(config); !started) {
        return started;
    }
    if (Status started = loaded->async.start(loaded->workers); !started) {
        loaded->workers.shutdown();
        return started;
    }
    Expected<UniquePtr<assets::MemoryMount>, Error> memory =
        make_unique<assets::MemoryMount>(*allocator_, "baked-rigs");
    if (!memory) {
        loaded->async.stop();
        loaded->workers.shutdown();
        return make_unexpected(memory.error());
    }
    if (Expected<assets::MountId, Error> mounted =
            loaded->files.mount_owned(std::move(*memory), assets::mount_priority::kMemory);
        !mounted) {
        loaded->async.stop();
        loaded->workers.shutdown();
        return make_unexpected(mounted.error());
    }
    if (Status started = loaded->assets.start(loaded->workers, loaded->async, loaded->files,
                                              assets::AssetSystemConfig{});
        !started) {
        loaded->async.stop();
        loaded->workers.shutdown();
        return started;
    }
    loaded->started = true;
    loaded->library = std::make_unique<animation::AnimationLibrary>(*allocator_, loaded->assets);
    Expected<ecs::ComponentTypeId, Error> animator = animation::register_animator(*play.world());
    if (!animator) {
        return make_unexpected(animator.error());
    }
    loaded->system = std::make_unique<animation::AnimationSystem>(*allocator_, *play.world(),
                                                                  *animator, play.tree());
    loaded->adapter = std::make_unique<game_backend::AnimationAdapter>(
        *allocator_, *play.world(), *loaded->system, *animator, play.tree());
    loaded_ = std::move(loaded);

    if (project_.empty()) {
        // No project, no baked rigs: never the working directory's.
        return ok();
    }
    std::error_code error;
    const std::filesystem::path root =
        std::filesystem::path(project_) / std::string(editor::kAnimationRigDirectory);
    std::vector<std::string> directories;
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (entry.is_directory(error)) {
            directories.push_back(entry.path().string());
        }
    }
    // In name order, so the rigs register in the same order on every machine.
    std::ranges::sort(directories);
    for (u32 index = 0; index < directories.size(); ++index) {
        if (Status loaded_rig = load_rig(directories[index], index); !loaded_rig) {
            problems_.push_back(directories[index] + ": " + loaded_rig.error().message);
        }
    }
    return ok();
}

Status PlayAnimation::load_rig(const std::string& directory, u32 index) noexcept {
    Array<u8> text(*allocator_);
    const std::string manifest_path = directory + "/" + std::string(editor::kAnimationRigManifest);
    if (Status read = assets::fs::read_whole(manifest_path.c_str(), text); !read) {
        return read;
    }
    editor::AnimationRigManifest manifest(*allocator_);
    if (Status parsed = editor::read_animation_rig(
            {reinterpret_cast<const char*>(text.data()), text.size()}, manifest);
        !parsed) {
        return parsed;
    }
    // Ids local to this Play: the rig's index and the record's place in it.
    const u64 rig = u64{index} + 1U;
    const AssetId skeleton{rig, 1};
    const AssetId program{rig, 2};
    Array<u8> record(*allocator_);
    if (Status read = read_cooked_record(project_, manifest.skeleton, record); !read) {
        return fail(ErrorCode::NotFound,
                    "the rig's skeleton is not cooked; import its model again");
    }
    if (Status stored = loaded_->store(skeleton, record); !stored) {
        return stored;
    }
    if (!inside(manifest.program.text())) {
        return fail(ErrorCode::InvalidArgument, "the rig's program lies outside its directory");
    }
    if (Status read =
            read_cooked_file(directory + "/" + std::string(manifest.program.text()), record);
        !read) {
        return read;
    }
    if (Status stored = loaded_->store(program, record); !stored) {
        return stored;
    }
    Array<AssetId> clips(*allocator_);
    for (const editor::AnimationRigClip& clip : manifest.clips) {
        if (!inside(clip.path.text())) {
            return fail(ErrorCode::InvalidArgument, "a rig's clip lies outside its directory");
        }
        if (Status read = read_cooked_file(directory + "/" + std::string(clip.path.text()), record);
            !read) {
            return read;
        }
        const AssetId id{rig, 3U + clips.size()};
        if (Status stored = loaded_->store(id, record); !stored) {
            return stored;
        }
        if (Status pushed = clips.push_back(id); !pushed) {
            return pushed;
        }
    }
    const animation::RigAssets wanted{skeleton, program, clips.span()};
    Expected<const animation::AnimationRig*, Error> bound = loaded_->library->rig(wanted);
    if (!bound) {
        return make_unexpected(bound.error());
    }
    Expected<animation::RigId, Error> added = loaded_->system->add_rig(**bound);
    if (!added) {
        return make_unexpected(added.error());
    }
    const std::string name(manifest.rig.text());
    if (Status registered = loaded_->adapter->add_rig(name.c_str(), *added); !registered) {
        return registered;
    }
    rigs_.push_back(name);
    return ok();
}

Status PlayAnimation::tick(f32 seconds) noexcept {
    if (loaded_ == nullptr) {
        return ok();
    }
    if (Status ran = loaded_->system->run(1, seconds, nullptr); !ran) {
        return ran;
    }
    if (Status moved = loaded_->adapter->update(seconds); !moved) {
        return moved;
    }
    return loaded_->adapter->begin_frame();
}

}  // namespace cy::sample::editor_window
