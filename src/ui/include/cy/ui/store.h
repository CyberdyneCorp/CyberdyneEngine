#ifndef CY_UI_STORE_H
#define CY_UI_STORE_H
// CyberUI's element storage and its retained tree. M8.b task 9.1.
//
// --- UI ELEMENTS ARE NOT ENTITIES, AND THIS FILE IS WHY ------------------------------------------
//
// `ui-system`: "UI elements SHALL be stored in a dedicated, data-oriented UI store addressed by a
// lightweight `UIElementID`, **not** as ECS entities", and the scenario is a measurement: "WHEN a
// game has 5,000 gameplay entities and 20,000 UI elements THEN gameplay queries SHALL scan only
// gameplay archetypes, and UI elements SHALL not appear in the ECS world at all."
//
// This module therefore does not depend on `cy::ecs` at all. Not "does not create entities" —
// cannot name one. The requirement is kept by the link graph rather than by a reviewer, which is
// the same arrangement `src/servers/camera/` uses for the same kind of rule.
//
// --- STRUCTURE OF ARRAYS, AND WHAT EACH PASS TOUCHES ---------------------------------------------
//
// "Storage SHALL be structure-of-arrays over parallel arrays for hierarchy, layout input, computed
// layout output, style reference, flags, and paint data, so each pass touches only what it needs."
// Those are six arrays and they are exactly the six below. The arrange pass reads `LayoutInput` and
// writes `LayoutOutput` and touches neither `PaintData` nor the style reference, which is the
// second scenario and is checkable by reading `arrange()`'s body.
//
// --- THREE DIRTY STATES, AND THREE DIFFERENT PROPAGATIONS ----------------------------------------
//
//   Measure  desired size may have changed   propagates UPWARD while ancestors depend on it
//   Arrange  final rect may have changed     propagates DOWNWARD through the affected subtree
//   Paint    visual output changed           the element only
//
// The middle column is the part that is easy to get wrong and expensive to get wrong: a paint dirty
// that propagated would relayout a document because a label's number changed, and "WHEN a label's
// numeric text changes without changing its measured size THEN only its paint state SHALL be
// dirtied, and no layout work SHALL occur" is the case that catches it.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>

namespace cy::ui {

/// A lightweight element identifier: an index and a generation. NOT an entity, and not a pointer.
struct ElementId {
    u32 index = 0;
    u32 generation = 0;

    [[nodiscard]] constexpr bool is_valid() const noexcept { return generation != 0; }
    friend constexpr bool operator==(ElementId a, ElementId b) noexcept {
        return a.index == b.index && a.generation == b.generation;
    }
    friend constexpr bool operator!=(ElementId a, ElementId b) noexcept { return !(a == b); }
};

inline constexpr ElementId kNoElement{};

/// A rectangle in UI space: position and size, in reference-resolution units before scaling.
struct Rect {
    f32 x = 0.0F;
    f32 y = 0.0F;
    f32 width = 0.0F;
    f32 height = 0.0F;

    [[nodiscard]] constexpr f32 right() const noexcept { return x + width; }
    [[nodiscard]] constexpr f32 bottom() const noexcept { return y + height; }
    [[nodiscard]] constexpr bool contains(Vec2 point) const noexcept {
        return point.x >= x && point.x < right() && point.y >= y && point.y < bottom();
    }
    [[nodiscard]] Rect intersected(const Rect& other) const noexcept;
    [[nodiscard]] constexpr bool empty() const noexcept { return width <= 0.0F || height <= 0.0F; }
};

/// Insets: margins and padding, in the order a CSS author expects.
struct Insets {
    f32 left = 0.0F;
    f32 top = 0.0F;
    f32 right = 0.0F;
    f32 bottom = 0.0F;

