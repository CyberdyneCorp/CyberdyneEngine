#ifndef CY_RENDERING_2D_SPRITE_H
#define CY_RENDERING_2D_SPRITE_H
// The 2D primitives, their sort key, and the batcher. M8.b task 9.5.
//
// `rendering-2d`: "2D rendering SHALL use the same RHI, render graph, shader system, and material
// model as 3D, but a distinct pipeline optimised for sorted, batched quad and mesh submission."
//
// --- WHAT THIS MODULE IS, AND WHERE THE DEVICE IS ------------------------------------------------
//
// Everything here is the CPU half: the primitives, the sort, the batch decision and the
// per-instance data. It names no device, no command buffer and no pipeline object — the submission
// is `src/rendering/`'s, the way `culling/` produces a visible set that `forward/` draws. That is
// what lets the whole of it be tested without a GPU, and it is the same division `rendering-2d`'s
// own first requirement draws between "the same RHI" and "a distinct pipeline".
//
// --- THE SORT KEY IS ONE NUMBER, AND ITS ORDER IS THE REQUIREMENT --------------------------------
//
// "2D draw order SHALL be determined by an explicit sort key composed of: layer, sort order within
// the layer, an optional Y-sort value, and a stable tiebreak." `SortKey::pack()` is those four, in
// that order, in one `u64` — so sorting is a single integer comparison and "WHEN two sprites have
// identical sort keys THEN the tiebreak SHALL be stable across frames so they do not flicker" is a
// property of the key rather than of the sort algorithm.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>

namespace cy::rendering2d {

/// A rectangle in texture or world space.
struct Rect2D {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 width = 0.0F;
    f32 height = 0.0F;
};

/// Which primitive a draw is. `rendering-2d` names seven and this is the seven.
enum class PrimitiveKind : u8 {
    Sprite = 0,
    NineSlice,
    TiledSprite,
    Line,
    Polygon,
    Mesh,
    AnimatedSprite,
    Count,
};

[[nodiscard]] const char* primitive_kind_name(PrimitiveKind kind) noexcept;

/// How a line's ends and corners are drawn.
enum class LineJoint : u8 { Bevel = 0, Round, Miter };
enum class LineCap : u8 { None = 0, Box, Round };

/// The sort key's parts. Packed into one `u64`, most significant first, so the sort is an integer
/// comparison and the order is the requirement's own list.
struct SortKey {
    /// The layer. Everything in layer 1 draws after everything in layer 0, whatever their orders.
    u16 layer = 0;
    /// The author's order within the layer.
    i16 order = 0;
    /// The world Y, quantised, for a Y-sorted layer. Ignored when the layer does not Y-sort.
    f32 y_sort = 0.0F;
    /// The stable tiebreak: the draw's own identity. Two sprites at the same position in the same
    /// layer keep their relative order between frames rather than swapping and flickering.
    u32 tiebreak = 0;

    /// Pack into a sortable integer. `y_sorted` decides whether the Y value takes part at all.
    [[nodiscard]] u64 pack(bool y_sorted) const noexcept;
};

/// One 2D draw, before batching.
struct Draw2D {
    PrimitiveKind kind = PrimitiveKind::Sprite;
    SortKey sort;
    /// The batch key's three parts: a pipeline, a material and a texture set. A change in any of
    /// them breaks a batch, and the report says which.
    u16 pipeline = 0;
    u16 material = 0;
    u16 texture_set = 0;
    /// A scissor rectangle. A change breaks a batch, because a scissor is pipeline state.
    Rect2D scissor;
    bool scissored = false;
    /// The render target. A change breaks a batch and is the most expensive break there is.
    u16 target = 0;

