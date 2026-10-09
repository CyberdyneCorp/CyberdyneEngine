// SPDX-License-Identifier: MIT
#pragma once

#include <cy/abi/game/animation.h>
#include <cy/abi/game/audio.h>
#include <cy/abi/host.h>
#include <cy/abi/module.h>
#include <cy/core/memory/ownership.h>
#include <cy/gameplay/play/session.h>
#include <cy/scene/serialization/worldfile.h>

#include <string>
#include <vector>

namespace cy::sample::editor_window {

#if defined(CY_EDITOR_WINDOW_HAS_VFX)
class SceneVfxRuntime;
#endif

/// Runs project Swift behaviours on authored nodes while the hosted Play session is active.
class ScriptRuntime : private abi::VfxEffectBackend {
public:
    ScriptRuntime(Allocator& allocator, const char* project,
                  const char* module_path = nullptr) noexcept;

    [[nodiscard]] Status start(gameplay::PlaySession& play,
                               const scene::serialization::World& authored) noexcept;
    void stop() noexcept;
    /// One fixed step of every behaviour.
    [[nodiscard]] Status tick(gameplay::PlaySession& play, f32 dt) noexcept;
    /// One frame of every behaviour that has `onUpdate`: Play runs one per fixed tick, after the
    /// tick's animation, so a behaviour reads the tick's animation events
    /// (`Animation.events(for:)`) where a game reads them.
    void frame(f32 dt) noexcept;
    [[nodiscard]] Expected<abi::ReloadReport, Error> reload(const char* library) noexcept;
    [[nodiscard]] bool active() const noexcept { return static_cast<bool>(runtime_); }
    [[nodiscard]] u32 count() const noexcept { return static_cast<u32>(identities_.size()); }
#if defined(CY_EDITOR_WINDOW_HAS_VFX)
    void bind_scene_vfx(SceneVfxRuntime* runtime) noexcept { scene_vfx_ = runtime; }
#endif
    /// What ABI 1.3's `audio_*` entries reach during Play: the editor's own audio server's
    /// adapter, so a Swift behaviour plays the cues and buses the author sees in the mixer — and
    /// the same adapter the gameplay graphs play through. Null: unavailable.
    void bind_audio(abi::game::AudioBackend* audio) noexcept { audio_ = audio; }
    /// What ABI 1.7's `animation_*` entries reach during Play: the adapter over Play's own
    /// animation system and the project's baked rigs (`play_animation.h`). Bound before `start`, so
    /// a behaviour's `onCreate` can attach an animator. Null: unavailable.
    void bind_animation(abi::game::AnimationBackend* animation) noexcept { animation_ = animation; }

private:
    [[nodiscard]] CyResult set(CyEntity entity, const char* emitter, const char* parameter,
                               const CyVar& value) noexcept override;
    [[nodiscard]] CyResult get(CyEntity entity, const char* emitter, const char* parameter,
                               CyVar& out_value) noexcept override;
    [[nodiscard]] u64 scene_node(CyEntity entity) const noexcept;
    Allocator* allocator_;
    std::string project_;
    std::string module_path_;
    std::string active_library_;
    abi::Host host_;
    UniquePtr<abi::World> binding_;
    UniquePtr<abi::BehaviourRuntime> runtime_;
    abi::ModuleManifest manifest_;
    std::vector<u64> identities_;
    gameplay::PlaySession* play_ = nullptr;
    const scene::serialization::World* authored_ = nullptr;
    abi::game::AudioBackend* audio_ = nullptr;
    abi::game::AnimationBackend* animation_ = nullptr;
#if defined(CY_EDITOR_WINDOW_HAS_VFX)
    SceneVfxRuntime* scene_vfx_ = nullptr;
#endif
};

}  // namespace cy::sample::editor_window
