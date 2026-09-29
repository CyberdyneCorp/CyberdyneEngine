// SPDX-License-Identifier: MIT
#pragma once

#include <cy/core/base/expected.h>
#include <cy/core/memory/array.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/vfx/world.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace cy::vfx {

/// Returns a cooked system that remains alive while its scene instances play.
using SceneSystemResolver = Expected<const CompiledSystem*, Error> (*)(std::string_view asset,
                                                                       void* context) noexcept;

/// Binds authored `.cyworld` effect components to independent Engine VFX instances.
class SceneEffects {
public:
    explicit SceneEffects(Allocator& allocator) noexcept : bindings_(allocator) {}

    /// Replace the bindings after a world load or edit. Unknown and folded overrides are refused.
    [[nodiscard]] Status load(const scene::serialization::World& scene, SimulationWorld& simulation,
                              SceneSystemResolver resolve, void* context) noexcept;
    /// Reuse playing effects when the same entities still reference the same assets.
    [[nodiscard]] bool same_instances(const scene::serialization::World& scene) const noexcept;
    /// Apply edited instance values and transforms without restarting or recompiling systems.
    [[nodiscard]] Status refresh(const scene::serialization::World& scene,
                                 SimulationWorld& simulation) noexcept;
    /// Follow authored transforms without rebuilding the compiled systems or restarting effects.
    [[nodiscard]] Status update_transforms(const scene::serialization::World& scene,
                                           SimulationWorld& simulation) noexcept;
    void clear(SimulationWorld& simulation) noexcept;

    [[nodiscard]] EffectHandle find(u64 node_identity) const noexcept;
    [[nodiscard]] usize size() const noexcept { return bindings_.size(); }

private:
    struct ParameterSnapshot {
        u64 field = 0;
        scene::serialization::WorldValueKind kind = scene::serialization::WorldValueKind::Nil;
        std::array<f32, 4> lanes{};
        i64 integer = 0;

        [[nodiscard]] bool operator==(const ParameterSnapshot&) const noexcept = default;
    };
    struct Binding {
        u64 node = 0;
        EffectHandle effect = kInvalidEffect;
        std::string asset;
        std::vector<ParameterSnapshot> parameters;
    };
    [[nodiscard]] static std::vector<ParameterSnapshot> snapshot_of(
        const scene::serialization::World& scene, const scene::serialization::WorldTypeDecl& type,
        const scene::serialization::WorldComponent& component);
    Array<Binding> bindings_;
};

}  // namespace cy::vfx
