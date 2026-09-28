// SPDX-License-Identifier: MIT
#pragma once

#if defined(CY_EDITOR_WINDOW_HAS_VFX)

#    include <cy/scene/serialization/worldfile.h>
#    include <cy/vfx/scene_effects.h>

#    include <deque>
#    include <string>

namespace cy::sample::editor_window {

/// Editor-hosted Engine VFX for effect entities saved in the open `.cyworld`.
class SceneVfxRuntime {
public:
    SceneVfxRuntime(Allocator& allocator, std::string project) noexcept;

    [[nodiscard]] Status initialize() noexcept;
    [[nodiscard]] Status load(const scene::serialization::World& scene) noexcept;
    [[nodiscard]] Status step(const scene::serialization::World& scene, f32 seconds) noexcept;
    [[nodiscard]] const vfx::SimulationWorld* world() const noexcept {
        return effects_.size() == 0 ? nullptr : &simulation_;
    }
    [[nodiscard]] usize instances() const noexcept { return effects_.size(); }
    [[nodiscard]] Status set_parameter(u64 node, Name emitter, Name parameter,
                                       Span<const f32> value) noexcept;
    [[nodiscard]] Expected<vfx::EffectParameterValue, Error> get_parameter(
        u64 node, Name emitter, Name parameter) const noexcept;

private:
    struct System {
        std::string path;
        vfx::CompiledSystem compiled;
    };

    [[nodiscard]] static Expected<const vfx::CompiledSystem*, Error> resolve(
        std::string_view path, void* context) noexcept;
    [[nodiscard]] Expected<const vfx::CompiledSystem*, Error> system(
        std::string_view path) noexcept;
    [[nodiscard]] Expected<std::string, Error> read(std::string_view path) const noexcept;

    Allocator* allocator_;
    std::string project_;
    vfx::SimulationWorld simulation_;
    vfx::SceneEffects effects_;
    std::deque<System> systems_;
    bool ready_ = false;
};

}  // namespace cy::sample::editor_window

#endif
