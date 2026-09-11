// The compiled program's two indexes and its one evaluation primitive.

#include <cy/sequencing/program.h>

#include <cmath>

namespace cy::sequencing {
namespace {

[[nodiscard]] f32 lerp(f32 a, f32 b, f32 t) noexcept {
    return a + ((b - a) * t);
}

/// Catmull-Rom through four values. `Smooth`'s shape: the tangent at each key is the slope between
/// its neighbours, which is what a curve editor draws when nobody has touched a handle.
[[nodiscard]] f32 catmull_rom(f32 p0, f32 p1, f32 p2, f32 p3, f32 t) noexcept {
    const f32 t2 = t * t;
    const f32 t3 = t2 * t;
    const f32 a = 2.0F * p1;
    const f32 b = (p2 - p0) * t;
    const f32 c = ((2.0F * p0) - (5.0F * p1) + (4.0F * p2) - p3) * t2;
    const f32 d = (-p0 + (3.0F * p1) - (3.0F * p2) + p3) * t3;
    return 0.5F * (a + b + c + d);
}

/// Normalised linear interpolation of two quaternions, on the short arc.
///
/// A ROTATION INTERPOLATES AS AN ORIENTATION — `sequencing-and-cinematics`'s own scenario — so the
/// hemisphere check is not an optimisation: without it, two keys a little either side of a half
/// turn interpolate the long way round and the camera spins. `nlerp` rather than `slerp` because
/// the engine's animation runtime made the same choice for the same reason (a trigonometric pair
/// per joint per frame against an error under a degree at the rates a timeline keys at), and two
/// modules interpolating rotations differently would be worse than either choice.
void nlerp(const f32 a[4], const f32 b[4], f32 t, f32 out[4]) noexcept {
    f32 dot = (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]) + (a[3] * b[3]);
    const f32 sign = (dot < 0.0F) ? -1.0F : 1.0F;
    f32 length = 0.0F;
    for (u32 index = 0; index < 4; ++index) {
        out[index] = lerp(a[index], sign * b[index], t);
        length += out[index] * out[index];
    }
    length = std::sqrt(length);
    if (length > 1e-8F) {
        for (u32 index = 0; index < 4; ++index) {
            out[index] /= length;
        }
    } else {
        out[0] = 0.0F;
        out[1] = 0.0F;
        out[2] = 0.0F;
        out[3] = 1.0F;
    }
}

}  // namespace

Program::Program(Allocator& allocator) noexcept
    : segments_(allocator),
      channels_(allocator),
      keys_(allocator),
      events_(allocator),
      markers_(allocator),
      bindings_(allocator),
      parameters_(allocator),
      preload_(allocator),
      debug_(allocator),
      outcomes_(allocator),
      bucket_offsets_(allocator),
      bucket_items_(allocator) {}

Status Program::active_at(SequenceTime time, Array<u32>& out) const noexcept {
    out.clear();
    if (bucket_count_ == 0 || segments_.empty()) {
        return ok();
    }
    const i64 ticks = time.ticks();
    // Outside the program's own range there is nothing active, and saying so here is what keeps a
    // pre-roll or a post-roll from indexing a bucket that does not exist.
    if (ticks < begin_ || ticks > end_) {
        return ok();
    }
    const i64 offset = ticks - begin_;
    auto bucket = static_cast<u32>(offset / bucket_span_);
    if (bucket >= bucket_count_) {
        bucket = bucket_count_ - 1;
    }

    // ONE BUCKET, and only the segments in it. The requirement's "without scanning tracks": the
    // loop below runs once per segment that overlaps this bucket and never once per segment that
    // does not.
    const u32 first = bucket_offsets_[bucket];
    const u32 last = bucket_offsets_[bucket + 1];
    for (u32 index = first; index < last; ++index) {
        const u32 segment = bucket_items_[index];
        const Segment& record = segments_[segment];
        if (ticks >= record.active_start && ticks <= record.active_end) {
            if (Status pushed = out.push_back(segment); !pushed) {
                return pushed;
            }
        }
    }
    return ok();
}

Span<const CompiledEvent> Program::events_between(SequenceTime from,
                                                  SequenceTime to) const noexcept {
    if (events_.empty() || to < from) {
        return {};
    }
    const i64 low = from.ticks();
    const i64 high = to.ticks();

    // Half-open at the bottom: an event exactly at `from` was crossed by the PREVIOUS interval, and
    // firing it again is how a paused sequence emits the same event on every frame.
    usize begin = 0;
    usize end = events_.size();
    while (begin < end) {
        const usize middle = begin + ((end - begin) / 2);
        if (events_[middle].ticks <= low) {
            begin = middle + 1;
        } else {
            end = middle;
        }
    }
    usize stop = begin;
    usize upper = events_.size();
    while (stop < upper) {
        const usize middle = stop + ((upper - stop) / 2);
        if (events_[middle].ticks <= high) {
            stop = middle + 1;
        } else {
            upper = middle;
        }
    }
    if (stop <= begin) {
        return {};
    }
    return {events_.data() + begin, stop - begin};
}

const CompiledMarker* Program::find_marker(Name marker) const noexcept {
    for (const CompiledMarker& record : markers_.span()) {
        if (record.name == marker) {
            return &record;
        }
    }
    return nullptr;
}

