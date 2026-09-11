#pragma once
// The camera bridge: a camera batch becomes stack entries, blends and cuts. M8.c task 3.2, and the
// milestone's exit criterion "A sequence drives cameras through the camera stack and does not write
// camera transforms."
//
// ================================================================================================
// WHAT THIS FILE IS ALLOWED TO CALL, AND WHAT IT IS NOT
// ================================================================================================
//
// It calls exactly four things on the camera system:
//
//   CameraStack::push()      a shot becomes a contribution with a priority, a weight and a blend
//   CameraStack::release()   a shot ends by blending out, not by disappearing
//   CameraServer::cut()      a camera-cut track raises a cut, anticipated where it has a pre-roll
//   CameraServer::set_target()   a shot parameterises what its rig FRAMES
//
// It does not call `override_pose()`. It cannot: `CameraRequest` has no pose in it (dispatch.h says
// why), so there is nothing here to write one from. `camera-system` allows direct pose control
// "only to low-level debug and custom node code", and a cinematic is neither.
//
// The consequence is worth stating because it looks like a limitation until it is understood: A
// SHOT CHANGES LENS BY SELECTING A RIG. `cy::camera` has no per-rig lens setter — a lens is the
// `Lens` node of a rig's compiled definition — so a wide shot and a long-lens shot are two rigs,
// and cutting between them is the stack blending two evaluated cameras whose lenses differ. That is
// `camera-system`'s own model ("lens blending SHALL respect the lens model in use") rather than a
// workaround, and it is why the blend a reader sees in this milestone's capture moves the field of
// view as well as the position.
//
// ================================================================================================
// THE BRIDGE OWNS NO CAMERA STATE
// ================================================================================================
//
// It keeps one thing: which stack entry a binding's shot currently occupies, so that a section
// ending can release the entry it pushed. Everything else — the blend's progress, the weights, the
// evaluated pose — belongs to `CameraStack`, is computed there, and is read back through
// `CameraStack::blend()`'s contribution report. A second copy here would be a second thing that can
// disagree with the camera.

#include <cy/core/base/expected.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/sequencing/dispatch.h>
#include <cy/servers/camera/server.h>

namespace cy::sequencing {

/// What one application of a batch did. Every field is a count of a call that was actually made,
/// which is what makes "the cut ran through the camera stack" checkable rather than assertable.
struct CameraBridgeReport {
    u32 pushed = 0;
    u32 released = 0;
    u32 cuts = 0;
    u32 anticipated_cuts = 0;
    u32 framing_targets_set = 0;
    /// A shot whose rig identity has not been bound to a real rig. Counted rather than ignored: it
    /// is the ordinary symptom of a binding that resolved to the wrong thing.
    u32 unresolved_rigs = 0;
    /// A framing target the rig refused — `set_target()` refuses a rig with no `Target` node. Named
    /// so that "the camera did not move to the subject" has an answer.
    u32 framing_refused = 0;
};

/// One shot's occupancy of a camera stack.
struct BridgeEntry {
    u32 binding = 0;
    u64 rig_identity = 0;
    cy::camera::RigHandle rig;
    cy::camera::StackEntryId stack_entry = cy::camera::kInvalidStackEntry;
    /// The subject this shot last asked its rig to frame. Kept so the target is set when it
    /// CHANGES rather than on every frame: `set_target()` resets nothing, but a call per frame per
    /// shot is a call whose count says nothing, and `framing_targets_set` is meant to be readable.
    u64 framing_target = 0;
};

class CameraStackBridge {
public:
    CameraStackBridge(Allocator& allocator, cy::camera::CameraServer& server) noexcept;

    /// The stack this bridge drives. One bridge per stack, because a stack is one local player's.
    [[nodiscard]] Status initialize(cy::camera::StackHandle stack) noexcept;

    /// Bind a rig identity — whatever the host resolved a camera binding to — to a real rig.
    /// The sequence never sees a `RigHandle`: a binding is a stable identifier, and "Bindings SHALL
    /// NEVER be raw pointers or transient runtime indices."
    [[nodiscard]] Status bind_rig(u64 identity, cy::camera::RigHandle rig) noexcept;

    /// Apply one frame's arbitrated camera requests.
    [[nodiscard]] Status apply(Span<const CameraRequest> requests,
                               CameraBridgeReport& report) noexcept;

    [[nodiscard]] Span<const BridgeEntry> entries() const noexcept { return entries_.span(); }
    [[nodiscard]] cy::camera::StackHandle stack() const noexcept { return stack_; }

private:
    struct RigBinding {
        u64 identity = 0;
        cy::camera::RigHandle rig;
    };

    [[nodiscard]] cy::camera::RigHandle rig_for(u64 identity) const noexcept;
    [[nodiscard]] BridgeEntry* entry_for(u32 binding) noexcept;
    [[nodiscard]] Status push_shot(const CameraRequest& request, cy::camera::RigHandle rig,
                                   CameraBridgeReport& report) noexcept;
    [[nodiscard]] Status release_shot(const CameraRequest& request,
                                      CameraBridgeReport& report) noexcept;

    cy::camera::CameraServer* server_;
    cy::camera::StackHandle stack_;
    Array<RigBinding> rigs_;
    Array<BridgeEntry> entries_;
};

/// The camera stack's blend curve for a `CameraBlend::curve` number. Out of range becomes
/// `EaseInOut`, which is the stack's own default.
[[nodiscard]] cy::camera::BlendCurve blend_curve_of(u8 curve) noexcept;

}  // namespace cy::sequencing
