// The element store, its tree, and the three dirty states. M8.b task 9.1.

#include <cy/ui/store.h>

namespace cy::ui {

Rect Rect::intersected(const Rect& other) const noexcept {
    const f32 left = (x > other.x) ? x : other.x;
    const f32 top = (y > other.y) ? y : other.y;
    const f32 right = (this->right() < other.right()) ? this->right() : other.right();
    const f32 bottom = (this->bottom() < other.bottom()) ? this->bottom() : other.bottom();
    Rect result;
    result.x = left;
    result.y = top;
    result.width = (right > left) ? (right - left) : 0.0F;
    result.height = (bottom > top) ? (bottom - top) : 0.0F;
    return result;
}

ElementStore::ElementStore(Allocator& allocator) noexcept
    : slots_(allocator),
      hierarchy_(allocator),
      layout_in_(allocator),
      layout_out_(allocator),
      paint_(allocator),
      flags_(allocator),
      style_(allocator),
      type_(allocator),
      key_(allocator),
      hit_(allocator),
      dirty_(allocator),
      free_(allocator),
      roots_(allocator) {}

i64 ElementStore::resolve(ElementId element) const noexcept {
    if (!element.is_valid() || element.index >= slots_.size()) {
        return -1;
    }
    const Slot& slot = slots_[element.index];
    if (!slot.alive || slot.generation != element.generation) {
        // A STALE ID FAILS VALIDATION rather than aliasing the element that took its slot. That is
        // the whole reason a generation is in the identifier.
        return -1;
    }
    return static_cast<i64>(element.index);
}

bool ElementStore::alive(ElementId element) const noexcept {
    return resolve(element) >= 0;
}

Expected<ElementId, Error> ElementStore::create(ElementId parent, Name type) noexcept {
    if (parent.is_valid() && resolve(parent) < 0) {
        return make_unexpected(
            Error{ErrorCode::NotFound, "the parent element does not exist any more", 0});
    }

    u32 index = 0;
    if (!free_.empty()) {
        index = free_[free_.size() - 1];
        free_.pop_back();
    } else {
        index = static_cast<u32>(slots_.size());
        if (Status pushed = slots_.push_back(Slot{}); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = hierarchy_.push_back(Hierarchy{}); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = layout_in_.push_back(LayoutInput{}); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = layout_out_.push_back(LayoutOutput{}); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = paint_.push_back(PaintData{}); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = flags_.push_back(ElementFlags::Visible); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = style_.push_back(0U); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = type_.push_back(Name{}); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = key_.push_back(0ULL); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = hit_.push_back(HitTestMode::Block); !pushed) {
            return make_unexpected(pushed.error());
        }
        if (Status pushed = dirty_.push_back(Dirty::None); !pushed) {
            return make_unexpected(pushed.error());
        }
    }

    Slot& slot = slots_[index];
    slot.generation += 1;
    slot.alive = true;
    hierarchy_[index] = Hierarchy{};
    layout_in_[index] = LayoutInput{};
    layout_out_[index] = LayoutOutput{};
    paint_[index] = PaintData{};
    flags_[index] = ElementFlags::Visible;
    style_[index] = 0;
    type_[index] = type;
    key_[index] = 0;
    hit_[index] = HitTestMode::Block;
    dirty_[index] = Dirty::Measure | Dirty::Arrange | Dirty::Paint;
    ++live_;
    ++created_;

    const ElementId id{index, slot.generation};
    if (parent.is_valid()) {
        if (Status attached = reparent(id, parent); !attached) {
            (void)destroy(id);
            return make_unexpected(attached.error());
        }
    } else if (Status pushed = roots_.push_back(id); !pushed) {
        (void)destroy(id);
        return make_unexpected(pushed.error());
    }
    return id;
}

void ElementStore::detach(u32 index) noexcept {
    Hierarchy& node = hierarchy_[index];
    const ElementId self{index, slots_[index].generation};
    if (node.parent.is_valid()) {
        const i64 parent = resolve(node.parent);
        if (parent >= 0) {
            Hierarchy& parent_node = hierarchy_[static_cast<usize>(parent)];
            if (parent_node.first_child == self) {
                parent_node.first_child = node.next_sibling;
            }
            if (parent_node.last_child == self) {
                parent_node.last_child = node.previous_sibling;
            }
            if (parent_node.child_count > 0) {
                --parent_node.child_count;
            }
        }
    } else {
        for (usize root = 0; root < roots_.size(); ++root) {
            if (roots_[root] == self) {
                roots_.remove_unordered(root);
                break;
            }
        }
    }
    const i64 previous = resolve(node.previous_sibling);
    if (previous >= 0) {
        hierarchy_[static_cast<usize>(previous)].next_sibling = node.next_sibling;
    }
    const i64 next = resolve(node.next_sibling);
    if (next >= 0) {
        hierarchy_[static_cast<usize>(next)].previous_sibling = node.previous_sibling;
    }
    node.parent = kNoElement;
    node.next_sibling = kNoElement;
    node.previous_sibling = kNoElement;
}

Status ElementStore::reparent(ElementId child, ElementId parent) noexcept {
    const i64 child_index = resolve(child);
    if (child_index < 0) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such element", 0});
    }
    const i64 parent_index = resolve(parent);
    if (parent.is_valid() && parent_index < 0) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such parent element", 0});
    }
    // A cycle would make every tree walk in this module non-terminating, so it is refused here
    // rather than found by a stack overflow in the arrange pass.
    for (ElementId walk = parent; walk.is_valid();) {
        if (walk == child) {
            return make_unexpected(
                Error{ErrorCode::InvalidArgument, "an element may not become its own ancestor", 0});
        }
        const i64 index = resolve(walk);
        if (index < 0) {
            break;
        }
        walk = hierarchy_[static_cast<usize>(index)].parent;
    }

    detach(static_cast<u32>(child_index));
    Hierarchy& node = hierarchy_[static_cast<usize>(child_index)];
    node.parent = parent;
    if (!parent.is_valid()) {
        node.depth = 0;
        if (Status pushed = roots_.push_back(child); !pushed) {
            return pushed;
        }
        mark(child, Dirty::Measure | Dirty::Arrange | Dirty::Paint);
        return ok();
    }

    Hierarchy& parent_node = hierarchy_[static_cast<usize>(parent_index)];
    node.depth = static_cast<u16>(parent_node.depth + 1U);
    node.previous_sibling = parent_node.last_child;
    node.next_sibling = kNoElement;
    const i64 last = resolve(parent_node.last_child);
    if (last >= 0) {
        hierarchy_[static_cast<usize>(last)].next_sibling = child;
    }
    parent_node.last_child = child;
    if (!parent_node.first_child.is_valid()) {
        parent_node.first_child = child;
    }
    ++parent_node.child_count;

    mark(child, Dirty::Measure | Dirty::Arrange | Dirty::Paint);
    mark(parent, Dirty::Measure | Dirty::Arrange);
    return ok();
}

