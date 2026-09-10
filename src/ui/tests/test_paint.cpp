// Flattening, batching, the budget ladder, data binding and the accessibility audit.
// M8.b task 9.3.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/ui/paint.h>
#include <cy/ui/widgets.h>

using namespace cy;
using namespace cy::ui;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Engine);
}

[[nodiscard]] ElementId add(ElementStore& store, ElementId parent, const char* type, Rect rect,
                            u16 material = 0, u16 atlas = 0) noexcept {
    auto created = store.create(parent, Name::intern(type));
    if (!created) {
        return kNoElement;
    }
    const ElementId element = created.value();
    LayoutOutput* output = store.layout_output(element);
    output->rect = rect;
    output->clip = Rect{0.0F, 0.0F, 1920.0F, 1080.0F};
    PaintData* paint = store.paint(element);
    paint->material = material;
    paint->atlas = atlas;
    paint->background = 0xFF202020U;
    paint->tint = 0xFFFFFFFFU;
    return element;
}

/// A binding source over a number the test moves.
class Dial final : public BindingSource {
public:
    [[nodiscard]] BoundValue read() noexcept override {
        dirty = false;
        BoundValue value;
        value.kind = kind;
        value.number = number;
        value.colour = colour;
        return value;
    }
    [[nodiscard]] bool changed() const noexcept override { return dirty; }
    [[nodiscard]] Status write(const BoundValue& value) noexcept override {
        number = value.number;
        written = true;
        return ok();
    }

    BoundValue::Kind kind = BoundValue::Kind::Number;
    f32 number = 0.0F;
    u32 colour = 0xFF000000U;
    bool dirty = true;
    bool written = false;
};

class RecordingBridge final : public AccessibilityBridge {
public:
    void publish(Span<const AccessibilityNode> nodes) noexcept override {
        published = static_cast<u32>(nodes.size());
    }
    void announce_focus(const AccessibilityNode& node) noexcept override {
        announced = node.element;
        announced_role = node.role;
        announced_value = node.value;
    }

    u32 published = 0;
    ElementId announced;
    Role announced_role = Role::None;
    Name announced_value;
};

}  // namespace

CY_TEST_CASE("ui_paint: many elements produce few batches, and the break reason is named") {
    // "WHEN 20,000 elements produce 5,000 visible primitives THEN they SHALL be drawn in a small,
    // reportable number of batches." Two hundred here, and the property is the same one: the batch
    // count follows the MATERIAL CHANGES, not the element count.
    ElementStore store(allocator());
    const ElementId root = add(store, kNoElement, "panel", Rect{0.0F, 0.0F, 1920.0F, 1080.0F});
    for (u32 index = 0; index < 200U; ++index) {
        const f32 y = static_cast<f32>(index % 40U) * 20.0F;
        (void)add(store, root, "label", Rect{0.0F, y, 100.0F, 18.0F}, 0, 0);
    }

    PrimitiveBuffer buffer(allocator());
    FlattenReport report;
    CY_REQUIRE(flatten(store, Rect{0.0F, 0.0F, 1920.0F, 1080.0F}, buffer, report).has_value());
    CY_CHECK_EQ(report.emitted, 201U);
    CY_CHECK_EQ(buffer.primitives().size(), 201U);
    // One material, one atlas, one clip: ONE batch for two hundred elements.
    CY_CHECK_EQ(report.batches, 1U);

    // A second material breaks the batch, and the reason says which.
    (void)add(store, root, "image", Rect{0.0F, 0.0F, 32.0F, 32.0F}, 3, 1);
    PrimitiveBuffer second(allocator());
    FlattenReport again;
    CY_REQUIRE(flatten(store, Rect{0.0F, 0.0F, 1920.0F, 1080.0F}, second, again).has_value());
    CY_CHECK_EQ(again.batches, 2U);
    CY_REQUIRE_EQ(second.batches().size(), 2U);
    CY_CHECK_EQ(second.batches()[0].reason, Batch::BreakReason::Material);
}

