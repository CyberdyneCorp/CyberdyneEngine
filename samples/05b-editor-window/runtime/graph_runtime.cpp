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
    return ok();
}

void GraphRuntime::stop() noexcept {
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
    return graphs_.update(dt);
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
    return graphs_.raise(entity, event, arguments);
}

}  // namespace cy::sample::editor_window
