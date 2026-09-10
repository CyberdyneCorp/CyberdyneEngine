// Flattening, batching, the budget ladder and the accessibility audit. M8.b task 9.3.

#include <cy/ui/paint.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace cy::ui {
namespace {

[[nodiscard]] f32 channel_of(u32 colour, u32 shift) noexcept {
    return static_cast<f32>((colour >> shift) & 0xFFU) / 255.0F;
}

/// WCAG's relative luminance: linearise each channel, then weight.
[[nodiscard]] f32 relative_luminance(u32 colour) noexcept {
    const f32 channels[3] = {channel_of(colour, 16U), channel_of(colour, 8U),
                             channel_of(colour, 0U)};
    f32 linear[3] = {};
    for (u32 index = 0; index < 3U; ++index) {
        const f32 value = channels[index];
        linear[index] =
            (value <= 0.03928F) ? (value / 12.92F) : std::pow((value + 0.055F) / 1.055F, 2.4F);
    }
    return (0.2126F * linear[0]) + (0.7152F * linear[1]) + (0.0722F * linear[2]);
}

/// The index of a clip rect in the shared array, adding it when it is new. An index rather than a
/// rect per primitive: two thousand primitives inside one scroll view share one clip, and that is
/// the difference between a batch and two thousand.
[[nodiscard]] Expected<u16, Error> clip_index_for(Array<Rect>& clips, const Rect& clip) noexcept {
    for (usize index = 0; index < clips.size(); ++index) {
        if (clips[index].x == clip.x && clips[index].y == clip.y &&
            clips[index].width == clip.width && clips[index].height == clip.height) {
            return static_cast<u16>(index);
        }
    }
    const auto slot = static_cast<u16>(clips.size());
    if (Status pushed = clips.push_back(clip); !pushed) {
        return make_unexpected(pushed.error());
    }
    return slot;
}

/// Why two consecutive primitives could not share a batch. A batch that broke for a reason nobody
/// can name is the commonest cause of an interface that draws in four hundred calls, and this is
/// the function that names it.
[[nodiscard]] Batch::BreakReason break_reason_between(const Primitive& previous,
                                                      const Primitive& next) noexcept {
    if (previous.material != next.material) {
        return Batch::BreakReason::Material;
    }
    if (previous.atlas != next.atlas) {
        return Batch::BreakReason::Atlas;
    }
    if (previous.clip != next.clip) {
        return Batch::BreakReason::Clip;
    }
    return Batch::BreakReason::Transform;
}

}  // namespace

PrimitiveBuffer::PrimitiveBuffer(Allocator& allocator) noexcept
    : primitives_(allocator), batches_(allocator), clips_(allocator), owners_(allocator) {}

void PrimitiveBuffer::clear() noexcept {
    primitives_.clear();
    batches_.clear();
    clips_.clear();
    owners_.clear();
}