    [[nodiscard]] constexpr f32 horizontal() const noexcept { return left + right; }
    [[nodiscard]] constexpr f32 vertical() const noexcept { return top + bottom; }
};

/// Which layout model a container uses. THREE, AND NO OTHERS — `ui-system` says so in a table, and
/// scrolling, wrapping and splitting are behaviours of containers built on these rather than a
/// fourth and fifth model.
enum class LayoutModel : u8 { Flex = 0, Grid = 1, Absolute = 2 };

enum class FlexDirection : u8 { Row = 0, Column = 1, RowReverse = 2, ColumnReverse = 3 };

/// Distribution along the main axis. The CSS names, because "Flex and grid semantics SHALL follow
/// their CSS definitions within the documented subset, so existing understanding transfers".
enum class Justify : u8 { Start = 0, Centre, End, SpaceBetween, SpaceAround, SpaceEvenly };

/// Alignment across the cross axis.
enum class Align : u8 { Start = 0, Centre, End, Stretch };

/// What a hit test does when it lands on an element.
enum class HitTestMode : u8 {
    /// Consume: the event stops here.
    Block = 0,
    /// Handle and continue to what is beneath.
    Pass = 1,
    /// Transparent: the element is not hit at all.
    Ignore = 2,
};

/// Per-element flags. A bit set rather than six bools, because this array is walked per frame.
enum class ElementFlags : u32 {
    None = 0,
    /// Drawn and laid out. A hidden element still occupies its place in the tree.
    Visible = 1U << 0U,
    /// Excluded from layout entirely, as CSS's `display: none` is.
    Collapsed = 1U << 1U,
    /// Clips its children to its own rect. A scroll view is this plus a scroll offset.
    ClipsChildren = 1U << 2U,
    /// May take keyboard focus.
    Focusable = 1U << 3U,
    /// Interactive but currently refusing input.
    Disabled = 1U << 4U,
    /// The pointer is over it.
    Hovered = 1U << 5U,
    /// The pointer is pressed on it.
    Pressed = 1U << 6U,
    /// It has keyboard focus.
    Focused = 1U << 7U,
    /// A checkbox or toggle that is on.
    Checked = 1U << 8U,
    /// Realised by a virtualised container this frame. A row scrolled out of view loses it.
    Realised = 1U << 9U,
};

[[nodiscard]] constexpr ElementFlags operator|(ElementFlags a, ElementFlags b) noexcept {
    return static_cast<ElementFlags>(static_cast<u32>(a) | static_cast<u32>(b));
}
[[nodiscard]] constexpr ElementFlags operator&(ElementFlags a, ElementFlags b) noexcept {
    return static_cast<ElementFlags>(static_cast<u32>(a) & static_cast<u32>(b));
}
[[nodiscard]] constexpr bool has_flag(ElementFlags set, ElementFlags flag) noexcept {
    return (static_cast<u32>(set) & static_cast<u32>(flag)) != 0U;
}
[[nodiscard]] constexpr ElementFlags without_flag(ElementFlags set, ElementFlags flag) noexcept {
    return static_cast<ElementFlags>(static_cast<u32>(set) & ~static_cast<u32>(flag));
}

/// The three dirty states, and they are independent.
enum class Dirty : u8 {
    None = 0,
    Measure = 1U << 0U,
    Arrange = 1U << 1U,
    Paint = 1U << 2U,
};

[[nodiscard]] constexpr Dirty operator|(Dirty a, Dirty b) noexcept {
    return static_cast<Dirty>(static_cast<u8>(a) | static_cast<u8>(b));
}
[[nodiscard]] constexpr bool has_dirty(Dirty set, Dirty flag) noexcept {
    return (static_cast<u8>(set) & static_cast<u8>(flag)) != 0U;
}

/// The hierarchy array: first child, next sibling, parent, depth.
struct Hierarchy {
    ElementId parent;
    ElementId first_child;
    ElementId last_child;
    ElementId next_sibling;
    ElementId previous_sibling;
    u32 child_count = 0;
    u16 depth = 0;
};

/// What layout READS. The arrange pass touches this array and `LayoutOutput`, and nothing else.
struct LayoutInput {
    LayoutModel model = LayoutModel::Flex;
    FlexDirection direction = FlexDirection::Row;
    Justify justify = Justify::Start;
    Align align = Align::Stretch;
    /// Cross-axis alignment of THIS element inside its parent, overriding the parent's `align`.
    Align self_align = Align::Stretch;
    bool wrap = false;
    f32 gap = 0.0F;

