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

/// The behaviours of the one built-in UI shader, selected by a primitive's material index.
///
/// `ui-system`: "Text glyphs, rounded rectangles, borders, gradients, shadows, and images SHALL
/// share the primitive representation and, where practical, one shader with material-indexed
/// behaviour." These are the indices that shader answers; a custom UI material (stage 2 of the
/// runtime UI work) takes an index from `kFirstCustomMaterial` up.
enum class BuiltinMaterial : u16 {
    /// A rectangle, rounded by `Primitive::corner_radius` and bordered by `border_width` in
    /// `border_colour`, as a signed distance. The default, and what every panel is.
    Shape = 0,
    /// The atlas page sampled at `uv`, premultiplied, multiplied by the colour and masked by the
    /// same rounded shape.
    Image = 1,
    /// The atlas page's red channel as coverage — a glyph — multiplied by the colour.
    Glyph = 2,
};

/// The first material index that is not a built-in behaviour.
inline constexpr u16 kFirstCustomMaterial = 16;

/// A built-in behaviour as the index `PaintData::material` and `Primitive::material` carry.
[[nodiscard]] constexpr u16 material_index(BuiltinMaterial material) noexcept {
    return static_cast<u16>(material);
}

/// One flattened primitive.
///
/// The requirement's six fields — bounds, UV rect, material, clip, transform and colour — plus the
/// atlas the batch key needs and the three numbers the built-in shape material reads. Colours are
/// PREMULTIPLIED, and the element's opacity, multiplied down the tree, is already in them: a
/// renderer draws `colour` and never sees an opacity.
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
    /// `BuiltinMaterial::Shape`'s and `Image`'s corner radius, in the document's units.
    f32 corner_radius = 0.0F;
    /// The border's width, drawn inside the bounds, in the document's units. Zero draws none.
    f32 border_width = 0.0F;
    /// The border's premultiplied colour, with the opacity folded in as `colour`'s is.
    u32 border_colour = 0;
};

/// A premultiplied colour scaled by an opacity: every channel, alpha included, rounded to the
/// nearest step. An opacity of one returns the colour unchanged, bit for bit.
[[nodiscard]] u32 scale_premultiplied(u32 colour, f32 opacity) noexcept;

/// What draws an element's own content — a label's glyphs, an icon — into the primitive stream.
///
/// `flatten()` calls it once per visible element, after the element's own background primitive and
/// before its children, so content sits over its panel and under anything nested inside it. A
/// painter appends primitives with their bounds, uv, material, atlas and colour; `flatten()` fills
/// in the clip, the transform and the source element, folds the inherited opacity into the colours,
/// and culls any that fall outside the clip. The interface is the twin of `ContentMeasurer`: a
/// module that included a font server could not be tested without one.
class ContentPainter {
public:
    ContentPainter() = default;
    virtual ~ContentPainter() = default;
    ContentPainter(const ContentPainter&) = delete;
    ContentPainter& operator=(const ContentPainter&) = delete;
    ContentPainter(ContentPainter&&) = delete;
    ContentPainter& operator=(ContentPainter&&) = delete;

    /// Append `element`'s content, laid out inside `rect`, to `out`.
    [[nodiscard]] virtual Status paint_content(ElementId element, const Rect& rect,
                                               Array<Primitive>& out) noexcept = 0;
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
    /// Visible elements with nothing of their own to draw — a built-in material with no colour and
    /// no border, which is what a layout container is — that put no primitive in the stream.
    u32 empty = 0;
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

    friend Status flatten(ElementStore&, const Rect&, PrimitiveBuffer&, FlattenReport&,
                          ContentPainter*) noexcept;

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
///
/// An element's opacity multiplies down the tree into every primitive beneath it, so a panel at
/// half opacity draws its children at half too. Overlapping children of a faded panel each blend
/// on their own: the offscreen opacity group `ui-system` asks for is not built yet.
///
/// `painter`, when given, draws each visible element's content; see `ContentPainter`.
[[nodiscard]] Status flatten(ElementStore& store, const Rect& viewport, PrimitiveBuffer& out,
                             FlattenReport& report, ContentPainter* painter = nullptr) noexcept;

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