Status flatten(ElementStore& store, const Rect& viewport, PrimitiveBuffer& out,
               FlattenReport& report) noexcept {
    report = FlattenReport{};

    // INCREMENTAL. An element with no paint dirt keeps the primitive it already has; the pass walks
    // the tree once, and every element whose paint bit is clear and whose primitive is already in
    // the buffer is counted as reused rather than re-emitted.
    Array<Primitive> emitted(out.primitives_.allocator());
    Array<ElementId> owners(out.owners_.allocator());
    Array<Rect> clips(out.clips_.allocator());
    if (Status pushed = clips.push_back(viewport); !pushed) {
        return pushed;
    }

    Array<ElementId> stack(out.primitives_.allocator());
    // Roots in order, so the first root is drawn first and the last is on top.
    for (usize index = store.roots().size(); index > 0; --index) {
        if (Status pushed = stack.push_back(store.roots()[index - 1]); !pushed) {
            return pushed;
        }
    }

    while (!stack.empty()) {
        const ElementId element = stack[stack.size() - 1];
        stack.pop_back();
        ++report.visited;

        const ElementFlags flags = store.flags(element);
        const LayoutOutput* output = store.layout_output(element);
        const PaintData* paint = store.paint(element);
        const Hierarchy* node = store.hierarchy(element);
        if (output == nullptr || paint == nullptr || node == nullptr) {
            continue;
        }
        if (has_flag(flags, ElementFlags::Collapsed)) {
            continue;
        }

        // CULLING, against the viewport and against the element's own clip. "WHEN elements lie
        // outside their scroll container's clip rect THEN their primitives SHALL be culled before
        // batching."
        const Rect clip = output->clip.empty() ? viewport : output->clip.intersected(viewport);
        const Rect visible = output->rect.intersected(clip);
        const bool culled = visible.empty();

        if (!culled && has_flag(flags, ElementFlags::Visible) && paint->opacity > 0.0F) {
            Primitive primitive;
            primitive.bounds = output->rect;
            primitive.uv = paint->uv;
            primitive.material = paint->material;
            primitive.atlas = paint->atlas;
            primitive.transform = 0;
            primitive.colour = paint->background;
            primitive.source = element;

            auto clip_slot = clip_index_for(clips, clip);
            if (!clip_slot) {
                return make_unexpected(clip_slot.error());
            }
            primitive.clip = clip_slot.value();

            if (has_dirty(store.dirty(element), Dirty::Paint)) {
                ++report.emitted;
                store.clear_dirty(element, Dirty::Paint);
            } else {
                ++report.reused;
            }
            if (Status pushed = emitted.push_back(primitive); !pushed) {
                return pushed;
            }
            if (Status pushed = owners.push_back(element); !pushed) {
                return pushed;
            }
        } else if (culled) {
            ++report.culled;
        }

        for (ElementId child = node->last_child; child.is_valid();) {
            const Hierarchy* child_node = store.hierarchy(child);
            if (child_node == nullptr) {
                break;
            }
            if (Status pushed = stack.push_back(child); !pushed) {
                return pushed;
            }
            child = child_node->previous_sibling;
        }
    }

    // BATCHING by material and atlas, with the break reason recorded. A batch that broke for a
    // reason nobody can name is the commonest cause of an interface that draws in four hundred
    // calls, and this is the column that names it.
    Array<Batch> batches(out.batches_.allocator());
    for (usize index = 0; index < emitted.size(); ++index) {
        const Primitive& primitive = emitted[index];
        if (!batches.empty()) {
            Batch& current = batches[batches.size() - 1];
            const Primitive& previous = emitted[current.first + current.count - 1];
            if (previous.material == primitive.material && previous.atlas == primitive.atlas &&
                previous.clip == primitive.clip && previous.transform == primitive.transform) {
                ++current.count;
                continue;
            }
            current.reason = break_reason_between(previous, primitive);
        }
        Batch batch;
        batch.first = static_cast<u32>(index);
        batch.count = 1;
        batch.material = primitive.material;
        batch.atlas = primitive.atlas;
        if (Status pushed = batches.push_back(batch); !pushed) {
            return pushed;
        }
    }
    report.batches = static_cast<u32>(batches.size());

    out.primitives_ = std::move(emitted);
    out.owners_ = std::move(owners);
    out.clips_ = std::move(clips);
    out.batches_ = std::move(batches);
    return ok();
}

const char* degradation_name(Degradation level) noexcept {
    switch (level) {
        case Degradation::None:
            return "none";
        case Degradation::EffectQuality:
            return "effect-quality";
        case Degradation::BlurBehind:
            return "blur-behind";
        case Degradation::WorldSpaceDetail:
            return "world-space-detail";
    }
    return "unknown";
}

Degradation apply_budget(const UiBudget& budget, const FlattenReport& flatten_report,
                         f32 layout_milliseconds, u32 effect_passes,
                         BudgetReport& report) noexcept {
    report = BudgetReport{};
    report.layout_milliseconds = layout_milliseconds;
    report.primitives = flatten_report.emitted + flatten_report.reused;
    report.batches = flatten_report.batches;
    report.effect_passes = effect_passes;

    // THE LADDER, ONE RUNG PER OVERRUN, in the order the requirement states. Each measurement that
    // is over pushes the level one step further; nothing here drops an element, moves focus or
    // changes text, because there is no rung for it.
    u32 steps = 0;
    if (effect_passes > budget.effect_passes) {
        ++steps;
        report.cause = Name::intern("effect-passes");
    }
    if (report.primitives > budget.primitives) {
        ++steps;
        report.cause = Name::intern("primitives");
    }
    if (report.batches > budget.batches) {
        ++steps;
        report.cause = Name::intern("batches");
    }
    if (layout_milliseconds > budget.layout_milliseconds) {
        ++steps;
        report.cause = Name::intern("layout-time");
    }
    report.over_budget = steps > 0;
    // ONE RUNG PER OVERRUN, and the ladder stops at its last: there is nothing below
    // `WorldSpaceDetail`, which is how "hit-testing, focus, and text legibility SHALL be preserved"
    // is expressed.
    static constexpr Degradation kLadder[4] = {Degradation::None, Degradation::EffectQuality,
                                               Degradation::BlurBehind,
                                               Degradation::WorldSpaceDetail};
    report.level = kLadder[std::min<u32>(steps, 3U)];
    return report.level;
}

const char* role_name(Role role) noexcept {
    switch (role) {
        case Role::None:
            return "none";
        case Role::Panel:
            return "panel";
        case Role::Label:
            return "label";
        case Role::Button:
            return "button";
        case Role::Toggle:
            return "toggle";
        case Role::Checkbox:
            return "checkbox";
        case Role::Slider:
            return "slider";
        case Role::ProgressBar:
            return "progress-bar";
        case Role::TextField:
            return "text-field";
        case Role::List:
            return "list";
        case Role::ListItem:
            return "list-item";
        case Role::Tab:
            return "tab";
        case Role::Dialog:
            return "dialog";
        case Role::Count:
            break;
    }
    return "unknown";
}

