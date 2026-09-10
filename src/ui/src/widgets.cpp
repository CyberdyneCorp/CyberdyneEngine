// The reconciler, virtualisation and data binding. M8.b tasks 9.1 and 9.2.

#include <cy/ui/widgets.h>

#include <cmath>

namespace cy::ui {

Expected<u32, Error> Description::add(const DescriptionNode& node) noexcept {
    if (node.parent != DescriptionNode::kNoParent && node.parent >= nodes_.size()) {
        // Parents before children, always: a description whose parent index points forward would
        // make the reconciler's single pass impossible and is a caller's bug rather than a shape to
        // support.
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "a description node's parent must already have been added",
                                     0});
    }
    const auto index = static_cast<u32>(nodes_.size());
    if (Status pushed = nodes_.push_back(node); !pushed) {
        return make_unexpected(pushed.error());
    }
    return index;
}

namespace {

/// Whether an existing element can be REUSED for a description node: the same type, and the same
/// key. A type change is a replacement, because a label becoming a button shares nothing.
[[nodiscard]] bool matches(const ElementStore& store, ElementId element,
                           const DescriptionNode& node) noexcept {
    return store.type_of(element) == node.type && store.key(element) == node.key;
}

/// Apply a node's properties to an element, dirtying at the finest granularity that is correct.
[[nodiscard]] Status apply_node(ElementStore& store, ElementId element,
                                const DescriptionNode& node) noexcept {
    LayoutInput* layout = store.layout_input(element);
    PaintData* paint = store.paint(element);
    if (layout == nullptr || paint == nullptr) {
        return make_unexpected(Error{ErrorCode::NotFound, "no such element", 0});
    }
    const bool layout_changed =
        layout->preferred.x != node.layout.preferred.x ||
        layout->preferred.y != node.layout.preferred.y || layout->model != node.layout.model ||
        layout->direction != node.layout.direction || layout->gap != node.layout.gap ||
        layout->flex_grow != node.layout.flex_grow ||
        layout->padding.left != node.layout.padding.left;
    const bool paint_changed = paint->background != node.paint.background ||
                               paint->tint != node.paint.tint ||
                               paint->opacity != node.paint.opacity;

    *layout = node.layout;
    *paint = node.paint;
    if (Status set = store.set_hit_test_mode(element, node.hit_test); !set) {
        return set;
    }
    if (store.flags(element) != node.flags) {
        if (Status set = store.set_flags(element, node.flags); !set) {
            return set;
        }
    }
    if (layout_changed) {
        store.mark(element, Dirty::Measure | Dirty::Arrange | Dirty::Paint);
    } else if (paint_changed) {
        // PAINT ONLY. A colour that changed is not a reason to relayout a document, and this branch
        // is the whole of "WHEN a label's numeric text changes without changing its measured size
        // THEN only its paint state SHALL be dirtied".
        store.mark(element, Dirty::Paint);
    }
    return ok();
}

}  // namespace

