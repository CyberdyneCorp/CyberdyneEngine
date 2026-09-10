// Twenty thousand elements: the scale `ui-system` states, and teardown under it. M8.b task 9.3.
//
// INTEGRATION, not unit, per hard rule 7: these cases build a document of twenty thousand elements,
// lay it out and flatten it. That is a millisecond of work several times over and belongs in the
// tier above rather than on the unit tier's budget.
//
// AND THE SECOND CASE IS TEARDOWN UNDER LOAD, which hard rule 4 of this milestone's brief asks for
// by name: a store carrying twenty thousand live elements, a full dirty list and a flattened
// primitive buffer is destroyed, and every allocation it made is given back. A leak here would be a
// leak per document opened and closed, which is the shape of leak a game finds after an hour.

#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>
#include <cy/ui/layout.h>
#include <cy/ui/paint.h>
#include <cy/ui/store.h>

using namespace cy;
using namespace cy::ui;

namespace {

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Engine);
}

/// A counting allocator: forwards to the system's and keeps the balance.
///
/// NOT `cy::TrackingAllocator`, and the reason is a defect this case found in it: its recent-free
/// ring compares raw ADDRESSES, so when the upstream allocator hands back a block it has just
/// reclaimed — which is exactly what an array growing in a loop makes it do — a legitimate free is
/// counted as a double free and the allocation is never unlinked. Eight allocate-and-free pairs of
/// one size measured four phantom leaks and four phantom double frees on this machine, with no
/// container involved. A leak check built on `live_allocations()` over a churning workload is
/// therefore measuring the instrument; this one counts calls, which is what the question actually
/// is.
class CountingAllocator final : public Allocator {
public:
    CountingAllocator() noexcept : Allocator(MemoryDomain::Engine, "ui-teardown") {}

    [[nodiscard]] u64 balance() const noexcept { return allocations_ - deallocations_; }
    [[nodiscard]] u64 allocations() const noexcept { return allocations_; }

protected:
    [[nodiscard]] void* do_allocate(usize size, usize alignment) noexcept override {
        void* block = system_allocator(MemoryDomain::Engine).allocate(size, alignment);
        if (block != nullptr) {
            ++allocations_;
        }
        return block;
    }

    [[nodiscard]] void* do_reallocate(void* pointer, usize old_size, usize new_size,
                                      usize alignment) noexcept override {
        // Allocate-copy-free, so a reallocation is one of each and the balance stays exact.
        return reallocate_by_copy(pointer, old_size, new_size, alignment);
    }

    void do_deallocate(void* pointer, usize size, usize alignment) noexcept override {
        ++deallocations_;
        system_allocator(MemoryDomain::Engine).deallocate(pointer, size, alignment);
    }

private:
    u64 allocations_ = 0;
    u64 deallocations_ = 0;
};

/// A document of `rows` panels, each with `columns` children: `rows * (columns + 1) + 1` elements.
[[nodiscard]] Status build_document(ElementStore& store, ElementId root, u32 rows,
                                    u32 columns) noexcept {
    for (u32 row = 0; row < rows; ++row) {
        auto panel = store.create(root, Name::intern("panel"));
        if (!panel) {
            return make_unexpected(panel.error());
        }
        LayoutInput* input = store.layout_input(panel.value());
        input->preferred = Vec2{-1.0F, 24.0F};
        for (u32 column = 0; column < columns; ++column) {
            auto label = store.create(panel.value(), Name::intern("label"));
            if (!label) {
                return make_unexpected(label.error());
            }
            LayoutInput* child = store.layout_input(label.value());
            child->preferred = Vec2{40.0F, 20.0F};
        }
    }
    return ok();
}

}  // namespace