CY_TEST_CASE("ui_paint: flattening is incremental — one repaint re-emits one primitive") {
    // "WHEN one panel repaints in an otherwise static document THEN only its primitives SHALL be
    // re-emitted and its GPU buffer range updated."
    ElementStore store(allocator());
    const ElementId root = add(store, kNoElement, "panel", Rect{0.0F, 0.0F, 800.0F, 600.0F});
    const ElementId first = add(store, root, "panel", Rect{0.0F, 0.0F, 100.0F, 40.0F});
    (void)add(store, root, "panel", Rect{0.0F, 50.0F, 100.0F, 40.0F});

    PrimitiveBuffer buffer(allocator());
    FlattenReport initial;
    CY_REQUIRE(flatten(store, Rect{0.0F, 0.0F, 800.0F, 600.0F}, buffer, initial).has_value());
    CY_CHECK_EQ(initial.emitted, 3U);
    CY_CHECK_EQ(initial.reused, 0U);

    // Nothing changed: everything is reused.
    FlattenReport idle;
    CY_REQUIRE(flatten(store, Rect{0.0F, 0.0F, 800.0F, 600.0F}, buffer, idle).has_value());
    CY_CHECK_EQ(idle.emitted, 0U);
    CY_CHECK_EQ(idle.reused, 3U);

    // One element repaints: one primitive re-emitted, two reused.
    store.mark(first, Dirty::Paint);
    FlattenReport partial;
    CY_REQUIRE(flatten(store, Rect{0.0F, 0.0F, 800.0F, 600.0F}, buffer, partial).has_value());
    CY_CHECK_EQ(partial.emitted, 1U);
    CY_CHECK_EQ(partial.reused, 2U);
}

CY_TEST_CASE("ui_paint: a primitive outside its clip is culled before batching") {
    // "WHEN elements lie outside their scroll container's clip rect THEN their primitives SHALL be
    // culled before batching."
    ElementStore store(allocator());
    const ElementId root = add(store, kNoElement, "panel", Rect{0.0F, 0.0F, 800.0F, 600.0F});
    const ElementId visible = add(store, root, "row", Rect{0.0F, 0.0F, 100.0F, 40.0F});
    const ElementId scrolled = add(store, root, "row", Rect{0.0F, 500.0F, 100.0F, 40.0F});
    store.layout_output(visible)->clip = Rect{0.0F, 0.0F, 200.0F, 100.0F};
    store.layout_output(scrolled)->clip = Rect{0.0F, 0.0F, 200.0F, 100.0F};

    PrimitiveBuffer buffer(allocator());
    FlattenReport report;
    CY_REQUIRE(flatten(store, Rect{0.0F, 0.0F, 800.0F, 600.0F}, buffer, report).has_value());
    CY_CHECK_EQ(report.culled, 1U);
    CY_CHECK_EQ(buffer.primitives().size(), 2U);  // the root and the visible row
}

CY_TEST_CASE("ui_budget: the ladder degrades in the declared order and never past it") {
    // "When budgets are exceeded, the system SHALL degrade in a defined order — reducing effect
    // quality, disabling blur-behind, and reducing world-space UI detail — before reducing anything
    // affecting interaction or legibility, and SHALL report the degradation."
    UiBudget budget;
    budget.primitives = 1000;
    budget.batches = 8;
    budget.effect_passes = 2;
    budget.layout_milliseconds = 2.0F;

    FlattenReport within;
    within.emitted = 500;
    within.batches = 4;
    BudgetReport report;
    CY_CHECK_EQ(apply_budget(budget, within, 1.0F, 1, report), Degradation::None);
    CY_CHECK_FALSE(report.over_budget);

    FlattenReport heavy = within;
    heavy.emitted = 4000;
    CY_CHECK_EQ(apply_budget(budget, heavy, 1.0F, 1, report), Degradation::EffectQuality);
    CY_CHECK(report.over_budget);
    CY_CHECK_EQ(report.cause, Name::intern("primitives"));

    heavy.batches = 40;
    CY_CHECK_EQ(apply_budget(budget, heavy, 1.0F, 1, report), Degradation::BlurBehind);

    // Everything over at once: the ladder stops at its last rung. There is no step below it that
    // drops an element, moves focus or shrinks text — "hit-testing, focus, and text legibility
    // SHALL be preserved" is the absence of a rung rather than a check somewhere.
    CY_CHECK_EQ(apply_budget(budget, heavy, 9.0F, 9, report), Degradation::WorldSpaceDetail);
    CY_CHECK_EQ(report.level, Degradation::WorldSpaceDetail);
}

