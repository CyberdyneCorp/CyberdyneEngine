// SPDX-License-Identifier: MIT
#ifndef CY_UI_TOOLKIT_TOOLKIT_H
#define CY_UI_TOOLKIT_TOOLKIT_H
// The widget toolkit: a retained widget tree on CyberUI's element store, with its widgets, its
// input, its focus, its styles, its animations and its bindings. Issue #91, stages 4 and 5.
//
// ================================================================================================
// WHAT A WIDGET IS
// ================================================================================================
//
// A widget is a few ELEMENTS of the store, and nothing else draws it: a button is a focusable box
// with a label inside, a slider a track, a fill and a thumb, a list a clipped viewport over a pool
// of rows. Each element's type name is the widget's (`button`) or its part's (`button-label`), so
// `.cyss` styles every one of them by type, class, id and state — `slider-thumb:hover` is a
// selector like any other — and layout, flattening and the device renderer see an interface they
// already draw. A widget is addressed by its ROOT element's id (`WidgetId`), so the same handle is
// a store element, an ABI `CyUiElement` and a key a document and a binding name.
//
// ================================================================================================
// ONE FRAME
// ================================================================================================
//
//     toolkit.pointer({position, down, scroll});   // routing: hover, press, drag, click, wheel
//     toolkit.action(UiAction::NavigateDown);      // focus, activation, tabs, scrolling
//     toolkit.text({"é", false}); toolkit.edit(TextCommand::DeleteBackward, false);
//     toolkit.update(dt, scale_settings, viewport); // bindings, animations, styles, layout
//     ui::flatten(store, viewport_rect, buffer, report, &text);   // what it always was
//     for (const WidgetEvent& event : toolkit.events()) { ... }  toolkit.clear_events();
//
// `pointer` and `action` return whether the interface CONSUMED the input: a press on a button, a
// navigation that moved focus, anything at all while a modal layer is open. Input the interface
// did not consume belongs to the game — "input routing so UI consumes events before gameplay" —
// and `cy::ui-input`'s bridge turns that answer into the input user's focus layer, so gameplay
// actions bound to the same controls are suppressed by the input system, not by game code.
//
// ================================================================================================
// FOCUS, NAVIGATION AND THE RING
// ================================================================================================
//
// Focus is `Interaction`'s: directional navigation (declared neighbours, then the nearest focusable
// element that way, inside the active layer), TAB ORDER (`UiAction::FocusNext` and `FocusPrevious`,
// document order, wrapping), and modal scope. The FOCUS RING is one element in the toolkit's
// overlay, placed around the focused widget after layout and shown only when focus moved by a
// keyboard or a gamepad — a mouse click focuses without drawing it, which is what a player expects
// and what CSS calls `:focus-visible`. A focused slider takes `NavigateLeft` and `NavigateRight` as
// steps; a focused list takes `NavigateUp` and `NavigateDown` as selection until its ends.
//
// ================================================================================================
// STYLE, STATE AND TRANSITIONS
// ================================================================================================
//
// Every element the toolkit makes is styled by the cascade `style.h` documents: the theme (the
// toolkit's built-in one unless replaced), the sheet, and the widget's inline declarations. A
// part's pseudo-states are its WIDGET's — hovering a slider hovers its thumb — so a sheet styles a
// whole widget's states without the parts' own flags. A change of state restyles that widget alone.
// A paint property that changes under a `transition-duration` is INTERPOLATED from what was shown
// to the new value, with the element's `transition-timing-function` and `transition-delay`; a
// layout property changes at once and relays out.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/memory/array.h>
#include <cy/core/values/name.h>
#include <cy/ui/interaction.h>
#include <cy/ui/layout.h>
#include <cy/ui/paint.h>
#include <cy/ui/store.h>
#include <cy/ui/style.h>
#include <cy/ui/text/text_painter.h>
#include <cy/ui/toolkit/animation.h>
#include <cy/ui/toolkit/text_edit.h>
#include <cy/ui/widgets.h>

#include <string_view>

