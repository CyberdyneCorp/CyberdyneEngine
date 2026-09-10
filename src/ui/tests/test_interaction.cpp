// Input routing, focus, navigation and the layer stack. M8.b task 9.2.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/ui/interaction.h>
#include <cy/ui/layout.h>

using namespace cy;
using namespace cy::ui;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Engine);
}

[[nodiscard]] ElementId add(ElementStore& store, ElementId parent, const char* type) noexcept {
    auto created = store.create(parent, Name::intern(type));
    return created.has_value() ? created.value() : kNoElement;
}

/// Place an element directly, so a routing case does not have to run a layout to get a rect.
void place(ElementStore& store, ElementId element, Rect rect) noexcept {
    LayoutOutput* output = store.layout_output(element);
    output->rect = rect;
    output->clip = Rect{0.0F, 0.0F, 1920.0F, 1080.0F};
}

[[nodiscard]] ElementId button(ElementStore& store, ElementId parent, Rect rect) noexcept {
    const ElementId element = add(store, parent, "button");
    place(store, element, rect);
    (void)store.set_flags(element, ElementFlags::Visible | ElementFlags::Focusable);
    return element;
}

}  // namespace

CY_TEST_CASE("ui_input: a hit lands on the topmost element and an ignored one is transparent") {
    ElementStore store(allocator());
    Interaction interaction(allocator());
    const ElementId root = add(store, kNoElement, "panel");
    place(store, root, Rect{0.0F, 0.0F, 800.0F, 600.0F});
    const ElementId back = button(store, root, Rect{0.0F, 0.0F, 200.0F, 100.0F});
    const ElementId front = button(store, root, Rect{50.0F, 20.0F, 100.0F, 40.0F});

    Layer layer;
    layer.kind = LayerKind::Hud;
    layer.behaviour = default_behaviour(LayerKind::Hud);
    layer.root = root;
    CY_REQUIRE(interaction.push_layer(layer).has_value());

    // The later sibling is drawn on top, so it is hit first.
    CY_CHECK_EQ(interaction.probe(store, Vec2{60.0F, 30.0F}).target, front);
    CY_CHECK_EQ(interaction.probe(store, Vec2{10.0F, 90.0F}).target, back);

    // AN IGNORED ELEMENT IS TRANSPARENT: the hit falls through to what is beneath.
    CY_REQUIRE(store.set_hit_test_mode(front, HitTestMode::Ignore).has_value());
    CY_CHECK_EQ(interaction.probe(store, Vec2{60.0F, 30.0F}).target, back);
}

CY_TEST_CASE("ui_input: an element outside its clip is not hit") {
    ElementStore store(allocator());
    Interaction interaction(allocator());
    const ElementId root = add(store, kNoElement, "panel");
    place(store, root, Rect{0.0F, 0.0F, 800.0F, 600.0F});
    const ElementId row = button(store, root, Rect{0.0F, 200.0F, 100.0F, 40.0F});
    // A scroll container's clip: the row is below it.
    store.layout_output(row)->clip = Rect{0.0F, 0.0F, 200.0F, 100.0F};

    Layer layer;
    layer.root = root;
    layer.behaviour = default_behaviour(LayerKind::Hud);
    CY_REQUIRE(interaction.push_layer(layer).has_value());
    CY_CHECK_NE(interaction.probe(store, Vec2{10.0F, 210.0F}).target, row);
}

CY_TEST_CASE("ui_input: a pressed element keeps the pointer until it is released") {
    // "WHEN a slider is pressed and the pointer moves outside it THEN the slider SHALL continue
    // receiving move events until release."
    ElementStore store(allocator());
    Interaction interaction(allocator());
    const ElementId root = add(store, kNoElement, "panel");
    place(store, root, Rect{0.0F, 0.0F, 800.0F, 600.0F});
    const ElementId slider = button(store, root, Rect{0.0F, 0.0F, 100.0F, 40.0F});

    Layer layer;
    layer.root = root;
    layer.behaviour = default_behaviour(LayerKind::Overlay);
    CY_REQUIRE(interaction.push_layer(layer).has_value());

    PointerEvent press;
    press.position = Vec2{50.0F, 20.0F};
    press.pressed = true;
    auto pressed = interaction.route_pointer(store, press);
    CY_REQUIRE(pressed.has_value());
    CY_CHECK_EQ(pressed.value().target, slider);
    CY_CHECK_EQ(interaction.capture(0), slider);

    PointerEvent drag;
    drag.position = Vec2{500.0F, 500.0F};  // far outside
    auto dragged = interaction.route_pointer(store, drag);
    CY_REQUIRE(dragged.has_value());
    CY_CHECK_EQ(dragged.value().target, slider);
    CY_CHECK(dragged.value().captured);

    PointerEvent release;
    release.position = Vec2{500.0F, 500.0F};
    release.released = true;
    auto released = interaction.route_pointer(store, release);
    CY_REQUIRE(released.has_value());
    CY_CHECK_FALSE(interaction.capture(0).is_valid());
    CY_CHECK_FALSE(has_flag(store.flags(slider), ElementFlags::Pressed));
}

