#include "script_runtime.h"

#include <cy/scene/node.h>

#include <cstdlib>
#include <filesystem>
#include <string_view>

namespace cy::sample::editor_window {
namespace {

namespace ser = scene::serialization;

struct ScriptedNode {
    u64 identity;
    std::string type;
};

std::string script_type(const ser::World& world, const ser::WorldNode& node) {
    for (const ser::WorldTypeDecl& declared : world.types()) {
        if (world.text(declared.name) != "ScriptBehaviour") {
            continue;
        }
        const ser::WorldComponent* component = node.find(declared.file_type);
        if (component == nullptr) {
            return {};
        }
        for (const ser::WorldFieldDecl& field : declared.fields()) {
            if (world.text(field.name) != "class") {
                continue;
            }
            const ser::WorldField* held = component->find(field.file_field);
            if (held == nullptr || held->value.kind != ser::WorldValueKind::Text) {
                return {};
            }
            const Span<const u8> bytes = world.blob(held->value);
            return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
        }
    }
    return {};
}

std::string latest_module(const std::string& project) {
    const std::filesystem::path directory =
        std::filesystem::path(project) / "build/script-module/out";
    std::error_code error;
    std::filesystem::directory_iterator files(directory, error);
    if (error) {
        return {};
    }
    constexpr std::string_view extension = ".so";
    u32 latest = 0;
    std::string chosen;
    for (const std::filesystem::directory_iterator end; files != end && !error;
         files.increment(error)) {
        const std::filesystem::directory_entry& file = *files;
        const std::string name = file.path().filename().string();
        constexpr std::string_view prefix = "libCyGame_g";
        if (!name.starts_with(prefix) || !name.ends_with(extension)) {
            continue;
        }
        const std::string digits =
            name.substr(prefix.size(), name.size() - prefix.size() - extension.size());
        if (digits.empty() || digits.find_first_not_of("0123456789") != std::string::npos) {
            continue;
        }
        const u32 generation = static_cast<u32>(std::strtoul(digits.c_str(), nullptr, 10));
        if (chosen.empty() || generation > latest) {
            latest = generation;
            chosen = file.path().string();
        }
    }
    return chosen;
}

}  // namespace

ScriptRuntime::ScriptRuntime(Allocator& allocator, const char* project,
                             const char* module_path) noexcept
    : allocator_(&allocator),
      project_(project == nullptr ? "" : project),
      module_path_(module_path == nullptr ? "" : module_path),
      host_(allocator) {}

Status ScriptRuntime::start(gameplay::PlaySession& play, const ser::World& authored) noexcept {
    stop();
    std::vector<ScriptedNode> nodes;
    for (const ser::WorldNode& node : authored.nodes()) {
        if (!node.live) {
            continue;
        }
        std::string type = script_type(authored, node);
        if (!type.empty()) {
            nodes.push_back(ScriptedNode{node.identity, std::move(type)});
        }
    }
    if (nodes.empty()) {
        return ok();
    }
    const std::string library = module_path_.empty() ? latest_module(project_) : module_path_;
    if (library.empty()) {
        return fail(ErrorCode::Unavailable,
                    "scene scripts need a built Swift module; run project.build first");
    }
    Expected<UniquePtr<abi::World>, Error> binding =
        make_unique<abi::World>(*allocator_, *allocator_, *play.world());
    if (!binding) {
        return make_unexpected(binding.error());
    }
    binding_ = std::move(*binding);
    host_.bind_world(binding_.get());
    Expected<UniquePtr<abi::BehaviourRuntime>, Error> runtime =
        make_unique<abi::BehaviourRuntime>(*allocator_, *allocator_, host_);
    if (!runtime) {
        stop();
        return make_unexpected(runtime.error());
    }
    runtime_ = std::move(*runtime);
    manifest_.name = "editor-project";
    manifest_.entry_symbol = "cy_module_entry";
    manifest_.min_abi_major = CY_ABI_MAJOR;
    manifest_.min_abi_minor = 0;
    manifest_.hot_reload = true;
    if (Expected<abi::ReloadReport, Error> loaded = runtime_->load(manifest_, library.c_str());
        !loaded) {
        stop();
        return make_unexpected(loaded.error());
    }
    active_library_ = library;
    for (const ScriptedNode& node : nodes) {
        const ecs::Entity entity = play.entity_for(node.identity);
        if (!entity.valid()) {
            stop();
            return fail(ErrorCode::NotFound, "scripted scene node has no Play entity");
        }
        if (Expected<u32, Error> created = runtime_->create(node.type.c_str(), entity.bits());
            !created) {
            stop();
            return make_unexpected(created.error());
        }
        identities_.push_back(node.identity);
    }
    return ok();
}

void ScriptRuntime::stop() noexcept {
    identities_.clear();
    active_library_.clear();
    runtime_.reset();
    host_.bind_world(nullptr);
    binding_.reset();
}

Expected<abi::ReloadReport, Error> ScriptRuntime::reload(const char* library) noexcept {
    if (!runtime_) {
        return fail(ErrorCode::Unavailable, "start Play before reloading a Swift module");
    }
    if (active_library_ == library) {
        return fail(ErrorCode::InvalidArgument, "this Swift module generation is already active");
    }
    Expected<abi::ReloadReport, Error> report = runtime_->reload(library);
    if (report && report->failure == abi::ReloadFailure::None) {
        active_library_ = library;
    }
    return report;
}

Status ScriptRuntime::tick(gameplay::PlaySession& play, f32 dt) noexcept {
    if (!runtime_) {
        return ok();
    }
    runtime_->fixed_update(dt);
    for (const u64 identity : identities_) {
        const ecs::Entity entity = play.entity_for(identity);
        if (Status changed = play.tree()->node(entity).mark_transform_changed(); !changed) {
            return changed;
        }
    }
    return ok();
}

}  // namespace cy::sample::editor_window