namespace cy::ui {

/// A widget is its root element.
using WidgetId = ElementId;

/// What a widget is. Each is a type name in the store and a type selector in `.cyss`.
enum class WidgetKind : u8 {
    /// Children placed by anchors and offsets: `LayoutModel::Absolute`.
    Panel = 0,
    /// Children in a flex row or column.
    Row,
    Column,
    /// Children in the cells of `WidgetDesc::columns` columns.
    Grid,
    /// Children layered over one another, each filling it.
    Stack,
    /// A clipped viewport over its children, scrolled by the wheel, `UI.ScrollUp` and
    /// `UI.ScrollDown`, and by focus moving into a child it hides.
    ScrollView,
    Label,
    /// Text in runs of colours and sizes with inline images, from markup — see `set_text`.
    RichText,
    Image,
    Button,
    /// A switch: `checked` on or off.
    Toggle,
    Checkbox,
    /// One choice of `items`, each a `Radio`.
    RadioGroup,
    Slider,
    ProgressBar,
    /// One line of editable text: caret, selection, composition, undo, clipboard.
    TextField,
    /// A button showing the selected item of `items`, opening a list of them over the interface.
    Dropdown,
    /// A virtualised list: only the rows that can be seen exist, however many items there are.
    ListView,
    /// A bar of tabs over pages; every child is a page, titled by its `title`.
    Tabs,
    /// A modal dialog: shown by `open`, over a dimmed backdrop, in a layer of its own.
    Dialog,
    // --- Made by the widgets above, and addressable for their events and their styles ----------
    Radio,
    Tab,
    ListRow,
    /// One item of an open drop-down's list.
    Option,
    Count,
};

[[nodiscard]] const char* widget_kind_name(WidgetKind kind) noexcept;
/// The kind a name spells, or `Count`.
[[nodiscard]] WidgetKind widget_kind_from_name(std::string_view name) noexcept;

/// Everything a widget is created with. What a kind does not use it ignores.
struct WidgetDesc {
    WidgetKind kind = WidgetKind::Panel;
    /// The author's id: `#id` in a sheet, `find()`'s key, a document's and a binding's name.
    std::string_view id;
    /// Space-separated classes, at most four: `.primary .wide`.
    std::string_view classes;
    /// A label's, button's, toggle's or checkbox's caption; a text field's initial text; a rich
    /// text's markup.
    std::string_view text;
    /// A text field's hint while it is empty.
    std::string_view placeholder;
    /// A page's title in its `Tabs`, or a dialog's.
    std::string_view title;
    /// A radio group's, drop-down's or list's items.
    Span<const std::string_view> items;
    f32 value = 0.0F;
    f32 minimum = 0.0F;
    f32 maximum = 1.0F;
    /// A slider's step: what a gamepad press moves it by, and what a drag snaps to. Zero is a
    /// hundredth of the range for a press and no snapping.
    f32 step = 0.0F;
    /// `Grid`: how many columns.
    u16 columns = 0;
    /// `Image`: the atlas page, the region of it, and its natural size in reference units.
    u16 image_page = 0;
    Rect image_uv{0.0F, 0.0F, 1.0F, 1.0F};
    Vec2 image_size{0.0F, 0.0F};
    /// `TextField`: the most bytes it holds (zero: no limit) and what it accepts.
    u32 max_length = 0;
    TextMask mask = TextMask::Any;
    /// `ListView`: one row's height.
    f32 row_height = 22.0F;
    bool checked = false;
    bool enabled = true;
};

/// What happened, for whoever reads `events()`.
enum class WidgetEventKind : u8 {
    /// A button was pressed and released over it, or accepted with focus on it.
    Clicked = 0,
    /// A value moved by the player's hand: a slider, a toggle, a selection, a text field's text.
    /// `value` and `index` say what it is now. A game's own `set_*` raises none.
    Changed,
    /// A text field's Enter, or `UI.Accept` on a list's selected row.
    Submitted,
    FocusGained,
    FocusLost,
    /// A drop-down's list or a dialog opened or closed.
    Opened,
    Closed,
    Count,
};

[[nodiscard]] const char* widget_event_kind_name(WidgetEventKind kind) noexcept;

struct WidgetEvent {
    WidgetEventKind kind = WidgetEventKind::Clicked;
    WidgetId widget;
    /// The widget's author id, for a reader that dispatches by name.
    Name id;
    /// A slider's or progress bar's value, a toggle's 0 or 1.
    f32 value = 0.0F;
    /// A selection's index.
    u32 index = 0;
};

/// The input a pointer gives the toolkit, once a frame or per platform event, in REFERENCE units:
/// the platform's pixels divided by the UI scale once, by the caller.
struct PointerInput {
    Vec2 position{0.0F, 0.0F};
    /// The primary button is down. A press and a release are edges the toolkit derives.
    bool down = false;
    /// Wheel notches, positive down.
    Vec2 scroll{0.0F, 0.0F};
    u32 pointer = 0;
};

/// A list's items when they are the game's: a ten-thousand-slot inventory is not copied into the
/// interface, it is asked for the rows that are on screen.
class ListSource {
public:
    ListSource() = default;
    virtual ~ListSource() = default;
    ListSource(const ListSource&) = delete;
    ListSource& operator=(const ListSource&) = delete;
    ListSource(ListSource&&) = delete;
    ListSource& operator=(ListSource&&) = delete;

