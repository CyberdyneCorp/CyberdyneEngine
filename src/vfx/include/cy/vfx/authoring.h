// SPDX-License-Identifier: MIT
#pragma once

#include <cy/vfx/asset.h>

#include <string_view>
#include <utility>

namespace cy::vfx {

/// A typed attribute the host emitter must provide to a reusable module.
struct ModuleInputDecl {
    Name name;
    Name type;
};

/// Separately saved editable stage graph, prior to dependency resolution and cooking.
struct VfxModuleAsset {
    VfxModuleAsset(Allocator& allocator, Name module_name, Stage module_stage,
                   Graph&& module_graph) noexcept
        : name(module_name),
          stage(module_stage),
          inputs(allocator),
          dependencies(allocator),
          graph(std::move(module_graph)) {}

    Name name;
    Stage stage = Stage::Count;
    Array<ModuleInputDecl> inputs;
    Array<Name> dependencies;
    Graph graph;
};

/// Read an editor .cyvfxdoc draft into the engine asset model. Both payload versions are accepted;
/// graph validation and compilation remain the compiler's responsibility.
[[nodiscard]] Expected<VfxSystemAsset, Error> read_authoring_document(
    std::string_view source, Allocator& allocator) noexcept;

/// Read one editor `.cyvfxmodule` source. Resolution and composition into an emitter happen later.
[[nodiscard]] Expected<VfxModuleAsset, Error> read_authoring_module(std::string_view source,
                                                                    Allocator& allocator) noexcept;

}  // namespace cy::vfx