Status reconcile(ElementStore& store, ElementId root, const Description& description,
                 ReconcileReport& report) noexcept {
    report = ReconcileReport{};
    if (!store.alive(root)) {
        return make_unexpected(Error{ErrorCode::NotFound, "the reconcile root does not exist", 0});
    }

    // The element each description node became, so a child can find its parent's element in one
    // pass — which is why a description's parents come before its children.
    Array<ElementId> mapped(store.allocator());
    if (Status sized = mapped.resize(description.size()); !sized) {
        return sized;
    }

    Array<ElementId> existing(store.allocator());
    Array<ElementId> claimed(store.allocator());

    for (usize index = 0; index < description.nodes().size(); ++index) {
        const DescriptionNode& node = description.nodes()[index];
        const ElementId parent =
            (node.parent == DescriptionNode::kNoParent) ? root : mapped[node.parent];
        if (Status children = store.children_of(parent, existing); !children) {
            return children;
        }

        // How many of this parent's children the description has already claimed tells us which
        // POSITION this node is at — the structural half of identity.
        u32 position = 0;
        for (usize probe = 0; probe < index; ++probe) {
            const DescriptionNode& earlier = description.nodes()[probe];
            const ElementId earlier_parent =
                (earlier.parent == DescriptionNode::kNoParent) ? root : mapped[earlier.parent];
            if (earlier_parent == parent) {
                ++position;
            }
        }

        ElementId element;
        if (node.key != 0) {
            // A KEY: state follows the key wherever the element moved to.
            for (const ElementId candidate : existing.span()) {
                if (store.key(candidate) == node.key && store.type_of(candidate) == node.type) {
                    element = candidate;
                    break;
                }
            }
            if (element.is_valid() && position < existing.size() && existing[position] != element) {
                // The keyed element moved: its identity is intact and its POSITION changed, which
                // is exactly what a key is for — and it is not churn.
                ++report.preserved;
            }
        } else if (position < existing.size() && matches(store, existing[position], node)) {
            element = existing[position];
        } else if (position < existing.size() && store.type_of(existing[position]) == node.type &&
                   store.key(existing[position]) != node.key) {
            // The same position now holds a different key. THE IDENTITY CHANGED, and this is the
            // churn the diagnostics ask for: a list that lost its state because its rows have no
            // keys reports here rather than mystifying somebody.
            ++report.churn;
        }

        if (!element.is_valid()) {
            auto created = store.create(parent, node.type);
            if (!created) {
                return make_unexpected(created.error());
            }
            element = created.value();
            if (Status set = store.set_key(element, node.key); !set) {
                return set;
            }
            ++report.created;
        } else {
            ++report.updated;
        }
        mapped[index] = element;
        if (Status pushed = claimed.push_back(element); !pushed) {
            return pushed;
        }
        if (Status applied = apply_node(store, element, node); !applied) {
            return applied;
        }
    }

    // Anything under the root the description no longer names is destroyed. A single pass over the
    // claimed set: a description with a thousand nodes is a thousand entries, and a hash set for a
    // list this size costs more than it saves.
    Array<ElementId> stack(store.allocator());
    if (Status pushed = stack.push_back(root); !pushed) {
        return pushed;
    }
    Array<ElementId> doomed(store.allocator());
    while (!stack.empty()) {
        const ElementId element = stack[stack.size() - 1];
        stack.pop_back();
        if (Status children = store.children_of(element, existing); !children) {
            return children;
        }
        for (const ElementId child : existing.span()) {
            bool kept = false;
            for (const ElementId candidate : claimed.span()) {
                if (candidate == child) {
                    kept = true;
                    break;
                }
            }
            if (kept) {
                if (Status pushed = stack.push_back(child); !pushed) {
                    return pushed;
                }
                continue;
            }
            if (Status pushed = doomed.push_back(child); !pushed) {
                return pushed;
            }
        }
    }
    for (const ElementId element : doomed.span()) {
        if (!store.alive(element)) {
            continue;  // already destroyed as part of an ancestor's subtree
        }
        if (Status destroyed = store.destroy(element); !destroyed) {
            return destroyed;
        }
        ++report.removed;
    }

    report.preserved += report.updated;
    store.note_identity_churn(report.churn);
    return ok();
}

VirtualRange virtual_range(const VirtualList& list) noexcept {
    VirtualRange range;
    const f32 height = (list.item_height > 0.0F) ? list.item_height : 1.0F;
    range.total_height = height * static_cast<f32>(list.item_count);
    if (list.item_count == 0 || list.viewport_height <= 0.0F) {
        return range;
    }

    // ARITHMETIC, NOT A WALK. This is why a hundred thousand rows cost what a hundred do: the
    // function never touches an item.
    const f32 scroll = (list.scroll < 0.0F) ? 0.0F : list.scroll;
    const auto first = static_cast<u32>(std::floor(scroll / height));
    const auto visible = static_cast<u32>(std::ceil(list.viewport_height / height)) + 1U;

    range.first = (first > list.overscan) ? (first - list.overscan) : 0U;
    const u64 last = static_cast<u64>(first) + visible + list.overscan;
    range.last = (last > list.item_count) ? list.item_count : static_cast<u32>(last);
    range.offset = -(scroll - (static_cast<f32>(range.first) * height));
    return range;
}

bool BoundValue::operator==(const BoundValue& other) const noexcept {
    if (kind != other.kind) {
        return false;
    }
    switch (kind) {
        case Kind::Number:
            return number == other.number;
        case Kind::Colour:
            return colour == other.colour;
        case Kind::Text:
            return text == other.text;
        case Kind::Boolean:
            return boolean == other.boolean;
    }
    return false;
}

