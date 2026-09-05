#pragma once
// Render view production: one evaluated camera to one or more `cy::render::ViewDescription`.
// Task 4.3.3.
//
// `camera-system` — "Render view production": each evaluated camera "SHALL produce one or more
// **render views** carrying: pose, projection semantics, viewport rectangle, target, layer mask,
// quality settings, **importance**, and a **history identity**"; views "SHALL be organisable into
// families that share work"; "Temporal jitter SHALL be applied to the view by the temporal
// framework, and the camera SHALL provide an unjittered projection"; and a view's history identity
// "SHALL be stable across target changes and shall be invalidated on cuts".
//
// ================================================================================================
// WHY THIS FILE IS A FUNCTION AND NOT A TYPE
// ================================================================================================
//
// `cy::render::ViewDescription` already exists and is already exactly the shape the requirement
// lists — M3 built it against this requirement, and its own header says so: "Views SHALL be
// produced from evaluated cameras, which supply pose, projection semantics, viewport, importance,
// and history identity." Declaring a second view type here so the camera could own one would put
// two descriptions of a view in the tree, and the second one would drift. So the camera module
// depends on the render server — both are layer 2, so this is a sideways dependency the layer
// checker permits — and produces the renderer's own type.
//
// The direction is worth being explicit about: the CAMERA depends on the RENDER SERVER, not the
// other way round. `cy::render` names nothing from `cy::camera`, which is what keeps a dedicated
// server build able to link the renderer's model without a camera system, and what keeps the
// renderer usable by a test that has no camera at all.
//
// ================================================================================================
// ONE CAMERA, SEVERAL VIEWS — AND WHY EACH NEEDS ITS OWN KEY
// ================================================================================================
//
// "**WHEN** a first-person camera renders the world and a separate weapon view **THEN** both SHALL
// be render views derived from one evaluated camera." Those two views accumulate different temporal
// history, so they cannot share a history identity. `RenderViewRequest::view_key` distinguishes
// them, and `history_identity()` mixes it with the camera's own identity and its cut epoch:
//
//   * changing the camera's TARGET does not change any of the three, so history survives — the
//     requirement's "changing a camera's target SHALL NOT automatically be a cut";
//   * a CUT bumps `cut_epoch`, so every view derived from that camera gets a new identity and the
//     temporal framework drops its accumulation — the requirement's "A cut SHALL invalidate the
//     view's temporal history".
//
// One number does both halves. A separate "invalidate" flag would have to be delivered to every
// consumer and acted on by each of them, and the one that forgot would smear across the cut.
//
// ================================================================================================
// WHAT IS DELIBERATELY NOT PRODUCED HERE
// ================================================================================================
//
// No jitter offset, no matrix, no descriptor, no backend anything. `camera-system` forbids all four
// in a gameplay-facing camera interface, and `render::View::refresh()` is where the matrices are
// built from the semantic projection this file fills in.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/values/name.h>
#include <cy/servers/camera/camera.h>
#include <cy/servers/render/model.h>

namespace cy::camera {

/// What a caller asks of one view. Everything a view needs that the camera does not supply.
struct RenderViewRequest {
    Name name;
    render::SceneHandle scene;
    render::ViewPurpose purpose = render::ViewPurpose::Primary;
    render::ViewportRect viewport;
    render::LayerMask layer_mask = render::kAllLayers;
    /// `rendering-architecture` and `residency` both consume this: a minimap declares less
    /// importance than a main view and receives lower detail and residency priority.
    f32 importance = 1.0F;
    render::ViewFamilyId family = render::kNoViewFamily;
    /// Null means the swapchain.
    render::TextureHandle target;

    /// Distinguishes this view from the camera's others. Zero is the camera's primary view; a
    /// weapon view, a portal and a minimap each take a distinct non-zero key. See the header
    /// comment — this is what keeps their temporal histories apart.
    u32 view_key = 0;

    /// A view may override the camera's lens — a weapon view renders the same pose through a
    /// narrower field of view. It does NOT override the pose: that would be a second camera
    /// pretending to be a view.
    bool override_lens = false;
    Lens lens;

    render::PostProcessSettings post_process;
    bool override_environment = false;
    render::EnvironmentSettings environment;
    render::DebugViewMode debug_mode = render::DebugViewMode::Off;
};

/// The identity temporal history follows for one view of one camera. See the header comment.
[[nodiscard]] u64 history_identity(const EvaluatedCamera& camera, u32 view_key) noexcept;

/// Fill `out` from an evaluated camera and a request.
///
/// Fails when the viewport has zero width or height: the aspect a projection needs would be a
/// division by zero, and a view nothing can be drawn into is a configuration error rather than a
/// frame with an empty rectangle in it.
[[nodiscard]] Status produce_view(const EvaluatedCamera& camera, const RenderViewRequest& request,
                                  render::ViewDescription& out) noexcept;

}  // namespace cy::camera