CY_TEST_CASE("ui_binding: a bound value that does not affect layout dirties paint only") {
    // "WHEN a bound value changes without affecting layout THEN only the bound element's paint
    // state SHALL be dirtied."
    ElementStore store(allocator());
    const ElementId root = add(store, kNoElement, "panel", Rect{0.0F, 0.0F, 200.0F, 100.0F});
    const ElementId bar = add(store, root, "progress", Rect{0.0F, 0.0F, 100.0F, 10.0F});
    for (const ElementId element : {root, bar}) {
        store.clear_dirty(element, Dirty::Measure | Dirty::Arrange | Dirty::Paint);
    }

    Dial source;
    source.kind = BoundValue::Kind::Colour;
    source.colour = 0xFF00FF00U;
    Binding colour_binding;
    colour_binding.element = bar;
    colour_binding.property = StyleProperty::BackgroundColour;
    colour_binding.source = &source;

    BindingReport report;
    CY_REQUIRE(update_bindings(store, Span<Binding>(&colour_binding, 1), report).has_value());
    CY_CHECK_EQ(report.updated, 1U);
    CY_CHECK_EQ(report.paint_dirtied, 1U);
    CY_CHECK_EQ(report.layout_dirtied, 0U);
    CY_CHECK(has_dirty(store.dirty(bar), Dirty::Paint));
    CY_CHECK_FALSE(has_dirty(store.dirty(bar), Dirty::Measure));
    CY_CHECK_EQ(store.dirty(root), Dirty::None);

    // A WIDTH DOES dirty layout, and the parent re-measures with it.
    store.clear_dirty(bar, Dirty::Paint);
    source.kind = BoundValue::Kind::Number;
    source.number = 60.0F;
    source.dirty = true;
    Binding width_binding;
    width_binding.element = bar;
    width_binding.property = StyleProperty::Width;
    width_binding.source = &source;
    CY_REQUIRE(update_bindings(store, Span<Binding>(&width_binding, 1), report).has_value());
    CY_CHECK_EQ(report.layout_dirtied, 1U);
    CY_CHECK(has_dirty(store.dirty(bar), Dirty::Measure));
}

CY_TEST_CASE("ui_binding: an unchanged source costs nothing, and a spurious change is counted") {
    ElementStore store(allocator());
    const ElementId element = add(store, kNoElement, "panel", Rect{0.0F, 0.0F, 100.0F, 100.0F});
    Dial source;
    source.kind = BoundValue::Kind::Colour;
    source.colour = 0xFF112233U;
    Binding binding;
    binding.element = element;
    binding.property = StyleProperty::BackgroundColour;
    binding.source = &source;

    BindingReport first;
    CY_REQUIRE(update_bindings(store, Span<Binding>(&binding, 1), first).has_value());
    CY_CHECK_EQ(first.updated, 1U);

    // The source says nothing changed: the binding is not even read.
    BindingReport quiet;
    CY_REQUIRE(update_bindings(store, Span<Binding>(&binding, 1), quiet).has_value());
    CY_CHECK_EQ(quiet.updated, 0U);
    CY_CHECK_EQ(quiet.spurious, 0U);

    // The source says it changed and produces the same value: counted, not acted on. A large number
    // here is a change-detection problem in the source, and this is how anybody finds out.
    source.dirty = true;
    BindingReport spurious;
    CY_REQUIRE(update_bindings(store, Span<Binding>(&binding, 1), spurious).has_value());
    CY_CHECK_EQ(spurious.updated, 0U);
    CY_CHECK_EQ(spurious.spurious, 1U);
}

CY_TEST_CASE("ui_binding: two-way writes back, and one-way refuses") {
    Dial source;
    Binding two_way;
    two_way.source = &source;
    two_way.mode = BindingMode::TwoWay;
    BoundValue edited;
    edited.kind = BoundValue::Kind::Number;
    edited.number = 42.0F;
    CY_REQUIRE(write_back(two_way, edited).has_value());
    CY_CHECK(source.written);
    CY_CHECK_EQ(source.number, 42.0F);

    Binding one_way;
    one_way.source = &source;
    const Status refused = write_back(one_way, edited);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::PermissionDenied);
}

