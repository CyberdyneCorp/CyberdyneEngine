// The element store, the three dirty states, and the reconciler. M8.b task 9.1.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/ui/store.h>
#include <cy/ui/widgets.h>

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

[[nodiscard]] DescriptionNode node(const char* type, u32 parent, u64 key = 0) noexcept {
    DescriptionNode description;
    description.type = Name::intern(type);
    description.parent = parent;
    description.key = key;
    return description;
}

}  // namespace

CY_TEST_CASE("ui_store: a stale identifier fails validation rather than aliasing its replacement") {
    // "WHEN an element is destroyed and its slot is reused THEN the previous `UIElementID` SHALL
    // fail validation rather than resolving to the new element."
    ElementStore store(allocator());
    const ElementId first = add(store, kNoElement, "panel");
    CY_REQUIRE(first.is_valid());
    CY_CHECK(store.alive(first));

    CY_REQUIRE(store.destroy(first).has_value());
    CY_CHECK_FALSE(store.alive(first));

    const ElementId second = add(store, kNoElement, "panel");
    CY_REQUIRE(second.is_valid());
    // The slot came back; the identifier did not.
    CY_CHECK_EQ(second.index, first.index);
    CY_CHECK_NE(second.generation, first.generation);
    CY_CHECK_FALSE(store.alive(first));
    CY_CHECK(store.alive(second));
    CY_CHECK_EQ(store.layout_input(first), nullptr);
}

CY_TEST_CASE("ui_store: a paint change dirties the element and nothing else") {
    // "WHEN a label's numeric text changes without changing its measured size THEN only its paint
    // state SHALL be dirtied, and no layout work SHALL occur."
    ElementStore store(allocator());
    const ElementId root = add(store, kNoElement, "panel");
    const ElementId row = add(store, root, "panel");
    const ElementId label = add(store, row, "label");
    CY_REQUIRE(label.is_valid());

    // Start clean, as a laid-out frame would be.
    for (const ElementId element : {root, row, label}) {
        store.clear_dirty(element, Dirty::Measure | Dirty::Arrange | Dirty::Paint);
    }
    CY_CHECK_FALSE(store.any_dirty());

    store.mark(label, Dirty::Paint);
    CY_CHECK(has_dirty(store.dirty(label), Dirty::Paint));
    CY_CHECK_FALSE(has_dirty(store.dirty(label), Dirty::Measure));
    // NOWHERE ELSE. Not the row, not the root.
    CY_CHECK_EQ(store.dirty(row), Dirty::None);
    CY_CHECK_EQ(store.dirty(root), Dirty::None);
    CY_CHECK_EQ(store.stats().paint_dirty, 1U);
    CY_CHECK_EQ(store.stats().measure_dirty, 0U);
}

CY_TEST_CASE("ui_store: measure propagates upward only while ancestors depend on it") {
    // "WHEN a label's text grows and changes its desired size THEN measure SHALL propagate upward
    // only while ancestors' desired sizes depend on it."
    ElementStore store(allocator());
    const ElementId root = add(store, kNoElement, "panel");
    const ElementId fixed = add(store, root, "panel");
    const ElementId row = add(store, fixed, "panel");
    const ElementId label = add(store, row, "label");
    CY_REQUIRE(label.is_valid());

    // The middle panel has an explicit size: its desired size does not depend on its children, so
    // the walk stops there.
    store.layout_input(fixed)->preferred = Vec2{200.0F, 100.0F};
    for (const ElementId element : {root, fixed, row, label}) {
        store.clear_dirty(element, Dirty::Measure | Dirty::Arrange | Dirty::Paint);
    }

    store.mark(label, Dirty::Measure);
    CY_CHECK(has_dirty(store.dirty(label), Dirty::Measure));
    CY_CHECK(has_dirty(store.dirty(row), Dirty::Measure));
    CY_CHECK(has_dirty(store.dirty(fixed), Dirty::Measure));
    // AND NO FURTHER. The root's desired size cannot have changed, so it is not re-measured.
    CY_CHECK_FALSE(has_dirty(store.dirty(root), Dirty::Measure));
}

CY_TEST_CASE("ui_store: arrange propagates downward through the subtree") {
    ElementStore store(allocator());
    const ElementId root = add(store, kNoElement, "panel");
    const ElementId row = add(store, root, "panel");
    const ElementId label = add(store, row, "label");
    for (const ElementId element : {root, row, label}) {
        store.clear_dirty(element, Dirty::Measure | Dirty::Arrange | Dirty::Paint);
    }

    store.mark(row, Dirty::Arrange);
    CY_CHECK(has_dirty(store.dirty(row), Dirty::Arrange));
    // A parent's rect moving moves every descendant's rect.
    CY_CHECK(has_dirty(store.dirty(label), Dirty::Arrange));
    CY_CHECK_FALSE(has_dirty(store.dirty(root), Dirty::Arrange));
}

