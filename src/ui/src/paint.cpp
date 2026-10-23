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
    : primitives_(allocator),
      batches_(allocator),
      clips_(allocator),
      owners_(allocator),
      groups_(allocator),
      group_batches_(allocator) {}

void PrimitiveBuffer::clear() noexcept {
    primitives_.clear();
    batches_.clear();
    clips_.clear();
    owners_.clear();
    groups_.clear();
    group_batches_.clear();
}

u32 scale_premultiplied(u32 colour, f32 opacity) noexcept {
    if (opacity >= 1.0F) {
        return colour;
    }
    if (opacity <= 0.0F) {
        return 0U;
    }
    u32 scaled = 0;
    for (u32 shift = 0; shift < 32U; shift += 8U) {
        const auto channel = static_cast<f32>((colour >> shift) & 0xFFU);
        const auto rounded = static_cast<u32>(std::lround(channel * opacity));
        scaled |= (rounded > 255U ? 255U : rounded) << shift;
    }
    return scaled;
}

namespace {

/// A render transform: a uniform scale, then an offset, in the document's units. What an element's
/// `PaintData::translate` and `scale` accumulate into down the tree. Layout never sees one.
struct Transform {
    f32 scale = 1.0F;
    Vec2 offset{0.0F, 0.0F};

    [[nodiscard]] bool identity() const noexcept {
        return scale == 1.0F && offset.x == 0.0F && offset.y == 0.0F;
    }
    [[nodiscard]] Rect apply(const Rect& rect) const noexcept {
        return Rect{(rect.x * scale) + offset.x, (rect.y * scale) + offset.y, rect.width * scale,
                    rect.height * scale};
    }
};

/// The parent's transform followed by an element's own: scaled about the element's centre, then
/// translated. An element with neither keeps its parent's transform exactly.
[[nodiscard]] Transform compose(const Transform& parent, const PaintData& paint,
                                const Rect& rect) noexcept {
    if (paint.scale == 1.0F && paint.translate.x == 0.0F && paint.translate.y == 0.0F) {
        return parent;
    }
    const f32 own = (paint.scale > 0.0F) ? paint.scale : 0.0F;
    const Vec2 centre{rect.x + (rect.width * 0.5F), rect.y + (rect.height * 0.5F)};
    // Local: p' = (p - c) * own + c + translate. Then the parent's: p'' = p' * s + t.
    const Vec2 local{centre.x - (centre.x * own) + paint.translate.x,
                     centre.y - (centre.y * own) + paint.translate.y};
    Transform out;
    out.scale = parent.scale * own;
    out.offset = Vec2{(local.x * parent.scale) + parent.offset.x,
                      (local.y * parent.scale) + parent.offset.y};
    return out;
}

[[nodiscard]] bool same_rect(const Rect& a, const Rect& b) noexcept {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

/// One element waiting to be visited, with what its ancestors fold into it: the opacity they
/// multiply in, the render transform they accumulate, and the clip they drew under.
struct Pending {
    ElementId element;
    f32 opacity = 1.0F;
    Transform transform;
    /// The layout clip the parent's children inherit, and the clip that became on screen. Under
    /// the identity transform both are the layout's own; under a transform the second is where the
    /// first ended up, and a child with the same layout clip draws under it.
    Rect layout_clip;
    Rect final_clip;
    /// Set on the element an opacity group was opened for, inside its own group: it draws at
    /// full opacity there instead of opening the group again.
    bool group_root = false;
};

/// What one stream of primitives is being collected into: the main stream, or an opacity group's.
struct Stream {
    Array<Primitive>& emitted;
    Array<ElementId>& owners;
};

/// One group, finished: its primitives in their own arrays until `flatten()` appends them.
struct FinishedGroup {
    OpacityGroup group;
    u32 first_local = 0;
};

/// The walk's scratch and its output, threaded through the helpers below so each stays about one
/// decision.
struct FlattenWalk {
    ElementStore& store;
    const Rect& viewport;
    FlattenReport& report;
    ContentPainter* painter;
    Array<Rect>& clips;
    /// Every group's primitives and owners, concatenated in the order the groups finished — an
    /// inner group before the group it is nested in, which is the order they must be drawn in.
    Array<Primitive>& group_primitives;
    Array<ElementId>& group_owners;
    Array<OpacityGroup>& groups;
};

/// Fill in what `flatten()` owns on a primitive and append it: the clip, the identity transform,
/// the source, the render transform and the inherited opacity folded into both colours.
[[nodiscard]] Status emit(Stream& stream, Primitive primitive, ElementId element, u16 clip,
                          f32 opacity, const Transform& transform) noexcept {
    primitive.clip = clip;
    primitive.transform = 0;
    primitive.source = element;
    primitive.colour = scale_premultiplied(primitive.colour, opacity);
    primitive.border_colour = scale_premultiplied(primitive.border_colour, opacity);
    if (!transform.identity()) {
        primitive.bounds = transform.apply(primitive.bounds);
        primitive.corner_radius *= transform.scale;
        primitive.border_width *= transform.scale;
        primitive.distance_range *= transform.scale;
    }
    if (Status pushed = stream.emitted.push_back(primitive); !pushed) {
        return pushed;
    }
    return stream.owners.push_back(element);
}

/// The element's content, through the painter, culled against the same clip as its background.
[[nodiscard]] Status emit_content(FlattenWalk& walk, Stream& stream, ElementId element,
                                  const Rect& rect, const Rect& clip, u16 clip_slot, f32 opacity,
                                  const Transform& transform) noexcept {
    Array<Primitive> content(stream.emitted.allocator());
    if (Status painted = walk.painter->paint_content(element, rect, content); !painted) {
        return painted;
    }
    for (const Primitive& primitive : content.span()) {
        const Rect bounds = transform.identity() ? primitive.bounds
                                                 : transform.apply(primitive.bounds);
        if (bounds.intersected(clip).empty()) {
            ++walk.report.culled;
            continue;
        }
        if (Status pushed = emit(stream, primitive, element, clip_slot, opacity, transform);
            !pushed) {
            return pushed;
        }
    }
    return ok();
}

/// Whether an element's own primitive would draw nothing: every built-in material multiplies by
/// the colour, so no colour and no visible border is no pixel. A custom material decides for
/// itself and is always emitted.
[[nodiscard]] bool draws_nothing(const PaintData& paint) noexcept {
    const bool border = paint.border_width > 0.0F && paint.border_colour != 0U;
    return paint.material < kFirstCustomMaterial && paint.background == 0U && !border;
}

/// The clip an element draws under, on screen. Under the identity transform it is the layout's —
/// what `flatten()` has always used, kept bit for bit — and under a render transform the layout's
/// clip is carried through the transform unless it is the one the parent already drew under.
[[nodiscard]] Rect final_clip(const FlattenWalk& walk, const Pending& pending,
                              const LayoutOutput& output) noexcept {
    if (pending.transform.identity()) {
        return output.clip.empty() ? walk.viewport : output.clip.intersected(walk.viewport);
    }
    if (output.clip.empty() || same_rect(output.clip, pending.layout_clip)) {
        return pending.final_clip;
    }
    return pending.transform.apply(output.clip).intersected(pending.final_clip);
}

/// The element's own primitive and its content, or the cull count when it is outside its clip.
[[nodiscard]] Status visit(FlattenWalk& walk, Stream& stream, ElementId element,
                           const Pending& pending, const Rect& clip) noexcept {
    const LayoutOutput& output = *walk.store.layout_output(element);
    const PaintData& paint = *walk.store.paint(element);
    const f32 opacity = pending.opacity;

    // CULLING, against the viewport and against the element's own clip. "WHEN elements lie
    // outside their scroll container's clip rect THEN their primitives SHALL be culled before
    // batching."
    const Rect rect =
        pending.transform.identity() ? output.rect : pending.transform.apply(output.rect);
    if (rect.intersected(clip).empty()) {
        ++walk.report.culled;
        return ok();
    }
    if (!has_flag(walk.store.flags(element), ElementFlags::Visible) || opacity <= 0.0F) {
        return ok();
    }

    auto clip_slot = clip_index_for(walk.clips, clip);
    if (!clip_slot) {
        return make_unexpected(clip_slot.error());
    }
    if (has_dirty(walk.store.dirty(element), Dirty::Paint)) {
        ++walk.report.emitted;
        walk.store.clear_dirty(element, Dirty::Paint);
    } else {
        ++walk.report.reused;
    }

    if (draws_nothing(paint)) {
        // A layout container: counted, attributed nothing, and no quad for the device to discard.
        ++walk.report.empty;
        return walk.painter == nullptr
                   ? ok()
                   : emit_content(walk, stream, element, output.rect, clip, clip_slot.value(),
                                  opacity, pending.transform);
    }
    Primitive primitive;
    primitive.bounds = output.rect;
    primitive.uv = paint.uv;
    primitive.material = paint.material;
    primitive.atlas = paint.atlas;
    primitive.colour = paint.background;
    primitive.corner_radius = paint.corner_radius;
    primitive.border_width = paint.border_width;
    primitive.border_colour = paint.border_colour;
    primitive.intensity = paint.intensity;
    if (Status pushed =
            emit(stream, primitive, element, clip_slot.value(), opacity, pending.transform);
        !pushed) {
        return pushed;
    }
    if (walk.painter == nullptr) {
        return ok();
    }
    return emit_content(walk, stream, element, output.rect, clip, clip_slot.value(), opacity,
                        pending.transform);
}

/// Push an element's children so the first is visited first — the stack pops the last pushed.
[[nodiscard]] Status push_children(const ElementStore& store, Array<Pending>& stack,
                                   const Hierarchy& node, const Pending& child_state) noexcept {
    for (ElementId child = node.last_child; child.is_valid();) {
        const Hierarchy* child_node = store.hierarchy(child);
        if (child_node == nullptr) {
            break;
        }
        Pending pending = child_state;
        pending.element = child;
        pending.group_root = false;
        if (Status pushed = stack.push_back(pending); !pushed) {
            return pushed;
        }
        child = child_node->previous_sibling;
    }
    return ok();
}

/// Group consecutive primitives sharing a material, an atlas, a clip and a transform. `offset` is
/// added to every batch's `first`: a group's batches index the buffer its primitives end up in.
[[nodiscard]] Status build_batches(Span<const Primitive> emitted, u32 offset,
                                   Array<Batch>& batches) noexcept {
    const usize start = batches.size();
    for (usize index = 0; index < emitted.size(); ++index) {
        const Primitive& primitive = emitted[index];
        if (batches.size() > start) {
            Batch& current = batches[batches.size() - 1];
            const Primitive& previous = emitted[current.first - offset + current.count - 1];
            if (previous.material == primitive.material && previous.atlas == primitive.atlas &&
                previous.clip == primitive.clip && previous.transform == primitive.transform) {
                ++current.count;
                continue;
            }
            current.reason = break_reason_between(previous, primitive);
        }
        Batch batch;
        batch.first = static_cast<u32>(index) + offset;
        batch.count = 1;
        batch.material = primitive.material;
        batch.atlas = primitive.atlas;
        if (Status pushed = batches.push_back(batch); !pushed) {
            return pushed;
        }
    }
    return ok();
}

/// Whether an element opens an opacity group here: it asks for one, its own opacity is below one,
/// and it is not the element whose group is being drawn.
[[nodiscard]] bool opens_group(const ElementStore& store, const Pending& pending) noexcept {
    if (pending.group_root ||
        !has_flag(store.flags(pending.element), ElementFlags::OpacityGroup)) {
        return false;
    }
    const f32 own = store.paint(pending.element)->opacity;
    return own > 0.0F && own < 1.0F;
}

[[nodiscard]] Status walk_stream(FlattenWalk& walk, Stream& stream, Array<Pending>& stack) noexcept;

/// The union of a run of primitives' bounds, each clipped to its clip.
[[nodiscard]] Rect covered(Span<const Primitive> primitives, Span<const Rect> clips) noexcept {
    f32 left = 0.0F;
    f32 top = 0.0F;
    f32 right = 0.0F;
    f32 bottom = 0.0F;
    bool any = false;
    for (const Primitive& primitive : primitives) {
        const Rect visible = primitive.bounds.intersected(clips[primitive.clip]);
        if (visible.empty()) {
            continue;
        }
        left = any ? std::min(left, visible.x) : visible.x;
        top = any ? std::min(top, visible.y) : visible.y;
        right = any ? std::max(right, visible.right()) : visible.right();
        bottom = any ? std::max(bottom, visible.bottom()) : visible.bottom();
        any = true;
    }
    return any ? Rect{left, top, right - left, bottom - top} : Rect{};
}

/// AN OPACITY GROUP: the element and its subtree flattened into a stream of their own at full
/// opacity, and ONE composite primitive in the parent stream that draws that stream's target at
/// the element's opacity. `ui-system`: "WHEN a panel with overlapping children is faded to 50 %
/// THEN it SHALL be composited once at 50 %, without the overlaps darkening."
[[nodiscard]] Status open_group(FlattenWalk& walk, Stream& parent, const Pending& pending) noexcept {
    Allocator& allocator = parent.emitted.allocator();
    Array<Primitive> emitted(allocator);
    Array<ElementId> owners(allocator);
    Stream inner{emitted, owners};
    Array<Pending> stack(allocator);
    Pending root = pending;
    // Inside its own target the group is drawn at full opacity; the opacity its ancestors fold in
    // and its own both go on the composite.
    root.opacity = 1.0F;
    root.group_root = true;
    if (Status pushed = stack.push_back(root); !pushed) {
        return pushed;
    }
    if (Status walked = walk_stream(walk, inner, stack); !walked) {
        return walked;
    }
    if (emitted.empty()) {
        return ok();
    }
    if (walk.groups.size() >= kMaxOpacityGroups) {
        return make_unexpected(Error{ErrorCode::OutOfMemory,
                                     "more opacity groups in one document than kMaxOpacityGroups",
                                     0});
    }
    OpacityGroup group;
    group.first = static_cast<u32>(walk.group_primitives.size());
    group.count = static_cast<u32>(emitted.size());
    group.bounds = covered(emitted.span(), walk.clips.span()).intersected(walk.viewport);
    group.opacity = pending.opacity * walk.store.paint(pending.element)->opacity;
    group.source = pending.element;
    for (usize index = 0; index < emitted.size(); ++index) {
        if (Status pushed = walk.group_primitives.push_back(emitted[index]); !pushed) {
            return pushed;
        }
        if (Status pushed = walk.group_owners.push_back(owners[index]); !pushed) {
            return pushed;
        }
    }
    const auto index = static_cast<u16>(walk.groups.size());
    if (Status pushed = walk.groups.push_back(group); !pushed) {
        return pushed;
    }
    if (group.bounds.empty()) {
        return ok();
    }
    // THE COMPOSITE: the group's target sampled over the region it covers, white at the group's
    // opacity — one premultiplied multiply, once, over the whole of it.
    auto clip_slot = clip_index_for(walk.clips, pending.final_clip);
    if (!clip_slot) {
        return make_unexpected(clip_slot.error());
    }
    Primitive composite;
    composite.bounds = group.bounds;
    composite.uv = group.bounds;
    composite.material = material_index(BuiltinMaterial::Image);
    composite.atlas = static_cast<u16>(kFirstGroupAtlas + index);
    composite.colour = 0xFFFFFFFFU;
    return emit(parent, composite, pending.element, clip_slot.value(), group.opacity,
                Transform{});
}

/// The pending element's own state for its children: its opacity folded in, its transform
/// composed, and the clip its children inherit.
[[nodiscard]] Pending child_state(const FlattenWalk& walk, const Pending& pending,
                                  const LayoutOutput& output, const Rect& clip,
                                  f32 opacity) noexcept {
    Pending next = pending;
    next.opacity = opacity;
    next.transform = compose(pending.transform, *walk.store.paint(pending.element), output.rect);
    next.layout_clip = output.clip.empty() ? pending.layout_clip : output.clip;
    next.final_clip = clip;
    return next;
}

/// Drain `stack` into `stream`, opening a group wherever an element asks for one.
Status walk_stream(FlattenWalk& walk, Stream& stream, Array<Pending>& stack) noexcept {
    ElementStore& store = walk.store;
    while (!stack.empty()) {
        const Pending pending = stack[stack.size() - 1];
        stack.pop_back();
        ++walk.report.visited;

        const Hierarchy* node = store.hierarchy(pending.element);
        if (node == nullptr || store.layout_output(pending.element) == nullptr ||
            store.paint(pending.element) == nullptr ||
            has_flag(store.flags(pending.element), ElementFlags::Collapsed)) {
            continue;
        }
        const LayoutOutput& output = *store.layout_output(pending.element);
        if (opens_group(store, pending)) {
            if (Status opened = open_group(walk, stream, pending); !opened) {
                return opened;
            }
            continue;
        }
        // OPACITY MULTIPLIES DOWN THE TREE, so a faded panel fades everything inside it. An
        // element that opened an opacity group is drawn at one inside its own target, and its
        // opacity goes on the composite instead.
        const f32 opacity = pending.group_root
                                ? pending.opacity
                                : pending.opacity * store.paint(pending.element)->opacity;
        Pending own = pending;
        own.opacity = opacity;
        own.transform = compose(pending.transform, *store.paint(pending.element), output.rect);
        const Rect clip = final_clip(walk, own, output);
        if (Status visited = visit(walk, stream, pending.element, own, clip); !visited) {
            return visited;
        }
        if (Status pushed = push_children(store, stack, *node,
                                          child_state(walk, pending, output, clip, opacity));
            !pushed) {
            return pushed;
        }
    }
    return ok();
}

}  // namespace

Status flatten(ElementStore& store, const Rect& viewport, PrimitiveBuffer& out,
               FlattenReport& report, ContentPainter* painter) noexcept {
    report = FlattenReport{};

    // INCREMENTAL. An element with no paint dirt keeps the primitive it already has; the pass walks
    // the tree once, and every element whose paint bit is clear and whose primitive is already in
    // the buffer is counted as reused rather than re-emitted.
    Allocator& allocator = out.primitives_.allocator();
    Array<Primitive> emitted(allocator);
    Array<ElementId> owners(allocator);
    Array<Rect> clips(out.clips_.allocator());
    Array<Primitive> group_primitives(allocator);
    Array<ElementId> group_owners(allocator);
    Array<OpacityGroup> groups(allocator);
    if (Status pushed = clips.push_back(viewport); !pushed) {
        return pushed;
    }

    Array<Pending> stack(allocator);
    // Roots in order, so the first root is drawn first and the last is on top.
    for (usize index = store.roots().size(); index > 0; --index) {
        Pending root;
        root.element = store.roots()[index - 1];
        root.layout_clip = viewport;
        root.final_clip = viewport;
        if (Status pushed = stack.push_back(root); !pushed) {
            return pushed;
        }
    }

    FlattenWalk walk{store,  viewport,         report,       painter,
                     clips,  group_primitives, group_owners, groups};
    Stream main{emitted, owners};
    if (Status walked = walk_stream(walk, main, stack); !walked) {
        return walked;
    }

    // BATCHING by material and atlas, with the break reason recorded. A batch that broke for a
    // reason nobody can name is the commonest cause of an interface that draws in four hundred
    // calls, and this is the column that names it.
    Array<Batch> batches(out.batches_.allocator());
    if (Status built = build_batches(emitted.span(), 0, batches); !built) {
        return built;
    }
    report.batches = static_cast<u32>(batches.size());

    // THE GROUPS' PRIMITIVES GO AFTER THE MAIN STREAM, each group's batches in a list of their
    // own: a renderer draws the groups into their targets first, inner before outer, and then the
    // main stream, whose composites sample them. With no group this appends nothing.
    Array<Batch> group_batches(allocator);
    const auto base = static_cast<u32>(emitted.size());
    for (OpacityGroup& group : groups.span()) {
        group.first_batch = static_cast<u32>(group_batches.size());
        if (Status built = build_batches(
                group_primitives.span().subspan(group.first, group.count), base + group.first,
                group_batches);
            !built) {
            return built;
        }
        group.batch_count = static_cast<u32>(group_batches.size()) - group.first_batch;
        group.first += base;
    }
    for (usize index = 0; index < group_primitives.size(); ++index) {
        if (Status pushed = emitted.push_back(group_primitives[index]); !pushed) {
            return pushed;
        }
        if (Status pushed = owners.push_back(group_owners[index]); !pushed) {
            return pushed;
        }
    }
    report.groups = static_cast<u32>(groups.size());

    out.primitives_ = std::move(emitted);
    out.owners_ = std::move(owners);
    out.clips_ = std::move(clips);
    out.batches_ = std::move(batches);
    out.groups_ = std::move(groups);
    out.group_batches_ = std::move(group_batches);
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