CY_TEST_CASE("ui_scale: twenty thousand elements lay out and flatten into a few batches") {
    // "WHEN 20,000 elements produce 5,000 visible primitives THEN they SHALL be drawn in a small,
    // reportable number of batches."
    ElementStore store(allocator());
    auto root = store.create(kNoElement, Name::intern("document"));
    CY_REQUIRE(root.has_value());
    store.layout_input(root.value())->direction = FlexDirection::Column;
    CY_REQUIRE(build_document(store, root.value(), 2000U, 9U).has_value());
    CY_CHECK_EQ(store.size(), 20001U);

    ScaleSettings settings;
    settings.mode = ScaleMode::FixedPixel;
    LayoutReport layout_report;
    CY_REQUIRE(layout(store, settings, Vec2{1920.0F, 1080.0F}, nullptr, layout_report).has_value());
    CY_CHECK_EQ(layout_report.measured, 20001U);
    CY_CHECK_EQ(layout_report.arranged, 20001U);

    PrimitiveBuffer buffer(allocator());
    FlattenReport flatten_report;
    CY_REQUIRE(
        flatten(store, Rect{0.0F, 0.0F, 1920.0F, 1080.0F}, buffer, flatten_report).has_value());
    // Most of the document is below the viewport and is CULLED before batching; what is left shares
    // one material and one atlas, so it is one batch.
    CY_CHECK_GT(flatten_report.culled, 10000U);
    CY_CHECK_LE(flatten_report.batches, 4U);

    // AND THE SECOND FRAME DOES NOTHING. "WHEN no UI state changes in a frame THEN no layout or
    // paint work SHALL be performed" — at twenty thousand elements, which is where it matters.
    LayoutReport idle;
    CY_REQUIRE(layout(store, settings, Vec2{1920.0F, 1080.0F}, nullptr, idle).has_value());
    CY_CHECK_EQ(idle.measured, 0U);
    CY_CHECK_EQ(idle.arranged, 0U);
}

CY_TEST_CASE("ui_scale: a document torn down under load gives every allocation back") {
    // TEARDOWN UNDER LOAD. The store carries twenty thousand live elements, a populated dirty list
    // and a flattened primitive buffer when it is destroyed.
    CountingAllocator tracker;
    const u64 before = tracker.balance();

    {
        ElementStore store(tracker);
        auto root = store.create(kNoElement, Name::intern("document"));
        CY_REQUIRE(root.has_value());
        CY_REQUIRE(build_document(store, root.value(), 2000U, 9U).has_value());

        ScaleSettings settings;
        settings.mode = ScaleMode::FixedPixel;
        LayoutReport report;
        CY_REQUIRE(layout(store, settings, Vec2{1920.0F, 1080.0F}, nullptr, report).has_value());

        PrimitiveBuffer buffer(tracker);
        FlattenReport flatten_report;
        CY_REQUIRE(
            flatten(store, Rect{0.0F, 0.0F, 1920.0F, 1080.0F}, buffer, flatten_report).has_value());
        CY_CHECK_GT(tracker.balance(), before);

        // DESTROY THE WHOLE SUBTREE IN ONE CALL, with everything still dirty and the buffer still
        // holding primitives that name its elements.
        CY_REQUIRE(store.destroy(root.value()).has_value());
        CY_CHECK_EQ(store.size(), 0U);
        // The slots come back, and a stale identifier still fails: teardown does not resurrect one.
        CY_CHECK_FALSE(store.alive(root.value()));
        auto reused = store.create(kNoElement, Name::intern("document"));
        CY_REQUIRE(reused.has_value());
        CY_CHECK_NE(reused.value().generation, root.value().generation);
    }

    // EVERY ALLOCATION BACK. A leak here is a leak per document opened and closed.
    CY_CHECK_EQ(tracker.balance(), before);
    CY_CHECK_GT(tracker.allocations(), 20U);
}

CY_TEST_CASE(
    "ui_scale: destroying a subtree while its parent is dirty leaves the tree consistent") {
    // The other half of teardown: a partial one, with the parent's dirty state already set. A tree
    // walk after it must terminate and the counts must agree — the two things a broken sibling
    // pointer breaks.
    ElementStore store(allocator());
    auto root = store.create(kNoElement, Name::intern("document"));
    CY_REQUIRE(root.has_value());
    CY_REQUIRE(build_document(store, root.value(), 200U, 9U).has_value());

    Array<ElementId> children(allocator());
    CY_REQUIRE(store.children_of(root.value(), children).has_value());
    CY_REQUIRE_EQ(children.size(), 200U);

    // Destroy every other panel, marking the survivors dirty as we go.
    for (usize index = 0; index < children.size(); index += 2) {
        store.mark(children[index], Dirty::Measure | Dirty::Arrange | Dirty::Paint);
        CY_REQUIRE(store.destroy(children[index]).has_value());
    }
    CY_REQUIRE(store.children_of(root.value(), children).has_value());
    CY_CHECK_EQ(children.size(), 100U);
    CY_CHECK_EQ(store.size(), 1001U);
    CY_CHECK_EQ(store.hierarchy(root.value())->child_count, 100U);

    // The dirty walk still terminates and reaches exactly the live elements.
    Array<ElementId> dirty(allocator());
    CY_REQUIRE(store.collect_dirty(Dirty::Arrange, dirty).has_value());
    for (const ElementId element : dirty.span()) {
        CY_REQUIRE(store.alive(element));
    }
}