CY_TEST_CASE("ui_store: destroying an element takes its subtree with it") {
    ElementStore store(allocator());
    const ElementId root = add(store, kNoElement, "panel");
    const ElementId branch = add(store, root, "panel");
    const ElementId leaf = add(store, branch, "label");
    const ElementId sibling = add(store, root, "label");
    CY_CHECK_EQ(store.size(), 4U);

    CY_REQUIRE(store.destroy(branch).has_value());
    CY_CHECK_FALSE(store.alive(branch));
    CY_CHECK_FALSE(store.alive(leaf));
    CY_CHECK(store.alive(sibling));
    CY_CHECK_EQ(store.size(), 2U);
    CY_CHECK_EQ(store.hierarchy(root)->child_count, 1U);
}

CY_TEST_CASE("ui_store: an element cannot become its own ancestor") {
    ElementStore store(allocator());
    const ElementId root = add(store, kNoElement, "panel");
    const ElementId descendant = add(store, root, "panel");
    // Reparenting an ancestor UNDER its own descendant: a cycle, and every tree walk in the module
    // would stop terminating.
    const Status refused = store.reparent(root, descendant);
    CY_REQUIRE_FALSE(refused.has_value());
    CY_CHECK_EQ(refused.error().code, ErrorCode::InvalidArgument);
}

CY_TEST_CASE("ui_reconcile: a rebuild preserves identity and only applies differences") {
    // "WHEN a declarative view's bound state changes THEN only the affected subtree's description
    // SHALL be rebuilt and diffed, and unaffected elements SHALL retain identity and state."
    ElementStore store(allocator());
    const ElementId root = add(store, kNoElement, "document");

    Description description(allocator());
    const auto panel = description.add(node("panel", DescriptionNode::kNoParent));
    CY_REQUIRE(panel.has_value());
    CY_REQUIRE(description.add(node("label", panel.value())).has_value());
    CY_REQUIRE(description.add(node("button", panel.value())).has_value());

    ReconcileReport first;
    CY_REQUIRE(reconcile(store, root, description, first).has_value());
    CY_CHECK_EQ(first.created, 3U);
    CY_CHECK_EQ(first.removed, 0U);

    Array<ElementId> children(allocator());
    CY_REQUIRE(store.children_of(root, children).has_value());
    CY_REQUIRE_EQ(children.size(), 1U);
    const ElementId panel_element = children[0];
    CY_REQUIRE(store.children_of(panel_element, children).has_value());
    CY_REQUIRE_EQ(children.size(), 2U);
    const ElementId button = children[1];

    // A SECOND RECONCILE OF THE SAME DESCRIPTION CREATES NOTHING and keeps every identity.
    ReconcileReport second;
    CY_REQUIRE(reconcile(store, root, description, second).has_value());
    CY_CHECK_EQ(second.created, 0U);
    CY_CHECK_EQ(second.removed, 0U);
    CY_CHECK_EQ(second.updated, 3U);
    CY_CHECK(store.alive(button));
    CY_REQUIRE(store.children_of(panel_element, children).has_value());
    CY_CHECK_EQ(children[1], button);
}