    [[nodiscard]] virtual u32 count() const noexcept = 0;
    /// Item `index`'s text, appended to `out`.
    [[nodiscard]] virtual Status text(u32 index, Array<char>& out) const noexcept = 0;
};

/// What a binding drives.
enum class WidgetProperty : u8 {
    /// A slider's or progress bar's value; a number.
    Value = 0,
    /// A toggle's or checkbox's state; a boolean.
    Checked,
    /// A label's, button's or text field's text; text.
    Text,
    /// A radio group's, drop-down's, list's or tabs' selection; a number.
    Selected,
    /// Shown or collapsed; a boolean.
    Visible,
    Enabled,
    Count,
};

/// The toolkit's settings, which a game changes at run time.
struct ToolkitSettings {
    /// The platform's reduced-motion preference: decorative animations jump to their end.
    bool reduced_motion = false;
    /// Seconds the pointer rests on a widget with a tooltip before it shows.
    f32 tooltip_delay = 0.5F;
    /// Reference units a wheel notch, `UI.ScrollUp` or `UI.ScrollDown` scrolls by.
    f32 scroll_step = 40.0F;
    /// The caret's blink period; zero holds it on.
    f32 caret_blink = 1.0F;
};

/// What one `update` did. The layout half is `ui::layout`'s own report.
struct ToolkitReport {
    LayoutReport layout;
    /// Elements whose style was resolved again.
    u32 restyled = 0;
    u32 animations = 0;
    u32 transitions = 0;
    u32 bindings_applied = 0;
    /// List rows given a new item this frame.
    u32 rows_realised = 0;
};

/// The built-in theme: every widget's default look, in `.cyss`. A game's sheet overrides any of it.
[[nodiscard]] std::string_view default_theme_source() noexcept;

class Toolkit {
public:
    /// `store` and `text` are the embedder's and outlive the toolkit; `text` must be started, and
    /// is the measurer and painter the embedder hands `layout` and `flatten`.
    Toolkit(Allocator& allocator, ElementStore& store, TextPainter& text) noexcept;
    ~Toolkit();

    Toolkit(const Toolkit&) = delete;
    Toolkit& operator=(const Toolkit&) = delete;
    Toolkit(Toolkit&&) = delete;
    Toolkit& operator=(Toolkit&&) = delete;

    /// Make the root (an absolute element covering the window — a new root, or `root` when given),
    /// the overlay above it for focus rings, tooltips, drop-downs and dialogs, and the HUD layer.
    /// Parses the built-in theme.
    [[nodiscard]] Status start(ElementId root = kNoElement) noexcept;
    [[nodiscard]] WidgetId root() const noexcept { return root_; }

    // --- The tree -------------------------------------------------------------------------------