Status ElementStore::destroy(ElementId element) noexcept {
    const i64 index = resolve(element);
    if (index < 0) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such element", 0});
    }
    // Depth-first, children before the parent, so the free list never holds a slot whose children
    // still name it.
    Array<ElementId> stack(free_.allocator());
    if (Status pushed = stack.push_back(element); !pushed) {
        return pushed;
    }
    Array<ElementId> order(free_.allocator());
    while (!stack.empty()) {
        const ElementId current = stack[stack.size() - 1];
        stack.pop_back();
        if (Status pushed = order.push_back(current); !pushed) {
            return pushed;
        }
        const i64 current_index = resolve(current);
        if (current_index < 0) {
            continue;
        }
        for (ElementId child = hierarchy_[static_cast<usize>(current_index)].first_child;
             child.is_valid();) {
            const i64 child_index = resolve(child);
            if (child_index < 0) {
                break;
            }
            const ElementId next = hierarchy_[static_cast<usize>(child_index)].next_sibling;
            if (Status pushed = stack.push_back(child); !pushed) {
                return pushed;
            }
            child = next;
        }
    }

    const ElementId parent = hierarchy_[static_cast<usize>(index)].parent;
    detach(static_cast<u32>(index));
    for (usize position = order.size(); position > 0; --position) {
        const ElementId victim = order[position - 1];
        const i64 victim_index = resolve(victim);
        if (victim_index < 0) {
            continue;
        }
        slots_[static_cast<usize>(victim_index)].alive = false;
        hierarchy_[static_cast<usize>(victim_index)] = Hierarchy{};
        dirty_[static_cast<usize>(victim_index)] = Dirty::None;
        if (Status pushed = free_.push_back(static_cast<u32>(victim_index)); !pushed) {
            return pushed;
        }
        --live_;
        ++destroyed_;
    }
    if (parent.is_valid()) {
        mark(parent, Dirty::Measure | Dirty::Arrange);
    }
    return ok();
}

