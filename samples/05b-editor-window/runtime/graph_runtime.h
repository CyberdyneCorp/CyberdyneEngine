// SPDX-License-Identifier: MIT
#pragma once
// Gameplay graphs during the hosted Play. Issue #29, visual scripting.
//
// The engine side of a graph is `cy::game_backend::GraphBehaviours`; this is the glue a host owes
// it, the same glue `ScriptRuntime` is for Swift: on Play it finds every authored node with a
// `ScriptGraph` component, reads the `.cyscript` its `graph` field names from the project, compiles
// each distinct graph ONCE, and attaches every node to its graph's shared program. Each fixed tick
// it runs the one graph system. It is also the editor's `script.*` seam: `script.event.raise`
// reaches `raise`, and `script.state.get` reads what `behaviours` returns.

#include <cy/core/base/expected.h>
#include <cy/editor/script_service.h>
#include <cy/game_backend/graph_behaviours.h>
#include <cy/gameplay/play/session.h>
#include <cy/scene/serialization/worldfile.h>

#include <string>
#include <vector>

namespace cy::sample::editor_window {

/// The authored component a node runs a graph through, and its one field.
inline constexpr const char* kScriptGraphComponent = "ScriptGraph";
inline constexpr const char* kScriptGraphField = "graph";

class GraphRuntime final : public editor::ScriptPlayRuntime {
public:
    GraphRuntime(Allocator& allocator, const char* project) noexcept;

    /// Compile and attach every authored graph. A graph that does not compile, or a cue the
    /// project does not have, refuses Play with the node and the reason.
    [[nodiscard]] Status start(gameplay::PlaySession& play,
                               const scene::serialization::World& authored,
                               abi::game::AudioBackend* audio) noexcept;
    void stop() noexcept;
    /// One fixed step of the graph system.
    [[nodiscard]] Status tick(f32 dt) noexcept;
    /// Instances running, for the Play detail.
    [[nodiscard]] u32 count() const noexcept { return graphs_.instance_count(); }
    /// Why the last `start` refused, for the Play detail.
    [[nodiscard]] const std::string& problem() const noexcept { return problem_; }

    [[nodiscard]] const game_backend::GraphBehaviours* behaviours() const noexcept override;
    [[nodiscard]] ecs::Entity entity_for(u64 identity) const noexcept override;
    [[nodiscard]] u64 identity_of(ecs::Entity entity) const noexcept override;
    [[nodiscard]] Expected<u32, Error> raise(ecs::Entity entity, Name event,
                                             Span<const f32> arguments) noexcept override;

private:
    [[nodiscard]] Expected<u32, Error> load(const std::string& reference) noexcept;

    Allocator* allocator_;
    std::string project_;
    game_backend::GraphBehaviours graphs_;
    std::vector<std::string> loaded_;
    gameplay::PlaySession* play_ = nullptr;
    const scene::serialization::World* authored_ = nullptr;
    std::string problem_;
};

}  // namespace cy::sample::editor_window
