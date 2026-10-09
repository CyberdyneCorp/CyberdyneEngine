// SPDX-License-Identifier: MIT
// play_animation.h — Play's animation: the project's baked rigs, animated as a game animates them.
// Issue #112's gaps (#29, animation).
//
// The animation panel bakes a graph for a project character (`animation.bake`,
// `cy/editor/animation_rig.h`) into `<project>/.cy/cooked/animation/<rig>/`. At Play this loads
// every such rig the way a shipped game loads one — the records into the asset system, bound by
// `AnimationLibrary`, the program's clip table matched by name — registers it with an
// `AnimationSystem` over the Play world, and installs the ABI's animation backend
// (`cy::game_backend::AnimationAdapter`) under the rig's name. So a Swift behaviour's
// `Animator.attach(to: entity, rig: "hero")` plays the graph the author drew, and
// `Animation.events(for:)` in its `onUpdate` delivers the events the author placed on the
// timeline, at the times they were placed.
//
// EACH FIXED TICK, after the behaviours' fixed step has made its requests: the system advances one
// tick (`AnimationSystem::run`), root motion is consumed, and the frame's events are snapshotted
// (`AnimationAdapter::begin_frame`) for the frame step that follows.
//
// A rig that does not load is skipped and named (`problems()`): one stale bake does not stop Play.

#pragma once

#include <cy/abi/game/animation.h>
#include <cy/core/memory/allocator.h>
#include <cy/gameplay/play/session.h>

#include <memory>
#include <string>
#include <vector>

namespace cy::sample::editor_window {

class PlayAnimation {
public:
    PlayAnimation(Allocator& allocator, const char* project) noexcept;
    ~PlayAnimation();

    PlayAnimation(const PlayAnimation&) = delete;
    PlayAnimation& operator=(const PlayAnimation&) = delete;
    PlayAnimation(PlayAnimation&&) = delete;
    PlayAnimation& operator=(PlayAnimation&&) = delete;

    /// Load every baked rig and animate `play`'s world. Fails only when the animation system
    /// itself cannot be built; a rig that does not load is recorded in `problems()`.
    [[nodiscard]] Status start(gameplay::PlaySession& play) noexcept;
    void stop() noexcept;
    /// One fixed tick of `seconds`, then this frame's events.
    [[nodiscard]] Status tick(f32 seconds) noexcept;

    /// What `animation_*` reaches during Play, or null outside it.
    [[nodiscard]] abi::game::AnimationBackend* backend() noexcept;
    /// The rigs registered, by the name a script attaches by.
    [[nodiscard]] const std::vector<std::string>& rigs() const noexcept { return rigs_; }
    /// Why each skipped rig was skipped.
    [[nodiscard]] const std::vector<std::string>& problems() const noexcept { return problems_; }

private:
    struct Loaded;

    [[nodiscard]] Status load_rig(const std::string& directory, u32 index) noexcept;

    Allocator* allocator_;
    std::string project_;
    std::unique_ptr<Loaded> loaded_;
    std::vector<std::string> rigs_;
    std::vector<std::string> problems_;
};

}  // namespace cy::sample::editor_window