    /// The per-instance data: where it is, what part of the atlas it samples, and its colour.
    Rect2D destination;
    Rect2D source;
    u32 tint = 0xFFFFFFFFU;
    Vec2 pivot{0.5F, 0.5F};
    f32 rotation = 0.0F;
    bool flip_x = false;
    bool flip_y = false;
    /// Four custom floats a material may read. The escape hatch that stops every new effect needing
    /// a new instance layout.
    f32 custom[4] = {};
};

/// The per-instance record that reaches the GPU. Deliberately small and flat: the whole point of
/// batching is that this array is what is uploaded.
struct Instance2D {
    Rect2D destination;
    Rect2D source;
    u32 tint = 0xFFFFFFFFU;
    f32 rotation = 0.0F;
    Vec2 pivot{0.5F, 0.5F};
    f32 custom[4] = {};
};

/// Why a batch ended. "The engine SHALL report batch counts and break reasons so content can be
/// organised to batch well."
enum class BreakReason : u8 {
    Pipeline = 0,
    Material,
    TextureSet,
    Scissor,
    RenderTarget,
    /// The last batch of the frame.
    End,
};

[[nodiscard]] const char* break_reason_name(BreakReason reason) noexcept;

/// One instanced draw.
struct Batch2D {
    u32 first_instance = 0;
    u32 instance_count = 0;
    u16 pipeline = 0;
    u16 material = 0;
    u16 texture_set = 0;
    u16 target = 0;
    BreakReason reason = BreakReason::End;
};

struct BatchReport {
    u32 draws = 0;
    u32 batches = 0;
    u32 instances = 0;
    /// How many batches each reason caused. The dominant one is what a developer needs when a scene
    /// draws in four hundred calls.
    u32 breaks[static_cast<usize>(BreakReason::End) + 1] = {};

    /// The reason that broke the most batches.
    [[nodiscard]] BreakReason dominant_break() const noexcept;
};

/// A layer's settings. `rendering-2d` puts Y-sorting and lighting per layer, so they live together.
struct Layer2D {
    Name name;
    u16 index = 0;
    /// "Y-sorting SHALL be supported per layer, ordering by world Y so entities lower on screen
    /// draw in front."
    bool y_sorted = false;
    /// "2D lighting SHALL be optional per layer, so a UI layer is unlit while a gameplay layer is
    /// lit." An unlit layer costs nothing in the lighting pass, which is checked rather than
    /// asserted — see `lighting.h`.
    bool lit = true;
};

/// Sort and batch a frame's draws.
///
/// `layers` decides which layers Y-sort. `order` receives the draws' indices in draw order, and the
/// instances and batches follow it — THE DRAWS THEMSELVES ARE NOT MOVED. Two reasons, and the
/// second is the one that decided it: a caller's draw array is often a view over data it owns and
/// sorting it in place would reorder that; and the key is then packed ONCE per draw rather than
/// once per comparison, which is the difference between a sort of five thousand sprites costing a
/// millisecond and costing a tenth of one.
///
/// Consecutive draws in that order sharing a pipeline, a material, a texture set, a scissor and a
/// target become one instanced draw — the requirement's own list of what breaks a batch.
[[nodiscard]] Status build_batches(Span<const Draw2D> draws, Span<const Layer2D> layers,
                                   Array<u32>& order, Array<Instance2D>& instances,
                                   Array<Batch2D>& batches, BatchReport& report) noexcept;

/// The nine-slice's nine destination and source rectangles, appended to `out`.
///
/// A separate function because a nine-slice is NINE SPRITES that batch with everything around them:
/// expanding it here rather than in a shader is what keeps the instance layout the same for every
/// primitive, which is what makes them batch together at all.
[[nodiscard]] Status expand_nine_slice(const Draw2D& draw, const Rect2D& borders,
                                       Array<Draw2D>& out) noexcept;

/// A polyline expanded into a triangle strip's vertices, appended to `out` as position pairs.
///
/// `width` may vary along the line — the requirement's "width curve" — through `width_at`, which is
/// given the normalised distance along the polyline.
[[nodiscard]] Status expand_line(Span<const Vec2> points, f32 width, LineJoint joint, LineCap cap,
                                 f32 (*width_at)(f32, void*) noexcept, void* user,
                                 Array<Vec2>& out) noexcept;

/// Which frame of an animated sprite is showing, given the elapsed time and the per-frame
/// durations.
///
/// Returns the frame index, and `looped` says whether the animation wrapped — which is what an
/// animation event fires on.
[[nodiscard]] u32 animation_frame(Span<const f32> durations, f32 elapsed, bool loop,
                                  bool& looped) noexcept;

}  // namespace cy::rendering2d

#endif  // CY_RENDERING_2D_SPRITE_H
