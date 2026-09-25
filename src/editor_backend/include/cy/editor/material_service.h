// SPDX-License-Identifier: MIT
#pragma once

#include <cy/abi/host.h>
#include <cy/core/memory/allocator.h>
#include <cy/rendering/material/compiler.h>
#include <string_view>

namespace cy::vfx {
class SimulationWorld;
}

namespace cy::editor {

/// One exact renderer binding named by a preview reload request.
struct MaterialPreviewTarget {
    u8 entity[16] = {};
    u32 material_slot = 0;
};

/// A typed parameter update after the wire envelope has been validated.
struct MaterialParameterUpdate {
    u32 identity = 0;
    u8 kind = 0;
    Span<const u8> value;
};

/// Runtime-owned application seam for compiled editor materials.
///
/// The editor backend owns schemas and request correlation. The viewport host owns GPU programs,
/// renderer bindings and preview-world resources. An acknowledgement is emitted only after this
/// interface accepts the corresponding operation.
class MaterialPreviewRuntime {
public:
    virtual ~MaterialPreviewRuntime() = default;

    [[nodiscard]] virtual Status publish(
        u64 artefact, const rendering::material::CompiledMaterial& material) noexcept = 0;
    [[nodiscard]] virtual Status create(u64 preview) noexcept = 0;
    [[nodiscard]] virtual Status reload(u64 preview, u64 artefact,
                                        Span<const MaterialPreviewTarget> targets) noexcept = 0;
    [[nodiscard]] virtual Status update(u64 preview, u64 artefact,
                                        const MaterialParameterUpdate& parameter) noexcept = 0;
    [[nodiscard]] virtual Status destroy(u64 preview) noexcept = 0;
};

/// Applies an unsaved, validated graph to the authored scene owned by the host.
class MaterialAuthoringRuntime {
public:
    virtual ~MaterialAuthoringRuntime() = default;
    [[nodiscard]] virtual Status preview(std::string_view reference,
                                         std::string_view canonical_graph) noexcept = 0;
};

/// Engine-owned material authoring backend. Both C ABI and live protocol adapters submit the same
/// operation names and payload schemas to this object.
class MaterialService final : public abi::EditorServiceBackend {
public:
    explicit MaterialService(Allocator& allocator,
                             MaterialPreviewRuntime* preview_runtime = nullptr,
                             MaterialAuthoringRuntime* authoring_runtime = nullptr) noexcept
        : allocator_(&allocator),
          preview_runtime_(preview_runtime),
          authoring_runtime_(authoring_runtime) {}

    [[nodiscard]] CyResult open(CyServiceSession* out_session) noexcept override;
    void close(CyServiceSession session) noexcept override;
    [[nodiscard]] CyResult submit(CyServiceSession session,
                                  const CyServiceRequest& request) noexcept override;
    [[nodiscard]] CyResult cancel(CyServiceSession session, u64 request_id) noexcept override;
    [[nodiscard]] CyResult poll(CyServiceSession session, CyServiceEvent& out_event,
                                bool& out_has_event) noexcept override;

    /// The effect currently simulated by this session, for the host's engine frame renderer.
    [[nodiscard]] const vfx::SimulationWorld* vfx_preview_world(
        CyServiceSession session) const noexcept;

private:
    Allocator* allocator_;
    MaterialPreviewRuntime* preview_runtime_;
    MaterialAuthoringRuntime* authoring_runtime_;
};

}  // namespace cy::editor
