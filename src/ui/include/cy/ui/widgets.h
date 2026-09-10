#ifndef CY_UI_WIDGETS_H
#define CY_UI_WIDGETS_H
// Declarative descriptions, the diff into the retained tree, virtualisation and data binding.
// M8.b tasks 9.1 and 9.2.
//
// --- A DESCRIPTION IS DIFFED, NOT REBUILT --------------------------------------------------------
//
// `ui-system`: "The engine SHALL provide declarative UI construction in Swift and in C++, producing
// a description that is **diffed** into the retained tree. The description SHALL NOT be rebuilt
// into new elements each frame: only differences SHALL be applied, preserving element identity,
// animation state, focus, and scroll position across rebuilds."
//
// So `Description` is a flat array of nodes a caller builds each time, `reconcile()` diffs it
// against the store, and the only elements created are the ones the description added. The case
// that proves it is the focus one: "WHEN a list rebuilds while one of its text fields has focus and
// a selection THEN focus, caret, selection, and scroll position SHALL be preserved for elements
// whose identity is unchanged."
//
// --- IDENTITY IS POSITION PLUS AN OPTIONAL KEY, AND THE CONSEQUENCE IS DOCUMENTED ----------------
//
// "Element identity SHALL derive from structural position plus an optional explicit key. **The
// documentation SHALL state the consequence: reordering children without keys causes state to
// follow position rather than data.**"
//
// Stated, then: WITHOUT A KEY, AN ELEMENT IS ITS POSITION. Reorder two rows and the first row's
// state — its focus, its scroll, its animation — stays with the first POSITION and therefore moves
// to what is now the first row's data. With a key, state follows the key. `ReconcileReport::churn`
// counts elements whose identity changed between two reconciles, which is the diagnostic
// `ui-system` asks for by name: "the most common and least obvious source of declarative UI bugs".

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/ui/paint.h>
#include <cy/ui/store.h>
#include <cy/ui/style.h>

#include <string_view>

namespace cy::ui {

/// One node of a declarative description.
struct DescriptionNode {
    /// The element type: `panel`, `label`, `button`. Matched against the existing element's type,
    /// and a mismatch is a replacement rather than a mutation.
    Name type;
    /// The author's key. Zero means "identity is structural position" — see the header.
    u64 key = 0;
    /// The index of this node's parent within the description, or `kNoParent` for a root.
    u32 parent = kNoParent;
    LayoutInput layout;
    PaintData paint;
    ElementFlags flags = ElementFlags::Visible;
    HitTestMode hit_test = HitTestMode::Block;
    Role role = Role::None;
    Name label;

    static constexpr u32 kNoParent = 0xFFFFFFFFU;
};

/// A description: nodes in document order, parents before children.
class Description {
public:
    explicit Description(Allocator& allocator) noexcept : nodes_(allocator) {}

    /// Add a node under `parent` (an index into this description, or `kNoParent`).
    [[nodiscard]] Expected<u32, Error> add(const DescriptionNode& node) noexcept;
    [[nodiscard]] Span<const DescriptionNode> nodes() const noexcept { return nodes_.span(); }
    void clear() noexcept { nodes_.clear(); }
    [[nodiscard]] usize size() const noexcept { return nodes_.size(); }

private:
    Array<DescriptionNode> nodes_;
};

struct ReconcileReport {
    u32 created = 0;
    u32 updated = 0;
    u32 removed = 0;
    /// Elements that kept their place and their state: the point of diffing.
    u32 preserved = 0;
    /// Elements whose identity changed — the same position now holding a different key, or the same
    /// key at a different type. The diagnostic the requirement asks for by name.
    u32 churn = 0;
};

/// Diff a description into the retained tree under `root`.
///
/// `root` must already exist; the description's roots become its children. Elements the description
/// no longer names are destroyed, and everything else keeps its identity and its state.
[[nodiscard]] Status reconcile(ElementStore& store, ElementId root, const Description& description,
                               ReconcileReport& report) noexcept;

// --- Virtualisation
// --------------------------------------------------------------------------------

/// What a virtualised list needs to know, and all it needs to know.
struct VirtualList {
    /// How many items the data has. A hundred thousand is a normal answer.
    u32 item_count = 0;
    /// The height of one row, in reference units. A uniform row height is what makes the arithmetic
    /// O(1); a variable one needs a prefix sum, which is a later change to this struct alone.
    f32 item_height = 24.0F;
    /// The scroll offset, in the same units.
    f32 scroll = 0.0F;
    /// The height of the viewport the list is inside.
    f32 viewport_height = 0.0F;
    /// Rows realised beyond the visible range, so scrolling does not pop. Two is enough for a
    /// smooth scroll and cheap enough not to matter.
    u32 overscan = 2;
};

/// Which rows a virtualised list must realise this frame.
struct VirtualRange {
    u32 first = 0;
    /// One past the last. `count()` is what a caller loops over.
    u32 last = 0;
    /// The offset to place the first realised row at, so the list scrolls smoothly rather than
    /// jumping by a row.
    f32 offset = 0.0F;
    /// The full height the scroll bar represents.
    f32 total_height = 0.0F;

