// The sort key, the batcher, and the primitive expansions. M8.b task 9.5.

#include <cy/rendering/2d/sprite.h>

#include <algorithm>
#include <cmath>

namespace cy::rendering2d {
namespace {

/// The Y value, quantised into 20 bits and biased so that a negative Y sorts below a positive one.
/// Quantised because the key is an integer and because two sprites a thousandth of a unit apart
/// should not swap between frames — which is the flicker the stable tiebreak exists to prevent.
[[nodiscard]] u64 quantise_y(f32 y) noexcept {
    constexpr f32 kScale = 16.0F;
    constexpr i64 kBias = 1 << 19U;
    const auto scaled = static_cast<i64>(std::floor(y * kScale)) + kBias;
    const i64 clamped = std::clamp<i64>(scaled, 0, 0xFFFFF);
    return static_cast<u64>(clamped);
}

/// Why two consecutive draws could not share a batch, most expensive first: a render target change
/// costs more than a pipeline change, and a developer reading the report wants the one worth
/// fixing.
[[nodiscard]] BreakReason break_reason_between(const Draw2D& previous,
                                               const Draw2D& next) noexcept {
    if (previous.target != next.target) {
        return BreakReason::RenderTarget;
    }
    if (previous.pipeline != next.pipeline) {
        return BreakReason::Pipeline;
    }
    if (previous.material != next.material) {
        return BreakReason::Material;
    }
    if (previous.texture_set != next.texture_set) {
        return BreakReason::TextureSet;
    }
    return BreakReason::Scissor;
}

}  // namespace

const char* primitive_kind_name(PrimitiveKind kind) noexcept {
    switch (kind) {
        case PrimitiveKind::Sprite:
            return "sprite";
        case PrimitiveKind::NineSlice:
            return "nine-slice";
        case PrimitiveKind::TiledSprite:
            return "tiled-sprite";
        case PrimitiveKind::Line:
            return "line";
        case PrimitiveKind::Polygon:
            return "polygon";
        case PrimitiveKind::Mesh:
            return "mesh";
        case PrimitiveKind::AnimatedSprite:
            return "animated-sprite";
        case PrimitiveKind::Count:
            break;
    }
    return "unknown";
}

const char* break_reason_name(BreakReason reason) noexcept {
    switch (reason) {
        case BreakReason::Pipeline:
            return "pipeline";
        case BreakReason::Material:
            return "material";
        case BreakReason::TextureSet:
            return "texture-set";
        case BreakReason::Scissor:
            return "scissor";
        case BreakReason::RenderTarget:
            return "render-target";
        case BreakReason::End:
            break;
    }
    return "end";
}

u64 SortKey::pack(bool y_sorted) const noexcept {
    // layer (16) | order (16, biased so a negative order sorts first) | y (20) | tiebreak (12).
    // The order is the requirement's own list, and packing it means the sort is one comparison.
    const u64 layer_bits = static_cast<u64>(layer) << 48U;
    const u64 order_bits = static_cast<u64>(static_cast<u16>(static_cast<i32>(order) + 0x8000))
                           << 32U;
    const u64 y_bits = y_sorted ? (quantise_y(y_sort) << 12U) : 0ULL;
    const u64 tiebreak_bits = static_cast<u64>(tiebreak) & 0xFFFULL;
    return layer_bits | order_bits | y_bits | tiebreak_bits;
}

BreakReason BatchReport::dominant_break() const noexcept {
    BreakReason worst = BreakReason::End;
    u32 most = 0;
    for (u32 index = 0; index < static_cast<u32>(BreakReason::End); ++index) {
        if (breaks[index] > most) {
            most = breaks[index];
            worst = static_cast<BreakReason>(index);
        }
    }
    return worst;
}

Status build_batches(Span<const Draw2D> draws, Span<const Layer2D> layers, Array<u32>& order,
                     Array<Instance2D>& instances, Array<Batch2D>& batches,
                     BatchReport& report) noexcept {
    order.clear();
    instances.clear();
    batches.clear();
    report = BatchReport{};
    report.draws = static_cast<u32>(draws.size());
    if (draws.empty()) {
        return ok();
    }

    const auto y_sorted_layer = [layers](u16 layer) noexcept {
        for (const Layer2D& entry : layers) {
            if (entry.index == layer) {
                return entry.y_sorted;
            }
        }
        return false;
    };

    // ONE PACK PER DRAW, not one per comparison: the key and the draw's index travel together and
    // the sort is over those pairs. A comparator that packed the key each time would do it
    // n log n times and would spend more time in the layer lookup than in the sort.
    struct Entry {
        u64 key = 0;
        u32 index = 0;
    };
    Array<Entry> entries(instances.allocator());
    if (Status sized = entries.resize(draws.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < draws.size(); ++index) {
        entries[index].key = draws[index].sort.pack(y_sorted_layer(draws[index].sort.layer));
        entries[index].index = static_cast<u32>(index);
    }
    // `stable_sort` rather than `sort`: the key's own tiebreak already makes the order total, and a
    // stable sort means two draws that somehow share a key keep the order they were submitted in
    // rather than swapping between frames.
    Span<Entry> span = entries.span();
    std::ranges::stable_sort(span,
                             [](const Entry& a, const Entry& b) noexcept { return a.key < b.key; });

    if (Status sized = order.resize(entries.size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < entries.size(); ++index) {
        order[index] = entries[index].index;
    }

    for (const u32 index : order.span()) {
        const Draw2D& draw = draws[index];
        Instance2D instance;
        instance.destination = draw.destination;
        instance.source = draw.source;
        instance.tint = draw.tint;
        instance.rotation = draw.rotation;
        instance.pivot = draw.pivot;
        for (u32 channel = 0; channel < 4U; ++channel) {
            instance.custom[channel] = draw.custom[channel];
        }
        // A flip is expressed in the source rectangle rather than in a flag, so the shader has one
        // path and the instance layout has no branch in it.
        if (draw.flip_x) {
            instance.source.x += instance.source.width;
            instance.source.width = -instance.source.width;
        }
        if (draw.flip_y) {
            instance.source.y += instance.source.height;
            instance.source.height = -instance.source.height;
        }
        if (Status pushed = instances.push_back(instance); !pushed) {
            return pushed;
        }
    }
    report.instances = static_cast<u32>(instances.size());

    const auto scissor_equal = [](const Draw2D& a, const Draw2D& b) noexcept {
        if (a.scissored != b.scissored) {
            return false;
        }
        if (!a.scissored) {
            return true;
        }
        return a.scissor.x == b.scissor.x && a.scissor.y == b.scissor.y &&
               a.scissor.width == b.scissor.width && a.scissor.height == b.scissor.height;
    };

    for (usize position = 0; position < order.size(); ++position) {
        const Draw2D& draw = draws[order[position]];
        if (!batches.empty()) {
            Batch2D& current = batches[batches.size() - 1];
            const Draw2D& previous =
                draws[order[current.first_instance + current.instance_count - 1]];
            if (previous.pipeline == draw.pipeline && previous.material == draw.material &&
                previous.texture_set == draw.texture_set && previous.target == draw.target &&
                scissor_equal(previous, draw)) {
                ++current.instance_count;
                continue;
            }
            // THE BREAK REASON, most expensive first: a render target change costs more than a
            // pipeline change, and a developer reading the report wants the one worth fixing.
            current.reason = break_reason_between(previous, draw);
            ++report.breaks[static_cast<usize>(current.reason)];
        }
        Batch2D batch;
        batch.first_instance = static_cast<u32>(position);
        batch.instance_count = 1;
        batch.pipeline = draw.pipeline;
        batch.material = draw.material;
        batch.texture_set = draw.texture_set;
        batch.target = draw.target;
        if (Status pushed = batches.push_back(batch); !pushed) {
            return pushed;
        }
    }
    report.batches = static_cast<u32>(batches.size());
    return ok();
}

Status expand_nine_slice(const Draw2D& draw, const Rect2D& borders, Array<Draw2D>& out) noexcept {
    // The nine pieces: three columns by three rows, with the corners at their natural size and the
    // edges and centre stretched. Expanding here rather than in a shader keeps every primitive on
    // one instance layout, which is what lets a nine-slice batch with the sprites around it.
    const f32 destination_columns[4] = {draw.destination.x, draw.destination.x + borders.x,
                                        draw.destination.x + draw.destination.width - borders.width,
                                        draw.destination.x + draw.destination.width};
    const f32 destination_rows[4] = {draw.destination.y, draw.destination.y + borders.y,
                                     draw.destination.y + draw.destination.height - borders.height,
                                     draw.destination.y + draw.destination.height};
    const f32 source_columns[4] = {draw.source.x, draw.source.x + borders.x,
                                   draw.source.x + draw.source.width - borders.width,
                                   draw.source.x + draw.source.width};
    const f32 source_rows[4] = {draw.source.y, draw.source.y + borders.y,
                                draw.source.y + draw.source.height - borders.height,
                                draw.source.y + draw.source.height};

    for (u32 row = 0; row < 3U; ++row) {
        for (u32 column = 0; column < 3U; ++column) {
            Draw2D piece = draw;
            piece.kind = PrimitiveKind::Sprite;
            piece.destination =
                Rect2D{destination_columns[column], destination_rows[row],
                       destination_columns[column + 1] - destination_columns[column],
                       destination_rows[row + 1] - destination_rows[row]};
            piece.source = Rect2D{source_columns[column], source_rows[row],
                                  source_columns[column + 1] - source_columns[column],
                                  source_rows[row + 1] - source_rows[row]};
            if (piece.destination.width <= 0.0F || piece.destination.height <= 0.0F) {
                continue;  // A border wider than the destination collapses that piece.
            }
            if (Status pushed = out.push_back(piece); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

Status expand_line(Span<const Vec2> points, f32 width, LineJoint joint, LineCap cap,
                   f32 (*width_at)(f32, void*) noexcept, void* user, Array<Vec2>& out) noexcept {
    out.clear();
    if (points.size() < 2) {
        return make_unexpected(
            Error{ErrorCode::InvalidArgument, "a line needs at least two points", 0});
    }
    (void)joint;

    // The total length first, so the width curve is parameterised by DISTANCE rather than by point
    // index — a polyline with one long segment and ten short ones would otherwise taper wrongly.
    f32 total = 0.0F;
    for (usize index = 0; index + 1 < points.size(); ++index) {
        total += length(points[index + 1] - points[index]);
    }
    if (total <= 0.0F) {
        total = 1.0F;
    }

    f32 travelled = 0.0F;
    for (usize index = 0; index < points.size(); ++index) {
        const Vec2 previous = points[(index == 0) ? 0 : (index - 1)];
        const Vec2 next = points[(index + 1 < points.size()) ? (index + 1) : index];
        Vec2 direction = next - previous;
        if (length_squared(direction) <= 1e-8F) {
            direction = Vec2{1.0F, 0.0F};
        }
        direction = normalize(direction);
        const Vec2 normal{-direction.y, direction.x};
        if (index > 0) {
            travelled += length(points[index] - points[index - 1]);
        }
        const f32 half =
            0.5F * ((width_at != nullptr) ? width_at(travelled / total, user) * width : width);
        if (Status pushed = out.push_back(points[index] + (normal * half)); !pushed) {
            return pushed;
        }
        if (Status pushed = out.push_back(points[index] - (normal * half)); !pushed) {
            return pushed;
        }
    }

    if (cap == LineCap::Box) {
        // A box cap extends the strip by half a width at each end, along the line.
        const Vec2 start_direction = normalize(points[1] - points[0]);
        const Vec2 end_direction = normalize(points[points.size() - 1] - points[points.size() - 2]);
        out[0] = out[0] - (start_direction * (width * 0.5F));
        out[1] = out[1] - (start_direction * (width * 0.5F));
        out[out.size() - 2] = out[out.size() - 2] + (end_direction * (width * 0.5F));
        out[out.size() - 1] = out[out.size() - 1] + (end_direction * (width * 0.5F));
    }
    return ok();
}

u32 animation_frame(Span<const f32> durations, f32 elapsed, bool loop, bool& looped) noexcept {
    looped = false;
    if (durations.empty()) {
        return 0;
    }
    f32 total = 0.0F;
    for (const f32 duration : durations) {
        total += (duration > 0.0F) ? duration : 0.0F;
    }
    if (total <= 0.0F) {
        return 0;
    }
    f32 time = (elapsed < 0.0F) ? 0.0F : elapsed;
    if (time >= total) {
        if (!loop) {
            return static_cast<u32>(durations.size() - 1);
        }
        looped = true;
        time = std::fmod(time, total);
    }
    f32 accumulated = 0.0F;
    for (usize index = 0; index < durations.size(); ++index) {
        accumulated += (durations[index] > 0.0F) ? durations[index] : 0.0F;
        if (time < accumulated) {
            return static_cast<u32>(index);
        }
    }
    return static_cast<u32>(durations.size() - 1);
}

}  // namespace cy::rendering2d
