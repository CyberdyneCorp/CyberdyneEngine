#ifndef CY_UI_PAINT_H
#define CY_UI_PAINT_H
// Flattening, batching, the frame budget and accessibility. M8.b task 9.3.
//
// --- FLATTENING IS THE WHOLE OF "GPU-DRIVEN" -----------------------------------------------------
//
// `ui-system`: "Laid-out UI SHALL be flattened into a primitive stream — per primitive: bounds, UV
// rect, material index, clip index, transform index, and colour — rather than submitted per
// element." `Primitive` is those six fields and nothing else; an element does not reach the GPU and
// there is no per-element draw call to make.
//
// "Primitives SHALL be culled against the viewport and their clip rects, batched by material and
// atlas, and drawn with a small number of indirect draws", and "Flattening SHALL be incremental:
// unchanged regions SHALL reuse their previous primitive data rather than being re-emitted."
// `flatten()` re-emits only what is paint-dirty and `FlattenReport::reused` counts the rest, which
// is what makes "WHEN one panel repaints in an otherwise static document THEN only its primitives
// SHALL be re-emitted" a number rather than an intention.
//
// --- THE BUDGET DEGRADES IN A DECLARED ORDER -----------------------------------------------------
//
// "When budgets are exceeded, the system SHALL degrade in a defined order — reducing effect
// quality, disabling blur-behind, and reducing world-space UI detail — before reducing anything
// affecting interaction or legibility, and SHALL report the degradation." `Degradation` is that
// order as an enumeration, `apply_budget()` walks it, and there is no step in it that drops an
// element, moves focus or changes text — "WHEN budgets are under pressure THEN hit-testing, focus,
// and text legibility SHALL be preserved" is expressed as the absence of a rung.
//
// --- ACCESSIBILITY IS PUBLISHED, NOT INFERRED ----------------------------------------------------
//
// "UI elements SHALL publish accessibility information — role, label, description, value, and state
// — to the platform accessibility layer where available." `AccessibilityNode` is those five, an
// `AccessibilityBridge` is what a platform implements, and `audit_accessibility()` is the check the
// milestone's exit criterion asks for: every interactive element reachable by keyboard, every focus
// indicator meeting a contrast ratio, every control carrying a label.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/ui/interaction.h>
#include <cy/ui/store.h>

#include <string_view>

namespace cy::ui {

/// One flattened primitive. Six fields, and the requirement names all six.
struct Primitive {
    Rect bounds;
    Rect uv;
    u16 material = 0;
    /// An index into the clip array, not a rect: two thousand primitives inside one scroll view
    /// share one clip.
    u16 clip = 0;
    /// An index into the transform array. Zero is the identity, which most primitives are.
    u16 transform = 0;
    u16 atlas = 0;
    u32 colour = 0xFFFFFFFFU;
    /// The element it came from, so the inspector can go back from a pixel to a node.
    ElementId source;
};

/// A run of primitives that share a material and an atlas, drawn as one indirect draw.
struct Batch {
    u32 first = 0;
    u32 count = 0;
    u16 material = 0;
    u16 atlas = 0;
    /// Why this batch ended rather than continuing. `ui-system`'s diagnostics ask for "batch counts
    /// with batch-break reasons", and a batch that broke for no reason anybody can name is the
    /// commonest cause of a UI that draws in four hundred calls.
    enum class BreakReason : u8 { Material = 0, Atlas, Clip, Transform, End };
    BreakReason reason = BreakReason::End;
};

struct FlattenReport {
    u32 emitted = 0;
    /// Primitives carried over from the previous frame because nothing dirtied them.
    u32 reused = 0;
    u32 culled = 0;
    u32 batches = 0;
    /// The elements that were visited at all. With nothing dirty this is zero.
    u32 visited = 0;
};

/// The flattened output, retained between frames so that "unchanged regions SHALL reuse their
/// previous primitive data" is possible at all.
class PrimitiveBuffer {
public:
    explicit PrimitiveBuffer(Allocator& allocator) noexcept;

    PrimitiveBuffer(const PrimitiveBuffer&) = delete;
    PrimitiveBuffer& operator=(const PrimitiveBuffer&) = delete;

    [[nodiscard]] Span<const Primitive> primitives() const noexcept { return primitives_.span(); }
    [[nodiscard]] Span<const Batch> batches() const noexcept { return batches_.span(); }
    [[nodiscard]] Span<const Rect> clips() const noexcept { return clips_.span(); }
    void clear() noexcept;