CY_TEST_CASE("ui_input: hover produces enter and exit, which a style reads as :hover") {
    ElementStore store(allocator());
    Interaction interaction(allocator());
    const ElementId root = add(store, kNoElement, "panel");
    place(store, root, Rect{0.0F, 0.0F, 800.0F, 600.0F});
    const ElementId first = button(store, root, Rect{0.0F, 0.0F, 100.0F, 40.0F});
    const ElementId second = button(store, root, Rect{200.0F, 0.0F, 100.0F, 40.0F});

    Layer layer;
    layer.root = root;
    layer.behaviour = default_behaviour(LayerKind::Overlay);
    CY_REQUIRE(interaction.push_layer(layer).has_value());

    PointerEvent over_first;
    over_first.position = Vec2{50.0F, 20.0F};
    CY_REQUIRE(interaction.route_pointer(store, over_first).has_value());
    CY_CHECK(has_flag(store.flags(first), ElementFlags::Hovered));
    CY_REQUIRE_EQ(interaction.entered().size(), 1U);
    CY_CHECK_EQ(interaction.entered()[0], first);

    interaction.clear_events();
    PointerEvent over_second;
    over_second.position = Vec2{250.0F, 20.0F};
    CY_REQUIRE(interaction.route_pointer(store, over_second).has_value());
    CY_CHECK_FALSE(has_flag(store.flags(first), ElementFlags::Hovered));
    CY_CHECK(has_flag(store.flags(second), ElementFlags::Hovered));
    CY_REQUIRE_EQ(interaction.exited().size(), 1U);
    CY_CHECK_EQ(interaction.exited()[0], first);
}

CY_TEST_CASE(
    "ui_focus: navigation takes the declared neighbour, then the nearest in that direction") {
    // "WHEN `UI.NavigateRight` is triggered THEN focus SHALL move to the explicitly declared right
    // neighbour, or to the nearest focusable element in that direction within the active layer."
    ElementStore store(allocator());
    Interaction interaction(allocator());
    const ElementId root = add(store, kNoElement, "panel");
    place(store, root, Rect{0.0F, 0.0F, 800.0F, 600.0F});
    const ElementId left = button(store, root, Rect{0.0F, 0.0F, 100.0F, 40.0F});
    const ElementId middle = button(store, root, Rect{150.0F, 0.0F, 100.0F, 40.0F});
    const ElementId far_right = button(store, root, Rect{600.0F, 0.0F, 100.0F, 40.0F});
    const ElementId below = button(store, root, Rect{0.0F, 200.0F, 100.0F, 40.0F});

    Layer layer;
    layer.root = root;
    layer.behaviour = default_behaviour(LayerKind::Overlay);
    CY_REQUIRE(interaction.push_layer(layer).has_value());
    CY_REQUIRE(interaction.set_focus(store, left).has_value());

    // Geometric: the nearest focusable element to the right.
    CY_CHECK_EQ(interaction.navigate(store, UiAction::NavigateRight), middle);
    CY_REQUIRE(interaction.set_focus(store, left).has_value());
    CY_CHECK_EQ(interaction.navigate(store, UiAction::NavigateDown), below);

    // An explicit neighbour wins over the geometry.
    CY_REQUIRE(interaction.set_focus(store, left).has_value());
    CY_REQUIRE(interaction.set_neighbour(left, UiAction::NavigateRight, far_right).has_value());
    CY_CHECK_EQ(interaction.navigate(store, UiAction::NavigateRight), far_right);
}