    /// Create a widget under `parent` (a widget, or the root when null).
    [[nodiscard]] Expected<WidgetId, Error> create(WidgetId parent,
                                                   const WidgetDesc& desc) noexcept;
    /// Destroy a widget and every widget under it.
    [[nodiscard]] Status destroy(WidgetId widget) noexcept;
    [[nodiscard]] bool alive(WidgetId widget) const noexcept;
    [[nodiscard]] WidgetKind kind(WidgetId widget) const noexcept;
    /// The widget a widget was created under, or none for the root's children.
    [[nodiscard]] WidgetId parent(WidgetId widget) const noexcept;
    /// The first widget at or under `scope` (the root when null) whose id is `id`, depth first.
    [[nodiscard]] WidgetId find(WidgetId scope, std::string_view id) const noexcept;
    [[nodiscard]] Name id_of(WidgetId widget) const noexcept;
    /// The widget an element belongs to — itself for a root, its widget for a part — or none.
    [[nodiscard]] WidgetId widget_of(ElementId element) const noexcept;
    /// A widget's children, in order, appended to `out`.
    [[nodiscard]] Status children(WidgetId widget, Array<WidgetId>& out) const noexcept;
    /// Put a widget's children (the root's, when null) in `order` — what a reloaded document
    /// whose lines moved does. Children not named keep their place before the ones that are.
    [[nodiscard]] Status reorder(WidgetId parent, Span<const WidgetId> order) noexcept;
    [[nodiscard]] u32 widget_count() const noexcept { return live_; }
    /// A part of a widget by its type name (`slider-thumb`), for a suite and an inspector.
    [[nodiscard]] ElementId part(WidgetId widget, std::string_view type) const noexcept;

    // --- Values ---------------------------------------------------------------------------------

    /// A caption, a text field's text, or a rich text's markup. Markup is `[color=#rrggbb]...
    /// [/color]`, `[size=N]...[/size]` and `[img=page,u,v,w,h]`; `[[` is a bracket.
    [[nodiscard]] Status set_text(WidgetId widget, std::string_view text) noexcept;
    /// The text a widget shows or holds; a text field's committed text.
    [[nodiscard]] std::string_view text(WidgetId widget) const noexcept;
    /// A slider's or progress bar's value, clamped to its range; a toggle's or checkbox's 0 or 1.
    [[nodiscard]] Status set_value(WidgetId widget, f32 value) noexcept;
    [[nodiscard]] f32 value(WidgetId widget) const noexcept;
    [[nodiscard]] Status set_range(WidgetId widget, f32 minimum, f32 maximum, f32 step) noexcept;
    [[nodiscard]] Status set_checked(WidgetId widget, bool checked) noexcept;
    [[nodiscard]] bool checked(WidgetId widget) const noexcept;
    /// A radio group's, drop-down's, list's or tabs' selected index.
    [[nodiscard]] Status set_selected(WidgetId widget, u32 index) noexcept;
    [[nodiscard]] u32 selected(WidgetId widget) const noexcept;
    /// A radio group's, drop-down's or list's items, copied.
    [[nodiscard]] Status set_items(WidgetId widget, Span<const std::string_view> items) noexcept;
    /// A list's items from the game, asked for only the rows on screen. Null goes back to the
    /// items `set_items` copied. The source outlives the list or is replaced first.
    [[nodiscard]] Status set_list_source(WidgetId widget, const ListSource* source) noexcept;
    /// How many items a radio group, drop-down or list has.
    [[nodiscard]] u32 item_count(WidgetId widget) const noexcept;
    /// Item `index`'s text, appended to `out`.
    [[nodiscard]] Status item_text(WidgetId widget, u32 index, Array<char>& out) const noexcept;
    /// The game tells the list its items changed in place: the visible rows are asked again.
    void refresh_items(WidgetId widget) noexcept;
    [[nodiscard]] Status set_enabled(WidgetId widget, bool enabled) noexcept;
    [[nodiscard]] bool enabled(WidgetId widget) const noexcept;
    /// Shown, or collapsed out of layout.
    [[nodiscard]] Status set_visible(WidgetId widget, bool visible) noexcept;
    [[nodiscard]] bool visible(WidgetId widget) const noexcept;
    [[nodiscard]] Status set_classes(WidgetId widget, std::string_view classes) noexcept;
    /// An inline declaration on the widget's root — the strongest source in the cascade.
    [[nodiscard]] Status set_style(WidgetId widget, StyleProperty property,
                                   const StyleValue& value) noexcept;
    /// Parse `declarations` (`width: 120; gap: 4`) as inline declarations, replacing any.
    [[nodiscard]] Status set_inline_style(WidgetId widget, std::string_view declarations) noexcept;
    /// Where a widget sits in a `Panel`: anchors as fractions of the parent, offsets in units.
    [[nodiscard]] Status set_anchors(WidgetId widget, Vec2 anchor_min, Vec2 anchor_max,
                                     Vec2 offset_min, Vec2 offset_max) noexcept;
    /// A tooltip shown when the pointer rests on the widget. Empty removes it.
    [[nodiscard]] Status set_tooltip(WidgetId widget, std::string_view text) noexcept;
    /// A drop-down's list or a dialog: open shows it in a layer of its own and moves focus into
    /// it; close puts focus back where it was.
    [[nodiscard]] Status open(WidgetId widget) noexcept;
    [[nodiscard]] Status close(WidgetId widget) noexcept;
    [[nodiscard]] bool is_open(WidgetId widget) const noexcept;
    /// A scroll view's or list's scroll offset, in reference units from the top.
    [[nodiscard]] Status set_scroll(WidgetId widget, f32 offset) noexcept;
    [[nodiscard]] f32 scroll(WidgetId widget) const noexcept;

