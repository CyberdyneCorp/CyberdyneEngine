#ifndef CY_UI_INTERACTION_H
#define CY_UI_INTERACTION_H
// Input routing, focus, and the layer stack. M8.b task 9.2.
//
// `ui-system`: "UI input SHALL be routed: hit-test from the topmost layer downward, deliver to the
// element under the pointer, then bubble to ancestors unless handled", with hit-test modes, pointer
// capture, focus navigation scoped to the active layer, and "Input consumption SHALL be resolved by
// the layer stack rather than by ad-hoc flags in game code".
//
// --- SEMANTIC ACTIONS, NEVER RAW DEVICE INPUT ----------------------------------------------------
//
// "UI SHALL consume semantic actions from `input-and-actions`, never raw device input: `UI.Accept`,
// `UI.Cancel`, `UI.NavigateUp`…". `UiAction` is that list and there is no key code, no button index
// and no mouse enumeration anywhere in this header — a button that handled Enter would be a button
// that does not work on a gamepad, and this interface makes that unwritable rather than
// discouraged.
//
// Text entry is deliberately NOT an action: "Text entry SHALL use the platform text input path, not
// interface actions or key interpretation", so a text field receives `TextInput` events carrying
// UTF-8 the platform composed, including from an input method editor.
//
// --- THE LAYER STACK DECIDES CONSUMPTION ---------------------------------------------------------
//
// A modal blocks input to what is beneath it because its LAYER says so, not because game code
// checks a flag. `LayerBehaviour` is that declaration, and `route_pointer` and `route_action` walk
// the stack from the front, stopping where a layer's behaviour says to stop.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/ui/store.h>

#include <string_view>

namespace cy::ui {

/// The semantic actions the interface consumes. `input-and-actions`' names, exactly.
enum class UiAction : u8 {
    Accept = 0,
    Cancel,
    NavigateUp,
    NavigateDown,
    NavigateLeft,
    NavigateRight,
    NextTab,
    PreviousTab,
    Context,
    ScrollUp,
    ScrollDown,
    Count,
};

[[nodiscard]] const char* ui_action_name(UiAction action) noexcept;

/// The layers, back to front. The set is closed: `ui-system` names these five and a sixth would be
/// a layer whose interaction with the others nobody has decided.
enum class LayerKind : u8 { Game = 0, Hud = 1, Overlay = 2, Modal = 3, System = 4, Count = 5 };

[[nodiscard]] const char* layer_kind_name(LayerKind kind) noexcept;

/// What a layer does when it is pushed. Declared per layer rather than assumed per kind, because an
/// overlay that dims and one that does not are both overlays.
struct LayerBehaviour {
    /// Move focus into this layer, remembering what had it.
    bool captures_focus = true;
    /// Stop pointer and action routing from reaching layers beneath.
    bool blocks_input = true;
    /// Handle `UI.Cancel` by popping this layer.
    bool handles_back = true;
    /// Dim what is beneath. Carried, and applied by the paint pass.
    f32 dim_beneath = 0.0F;
    /// Seconds. Zero means no transition.
    f32 enter_seconds = 0.0F;
    f32 exit_seconds = 0.0F;
};

/// The default behaviour for a kind — the semantics `ui-system` describes, in one place, so that a
/// caller pushing a `Modal` gets modal behaviour without restating it.
[[nodiscard]] LayerBehaviour default_behaviour(LayerKind kind) noexcept;

/// One entry in the stack.
struct Layer {
    Name name;
    LayerKind kind = LayerKind::Hud;
    LayerBehaviour behaviour;
    /// The subtree this layer owns. Focus is scoped to it and hit-testing starts at it.
    ElementId root;
    /// What had focus when this layer was pushed, restored when it pops.
    ElementId restore_focus;
    /// Seconds since it was pushed, for the enter transition.
    f32 elapsed = 0.0F;
};

/// A pointer event, in the document's own units. The platform's pixels became these by dividing by
/// the UI scale, once, in the frame's first step.
struct PointerEvent {
    Vec2 position;
    /// Which pointer: a mouse is 0 and each touch has its own index, so multi-touch is tracked
    /// without a second event type.
    u32 pointer = 0;
    bool pressed = false;
    bool released = false;
    Vec2 scroll;
};

/// What routing decided, and why. The "hit-test debugging showing which element would receive a
/// pointer event and why" the diagnostics require.
struct RouteResult {
    ElementId target;
    /// The layer that consumed it, or `kNoLayer`.
    u32 layer = 0xFFFFFFFFU;
    /// How many ancestors the event bubbled through before something handled it.
    u32 bubbled = 0;
    /// True when a layer's `blocks_input` stopped the walk before reaching what is beneath.
    bool blocked_by_layer = false;
    /// True when an element had captured the pointer and received the event wherever it landed.
    bool captured = false;

