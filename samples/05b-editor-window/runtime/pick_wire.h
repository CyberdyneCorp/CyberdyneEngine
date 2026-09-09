#pragma once
// The editor's pick messages, decoded and answered. M8.a task 1.4.
//
// ================================================================================================
// WHY THIS ENCODING IS RESTATED HERE
// ================================================================================================
//
// `cy_editor_viewport::picking::PickRequest::encode` and `PickResponse::decode` are the other end,
// and they live in the editor's Cargo workspace because — as that crate's README says — the
// viewport's messages are not in `cy_editor_protocol`'s enum, which is another crate's. There is no
// generated wire, so both ends are written out and both ends are tested; this file is the C++ half.
//
// It is in its own translation unit rather than in `main.cpp` so that a test can drive it without a
// Vulkan device. A decoder that has only ever been exercised by a running editor is a decoder whose
// off-by-one shows up as "picking does not work", which is exactly how M7's transaction decoder's
// first defect presented.
//
// ================================================================================================
// WHAT IT DOES NOT DECIDE
// ================================================================================================
//
// WHAT IS HIT. That is `cy::render::pick_ray`, `pick_rect` and `pick_polygon`, resolving against
// the draw list the frame produced. This file turns bytes into their arguments and their answer
// back into bytes, and it is deliberately incapable of inventing a candidate.
//
// WHICH CANDIDATE THE CLICK TAKES. Cycling through overlapping candidates is the editor's, because
// it is a question about the user's clicks rather than about the scene — `picking.h` says so and
// `cy_editor_viewport::picking::ClickCycle` is where it lives.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/servers/render/picking.h>

namespace cy::sample::editor_window {

/// What the pointer did. `cy_editor_viewport::picking::PickIntent`'s three variants.
enum class PickKind : u8 {
    Click = 0,
    Rectangle = 1,
    Lasso = 2,
};

/// One decoded pick.
class PickRequest {
public:
    explicit PickRequest(Allocator& allocator) noexcept : points(allocator), excluded(allocator) {}

    PickRequest(const PickRequest&) = delete;
    PickRequest& operator=(const PickRequest&) = delete;

    u64 viewport = 0;
    /// **The frame that was on screen when it was clicked**, which is what the answer must be
    /// resolved against rather than whatever the runtime has since rendered.
    u64 frame = 0;
    PickKind kind = PickKind::Click;
    /// A click, in the presented frame's pixels from its top-left corner.
    f32 x = 0.0F;
    f32 y = 0.0F;
    /// A marquee, in the same convention.
    f32 min_x = 0.0F;
    f32 min_y = 0.0F;
    f32 max_x = 0.0F;
    f32 max_y = 0.0F;
    /// A lasso.
    Array<Vec2> points;
    /// The engine layers the viewport draws.
    ///
    /// **DECODED AND NOT APPLIED**, deliberately. `cy::render::PickFilter` has no layer field
    /// because a pick resolves against the draw list, and the draw list is already what the view's
    /// layer mask let through — re-applying it here would be the second visibility rule
    /// `picking.h` spends a header explaining why there must not be. It is decoded because it is on
    /// the wire and a reader that skipped it would be reading the next field.
    u32 layers = 0xFFFF'FFFFU;
    /// What the editor will accept.
    bool include_transparent = true;
    u32 max_candidates = 0;
    /// Identities the editor will not select: locked objects, and anything isolation excludes.
    Array<u64> excluded;
};

/// Decode one. False for a truncated or unreadable request, which is refused rather than answered
/// with a guess — an invented hit is the forbidden pattern `editor-viewport-and-gizmos` names.
[[nodiscard]] bool decode_pick_request(Span<const u8> bytes, PickRequest& out) noexcept;

/// Resolve a decoded request against the frame's records.
///
/// One entry point for all three intents so that a caller cannot answer a marquee with a ray, and
/// so the three stay in step when a filter grows a field.
[[nodiscard]] Status resolve_pick(const PickRequest& request, const render::View& view,
                                  Span<const render::GpuInstance> instances,
                                  Span<const render::DrawItem> draws,
                                  Array<render::PickCandidate>& out) noexcept;

/// Encode the answer. Appends to `out`.
[[nodiscard]] Status encode_pick_response(u64 frame, Span<const render::PickCandidate> candidates,
                                          Array<u8>& out) noexcept;

}  // namespace cy::sample::editor_window