    /// Preferred, minimum and maximum size. A negative preferred size means "ask the content",
    /// which is what a label does.
    Vec2 preferred{-1.0F, -1.0F};
    Vec2 minimum{0.0F, 0.0F};
    Vec2 maximum{1e9F, 1e9F};
    Insets margin;
    Insets padding;

    f32 flex_grow = 0.0F;
    f32 flex_shrink = 1.0F;
    /// A negative basis means "use the preferred size", which is CSS's `auto`.
    f32 flex_basis = -1.0F;
    /// Width divided by height. Zero means unconstrained.
    f32 aspect_ratio = 0.0F;

    /// `Absolute`: anchors as fractions of the parent's rect, plus offsets in units.
    Vec2 anchor_min{0.0F, 0.0F};
    Vec2 anchor_max{0.0F, 0.0F};
    Vec2 offset_min{0.0F, 0.0F};
    Vec2 offset_max{0.0F, 0.0F};

    /// `Grid`: the track this element occupies and how many it spans.
    u16 grid_column = 0;
    u16 grid_row = 0;
    u16 grid_column_span = 1;
    u16 grid_row_span = 1;
    /// `Grid` container: how many explicit columns. Zero means one column.
    u16 grid_columns = 0;
};

/// What layout WRITES.
struct LayoutOutput {
    /// The desired size the measure pass computed, including this element's own padding.
    Vec2 desired{0.0F, 0.0F};
    /// The final rect the arrange pass assigned, in the document's own space.
    Rect rect;
    /// The clip rect in force, intersected down the tree. A scroll view narrows it for its subtree.
    Rect clip;
    /// A scroll offset applied to children. Positive scrolls content up and left.
    Vec2 scroll{0.0F, 0.0F};
};

/// What painting reads. Deliberately separate from layout, so the arrange pass never touches it.
struct PaintData {
    /// Premultiplied RGBA, 8 bits each.
    u32 background = 0;
    u32 border_colour = 0;
    u32 tint = 0xFFFFFFFFU;
    f32 border_width = 0.0F;
    f32 corner_radius = 0.0F;
    f32 opacity = 1.0F;
    /// Which material the primitive uses. Batching groups by this, so it is an index rather than a
    /// pointer.
    u16 material = 0;
    /// The atlas page a texture or glyph lives on. The second half of a batch key.
    u16 atlas = 0;
    /// The UV rectangle within that atlas.
    Rect uv;
};

/// Which of the six per-frame counters a report carries. `ui-system`'s diagnostics ask for "counts
/// of measure, arrange, and paint invalidations per frame".
struct StoreStats {
    u32 elements = 0;
    u32 measure_dirty = 0;
    u32 arrange_dirty = 0;
    u32 paint_dirty = 0;
    u32 created = 0;
    u32 destroyed = 0;
    /// Elements whose identity changed between two diffs of a declarative description. "the most
    /// common and least obvious source of declarative UI bugs", and the diagnostic the requirement
    /// asks for by name.
    u32 identity_churn = 0;
};

/// The element store: six parallel arrays and a dense free list.
class ElementStore {
public:
    explicit ElementStore(Allocator& allocator) noexcept;

    ElementStore(const ElementStore&) = delete;
    ElementStore& operator=(const ElementStore&) = delete;

    // --- Lifetime -------------------------------------------------------------------------------

    /// Create an element under `parent`, or a root when `parent` is invalid.
    [[nodiscard]] Expected<ElementId, Error> create(ElementId parent, Name type) noexcept;
    /// Destroy an element and its subtree. Its slot is reused with a bumped generation, so the old
    /// id fails validation rather than aliasing the replacement.
    [[nodiscard]] Status destroy(ElementId element) noexcept;
    [[nodiscard]] bool alive(ElementId element) const noexcept;
    [[nodiscard]] usize size() const noexcept { return live_; }
    [[nodiscard]] usize capacity() const noexcept { return slots_.size(); }