const Hierarchy* ElementStore::hierarchy(ElementId element) const noexcept {
    const i64 index = resolve(element);
    return (index < 0) ? nullptr : &hierarchy_[static_cast<usize>(index)];
}

const LayoutInput* ElementStore::layout_input(ElementId element) const noexcept {
    const i64 index = resolve(element);
    return (index < 0) ? nullptr : &layout_in_[static_cast<usize>(index)];
}

LayoutInput* ElementStore::layout_input(ElementId element) noexcept {
    const i64 index = resolve(element);
    return (index < 0) ? nullptr : &layout_in_[static_cast<usize>(index)];
}

const LayoutOutput* ElementStore::layout_output(ElementId element) const noexcept {
    const i64 index = resolve(element);
    return (index < 0) ? nullptr : &layout_out_[static_cast<usize>(index)];
}

LayoutOutput* ElementStore::layout_output(ElementId element) noexcept {
    const i64 index = resolve(element);
    return (index < 0) ? nullptr : &layout_out_[static_cast<usize>(index)];
}

const PaintData* ElementStore::paint(ElementId element) const noexcept {
    const i64 index = resolve(element);
    return (index < 0) ? nullptr : &paint_[static_cast<usize>(index)];
}

PaintData* ElementStore::paint(ElementId element) noexcept {
    const i64 index = resolve(element);
    return (index < 0) ? nullptr : &paint_[static_cast<usize>(index)];
}

ElementFlags ElementStore::flags(ElementId element) const noexcept {
    const i64 index = resolve(element);
    return (index < 0) ? ElementFlags::None : flags_[static_cast<usize>(index)];
}

Status ElementStore::set_flags(ElementId element, ElementFlags value) noexcept {
    const i64 index = resolve(element);
    if (index < 0) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such element", 0});
    }
    const ElementFlags previous = flags_[static_cast<usize>(index)];
    flags_[static_cast<usize>(index)] = value;
    // A flag that changes what is DRAWN dirties paint; one that changes whether the element takes
    // part in layout dirties layout. Getting this wrong in the cheap direction is a stale frame; in
    // the expensive direction it is a relayout per hover.
    const bool layout_changed =
        has_flag(previous, ElementFlags::Collapsed) != has_flag(value, ElementFlags::Collapsed);
    mark(element, layout_changed ? (Dirty::Measure | Dirty::Arrange | Dirty::Paint) : Dirty::Paint);
    return ok();
}

Name ElementStore::type_of(ElementId element) const noexcept {
    const i64 index = resolve(element);
    return (index < 0) ? Name{} : type_[static_cast<usize>(index)];
}

u32 ElementStore::style(ElementId element) const noexcept {
    const i64 index = resolve(element);
    return (index < 0) ? 0U : style_[static_cast<usize>(index)];
}

Status ElementStore::set_style(ElementId element, u32 style_index) noexcept {
    const i64 index = resolve(element);
    if (index < 0) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such element", 0});
    }
    style_[static_cast<usize>(index)] = style_index;
    return ok();
}

u64 ElementStore::key(ElementId element) const noexcept {
    const i64 index = resolve(element);
    return (index < 0) ? 0ULL : key_[static_cast<usize>(index)];
}

Status ElementStore::set_key(ElementId element, u64 value) noexcept {
    const i64 index = resolve(element);
    if (index < 0) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such element", 0});
    }
    key_[static_cast<usize>(index)] = value;
    return ok();
}

HitTestMode ElementStore::hit_test_mode(ElementId element) const noexcept {
    const i64 index = resolve(element);
    return (index < 0) ? HitTestMode::Ignore : hit_[static_cast<usize>(index)];
}

Status ElementStore::set_hit_test_mode(ElementId element, HitTestMode mode) noexcept {
    const i64 index = resolve(element);
    if (index < 0) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such element", 0});
    }
    hit_[static_cast<usize>(index)] = mode;
    return ok();
}

Status ElementStore::children_of(ElementId element, Array<ElementId>& out) const noexcept {
    out.clear();
    const i64 index = resolve(element);
    if (index < 0) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such element", 0});
    }
    for (ElementId child = hierarchy_[static_cast<usize>(index)].first_child; child.is_valid();) {
        const i64 child_index = resolve(child);
        if (child_index < 0) {
            break;
        }
        if (Status pushed = out.push_back(child); !pushed) {
            return pushed;
        }
        child = hierarchy_[static_cast<usize>(child_index)].next_sibling;
    }
    return ok();
}

