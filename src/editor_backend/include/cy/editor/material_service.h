// SPDX-License-Identifier: MIT
#pragma once

#include <cy/abi/host.h>
#include <cy/core/memory/allocator.h>
#include <cy/rendering/material/compiler.h>
#include <array>
#include <string_view>

namespace cy::vfx {
class SimulationWorld;
}

namespace cy::editor {

class TerrainPreview;
class AudioAuthoring;
class ScriptPlayRuntime;
class AnimationPreviewRuntime;

/// Every operation prefix `MaterialService` serves. A host that routes one binding over several
/// services (`CompositeEditorService`) routes each of these here, so an operation this service
/// gains is not lost to a routing table that forgot it.
inline constexpr std::array<std::string_view, 7> kMaterialServicePrefixes{
    "material.", "vfx.", "preview.", "terrain.", "audio.", "script.", "animation."};

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
    [[nodiscard]] static const vfx::SimulationWorld* vfx_preview_world(
        CyServiceSession session) noexcept;

    /// The terrain this session last evaluated for `terrain.evaluate`, or null before the first.
    [[nodiscard]] static const TerrainPreview* terrain_preview(CyServiceSession session) noexcept;

    /// Whether the terrain this session evaluated has navigation it flagged stale.
    [[nodiscard]] static bool terrain_navigation_stale(CyServiceSession session) noexcept;
    /// Navigation was rebaked: the terrain's stale regions are consumed. The one stale flag
    /// terrain edits raise is the one a navigation bake clears.
    static void terrain_navigation_rebaked(CyServiceSession session) noexcept;

    /// Serve the `audio.*` operations from the host's audio server (see audio_service.h). Without
    /// one they are refused with `audio.unavailable`. `audio` is borrowed.
    void set_audio(AudioAuthoring* audio) noexcept { audio_ = audio; }

    /// Serve `script.event.raise` and `script.state.get` from the host's Play (script_service.h).
    /// `script.catalogue.get` and `script.compile` need no host. `play` is borrowed; null refuses
    /// the two Play operations with `script.play.unavailable`.
    void set_scripts(ScriptPlayRuntime* play) noexcept { scripts_ = play; }

    /// Serve `animation.preview.*` from the host's preview character (animation_service.h).
    /// `animation.catalogue.get` and `animation.compile` need no host. `preview` is borrowed; null
    /// refuses the previews with `animation.preview.unavailable`.
    void set_animation(AnimationPreviewRuntime* preview) noexcept { animation_ = preview; }

private:
    Allocator* allocator_;
    MaterialPreviewRuntime* preview_runtime_;
    MaterialAuthoringRuntime* authoring_runtime_;
    AudioAuthoring* audio_ = nullptr;
    ScriptPlayRuntime* scripts_ = nullptr;
    AnimationPreviewRuntime* animation_ = nullptr;
};

}  // namespace cy::editor