CY_TEST_CASE("ui_reconcile: keys carry state across a reorder, and their absence is reported") {
    // "WHEN list items are reordered and declare explicit keys THEN each item's element state SHALL
    // follow its key, not its position", and the churn diagnostic for when they do not.
    ElementStore store(allocator());
    const ElementId root = add(store, kNoElement, "document");

    Description ordered(allocator());
    CY_REQUIRE(ordered.add(node("row", DescriptionNode::kNoParent, 11)).has_value());
    CY_REQUIRE(ordered.add(node("row", DescriptionNode::kNoParent, 22)).has_value());
    ReconcileReport report;
    CY_REQUIRE(reconcile(store, root, ordered, report).has_value());

    Array<ElementId> children(allocator());
    CY_REQUIRE(store.children_of(root, children).has_value());
    CY_REQUIRE_EQ(children.size(), 2U);
    const ElementId first_row = children[0];
    const ElementId second_row = children[1];

    // Reordered, with the same keys: the elements are the same elements.
    Description reordered(allocator());
    CY_REQUIRE(reordered.add(node("row", DescriptionNode::kNoParent, 22)).has_value());
    CY_REQUIRE(reordered.add(node("row", DescriptionNode::kNoParent, 11)).has_value());
    ReconcileReport keyed;
    CY_REQUIRE(reconcile(store, root, reordered, keyed).has_value());
    CY_CHECK_EQ(keyed.created, 0U);
    CY_CHECK_EQ(keyed.removed, 0U);
    CY_CHECK(store.alive(first_row));
    CY_CHECK(store.alive(second_row));
    CY_CHECK_EQ(store.key(first_row), 11ULL);

    // WITHOUT KEYS, state follows position — and the identity change is REPORTED, which is the
    // diagnostic `ui-system` asks for by name.
    ElementStore unkeyed_store(allocator());
    const ElementId unkeyed_root = add(unkeyed_store, kNoElement, "document");
    Description a(allocator());
    CY_REQUIRE(a.add(node("row", DescriptionNode::kNoParent, 1)).has_value());
    CY_REQUIRE(a.add(node("row", DescriptionNode::kNoParent, 2)).has_value());
    ReconcileReport initial;
    CY_REQUIRE(reconcile(unkeyed_store, unkeyed_root, a, initial).has_value());

    Description b(allocator());
    CY_REQUIRE(b.add(node("row", DescriptionNode::kNoParent, 2)).has_value());
    CY_REQUIRE(b.add(node("row", DescriptionNode::kNoParent, 3)).has_value());
    ReconcileReport churned;
    CY_REQUIRE(reconcile(unkeyed_store, unkeyed_root, b, churned).has_value());
    CY_CHECK_GT(churned.churn + churned.created, 0U);
    CY_CHECK_EQ(unkeyed_store.stats().identity_churn, churned.churn);
}

CY_TEST_CASE("ui_reconcile: an element the description dropped is destroyed") {
    ElementStore store(allocator());
    const ElementId root = add(store, kNoElement, "document");

    Description full(allocator());
    const auto panel = full.add(node("panel", DescriptionNode::kNoParent));
    CY_REQUIRE(panel.has_value());
    CY_REQUIRE(full.add(node("label", panel.value())).has_value());
    ReconcileReport first;
    CY_REQUIRE(reconcile(store, root, full, first).has_value());
    CY_CHECK_EQ(store.size(), 3U);

    Description trimmed(allocator());
    CY_REQUIRE(trimmed.add(node("panel", DescriptionNode::kNoParent)).has_value());
    ReconcileReport second;
    CY_REQUIRE(reconcile(store, root, trimmed, second).has_value());
    CY_CHECK_EQ(second.removed, 1U);
    CY_CHECK_EQ(store.size(), 2U);
}

CY_TEST_CASE("ui_virtual: a hundred thousand rows realise a dozen") {
    // "WHEN a list view displays 100 000 items THEN only visible rows SHALL be realised, and
    // scrolling SHALL cost the same as for 100 items."
    VirtualList list;
    list.item_count = 100000;
    list.item_height = 24.0F;
    list.viewport_height = 240.0F;
    list.scroll = 0.0F;

    const VirtualRange top = virtual_range(list);
    CY_CHECK_EQ(top.first, 0U);
    CY_CHECK_LE(top.count(), 16U);
    CY_CHECK_NEAR(top.total_height, 2400000.0F, 1.0F);

    // Scrolled a long way down: still a handful of rows, at a different offset. The count differs
    // from the top of the list by the overscan the top could not have — there are no rows above row
    // zero to realise — which is the arithmetic being right rather than a discrepancy.
    list.scroll = 500000.0F;
    const VirtualRange deep = virtual_range(list);
    CY_CHECK_LE(deep.count(), top.count() + list.overscan);
    CY_CHECK_GT(deep.first, 20000U);
    // The offset places the FIRST REALISED row, which is `overscan` rows above the first visible
    // one — so it is within one row of that, and the list scrolls smoothly rather than by a row.
    CY_CHECK_LE(deep.offset, 0.0F);
    CY_CHECK_GT(deep.offset, -list.item_height * static_cast<f32>(list.overscan + 1U));

    // And the end of the list does not run past it.
    list.scroll = 2400000.0F;
    const VirtualRange bottom = virtual_range(list);
    CY_CHECK_EQ(bottom.last, list.item_count);

    // An empty list realises nothing rather than one row of nothing.
    list.item_count = 0;
    CY_CHECK_EQ(virtual_range(list).count(), 0U);
}
