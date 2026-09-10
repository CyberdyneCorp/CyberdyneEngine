#ifndef CY_UI_LAYOUT_H
#define CY_UI_LAYOUT_H
// Measure, arrange, and the three layout models. M8.b task 9.1.
//
// `ui-system`: "Layout SHALL be a two-pass process: measure (compute each element's desired size
// bottom-up) then arrange (assign final rects top-down), driven by the dirty states in the retained
// tree", over "Three layout models ... and no others": Flex, Grid, Absolute.
//
// --- INCREMENTAL IS THE REQUIREMENT, NOT AN OPTIMISATION -----------------------------------------
//
// "WHEN one element is invalidated THEN only the dirty subtree and its size-affecting ancestors
// SHALL be re-measured and re-arranged", and "WHEN no UI state changes in a frame THEN no layout or
// paint work SHALL be performed". So `layout()` walks the DIRTY LISTS the store maintains, not the
// tree, and `LayoutReport` counts what it touched — which is what makes "idle UI costs nothing" a
// number a test reads rather than a claim.
//
// --- RESOLUTION INDEPENDENCE IS A TRANSFORM, NOT A SECOND LAYOUT ---------------------------------
//
// "UI SHALL be authored against a reference resolution and scaled to the actual output, with
// selectable strategies." The strategy produces ONE scale factor, layout runs in reference units,
// and the scale is applied when primitives are emitted. A layout that ran in physical pixels would
// give a different flex distribution at every window size, which is the defect this separation
// prevents.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/ui/store.h>

namespace cy::ui {

/// How a reference resolution becomes the output's scale.
enum class ScaleMode : u8 {
    /// One reference unit is one pixel, whatever the window is.
    FixedPixel = 0,
    ScaleWithWidth,
    ScaleWithHeight,
    /// The smaller of the two ratios: everything fits, with letterboxing at the edges.
    ScaleWithSmaller,
    ScaleWithLarger,
    /// A blend between width and height, by `match`.
    Match,
};

struct ScaleSettings {
    ScaleMode mode = ScaleMode::ScaleWithSmaller;
    Vec2 reference{1920.0F, 1080.0F};
    /// `Match` only: 0 follows width, 1 follows height.
    f32 match = 0.5F;
    /// The platform's DPI scale, which the UI respects rather than overrides.
    f32 dpi_scale = 1.0F;
    /// The player's own UI scale, on top of everything else. An accessibility setting.
    f32 user_scale = 1.0F;
    /// The player's text scale, applied to text sizes only. Also an accessibility setting, and
    /// separate because scaling the whole interface and scaling only its text are different needs.
    f32 text_scale = 1.0F;
};

/// The scale factor for an output size. One number, computed in one place.
[[nodiscard]] f32 resolve_scale(const ScaleSettings& settings, Vec2 output) noexcept;

/// How a caller measures content this module cannot: text, an image's natural size, a plugin's.
class ContentMeasurer {
public:
    ContentMeasurer() = default;
    virtual ~ContentMeasurer() = default;
    ContentMeasurer(const ContentMeasurer&) = delete;
    ContentMeasurer& operator=(const ContentMeasurer&) = delete;
    ContentMeasurer(ContentMeasurer&&) = delete;
    ContentMeasurer& operator=(ContentMeasurer&&) = delete;

    /// The natural size of an element's own content, given the width available to it. `available.x`
    /// is negative when the width is unconstrained, which is what a label being measured inside a
    /// row is told.
    ///
    /// A text element's answer comes from `cy::servers-text`, which is why this is an interface: a
    /// layout module that included a font server could not be tested without one.
    virtual Vec2 measure_content(ElementId element, Vec2 available) noexcept = 0;
};

/// What one layout pass did. `ui-system`'s diagnostics ask for exactly these counters.
struct LayoutReport {
    u32 measured = 0;
    u32 arranged = 0;
    /// Elements skipped because they were collapsed.
    u32 skipped = 0;
    /// The deepest element the pass touched, for a report that says where the cost is.
    u16 deepest = 0;
};

/// Run measure and arrange over whatever is dirty.
///
/// `viewport` is the output size in physical pixels; the layout runs in reference units and the
/// scale is `resolve_scale`'s answer. `measurer` may be null, in which case an element with no
/// preferred size measures to zero — which is what an image with no texture is.
[[nodiscard]] Status layout(ElementStore& store, const ScaleSettings& settings, Vec2 viewport,
                            ContentMeasurer* measurer, LayoutReport& report) noexcept;

/// Measure one subtree, bottom-up, without arranging. Exposed because a virtualised list measures a
/// row to decide how many fit before it realises any of them.
[[nodiscard]] Status measure_subtree(ElementStore& store, ElementId root, Vec2 available,
                                     ContentMeasurer* measurer, LayoutReport& report) noexcept;

/// Arrange one subtree into a rect, top-down. Exposed for the same reason.
[[nodiscard]] Status arrange_subtree(ElementStore& store, ElementId root, const Rect& rect,
                                     const Rect& clip, LayoutReport& report) noexcept;

}  // namespace cy::ui

#endif  // CY_UI_LAYOUT_H
