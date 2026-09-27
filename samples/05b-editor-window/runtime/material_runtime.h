// SPDX-License-Identifier: MIT
#pragma once

#include <cy/backends/shader/compiler.h>
#include <cy/core/memory/array.h>
#include <cy/editor/material_service.h>

#include "renderer.h"
#include "world_view.h"

namespace cy::sample::editor_window {

/// Assemble the engine-compiled surface and vertex sources into the hosted material program.
[[nodiscard]] Status assemble_material_unit(const rendering::material::CompiledProgram& program,
                                            Array<char>& unit) noexcept;

/// Compile the graph's vertex expression against the authored scene frame's actual streams,
/// transforms and descriptor convention. The returned MSL is retained for scene pipeline creation.
struct SceneMaterialVertexArtefacts {
    explicit SceneMaterialVertexArtefacts(Allocator& allocator) noexcept
        : visible(allocator), shadow(allocator) {}

    shader::TargetArtefact visible;
    shader::TargetArtefact shadow;
};

[[nodiscard]] Status assemble_scene_material_vertex_unit(
    const rendering::material::CompiledProgram& program, Array<char>& unit) noexcept;
[[nodiscard]] Expected<SceneMaterialVertexArtefacts, Error> compile_scene_material_vertices(
    const rendering::material::CompiledProgram& program, Allocator& allocator) noexcept;

/// The Mac editor-preview adapter. It owns the retained compiler layouts and preview bindings;
/// `first_light::Renderer` owns the Metal shader modules, pipelines, descriptor sets and buffers.
class MetalMaterialRuntime final : public editor::MaterialPreviewRuntime {
public:
    MetalMaterialRuntime(Allocator& allocator, first_light::Renderer& renderer,
                         WorldView& world) noexcept;
    ~MetalMaterialRuntime() override;

    [[nodiscard]] Status publish(
        u64 artefact, const rendering::material::CompiledMaterial& material) noexcept override;
    [[nodiscard]] Status create(u64 preview) noexcept override;
    [[nodiscard]] Status reload(
        u64 preview, u64 artefact,
        Span<const editor::MaterialPreviewTarget> targets) noexcept override;
    [[nodiscard]] Status update(u64 preview, u64 artefact,
                                const editor::MaterialParameterUpdate& parameter) noexcept override;
    [[nodiscard]] Status destroy(u64 preview) noexcept override;

private:
    struct Program;
    struct Preview;

    [[nodiscard]] Program* find_program(u64 artefact) noexcept;
    [[nodiscard]] Preview* find_preview(u64 preview) noexcept;

    Allocator* allocator_;
    first_light::Renderer* renderer_;
    WorldView* world_;
    Array<Program> programs_;
    Array<Preview> previews_;
};

}  // namespace cy::sample::editor_window