void ElementStore::mark_subtree_arrange(u32 index) noexcept {
    // ARRANGE PROPAGATES DOWNWARD. A parent's rect moving moves every descendant's rect, and an
    // arrange dirty that stopped at the parent would leave the subtree drawn where it used to be.
    dirty_[index] = dirty_[index] | Dirty::Arrange;
    for (ElementId child = hierarchy_[index].first_child; child.is_valid();) {
        const i64 child_index = resolve(child);
        if (child_index < 0) {
            break;
        }
        mark_subtree_arrange(static_cast<u32>(child_index));
        child = hierarchy_[static_cast<usize>(child_index)].next_sibling;
    }
}

void ElementStore::mark(ElementId element, Dirty state) noexcept {
    const i64 index = resolve(element);
    if (index < 0) {
        return;
    }
    const auto slot = static_cast<u32>(index);
    dirty_[slot] = dirty_[slot] | state;

    if (has_dirty(state, Dirty::Measure)) {
        // MEASURE PROPAGATES UPWARD, AND ONLY WHILE ANCESTORS DEPEND ON IT. An ancestor with an
        // explicit preferred size does not: its desired size is what the author wrote, whatever its
        // children do, so the walk stops there. That stop is the difference between "a label's text
        // grew" costing one panel and costing the document.
        ElementId walk = hierarchy_[slot].parent;
        while (walk.is_valid()) {
            const i64 parent_index = resolve(walk);
            if (parent_index < 0) {
                break;
            }
            const auto parent_slot = static_cast<u32>(parent_index);
            dirty_[parent_slot] = dirty_[parent_slot] | Dirty::Measure | Dirty::Arrange;
            const LayoutInput& input = layout_in_[parent_slot];
            const bool fixed = input.preferred.x >= 0.0F && input.preferred.y >= 0.0F;
            if (fixed) {
                break;
            }
            walk = hierarchy_[parent_slot].parent;
        }
    }
    if (has_dirty(state, Dirty::Arrange)) {
        mark_subtree_arrange(slot);
    }
    // PAINT PROPAGATES NOWHERE. It is the element's own.
}

Dirty ElementStore::dirty(ElementId element) const noexcept {
    const i64 index = resolve(element);
    return (index < 0) ? Dirty::None : dirty_[static_cast<usize>(index)];
}

void ElementStore::clear_dirty(ElementId element, Dirty state) noexcept {
    const i64 index = resolve(element);
    if (index < 0) {
        return;
    }
    const auto slot = static_cast<u32>(index);
    dirty_[slot] = static_cast<Dirty>(static_cast<u8>(dirty_[slot]) & ~static_cast<u8>(state));
}

Status ElementStore::collect_dirty(Dirty state, Array<ElementId>& out) const noexcept {
    out.clear();
    // Depth order, parents first: the arrange pass needs a parent's rect before its children's, and
    // sorting by the depth the hierarchy already stores costs one pass rather than a sort.
    u16 depth = 0;
    bool more = true;
    while (more) {
        more = false;
        for (usize index = 0; index < slots_.size(); ++index) {
            if (!slots_[index].alive) {
                continue;
            }
            if (hierarchy_[index].depth > depth) {
                more = true;
                continue;
            }
            if (hierarchy_[index].depth != depth || !has_dirty(dirty_[index], state)) {
                continue;
            }
            if (Status pushed =
                    out.push_back(ElementId{static_cast<u32>(index), slots_[index].generation});
                !pushed) {
                return pushed;
            }
        }
        ++depth;
        if (depth > 512) {
            break;  // A tree deeper than this is a cycle the reparent guard should have refused.
        }
    }
    return ok();
}

bool ElementStore::any_dirty() const noexcept {
    for (usize index = 0; index < slots_.size(); ++index) {
        if (slots_[index].alive && dirty_[index] != Dirty::None) {
            return true;
        }
    }
    return false;
}

StoreStats ElementStore::stats() const noexcept {
    StoreStats stats;
    stats.elements = static_cast<u32>(live_);
    stats.created = created_;
    stats.destroyed = destroyed_;
    stats.identity_churn = churn_;
    for (usize index = 0; index < slots_.size(); ++index) {
        if (!slots_[index].alive) {
            continue;
        }
        if (has_dirty(dirty_[index], Dirty::Measure)) {
            ++stats.measure_dirty;
        }
        if (has_dirty(dirty_[index], Dirty::Arrange)) {
            ++stats.arrange_dirty;
        }
        if (has_dirty(dirty_[index], Dirty::Paint)) {
            ++stats.paint_dirty;
        }
    }
    return stats;
}

}  // namespace cy::ui
