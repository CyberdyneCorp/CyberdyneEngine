// SPDX-License-Identifier: MIT
#include "scene_vfx_runtime.h"

#if defined(CY_EDITOR_WINDOW_HAS_VFX)

#    include <cy/vfx/authoring.h>
#    include <cy/vfx/catalogue.h>
#    include <cy/vfx/compile.h>
#    include <cy/vfx/interfaces.h>

#    include <algorithm>
#    include <filesystem>
#    include <fstream>
#    include <sstream>
#    include <utility>
#    include <vector>

namespace cy::sample::editor_window {

SceneVfxRuntime::SceneVfxRuntime(Allocator& allocator, std::string project) noexcept
    : allocator_(&allocator),
      project_(std::move(project)),
      simulation_(allocator),
      effects_(allocator) {}

Status SceneVfxRuntime::initialize() noexcept {
    vfx::WorldDescription description;
    description.pool_bytes = 64ULL * 1024ULL * 1024ULL;
    description.max_instances = 512;
    if (Status initialized = simulation_.initialize(description); !initialized) {
        return initialized;
    }
    ready_ = true;
    return ok();
}

Expected<std::string, Error> SceneVfxRuntime::read(std::string_view reference) const noexcept {
    const std::filesystem::path relative(reference);
    if (reference.empty() || relative.is_absolute() ||
        !std::ranges::all_of(
            relative,
            [](const std::filesystem::path& part) { return part != "." && part != ".."; }) ||
        reference.find_first_of("\\:") != std::string_view::npos) {
        return fail(ErrorCode::InvalidArgument,
                    "scene VFX asset path must stay inside the project");
    }
    const std::filesystem::path path = std::filesystem::path(project_) / relative;
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        return fail(ErrorCode::NotFound, "scene VFX asset could not be opened");
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (input.bad() || buffer.str().empty()) {
        return fail(ErrorCode::Io, "scene VFX asset could not be read");
    }
    return buffer.str();
}

Expected<const vfx::CompiledSystem*, Error> SceneVfxRuntime::resolve(std::string_view path,
                                                                     void* context) noexcept {
    return static_cast<SceneVfxRuntime*>(context)->system(path);
}

Expected<const vfx::CompiledSystem*, Error> SceneVfxRuntime::system(
    std::string_view path) noexcept {
    for (const System& existing : systems_) {
        if (existing.path == path) {
            return &existing.compiled;
        }
    }
    if (!path.ends_with(".cyvfxdoc")) {
        return fail(ErrorCode::InvalidArgument, "scene effect needs a .cyvfxdoc asset");
    }
    auto source = read(path);
    if (!source) {
        return make_unexpected(source.error());
    }
    auto asset = vfx::read_authoring_document(*source, *allocator_);
    if (!asset) {
        return make_unexpected(asset.error());
    }
    std::vector<std::pair<Name, std::string>> modules;
    modules.reserve(asset->module_assets().size());
    for (const vfx::ModuleAssetRef& mapped : asset->module_assets()) {
        auto text = read(mapped.path.text());
        if (!text) {
            return make_unexpected(text.error());
        }
        modules.emplace_back(mapped.name, std::move(*text));
    }
    std::vector<vfx::ModuleSource> sources;
    sources.reserve(modules.size());
    for (const auto& [name, text] : modules) {
        sources.push_back({name, text});
    }
    graph::DiagnosticSink diagnostics(*allocator_);
    vfx::CompileReport report(*allocator_);
    if (Status resolved = vfx::resolve_authoring_modules(*asset, {sources.data(), sources.size()},
                                                         diagnostics, report, *allocator_);
        !resolved) {
        return make_unexpected(resolved.error());
    }
    graph::NodeRegistry registry(*allocator_);
    vfx::DataInterfaceRegistry interfaces(*allocator_);
    if (Status registered = vfx::register_vfx_nodes(registry); !registered) {
        return make_unexpected(registered.error());
    }
    if (Status registered = vfx::register_builtin_interfaces(interfaces); !registered) {
        return make_unexpected(registered.error());
    }
    asset->resolve(registry);
    auto compiled = vfx::compile_system(*asset, registry, interfaces, vfx::CompileOptions{},
                                        diagnostics, report);
    if (!compiled) {
        return make_unexpected(compiled.error());
    }
    systems_.push_back({std::string(path), std::move(*compiled)});
    return &systems_.back().compiled;
}

Status SceneVfxRuntime::load(const scene::serialization::World& scene) noexcept {
    if (!ready_) {
        return fail(ErrorCode::Unavailable, "scene VFX runtime was not initialized");
    }
    if (effects_.same_instances(scene)) {
        return effects_.refresh(scene, simulation_);
    }
    effects_.clear(simulation_);
    systems_.clear();
    return effects_.load(scene, simulation_, &SceneVfxRuntime::resolve, this);
}

Status SceneVfxRuntime::step(const scene::serialization::World& scene, f32 seconds) noexcept {
    if (effects_.size() == 0) {
        return ok();
    }
    if (Status moved = effects_.update_transforms(scene, simulation_); !moved) {
        return moved;
    }
    vfx::StepReport report;
    return simulation_.step(seconds, report);
}

Status SceneVfxRuntime::set_parameter(u64 node, Name emitter, Name parameter,
                                      Span<const f32> value) noexcept {
    const vfx::EffectHandle effect = effects_.find(node);
    if (effect == vfx::kInvalidEffect) {
        return fail(ErrorCode::NotFound, "scene VFX entity has no playing effect");
    }
    return simulation_.set_parameter(effect, emitter, parameter, value);
}

Expected<vfx::EffectParameterValue, Error> SceneVfxRuntime::get_parameter(
    u64 node, Name emitter, Name parameter) const noexcept {
    const vfx::EffectHandle effect = effects_.find(node);
    if (effect == vfx::kInvalidEffect) {
        return fail(ErrorCode::NotFound, "scene VFX entity has no playing effect");
    }
    return simulation_.get_parameter(effect, emitter, parameter);
}

}  // namespace cy::sample::editor_window

#endif