    // --- Focus ----------------------------------------------------------------------------------

    /// Move focus to a widget, as navigation would — the ring shows.
    [[nodiscard]] Status focus(WidgetId widget) noexcept;
    /// Take focus from whatever has it — a game going back to gameplay — so no widget answers
    /// navigation and the input bridge hands the controls back.
    void clear_focus() noexcept;
    /// The widget with focus, or none.
    [[nodiscard]] WidgetId focused() const noexcept;
    /// Whether the focus ring is drawn: focus moved by a keyboard or a gamepad, not a pointer.
    [[nodiscard]] bool focus_visible() const noexcept { return focus_visible_; }
    /// Declare where `UI.Navigate*` goes from a widget, over the geometric guess.
    [[nodiscard]] Status set_neighbour(WidgetId from, UiAction direction, WidgetId to) noexcept;
    [[nodiscard]] const Interaction& interaction() const noexcept { return interaction_; }

    // --- Input ----------------------------------------------------------------------------------

    /// Route the pointer. Returns true when the interface consumed it.
    [[nodiscard]] bool pointer(const PointerInput& input) noexcept;
    /// Route a semantic action. Returns true when the interface consumed it.
    [[nodiscard]] bool action(UiAction action) noexcept;
    /// Text the platform composed, to the focused text field. Committed text is inserted; a
    /// composition is shown at the caret until committed or cleared.
    [[nodiscard]] bool text_input(const TextInput& input) noexcept;
    /// An editing command, to the focused text field.
    [[nodiscard]] bool edit(TextCommand command, bool extend) noexcept;
    /// True while a text field has focus: the platform should be delivering text and its input
    /// method should be on.
    [[nodiscard]] bool wants_text_input() const noexcept;
    /// Where the platform's IME candidate window goes: the caret's rect in reference units, under
    /// the composition. False when no text field has focus.
    [[nodiscard]] bool text_input_rect(Rect& out) const noexcept;
    /// True while the interface holds the input: a dialog or a drop-down is open, or a text field
    /// has focus. What `cy::ui-input` suppresses gameplay by.
    [[nodiscard]] bool captures_input() const noexcept;
    void set_clipboard(Clipboard* clipboard) noexcept;

    // --- Events ---------------------------------------------------------------------------------

    [[nodiscard]] Span<const WidgetEvent> events() const noexcept { return events_.span(); }
    void clear_events() noexcept { events_.clear(); }

    // --- Styles ---------------------------------------------------------------------------------

    /// Replace the game's sheet (the theme stays under it). Diagnostics are kept in the sheet; a
    /// sheet with errors still applies what parsed, as a hot reload should.
    [[nodiscard]] Status set_stylesheet(std::string_view source) noexcept;
    /// Replace the theme — the base every sheet overrides. Empty is the built-in theme.
    [[nodiscard]] Status set_theme(std::string_view source) noexcept;
    [[nodiscard]] const StyleSheet& stylesheet() const noexcept { return sheet_; }
    [[nodiscard]] const StyleSheet& theme_sheet() const noexcept { return theme_sheet_; }

    // --- Animation ------------------------------------------------------------------------------