    static constexpr u32 kNoLayer = 0xFFFFFFFFU;
};

/// Text the platform composed, including an input method editor's in-progress composition.
struct TextInput {
    std::string_view text;
    /// True while this is a composition rather than committed text: the field shows it inline and
    /// does not put it in its undo stack yet.
    bool composing = false;
    /// The caret within the composition, for placing the candidate window.
    u32 composition_caret = 0;
};

/// The interaction state for one interface: its layer stack, its focus, its pointers.
class Interaction {
public:
    explicit Interaction(Allocator& allocator) noexcept;

    Interaction(const Interaction&) = delete;
    Interaction& operator=(const Interaction&) = delete;

    // --- The layer stack --------------------------------------------------------------------

    [[nodiscard]] Status push_layer(const Layer& layer) noexcept;
    /// Pop the topmost layer, restoring the focus it captured.
    [[nodiscard]] Status pop_layer() noexcept;
    [[nodiscard]] Span<const Layer> layers() const noexcept { return layers_.span(); }
    [[nodiscard]] const Layer* top_layer() const noexcept;

    // --- Focus ------------------------------------------------------------------------------

    [[nodiscard]] ElementId focus() const noexcept { return focus_; }
    /// Move focus. Refused when the element is not focusable or is outside the active layer:
    /// "Focus SHALL be scoped to the active layer", and a refusal is how that is enforced rather
    /// than by callers remembering.
    [[nodiscard]] Status set_focus(const ElementStore& store, ElementId element) noexcept;
    /// Move focus in a direction. Uses the element's declared neighbour when it has one, and the
    /// nearest focusable element in that direction otherwise — both constrained to the layer.
    [[nodiscard]] ElementId navigate(const ElementStore& store, UiAction direction) noexcept;
    /// Declare an explicit neighbour, which wins over the geometric fallback.
    [[nodiscard]] Status set_neighbour(ElementId from, UiAction direction, ElementId to) noexcept;

    // --- Pointer ----------------------------------------------------------------------------

    /// Route a pointer event through the stack. Updates hover, press and capture state on the
    /// store's flags, which is what a style's `:hover` and `:active` read.
    [[nodiscard]] Expected<RouteResult, Error> route_pointer(ElementStore& store,
                                                             const PointerEvent& event) noexcept;
    /// The element that captured the pointer, or none. "WHEN a slider is pressed and the pointer
    /// moves outside it THEN the slider SHALL continue receiving move events until release."
    [[nodiscard]] ElementId capture(u32 pointer) const noexcept;
    [[nodiscard]] Status set_capture(u32 pointer, ElementId element) noexcept;

    /// Which element the pointer is over, without changing anything. What the hit-test debugger
    /// calls.
    [[nodiscard]] RouteResult probe(const ElementStore& store, Vec2 position) const noexcept;

    // --- Actions ----------------------------------------------------------------------------

    /// Route a semantic action. Returns the element that handled it, or none.
    [[nodiscard]] Expected<RouteResult, Error> route_action(ElementStore& store,
                                                            UiAction action) noexcept;

    /// Elements that entered or left the pointer this frame, for enter and exit events.
    [[nodiscard]] Span<const ElementId> entered() const noexcept { return entered_.span(); }
    [[nodiscard]] Span<const ElementId> exited() const noexcept { return exited_.span(); }
    void clear_events() noexcept;

private:
    struct Neighbour {
        ElementId from;
        UiAction direction = UiAction::NavigateUp;
        ElementId to;
    };

    struct Capture {
        u32 pointer = 0;
        ElementId element;
    };

    [[nodiscard]] static bool within_layer(const ElementStore& store, ElementId element,
                                           ElementId root) noexcept;
    [[nodiscard]] ElementId hit_test(const ElementStore& store, ElementId root, Vec2 position,
                                     u32& bubbled) const noexcept;

    Array<Layer> layers_;
    Array<Neighbour> neighbours_;
    Array<Capture> captures_;
    Array<ElementId> entered_;
    Array<ElementId> exited_;
    Array<ElementId> hovered_;
    ElementId focus_;
};

}  // namespace cy::ui

#endif  // CY_UI_INTERACTION_H
