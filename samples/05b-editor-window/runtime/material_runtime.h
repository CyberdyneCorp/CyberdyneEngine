// SPDX-License-Identifier: MIT
#pragma once

#include <cy/core/memory/array.h>
#include <cy/editor/material_service.h>

#include "renderer.h"
#include "world_view.h"

namespace cy::sample::editor_window {

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