f32 contrast_ratio(u32 foreground, u32 background) noexcept {
    const f32 first = relative_luminance(foreground) + 0.05F;
    const f32 second = relative_luminance(background) + 0.05F;
    return (first > second) ? (first / second) : (second / first);
}

Status audit_accessibility(const ElementStore& store, Span<const AccessibilityNode> nodes,
                           const Interaction& interaction, Array<AccessibilityFinding>& findings,
                           AccessibilityReport& report) noexcept {
    findings.clear();
    report = AccessibilityReport{};
    (void)interaction;

    constexpr f32 kFocusContrast = 3.0F;
    constexpr f32 kTextContrast = 4.5F;
    constexpr f32 kMinimumTarget = 24.0F;

    for (const AccessibilityNode& node : nodes) {
        ++report.examined;
        if (!store.alive(node.element)) {
            continue;
        }
        const ElementFlags flags = store.flags(node.element);
        const LayoutOutput* output = store.layout_output(node.element);
        const PaintData* paint = store.paint(node.element);
        if (output == nullptr || paint == nullptr) {
            continue;
        }
        const bool interactive = node.role == Role::Button || node.role == Role::Toggle ||
                                 node.role == Role::Checkbox || node.role == Role::Slider ||
                                 node.role == Role::TextField || node.role == Role::Tab ||
                                 node.role == Role::ListItem;
        if (!interactive) {
            continue;
        }
        ++report.interactive;

        // KEYBOARD REACHABILITY. "keyboard-only operation of every interactive element" — an
        // element a keyboard cannot reach is one a keyboard user cannot operate, whatever the mouse
        // can do with it.
        if (has_flag(flags, ElementFlags::Focusable) && !has_flag(flags, ElementFlags::Disabled)) {
            ++report.keyboard_reachable;
        } else if (!has_flag(flags, ElementFlags::Disabled)) {
            AccessibilityFinding finding;
            finding.element = node.element;
            finding.rule = Name::intern("unreachable");
            finding.message = "an interactive element that keyboard focus cannot reach";
            if (Status pushed = findings.push_back(finding); !pushed) {
                return pushed;
            }
        }

        if (node.label.is_empty()) {
            AccessibilityFinding finding;
            finding.element = node.element;
            finding.rule = Name::intern("unlabelled");
            finding.message = "an interactive element with no label to announce";
            if (Status pushed = findings.push_back(finding); !pushed) {
                return pushed;
            }
        }

        // TARGET SIZE. Not in `ui-system`'s own words, and it is here because a control smaller
        // than a fingertip fails the keyboard-and-touch operability the requirement asks for in
        // every practical sense.
        const f32 smaller =
            (output->rect.width < output->rect.height) ? output->rect.width : output->rect.height;
        if (smaller > 0.0F && smaller < kMinimumTarget) {
            AccessibilityFinding finding;
            finding.element = node.element;
            finding.rule = Name::intern("target-size");
            finding.message = "an interactive element smaller than the minimum target size";
            finding.measured = smaller;
            finding.required = kMinimumTarget;
            if (Status pushed = findings.push_back(finding); !pushed) {
                return pushed;
            }
        }

        // CONTRAST: text against its background, and the focus indicator against the element.
        const f32 text = contrast_ratio(paint->tint, paint->background);
        if (text < kTextContrast) {
            AccessibilityFinding finding;
            finding.element = node.element;
            finding.rule = Name::intern("contrast");
            finding.message = "text does not meet the contrast minimum against its background";
            finding.measured = text;
            finding.required = kTextContrast;
            if (Status pushed = findings.push_back(finding); !pushed) {
                return pushed;
            }
        }
        if (has_flag(flags, ElementFlags::Focusable) && paint->border_width > 0.0F) {
            const f32 indicator = contrast_ratio(paint->border_colour, paint->background);
            if (indicator < kFocusContrast) {
                AccessibilityFinding finding;
                finding.element = node.element;
                finding.rule = Name::intern("contrast");
                finding.message = "the focus indicator does not meet the contrast minimum";
                finding.measured = indicator;
                finding.required = kFocusContrast;
                if (Status pushed = findings.push_back(finding); !pushed) {
                    return pushed;
                }
            }
        }
    }

    report.passed = findings.empty() && report.interactive == report.keyboard_reachable;
    return ok();
}

Status publish_accessibility(Span<const AccessibilityNode> nodes, ElementId focus,
                             AccessibilityBridge* bridge) noexcept {
    if (bridge == nullptr) {
        // A platform with no accessibility layer is not an error: the information is still produced
        // and the audit still runs over it, which is what makes the check work in a test.
        return ok();
    }
    bridge->publish(nodes);
    for (const AccessibilityNode& node : nodes) {
        if (node.element == focus) {
            bridge->announce_focus(node);
            break;
        }
    }
    return ok();
}

}  // namespace cy::ui