CY_TEST_CASE("ui_focus: a modal scopes focus, blocks input, and restores focus when it pops") {
    // "WHEN a settings screen is pushed as a Modal THEN focus SHALL move into it, input to layers
    // beneath SHALL be blocked, a back action SHALL be registered, and the enter transition SHALL
    // play — without game code coordinating any of it", and "WHEN a modal is dismissed THEN focus
    // SHALL return to the element that had it before the modal opened."
    ElementStore store(allocator());
    Interaction interaction(allocator());
    const ElementId hud_root = add(store, kNoElement, "panel");
    place(store, hud_root, Rect{0.0F, 0.0F, 800.0F, 600.0F});
    const ElementId hud_button = button(store, hud_root, Rect{0.0F, 0.0F, 100.0F, 40.0F});

    Layer hud;
    hud.name = Name::intern("hud");
    hud.kind = LayerKind::Hud;
    hud.behaviour = default_behaviour(LayerKind::Hud);
    hud.root = hud_root;
    CY_REQUIRE(interaction.push_layer(hud).has_value());
    CY_REQUIRE(interaction.set_focus(store, hud_button).has_value());

    const ElementId modal_root = add(store, kNoElement, "panel");
    place(store, modal_root, Rect{200.0F, 200.0F, 400.0F, 200.0F});
    const ElementId accept = button(store, modal_root, Rect{220.0F, 320.0F, 100.0F, 40.0F});
    Layer modal;
    modal.name = Name::intern("settings");
    modal.kind = LayerKind::Modal;
    modal.behaviour = default_behaviour(LayerKind::Modal);
    modal.root = modal_root;
    CY_REQUIRE(interaction.push_layer(modal).has_value());
    CY_CHECK_FALSE(interaction.focus().is_valid());
    CY_REQUIRE(interaction.set_focus(store, accept).has_value());

    // FOCUS CANNOT ESCAPE THE MODAL. "WHEN directional navigation reaches the edge of a modal THEN
    // focus SHALL NOT escape to elements beneath it."
    const Status escaped = interaction.set_focus(store, hud_button);
    CY_REQUIRE_FALSE(escaped.has_value());
    CY_CHECK_EQ(escaped.error().code, ErrorCode::PermissionDenied);
    CY_CHECK_EQ(interaction.navigate(store, UiAction::NavigateLeft), accept);

    // AND INPUT DOES NOT REACH BENEATH IT, by the layer's declared behaviour.
    const RouteResult beneath = interaction.probe(store, Vec2{50.0F, 20.0F});
    CY_CHECK(beneath.blocked_by_layer);
    CY_CHECK_FALSE(beneath.target.is_valid());

    // Cancel pops the modal and focus comes back.
    auto cancelled = interaction.route_action(store, UiAction::Cancel);
    CY_REQUIRE(cancelled.has_value());
    CY_CHECK_EQ(interaction.layers().size(), 1U);
    CY_CHECK_EQ(interaction.focus(), hud_button);
}

CY_TEST_CASE("ui_input: an action reaches focus and bubbles past a disabled element") {
    ElementStore store(allocator());
    Interaction interaction(allocator());
    const ElementId root = add(store, kNoElement, "panel");
    place(store, root, Rect{0.0F, 0.0F, 800.0F, 600.0F});
    const ElementId group = add(store, root, "panel");
    place(store, group, Rect{0.0F, 0.0F, 400.0F, 200.0F});
    const ElementId control = button(store, group, Rect{0.0F, 0.0F, 100.0F, 40.0F});

    Layer layer;
    layer.root = root;
    layer.behaviour = default_behaviour(LayerKind::Overlay);
    CY_REQUIRE(interaction.push_layer(layer).has_value());
    CY_REQUIRE(interaction.set_focus(store, control).has_value());

    auto accepted = interaction.route_action(store, UiAction::Accept);
    CY_REQUIRE(accepted.has_value());
    CY_CHECK_EQ(accepted.value().target, control);
    CY_CHECK_EQ(accepted.value().bubbled, 0U);

    // A disabled element does not handle the action: it bubbles to its parent.
    CY_REQUIRE(store.set_flags(control, store.flags(control) | ElementFlags::Disabled).has_value());
    auto bubbled = interaction.route_action(store, UiAction::Accept);
    CY_REQUIRE(bubbled.has_value());
    CY_CHECK_EQ(bubbled.value().target, group);
    CY_CHECK_EQ(bubbled.value().bubbled, 1U);
}

CY_TEST_CASE("ui_layers: the default behaviours are the semantics the specification describes") {
    // The table in `ui-system`, as code: a HUD is drawn and not interactive, an overlay and a modal
    // capture focus and block, and a system layer does not answer to back navigation.
    CY_CHECK_FALSE(default_behaviour(LayerKind::Hud).blocks_input);
    CY_CHECK_FALSE(default_behaviour(LayerKind::Hud).captures_focus);
    CY_CHECK(default_behaviour(LayerKind::Overlay).blocks_input);
    CY_CHECK(default_behaviour(LayerKind::Modal).captures_focus);
    CY_CHECK(default_behaviour(LayerKind::Modal).handles_back);
    CY_CHECK_GT(default_behaviour(LayerKind::Modal).dim_beneath,
                default_behaviour(LayerKind::Overlay).dim_beneath);
    CY_CHECK_FALSE(default_behaviour(LayerKind::System).handles_back);
}

CY_TEST_CASE("ui_focus: an element that does not take focus is refused") {
    ElementStore store(allocator());
    Interaction interaction(allocator());
    const ElementId root = add(store, kNoElement, "panel");
    place(store, root, Rect{0.0F, 0.0F, 800.0F, 600.0F});
    const ElementId label = add(store, root, "label");
    place(store, label, Rect{0.0F, 0.0F, 100.0F, 20.0F});

    const Status refused = interaction.set_focus(store, label);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::InvalidArgument);

    // And so is one that is focusable but disabled.
    const ElementId control = button(store, root, Rect{0.0F, 40.0F, 100.0F, 40.0F});
    CY_REQUIRE(store.set_flags(control, store.flags(control) | ElementFlags::Disabled).has_value());
    CY_CHECK_FALSE(interaction.set_focus(store, control).has_value());
}