namespace {

/// Whether a property changes the element's size. The binding's dirty granularity turns on this
/// answer, and having it in one function is what keeps the answer consistent.
[[nodiscard]] bool affects_layout(StyleProperty property) noexcept {
    switch (property) {
        case StyleProperty::Width:
        case StyleProperty::Height:
        case StyleProperty::MinWidth:
        case StyleProperty::MinHeight:
        case StyleProperty::MaxWidth:
        case StyleProperty::MaxHeight:
        case StyleProperty::MarginLeft:
        case StyleProperty::MarginTop:
        case StyleProperty::MarginRight:
        case StyleProperty::MarginBottom:
        case StyleProperty::PaddingLeft:
        case StyleProperty::PaddingTop:
        case StyleProperty::PaddingRight:
        case StyleProperty::PaddingBottom:
        case StyleProperty::FlexGrow:
        case StyleProperty::FlexShrink:
        case StyleProperty::FlexBasis:
        case StyleProperty::Gap:
        case StyleProperty::AspectRatio:
        case StyleProperty::FontSize:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] Status push_value(ElementStore& store, const Binding& binding,
                                const BoundValue& value) noexcept {
    LayoutInput* layout = store.layout_input(binding.element);
    PaintData* paint = store.paint(binding.element);
    if (layout == nullptr || paint == nullptr) {
        return make_unexpected(Error{ErrorCode::NotFound, "the bound element does not exist", 0});
    }
    switch (binding.property) {
        case StyleProperty::Width:
            layout->preferred.x = value.number;
            break;
        case StyleProperty::Height:
            layout->preferred.y = value.number;
            break;
        case StyleProperty::BackgroundColour:
            paint->background = value.colour;
            break;
        case StyleProperty::Colour:
            paint->tint = value.colour;
            break;
        case StyleProperty::Opacity:
            paint->opacity = value.number;
            break;
        case StyleProperty::FlexGrow:
            layout->flex_grow = value.number;
            break;
        default:
            return make_unexpected(
                Error{ErrorCode::Unsupported, "this property cannot be bound directly", 0});
    }
    return ok();
}

}  // namespace

Status update_bindings(ElementStore& store, Span<Binding> bindings,
                       BindingReport& report) noexcept {
    report = BindingReport{};
    for (Binding& binding : bindings) {
        ++report.evaluated;
        if (binding.source == nullptr || !store.alive(binding.element)) {
            continue;
        }
        // ONLY WHEN THE SOURCE CHANGED. "Bindings ... SHALL update only when their source changes".
        if (!binding.source->changed()) {
            continue;
        }
        if (binding.editing && binding.mode == BindingMode::TwoWay) {
            // "external changes SHALL update the field unless it is being edited".
            continue;
        }
        BoundValue value = binding.source->read();
        if (binding.converter != nullptr) {
            value = binding.converter(value, binding.converter_user);
        }
        if (value == binding.last) {
            // The source said it changed and produced the same value. Counted rather than acted on:
            // a large number here is a change-detection problem in the source.
            ++report.spurious;
            continue;
        }
        if (Status pushed = push_value(store, binding, value); !pushed) {
            return pushed;
        }
        binding.last = value;
        ++report.updated;
        if (affects_layout(binding.property)) {
            store.mark(binding.element, Dirty::Measure | Dirty::Arrange | Dirty::Paint);
            ++report.layout_dirtied;
        } else {
            store.mark(binding.element, Dirty::Paint);
            ++report.paint_dirtied;
        }
    }
    return ok();
}

Status write_back(Binding& binding, const BoundValue& value) noexcept {
    if (binding.mode != BindingMode::TwoWay) {
        return make_unexpected(Error{ErrorCode::PermissionDenied, "this binding is one-way", 0});
    }
    if (binding.source == nullptr) {
        return make_unexpected(Error{ErrorCode::NotFound, "the binding has no source", 0});
    }
    if (Status written = binding.source->write(value); !written) {
        return written;
    }
    binding.last = value;
    return ok();
}

}  // namespace cy::ui
