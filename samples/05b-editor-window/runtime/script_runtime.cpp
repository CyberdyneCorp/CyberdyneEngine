// SPDX-License-Identifier: MIT
#include "script_runtime.h"

#include <cy/scene/node.h>

#include <bit>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <vector>

namespace cy::sample::editor_window {
namespace {

namespace ser = scene::serialization;

struct ScriptedNode {
    u64 identity;
    std::string type;
    const ser::WorldNode* authored;
};

void append_word(std::vector<u8>& bytes, u32 value) {
    for (u32 shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}

void append_integer(std::vector<u8>& bytes, u64 value) {
    for (u32 shift = 0; shift < 64; shift += 8) {
        bytes.push_back(static_cast<u8>(value >> shift));
    }
}

bool append_export(std::vector<u8>& bytes, const ser::World& world,
                   const ser::WorldFieldDecl& field, const ser::WorldValue& value) {
    const std::string_view name = world.text(field.name);
    if (name == "class" || name.empty() || name.size() > UINT32_MAX) {
        return false;
    }
    u32 kind = CY_VAR_NIL;
    std::vector<u8> payload;
    switch (value.kind) {
        case ser::WorldValueKind::Bool:
            kind = CY_VAR_BOOL;
            payload.push_back(value.integer != 0 ? 1 : 0);
            break;
        case ser::WorldValueKind::Int:
            kind = CY_VAR_I64;
            append_integer(payload, static_cast<u64>(value.integer));
            break;
        case ser::WorldValueKind::Float:
            kind = CY_VAR_F32;
            append_word(payload, std::bit_cast<u32>(value.lanes[0]));
            break;
        case ser::WorldValueKind::Double:
            kind = CY_VAR_F64;
            append_integer(payload, std::bit_cast<u64>(value.real));
            break;
        case ser::WorldValueKind::Text: {
            kind = CY_VAR_STRING;
            const Span<const u8> text = world.blob(value);
            append_word(payload, static_cast<u32>(text.size()));
            payload.insert(payload.end(), text.begin(), text.end());
            break;
        }
        default:
            return false;
    }
    append_word(bytes, static_cast<u32>(name.size()));
    bytes.insert(bytes.end(), name.begin(), name.end());
    append_word(bytes, kind);
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return true;
}

std::vector<u8> authored_exports(const ser::World& world, const ser::WorldNode& node, u32 schema) {
    std::vector<u8> bytes;
    append_word(bytes, 0x54535943);  // CYST, the Swift state-blob magic.
    append_word(bytes, schema);
    append_word(bytes, 0);
    u32 count = 0;
    for (const ser::WorldTypeDecl& type : world.types()) {
        if (world.text(type.name) != "ScriptBehaviour") {
            continue;
        }
        const ser::WorldComponent* component = node.find(type.file_type);
        if (component == nullptr) {
            break;
        }
        for (const ser::WorldFieldDecl& field : type.fields()) {
            const ser::WorldField* held = component->find(field.file_field);
            if (held != nullptr && append_export(bytes, world, field, held->value)) {
                ++count;
            }
        }
        break;
    }
    for (u32 index = 0; index < 4; ++index) {
        bytes[8 + index] = static_cast<u8>(count >> (index * 8));
    }
    return count == 0 ? std::vector<u8>{} : bytes;
}

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
            nodes.push_back(ScriptedNode{node.identity, std::move(type), &node});
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
        } else {
            const abi::BehaviourInstance* instance = runtime_->instance(*created);
            if (instance == nullptr || instance->record == nullptr) {
                stop();
                return fail(ErrorCode::Internal, "the created script instance was lost");
            }
            const CyBehaviourVTable& callbacks = instance->record->vtable;
            const std::vector<u8> exports =
                authored_exports(authored, *node.authored, callbacks.schema_version);
            if (!exports.empty() && callbacks.deserialize != nullptr &&
                callbacks.deserialize(instance->instance, exports.data(),
                                      static_cast<u32>(exports.size()), callbacks.schema_version,
                                      callbacks.user_data) != CY_RESULT_OK) {
                stop();
                return fail(ErrorCode::InvalidArgument,
                            "the authored ScriptBehaviour exports could not be applied");
            }
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
