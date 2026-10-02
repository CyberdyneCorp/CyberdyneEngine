// SPDX-License-Identifier: MIT
#include "graph_runtime.h"

#include <cy/core/memory/system_allocator.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

namespace cy::sample::editor_window {
namespace {

namespace ser = scene::serialization;

/// The text a node's `component.field` holds, or empty.
std::string text_field(const ser::World& world, const ser::WorldNode& node,
                       std::string_view component, std::string_view field) {
    for (const ser::WorldTypeDecl& declared : world.types()) {
        if (world.text(declared.name) != component) {
            continue;
        }
        const ser::WorldComponent* held = node.find(declared.file_type);
        if (held == nullptr) {
            return {};
        }
        for (const ser::WorldFieldDecl& declared_field : declared.fields()) {
            if (world.text(declared_field.name) != field) {
                continue;
            }
            const ser::WorldField* value = held->find(declared_field.file_field);
            if (value == nullptr || value->value.kind != ser::WorldValueKind::Text) {
                return {};
            }
            const Span<const u8> bytes = world.blob(value->value);
            return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        }
    }
    return {};
}

/// A project-relative `.cyscript` that stays inside the project.
bool acceptable(const std::string& reference) {
    const std::filesystem::path path(reference);
    return !reference.empty() && path.is_relative() && path.extension() == ".cyscript" &&
           reference.find("..") == std::string::npos;
}

/// The first error, as a person reads it: where, what, and the name it is about.
std::string describe(const std::string& reference, const graph::DiagnosticSink& sink,
                     const char* fallback) {
    for (const graph::Diagnostic& diagnostic : sink.entries()) {
        if (diagnostic.severity != graph::Severity::Error) {
            continue;
        }
        std::ostringstream text;
        text << reference << ": node " << diagnostic.node << ": " << diagnostic.message;
        if (!diagnostic.detail.is_empty()) {
            text << " (" << diagnostic.detail.text() << ")";
        }
        return text.str();
    }
    return reference + ": " + fallback;
}

}  // namespace

GraphRuntime::GraphRuntime(Allocator& allocator, const char* project) noexcept
    : allocator_(&allocator), project_(project == nullptr ? "" : project), graphs_(allocator) {}

Expected<u32, Error> GraphRuntime::load(const std::string& reference) noexcept {
    for (usize index = 0; index < loaded_.size(); ++index) {
        if (loaded_[index] == reference) {
            return static_cast<u32>(index);
        }
    }
    if (!acceptable(reference)) {
        problem_ = reference + ": a ScriptGraph names a project-relative .cyscript";
        return fail(ErrorCode::InvalidArgument, "a ScriptGraph names a project-relative .cyscript");
    }
    std::ifstream file(std::filesystem::path(project_) / reference, std::ios::binary);
    if (!file.good()) {
        problem_ = reference + ": the graph is not in the project";
        return fail(ErrorCode::NotFound, "a ScriptGraph's graph is not in the project");
    }
    std::ostringstream read;
    read << file.rdbuf();
    const std::string source = read.str();
    graph::DiagnosticSink sink(*allocator_);
    const std::string stem = std::filesystem::path(reference).stem().string();
    Expected<u32, Error> loaded =
        graphs_.load(Name::intern(stem), source, game_backend::GraphBackend::Bytecode, sink);
    if (!loaded) {
        problem_ = describe(reference, sink, loaded.error().message);
        return loaded;
    }
    loaded_.push_back(reference);
    return loaded;
}

Status GraphRuntime::start(gameplay::PlaySession& play, const ser::World& authored,
                           abi::game::AudioBackend* audio) noexcept {
    stop();
    if (play.tree() == nullptr) {
        return fail(ErrorCode::Unavailable, "graphs need a play session with a scene");
    }
    graphs_.start(*play.tree(), audio);
    play_ = &play;
    authored_ = &authored;
    for (const ser::WorldNode& node : authored.nodes()) {
        if (!node.live) {
            continue;
        }
        const std::string reference =
            text_field(authored, node, kScriptGraphComponent, kScriptGraphField);
        if (reference.empty()) {
            continue;
        }
        const ecs::Entity entity = play.entity_for(node.identity);
        if (!entity.valid()) {
            problem_ = reference + ": its node has no Play entity";
            stop();
            return fail(ErrorCode::NotFound, "a graph's node has no Play entity");
        }
        const Expected<u32, Error> graph = load(reference);
        if (!graph) {
            const std::string why = problem_;
            stop();
            problem_ = why;
            return make_unexpected(graph.error());
        }
        if (Status attached = graphs_.attach(*graph, entity); !attached) {
            stop();
            return attached;
        }
    }
    // The editor's Play is a development session: where the build has the debugger, attach it, so
    // the trace highlights from the first tick and a breakpoint set later stops at once.
    if constexpr (graph::script::kGraphDebuggerEnabled) {
        if (Status debugging = graphs_.set_debugging(true); !debugging) {
            problem_ = debugging.error().message;
            stop();
            return debugging;
        }
    }
    return ok();
}

void GraphRuntime::stop() noexcept {
    if (held_ && hold_listener_ != nullptr) {
        hold_listener_(hold_user_, false);
    }
    held_ = false;
    resume_on_release_ = false;
    graphs_.stop();
    loaded_.clear();
    play_ = nullptr;
    authored_ = nullptr;
    problem_.clear();
}

Status GraphRuntime::tick(f32 dt) noexcept {
    if (play_ == nullptr || graphs_.instance_count() == 0) {
        return ok();
    }
    if (Status updated = graphs_.update(dt); !updated) {
        return updated;
    }
    sync_hold();
    return ok();
}

void GraphRuntime::sync_hold() noexcept {
    if (play_ == nullptr) {
        return;
    }
    if (graphs_.paused() && !held_) {
        // A graph broke: the whole simulation stops here, mid-tick, until the debugger lets it go.
        held_ = true;
        resume_on_release_ = play_->state() == gameplay::PlayState::Playing;
        if (resume_on_release_) {
            (void)play_->pause();
        }
        if (hold_listener_ != nullptr) {
            hold_listener_(hold_user_, true);
        }
        return;
    }
    if (!graphs_.paused() && held_) {
        held_ = false;
        if (resume_on_release_ && play_->state() == gameplay::PlayState::Paused) {
            (void)play_->resume();
        }
        resume_on_release_ = false;
        if (hold_listener_ != nullptr) {
            hold_listener_(hold_user_, false);
        }
    }
}

const game_backend::GraphBehaviours* GraphRuntime::behaviours() const noexcept {
    return play_ != nullptr ? &graphs_ : nullptr;
}

ecs::Entity GraphRuntime::entity_for(u64 identity) const noexcept {
    return play_ != nullptr ? play_->entity_for(identity) : ecs::Entity{};
}

u64 GraphRuntime::identity_of(ecs::Entity entity) const noexcept {
    if (play_ == nullptr || authored_ == nullptr) {
        return 0;
    }
    for (const ser::WorldNode& node : authored_->nodes()) {
        if (node.live && play_->entity_for(node.identity) == entity) {
            return node.identity;
        }
    }
    return 0;
}

Expected<u32, Error> GraphRuntime::raise(ecs::Entity entity, Name event,
                                         Span<const f32> arguments) noexcept {
    if (play_ == nullptr) {
        return fail(ErrorCode::Unavailable, "graphs run only during Play");
    }
    Expected<u32, Error> started = graphs_.raise(entity, event, arguments);
    // A handler the raise started may have broken: that holds the simulation exactly as a break
    // inside a tick does.
    sync_hold();
    return started;
}

Status GraphRuntime::set_breakpoint(Name graph, u64 node, ecs::Entity entity,
                                    bool enabled) noexcept {
    if (play_ == nullptr) {
        return fail(ErrorCode::Unavailable, "graphs run only during Play");
    }
    if (!graphs_.debugging()) {
        return fail(ErrorCode::Unsupported, "the graph debugger is compiled out of this build");
    }
    return graphs_.set_breakpoint(graph, node, entity, enabled);
}

Status GraphRuntime::debug(editor::ScriptDebugAction action) noexcept {
    if (play_ == nullptr) {
        return fail(ErrorCode::Unavailable, "graphs run only during Play");
    }
    if (!graphs_.debugging()) {
        return fail(ErrorCode::Unsupported, "the graph debugger is compiled out of this build");
    }
    Status done = ok();
    switch (action) {
        case editor::ScriptDebugAction::Pause:
            done = graphs_.debug_pause();
            break;
        case editor::ScriptDebugAction::Continue:
            done = graphs_.debug_continue();
            break;
        case editor::ScriptDebugAction::StepInto:
            done = graphs_.debug_step(game_backend::GraphStep::Into);
            break;
        case editor::ScriptDebugAction::StepOver:
            done = graphs_.debug_step(game_backend::GraphStep::Over);
            break;
    }
    sync_hold();
    return done;
}

Expected<u32, Error> GraphRuntime::reload(std::string_view reference, std::string_view source,
                                          graph::DiagnosticSink& sink) noexcept {
    if (play_ == nullptr) {
        return fail(ErrorCode::Unavailable, "graphs run only during Play");
    }
    const std::string wanted(reference);
    for (const std::string& loaded : loaded_) {
        if (loaded == wanted) {
            const std::string stem = std::filesystem::path(loaded).stem().string();
            return graphs_.reload(Name::intern(stem), source, sink);
        }
    }
    return fail(ErrorCode::NotFound, "Play does not run that graph; attach it and enter Play");
}

}  // namespace cy::sample::editor_window