u32 Program::parameter_index(Name parameter) const noexcept {
    for (usize index = 0; index < parameters_.size(); ++index) {
        if (parameters_[index].name == parameter) {
            return static_cast<u32>(index);
        }
    }
    return kInvalidIndex;
}

u32 Program::binding_index(u32 stable_id) const noexcept {
    for (usize index = 0; index < bindings_.size(); ++index) {
        if (bindings_[index].stable_id == stable_id) {
            return static_cast<u32>(index);
        }
    }
    return kInvalidIndex;
}

bool sample_keys(Span<const CompiledKey> keys, ChannelType type, bool constant, SequenceTime local,
                 ChannelValue& out) noexcept {
    const usize count = keys.size();
    if (count == 0) {
        return false;
    }
    out.type = type;
    const CompiledKey* first = keys.data();
    if (constant || count == 1) {
        // A FOLDED CHANNEL COSTS NO SEARCH AT ALL. That is what constant-section folding buys, and
        // it is why the cook option exists rather than being a size optimisation.
        out.integer = first->integer;
        for (u32 index = 0; index < 4; ++index) {
            out.components[index] = first->value[index];
        }
        return true;
    }

    const i64 ticks = local.ticks();
    usize low = 0;
    usize high = count;
    while (low < high) {
        const usize middle = low + ((high - low) / 2);
        if (first[middle].ticks <= ticks) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    const usize index = (low == 0) ? 0 : (low - 1);
    const CompiledKey& left = first[index];
    out.integer = left.integer;
    for (u32 component = 0; component < 4; ++component) {
        out.components[component] = left.value[component];
    }

    const bool discrete = type == ChannelType::Boolean || type == ChannelType::Enumeration;
    if (discrete || left.interpolation == Interpolation::Constant || index + 1 >= count) {
        return true;
    }
    const CompiledKey& right = first[index + 1];
    const i64 span = right.ticks - left.ticks;
    if (span <= 0 || ticks <= left.ticks) {
        return true;
    }
    const f32 t = static_cast<f32>(static_cast<f64>(ticks - left.ticks) / static_cast<f64>(span));

    if (type == ChannelType::Rotation) {
        nlerp(left.value, right.value, t, out.components);
        return true;
    }
    if (left.interpolation == Interpolation::Smooth) {
        const CompiledKey& before = first[(index == 0) ? 0 : (index - 1)];
        const CompiledKey& after = first[(index + 2 >= count) ? (count - 1) : (index + 2)];
        for (u32 component = 0; component < 4; ++component) {
            out.components[component] =
                catmull_rom(before.value[component], left.value[component], right.value[component],
                            after.value[component], t);
        }
        return true;
    }
    for (u32 component = 0; component < 4; ++component) {
        out.components[component] = lerp(left.value[component], right.value[component], t);
    }
    return true;
}

ChannelValue blend_values(const ChannelValue& a, const ChannelValue& b, f32 t) noexcept {
    ChannelValue out = a;
    out.type = a.type;
    if (a.type == ChannelType::Boolean || a.type == ChannelType::Enumeration) {
        out.integer = (t >= 0.5F) ? b.integer : a.integer;
        return out;
    }
    if (a.type == ChannelType::Rotation) {
        nlerp(a.components, b.components, t, out.components);
        return out;
    }
    for (u32 index = 0; index < 4; ++index) {
        out.components[index] = lerp(a.components[index], b.components[index], t);
    }
    return out;
}

ChannelValue add_values(const ChannelValue& base, const ChannelValue& delta, f32 weight) noexcept {
    ChannelValue out = base;
    if (base.type == ChannelType::Boolean || base.type == ChannelType::Enumeration) {
        return out;
    }
    if (base.type == ChannelType::Rotation) {
        // A rotation composes: base * (identity -> delta by weight). Composing rather than adding
        // is the difference between an additive shake and a quaternion that stops being a rotation.
        f32 scaled[4] = {0.0F, 0.0F, 0.0F, 1.0F};
        const f32 identity[4] = {0.0F, 0.0F, 0.0F, 1.0F};
        nlerp(identity, delta.components, weight, scaled);
        const f32* q = base.components;
        out.components[0] =
            (q[3] * scaled[0]) + (q[0] * scaled[3]) + (q[1] * scaled[2]) - (q[2] * scaled[1]);
        out.components[1] =
            (q[3] * scaled[1]) - (q[0] * scaled[2]) + (q[1] * scaled[3]) + (q[2] * scaled[0]);
        out.components[2] =
            (q[3] * scaled[2]) + (q[0] * scaled[1]) - (q[1] * scaled[0]) + (q[2] * scaled[3]);
        out.components[3] =
            (q[3] * scaled[3]) - (q[0] * scaled[0]) - (q[1] * scaled[1]) - (q[2] * scaled[2]);
        return out;
    }
    for (u32 index = 0; index < 4; ++index) {
        out.components[index] = base.components[index] + (delta.components[index] * weight);
    }
    return out;
}

bool Program::sample(u32 channel, SequenceTime local, ChannelValue& out) const noexcept {
    if (channel >= channels_.size()) {
        return false;
    }
    const CompiledChannel& record = channels_[channel];
    if (record.key_count == 0) {
        return false;
    }
    return sample_keys(Span<const CompiledKey>(keys_.data() + record.key_begin, record.key_count),
                       record.type, record.constant, local, out);
}

}  // namespace cy::sequencing