    friend Status flatten(ElementStore&, const Rect&, PrimitiveBuffer&, FlattenReport&) noexcept;

private:
    Array<Primitive> primitives_;
    Array<Batch> batches_;
    Array<Rect> clips_;
    /// The element each primitive belongs to, so an incremental re-emit can find and replace one
    /// element's run without rebuilding the array.
    Array<ElementId> owners_;
};

/// Flatten the tree into primitives, cull against `viewport`, and batch.
///
/// Incremental: an element with no paint dirt keeps the primitive it had. A caller that wants a
/// full rebuild clears the buffer first, which is what a resolution change does.
[[nodiscard]] Status flatten(ElementStore& store, const Rect& viewport, PrimitiveBuffer& out,
                             FlattenReport& report) noexcept;

// --- The frame budget
// -----------------------------------------------------------------------------

/// What the interface is allowed to spend.
struct UiBudget {
    f32 layout_milliseconds = 2.0F;
    u32 primitives = 20000;
    u32 batches = 64;
    /// Blur-behind and render-target capture passes, which are the expensive optional effects.
    u32 effect_passes = 4;
};

/// The degradation ladder, in the order the requirement states. NOTHING BELOW `WorldSpaceDetail`
/// exists: interaction and legibility are not on this ladder, which is how "hit-testing, focus, and
/// text legibility SHALL be preserved" is enforced rather than remembered.
enum class Degradation : u8 {
    None = 0,
    /// First: cheaper effects — fewer shadow samples, simpler gradients.
    EffectQuality = 1,
    /// Then: no blur behind panels at all.
    BlurBehind = 2,
    /// Then: world-space interfaces render at lower detail.
    WorldSpaceDetail = 3,
};

[[nodiscard]] const char* degradation_name(Degradation level) noexcept;

/// What one frame cost and what was given up to fit it.
struct BudgetReport {
    f32 layout_milliseconds = 0.0F;
    u32 primitives = 0;
    u32 batches = 0;
    u32 effect_passes = 0;
    Degradation level = Degradation::None;
    /// Which measurement forced the degradation, for a report that names the cause.
    Name cause;
    bool over_budget = false;
};

/// Decide the degradation level for a frame's measurements.
[[nodiscard]] Degradation apply_budget(const UiBudget& budget, const FlattenReport& flatten,
                                       f32 layout_milliseconds, u32 effect_passes,
                                       BudgetReport& report) noexcept;

// --- Accessibility
// ---------------------------------------------------------------------------------

/// What an element is, to a screen reader.
enum class Role : u8 {
    None = 0,
    Panel,
    Label,
    Button,
    Toggle,
    Checkbox,
    Slider,
    ProgressBar,
    TextField,
    List,
    ListItem,
    Tab,
    Dialog,
    Count,
};

[[nodiscard]] const char* role_name(Role role) noexcept;

/// The five things an element publishes.
struct AccessibilityNode {
    ElementId element;
    Role role = Role::None;
    Name label;
    Name description;
    /// The current value, as text — "7 of 10", "on". Text because that is what is announced.
    Name value;
    /// The element's state, as the platform layers understand it.
    bool focusable = false;
    bool focused = false;
    bool disabled = false;
    bool checked = false;
};

/// What a platform accessibility layer implements. Absent on a platform that has none, which is why
/// every call site takes a pointer.
class AccessibilityBridge {
public:
    AccessibilityBridge() = default;
    virtual ~AccessibilityBridge() = default;
    AccessibilityBridge(const AccessibilityBridge&) = delete;
    AccessibilityBridge& operator=(const AccessibilityBridge&) = delete;
    AccessibilityBridge(AccessibilityBridge&&) = delete;
    AccessibilityBridge& operator=(AccessibilityBridge&&) = delete;

    virtual void publish(Span<const AccessibilityNode> nodes) noexcept = 0;
    /// Focus moved. "WHEN focus moves to a slider with accessibility available THEN its role,
    /// label, and current value SHALL be announced."
    virtual void announce_focus(const AccessibilityNode& node) noexcept = 0;
};

/// One accessibility finding.
struct AccessibilityFinding {
    ElementId element;
    /// What is wrong, as a stable identifier a gate can count: `unlabelled`, `unreachable`,
    /// `contrast`, `target-size`.
    Name rule;
    const char* message = "";
    /// The measured value that failed, where there is one — a contrast ratio, a target's size.
    f32 measured = 0.0F;
    f32 required = 0.0F;
};

struct AccessibilityReport {
    u32 examined = 0;
    u32 interactive = 0;
    u32 keyboard_reachable = 0;
    /// True when every interactive element is reachable, labelled, and meets the contrast and
    /// target-size minimums. THE EXIT CRITERION reads this.
    bool passed = false;
};

/// The contrast ratio between two premultiplied colours, by WCAG's relative-luminance formula.
/// Exposed because a theme author wants to check a pair without building a document.
[[nodiscard]] f32 contrast_ratio(u32 foreground, u32 background) noexcept;

/// Audit an interface.
///
/// Checks, each of which is a sentence in `ui-system`'s accessibility requirement:
///   * every interactive element is keyboard-reachable — "keyboard-only operation of every
///     interactive element";
///   * every interactive element carries a label, because a control a screen reader cannot name is
///     a control a screen reader user cannot use;
///   * the focus indicator meets 3:1 against the element it sits on — "focus indication that meets
///     contrast requirements";
///   * text meets 4.5:1 against its background, WCAG AA's threshold for body text;
///   * an interactive element is at least 24 units on its smaller side, which is the smallest
///   target
///     a touch interface may offer.
[[nodiscard]] Status audit_accessibility(const ElementStore& store,
                                         Span<const AccessibilityNode> nodes,
                                         const Interaction& interaction,
                                         Array<AccessibilityFinding>& findings,
                                         AccessibilityReport& report) noexcept;

/// Publish the tree to a platform bridge, and announce the focused element.
[[nodiscard]] Status publish_accessibility(Span<const AccessibilityNode> nodes, ElementId focus,
                                           AccessibilityBridge* bridge) noexcept;

}  // namespace cy::ui

#endif  // CY_UI_PAINT_H
