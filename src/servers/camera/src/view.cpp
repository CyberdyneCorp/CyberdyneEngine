// Render view production. See cy/servers/camera/view.h.

#include <cy/servers/camera/view.h>

namespace cy::camera {
namespace {

/// A 64-bit mix. Three multiply-xorshift rounds of the splitmix64 finaliser, which spreads a small
/// change in any input across the whole output — so a view key of 0 and 1 produce identities that
/// are not adjacent, and a cut epoch that increments by one does not produce a history identity a
/// consumer's own hashing might collide with the previous frame's.
[[nodiscard]] constexpr u64 mix(u64 value) noexcept {
    u64 result = value;
    result ^= result >> 30U;
    result *= 0xBF58476D1CE4E5B9ULL;
    result ^= result >> 27U;
    result *= 0x94D049BB133111EBULL;
    result ^= result >> 31U;
    return result;
}

}  // namespace

u64 history_identity(const EvaluatedCamera& camera, u32 view_key) noexcept {
    // Three inputs, and each one is there for a stated reason:
    //   history_id   the camera's own identity, so two cameras never share a history;
    //   view_key     the view within that camera, so a weapon view and the main view do not share;
    //   cut_epoch    bumped by every cut, so a cut invalidates the history of every view of it.
    // The TARGET is deliberately not an input: "changing a camera's target SHALL NOT automatically
    // be a cut", so history must survive it.
    return mix(camera.history_id ^
               mix((static_cast<u64>(view_key) << 32U) | static_cast<u64>(camera.cut_epoch)));
}

Status produce_view(const EvaluatedCamera& camera, const RenderViewRequest& request,
                    render::ViewDescription& out) noexcept {
    if (request.viewport.width == 0 || request.viewport.height == 0) {
        return fail(ErrorCode::InvalidArgument,
                    "a render view's viewport has zero width or height, so it has no aspect and "
                    "nothing can be drawn into it");
    }

    const Lens& lens = request.override_lens ? request.lens : camera.lens;

    out = render::ViewDescription{};
    out.name = request.name;
    out.scene = request.scene;
    out.purpose = request.purpose;
    out.camera = camera.pose;
    out.projection = lens.to_projection();
    out.viewport = request.viewport;
    out.layer_mask = request.layer_mask;
    out.history_id = history_identity(camera, request.view_key);
    out.family = request.family;
    out.importance = request.importance;
    out.post_process = request.post_process;
    out.override_environment = request.override_environment;
    out.environment = request.environment;
    out.debug_mode = request.debug_mode;
    out.target = request.target;

    // NO JITTER, NO MATRIX. `camera-system`: "the camera SHALL provide an unjittered projection and
    // the temporal framework SHALL apply jitter". `render::View::refresh()` builds the matrices
    // from the semantic projection above, and there is nothing in this function that could apply an
    // offset even if a caller asked for one.
    return ok();
}

}  // namespace cy::camera