    [[nodiscard]] constexpr u32 count() const noexcept {
        return (last > first) ? (last - first) : 0U;
    }
};

/// The rows to realise. "WHEN a list view displays 100 000 items THEN only visible rows SHALL be
/// realised, and scrolling SHALL cost the same as for 100 items" — which is true of this function
/// by construction: it does arithmetic, not a walk.
[[nodiscard]] VirtualRange virtual_range(const VirtualList& list) noexcept;

// --- Data binding
// -----------------------------------------------------------------------------------

/// Which way a binding moves.
enum class BindingMode : u8 {
    /// Source to element.
    OneWay = 0,
    /// Both, with the element's edits written back.
    TwoWay = 1,
};

/// What a bound value is.
struct BoundValue {
    enum class Kind : u8 { Number = 0, Colour, Text, Boolean };

    Kind kind = Kind::Number;
    f32 number = 0.0F;
    u32 colour = 0;
    Name text;
    bool boolean = false;

    [[nodiscard]] bool operator==(const BoundValue& other) const noexcept;
};

/// A source of bound data. Implemented over a reflected component field, a resource, a script
/// observable, or reactive state — the four `ui-system` names — and this module cares about none of
/// them beyond "has it changed".
class BindingSource {
public:
    BindingSource() = default;
    virtual ~BindingSource() = default;
    BindingSource(const BindingSource&) = delete;
    BindingSource& operator=(const BindingSource&) = delete;
    BindingSource(BindingSource&&) = delete;
    BindingSource& operator=(BindingSource&&) = delete;

    /// The current value.
    [[nodiscard]] virtual BoundValue read() noexcept = 0;
    /// Whether it changed since the last `read()`. A source over ECS component data answers from
    /// change detection rather than by comparing — which is the requirement's "using ECS change
    /// detection where the source is component data".
    [[nodiscard]] virtual bool changed() const noexcept = 0;
    /// Two-way only. A source that cannot be written refuses, and the binding reports it.
    [[nodiscard]] virtual Status write(const BoundValue& value) noexcept {
        (void)value;
        return make_unexpected(
            Error{ErrorCode::Unsupported, "this binding source is read-only", 0});
    }
};

/// Converts a source value into what an element wants. A percentage into a width, an enumeration
/// into a colour.
using BindingConverter = BoundValue (*)(const BoundValue&, void*) noexcept;

struct Binding {
    ElementId element;
    /// Which property of the element the value drives.
    StyleProperty property = StyleProperty::Count;
    BindingSource* source = nullptr;
    BindingMode mode = BindingMode::OneWay;
    BindingConverter converter = nullptr;
    void* converter_user = nullptr;
    /// True while the element is being edited, which suppresses the source's updates: "editing the
    /// field SHALL write back, and external changes SHALL update the field unless it is being
    /// edited".
    bool editing = false;
    /// The value last pushed, so a source that reports a change but produced the same value costs
    /// nothing.
    BoundValue last;
};

struct BindingReport {
    u32 evaluated = 0;
    u32 updated = 0;
    /// Bindings whose source said "changed" and whose value was the same. A large number here is a
    /// change-detection problem in the source, and the counter is how anybody finds out.
    u32 spurious = 0;
    u32 written_back = 0;
    /// Elements dirtied, by kind. A bound value that does not affect layout dirties PAINT ONLY,
    /// which is the requirement's "WHEN a bound value changes without affecting layout THEN only
    /// the bound element's paint state SHALL be dirtied".
    u32 paint_dirtied = 0;
    u32 layout_dirtied = 0;
};

/// Evaluate a set of bindings, pushing changed values into the store.
[[nodiscard]] Status update_bindings(ElementStore& store, Span<Binding> bindings,
                                     BindingReport& report) noexcept;

/// Write an edited element value back to its source. Two-way bindings only.
[[nodiscard]] Status write_back(Binding& binding, const BoundValue& value) noexcept;

}  // namespace cy::ui

#endif  // CY_UI_WIDGETS_H