    // --- The six arrays -------------------------------------------------------------------------

    [[nodiscard]] const Hierarchy* hierarchy(ElementId element) const noexcept;
    [[nodiscard]] const LayoutInput* layout_input(ElementId element) const noexcept;
    [[nodiscard]] LayoutInput* layout_input(ElementId element) noexcept;
    [[nodiscard]] const LayoutOutput* layout_output(ElementId element) const noexcept;
    [[nodiscard]] LayoutOutput* layout_output(ElementId element) noexcept;
    [[nodiscard]] const PaintData* paint(ElementId element) const noexcept;
    [[nodiscard]] PaintData* paint(ElementId element) noexcept;
    [[nodiscard]] ElementFlags flags(ElementId element) const noexcept;
    [[nodiscard]] Status set_flags(ElementId element, ElementFlags value) noexcept;
    [[nodiscard]] Name type_of(ElementId element) const noexcept;

    /// The style reference: which rule set this element resolved against, and its classes.
    [[nodiscard]] u32 style(ElementId element) const noexcept;
    [[nodiscard]] Status set_style(ElementId element, u32 style_index) noexcept;

    /// An author's key, for declarative diffing. Zero means "identity is structural position".
    [[nodiscard]] u64 key(ElementId element) const noexcept;
    [[nodiscard]] Status set_key(ElementId element, u64 value) noexcept;

    [[nodiscard]] HitTestMode hit_test_mode(ElementId element) const noexcept;
    [[nodiscard]] Status set_hit_test_mode(ElementId element, HitTestMode mode) noexcept;

    // --- The tree -------------------------------------------------------------------------------

    [[nodiscard]] Span<const ElementId> roots() const noexcept { return roots_.span(); }
    /// Append `child` to `parent`'s children, detaching it from its previous parent.
    [[nodiscard]] Status reparent(ElementId child, ElementId parent) noexcept;
    /// The children of an element, appended to `out` in order.
    [[nodiscard]] Status children_of(ElementId element, Array<ElementId>& out) const noexcept;

    // --- Dirty state ----------------------------------------------------------------------------

    /// Mark an element dirty. THE PROPAGATION IS THE POINT — see the header: measure goes up while
    /// ancestors depend on it, arrange goes down, paint goes nowhere.
    void mark(ElementId element, Dirty state) noexcept;
    [[nodiscard]] Dirty dirty(ElementId element) const noexcept;
    void clear_dirty(ElementId element, Dirty state) noexcept;
    /// Every element carrying `state`, in depth order (parents before children), appended to `out`.
    [[nodiscard]] Status collect_dirty(Dirty state, Array<ElementId>& out) const noexcept;
    [[nodiscard]] bool any_dirty() const noexcept;

    /// The allocator every array here was built with. Layout and painting allocate their own
    /// scratch from it, so a document's whole cost is attributed to one place.
    [[nodiscard]] Allocator& allocator() const noexcept { return slots_.allocator(); }

    [[nodiscard]] StoreStats stats() const noexcept;
    void note_identity_churn(u32 count) noexcept { churn_ += count; }
    void reset_churn() noexcept { churn_ = 0; }

private:
    struct Slot {
        u32 generation = 0;
        bool alive = false;
    };

    [[nodiscard]] i64 resolve(ElementId element) const noexcept;
    void detach(u32 index) noexcept;
    void mark_subtree_arrange(u32 index) noexcept;

    Array<Slot> slots_;
    Array<Hierarchy> hierarchy_;
    Array<LayoutInput> layout_in_;
    Array<LayoutOutput> layout_out_;
    Array<PaintData> paint_;
    Array<ElementFlags> flags_;
    Array<u32> style_;
    Array<Name> type_;
    Array<u64> key_;
    Array<HitTestMode> hit_;
    Array<Dirty> dirty_;
    Array<u32> free_;
    Array<ElementId> roots_;
    usize live_ = 0;
    u32 created_ = 0;
    u32 destroyed_ = 0;
    u32 churn_ = 0;
};

}  // namespace cy::ui

#endif  // CY_UI_STORE_H