CY_TEST_CASE("ui_accessibility: an interface that passes, and each way one fails") {
    // The exit criterion's own check. Each finding below is a sentence in `ui-system`'s
    // accessibility requirement.
    ElementStore store(allocator());
    Interaction interaction(allocator());
    const ElementId root = add(store, kNoElement, "panel", Rect{0.0F, 0.0F, 800.0F, 600.0F});
    const ElementId good = add(store, root, "button", Rect{0.0F, 0.0F, 120.0F, 40.0F});
    CY_REQUIRE(store.set_flags(good, ElementFlags::Visible | ElementFlags::Focusable).has_value());
    store.paint(good)->background = 0xFF101010U;
    store.paint(good)->tint = 0xFFFFFFFFU;

    AccessibilityNode node;
    node.element = good;
    node.role = Role::Button;
    node.label = Name::intern("Continue");
    node.value = Name::intern("");
    node.focusable = true;

    Array<AccessibilityFinding> findings(allocator());
    AccessibilityReport report;
    CY_REQUIRE(audit_accessibility(store, Span<const AccessibilityNode>(&node, 1), interaction,
                                   findings, report)
                   .has_value());
    CY_CHECK(report.passed);
    CY_CHECK_EQ(findings.size(), 0U);
    CY_CHECK_EQ(report.interactive, 1U);
    CY_CHECK_EQ(report.keyboard_reachable, 1U);

    // UNREACHABLE: an interactive element keyboard focus cannot reach.
    CY_REQUIRE(store.set_flags(good, ElementFlags::Visible).has_value());
    CY_REQUIRE(audit_accessibility(store, Span<const AccessibilityNode>(&node, 1), interaction,
                                   findings, report)
                   .has_value());
    CY_CHECK_FALSE(report.passed);
    CY_REQUIRE(findings.size() > 0U);
    CY_CHECK_EQ(findings[0].rule, Name::intern("unreachable"));

    // UNLABELLED: a control a screen reader cannot name.
    CY_REQUIRE(store.set_flags(good, ElementFlags::Visible | ElementFlags::Focusable).has_value());
    AccessibilityNode nameless = node;
    nameless.label = Name{};
    CY_REQUIRE(audit_accessibility(store, Span<const AccessibilityNode>(&nameless, 1), interaction,
                                   findings, report)
                   .has_value());
    CY_CHECK_FALSE(report.passed);
    CY_CHECK_EQ(findings[0].rule, Name::intern("unlabelled"));

    // CONTRAST: white text on a light background fails WCAG AA's 4.5:1.
    store.paint(good)->background = 0xFFE8E8E8U;
    CY_REQUIRE(audit_accessibility(store, Span<const AccessibilityNode>(&node, 1), interaction,
                                   findings, report)
                   .has_value());
    CY_CHECK_FALSE(report.passed);
    CY_CHECK_EQ(findings[0].rule, Name::intern("contrast"));
    CY_CHECK_LT(findings[0].measured, 4.5F);

    // TARGET SIZE: a control too small to hit.
    store.paint(good)->background = 0xFF101010U;
    store.layout_output(good)->rect = Rect{0.0F, 0.0F, 120.0F, 12.0F};
    CY_REQUIRE(audit_accessibility(store, Span<const AccessibilityNode>(&node, 1), interaction,
                                   findings, report)
                   .has_value());
    CY_CHECK_FALSE(report.passed);
    CY_CHECK_EQ(findings[0].rule, Name::intern("target-size"));
}

CY_TEST_CASE("ui_accessibility: the contrast ratio is WCAG's, and focus is announced") {
    // Black on white is 21:1, the maximum; a colour against itself is 1:1.
    CY_CHECK_NEAR(contrast_ratio(0xFF000000U, 0xFFFFFFFFU), 21.0F, 0.05F);
    CY_CHECK_NEAR(contrast_ratio(0xFF808080U, 0xFF808080U), 1.0F, 1e-4F);

    ElementStore store(allocator());
    const ElementId slider = add(store, kNoElement, "slider", Rect{0.0F, 0.0F, 200.0F, 32.0F});
    AccessibilityNode node;
    node.element = slider;
    node.role = Role::Slider;
    node.label = Name::intern("Master volume");
    node.value = Name::intern("70%");

    RecordingBridge bridge;
    CY_REQUIRE(publish_accessibility(Span<const AccessibilityNode>(&node, 1), slider, &bridge)
                   .has_value());
    CY_CHECK_EQ(bridge.published, 1U);
    CY_CHECK_EQ(bridge.announced, slider);
    CY_CHECK_EQ(bridge.announced_role, Role::Slider);
    // "WHEN focus moves to a slider with accessibility available THEN its role, label, and current
    // value SHALL be announced."
    CY_CHECK_EQ(bridge.announced_value, Name::intern("70%"));

    // A platform with no accessibility layer is not an error.
    CY_CHECK(publish_accessibility(Span<const AccessibilityNode>(&node, 1), slider, nullptr)
                 .has_value());
}