    /// Animate one paint property of a widget's root. Starts from `from`, not from what is shown.
    [[nodiscard]] Status animate(WidgetId widget, const Animation& animation) noexcept;
    /// The same animation on each widget, each `step` seconds after the one before: a staggered
    /// entrance, evaluated here rather than by the caller.
    [[nodiscard]] Status animate_staggered(Span<const WidgetId> widgets, const Animation& animation,
                                           f32 step) noexcept;
    /// Fade, slide and pop, the three every interface wants, as one call each.
    [[nodiscard]] Status fade(WidgetId widget, f32 from, f32 to, f32 seconds) noexcept;
    [[nodiscard]] Status slide(WidgetId widget, Vec2 from, f32 seconds) noexcept;
    [[nodiscard]] Status pop(WidgetId widget, f32 seconds) noexcept;
    /// Stop a widget's animations, leaving what they last set.
    void stop_animations(WidgetId widget) noexcept;
    [[nodiscard]] u32 running_animations() const noexcept;
    [[nodiscard]] ToolkitSettings& settings() noexcept { return settings_; }

    // --- Binding --------------------------------------------------------------------------------

    /// Drive a widget's property from a source, and — `TwoWay` — write the player's changes back.
    /// While the player is editing (a text field with focus, a slider being dragged) the source's
    /// changes do not overwrite the field: "external changes SHALL update the field unless it is
    /// being edited". The source outlives the binding or is unbound first.
    [[nodiscard]] Status bind(WidgetId widget, WidgetProperty property, BindingSource& source,
                              BindingMode mode = BindingMode::OneWay) noexcept;
    void unbind(WidgetId widget) noexcept;

    // --- The frame ------------------------------------------------------------------------------

    /// Bindings, animations and transitions, styles, the lists' rows, layout, and what is placed
    /// after layout — the focus ring, the caret, an open drop-down's list and a tooltip.
    [[nodiscard]] Status update(f32 seconds, const ScaleSettings& settings, Vec2 viewport,
                                ToolkitReport* report = nullptr) noexcept;

    [[nodiscard]] ElementStore& store() noexcept { return store_; }
    [[nodiscard]] TextPainter& text_painter() noexcept { return text_; }

private:
    static constexpr u32 kNone = 0xFFFFFFFFU;
    static constexpr u32 kMaxParts = 6;
    static constexpr u32 kMaxClasses = 4;

    struct Widget;
    struct Part;
    struct Track;
    struct BoundWidget;

    // toolkit.cpp — the tree.
    [[nodiscard]] Widget* record(WidgetId widget) noexcept;
    [[nodiscard]] const Widget* record(WidgetId widget) const noexcept;
    [[nodiscard]] Part* part_record(ElementId element) noexcept;
    [[nodiscard]] const Part* part_record(ElementId element) const noexcept;
    [[nodiscard]] Expected<ElementId, Error> make_element(ElementId parent, const char* type,
                                                          u32 widget) noexcept;
    [[nodiscard]] Status build(Widget& widget, const WidgetDesc& desc) noexcept;
    [[nodiscard]] Status attach_to_parent(Widget& widget, WidgetId parent) noexcept;
    void forget(ElementId element) noexcept;
    /// Destroy every dialog made under `holder` or under its widgets: they live in the overlay.
    [[nodiscard]] Status destroy_held_dialogs(WidgetId holder) noexcept;
    /// Close the drop-down list and the dialogs `doomed` holds, before it is destroyed.
    [[nodiscard]] Status close_layers_in(Span<const ElementId> doomed) noexcept;
    void push_event(WidgetEventKind kind, const Widget& widget) noexcept;
    [[nodiscard]] Status set_caption(Widget& widget, std::string_view text) noexcept;
    [[nodiscard]] Status set_element_text(ElementId element, std::string_view text) noexcept;
    /// Set an element's text again in `style`, keeping its text and its colour spans.
    [[nodiscard]] Status restyle_text(ElementId element, const TextStyle& style) noexcept;
    [[nodiscard]] Status restyle_element(ElementId element) noexcept;
    [[nodiscard]] Status restyle_subtree(ElementId element) noexcept;
    void mark_style(ElementId element) noexcept;
    void mark_widget_style(const Widget& widget) noexcept;
    [[nodiscard]] Status update_parts(Widget& widget) noexcept;
    [[nodiscard]] Status build_rich_text(Widget& widget) noexcept;
    [[nodiscard]] Status refresh_items(Widget& widget) noexcept;

    // toolkit_input.cpp — pointer, actions, text.
    [[nodiscard]] Widget* interactive_widget(ElementId element) noexcept;
    [[nodiscard]] bool activate(Widget& widget) noexcept;
    [[nodiscard]] bool handle_press(Widget& widget, Vec2 position) noexcept;
    [[nodiscard]] bool handle_drag(Widget& widget, Vec2 position) noexcept;
    [[nodiscard]] bool handle_scroll(ElementId target, f32 notches) noexcept;
    [[nodiscard]] bool handle_widget_action(Widget& widget, UiAction action) noexcept;
    [[nodiscard]] bool handle_tabs_action(UiAction action) noexcept;
    [[nodiscard]] bool handle_layer_cancel() noexcept;
    void note_focus() noexcept;
    void scroll_into_view(ElementId element) noexcept;
    [[nodiscard]] Status user_changed(Widget& widget) noexcept;
    [[nodiscard]] Status select(Widget& widget, u32 index, bool by_player) noexcept;
    [[nodiscard]] Status set_slider_from(Widget& widget, f32 x) noexcept;
    [[nodiscard]] Status place_caret(Widget& widget, f32 x, bool extend) noexcept;
    [[nodiscard]] Status refresh_text_field(Widget& widget) noexcept;
    [[nodiscard]] Status open_dropdown(Widget& widget) noexcept;
    [[nodiscard]] Status close_dropdown() noexcept;
    [[nodiscard]] Status open_dialog(Widget& widget) noexcept;
    [[nodiscard]] Status close_dialog(Widget& widget) noexcept;

    // toolkit_style.cpp — the cascade, transitions and animations.
    [[nodiscard]] Status restyle() noexcept;
    [[nodiscard]] Status advance_tracks(f32 seconds) noexcept;
    [[nodiscard]] Status apply_paint(ElementId element) noexcept;
    [[nodiscard]] Status start_transition(Part& part, u32 property, f32 from, f32 to, u32 from_c,
                                          u32 to_c) noexcept;
    void sync_states() noexcept;

    // toolkit_list.cpp — scrolling, virtual rows, and what layout places.
    [[nodiscard]] Status realise_lists(u32& realised) noexcept;
    [[nodiscard]] Status realise_list(Widget& widget, u32& realised) noexcept;
    [[nodiscard]] Status place_thumb(ElementId thumb, f32 window, f32 total, f32 scroll) noexcept;
    [[nodiscard]] Status place_overlays(f32 seconds) noexcept;
    [[nodiscard]] Status update_bindings(u32& applied) noexcept;
    [[nodiscard]] Status write_back(Widget& widget, WidgetProperty property) noexcept;
    [[nodiscard]] f32 max_scroll(const Widget& widget) const noexcept;

    Allocator* allocator_;
    ElementStore& store_;
    TextPainter& text_;
    Interaction interaction_;
    StyleSheet theme_sheet_;
    StyleSheet sheet_;
    Theme theme_;
    MemoryClipboard memory_clipboard_;
    Clipboard* clipboard_ = nullptr;
    ToolkitSettings settings_;

    Array<Widget> widgets_;
    Array<u32> free_widgets_;
    /// Per element index: the part record, or `kNone`.
    Array<u32> part_of_;
    Array<Part> parts_;
    Array<u32> free_parts_;
    Array<Track> tracks_;
    Array<BoundWidget> bindings_;
    Array<WidgetEvent> events_;
    /// Elements whose style must be resolved again this frame.
    Array<ElementId> restyle_;
    bool restyle_all_ = true;
    Array<char> scratch_;

    ElementId root_;
    ElementId overlay_;
    ElementId focus_ring_;
    ElementId tooltip_;
    ElementId tooltip_label_;
    /// An open drop-down's list, the drop-down it belongs to, and its options.
    ElementId popup_;
    WidgetId popup_owner_;
    /// The dialog open on top, if any; dialogs stack with their layers.
    Array<WidgetId> dialogs_;

    WidgetId pressed_;
    WidgetId hovered_;
    WidgetId last_focus_;
    WidgetId tooltip_target_;
    Vec2 pointer_position_{0.0F, 0.0F};
    u32 style_frame_ = 0;
    f32 hover_seconds_ = 0.0F;
    f32 caret_seconds_ = 0.0F;
    bool pointer_down_ = false;
    bool dragging_ = false;
    bool focus_visible_ = false;
    u32 live_ = 0;
};

}  // namespace cy::ui

#endif  // CY_UI_TOOLKIT_TOOLKIT_H
