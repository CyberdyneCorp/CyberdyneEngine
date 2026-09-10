#include <cy/animation/clip.h>

#include <cy/core/math/scalar.h>

#include <cmath>
#include <utility>

namespace cy::animation {
namespace {

constexpr f32 kQuantiseMax = 65535.0F;
constexpr u32 kNoTrack = 0xFFFFFFFFU;

[[nodiscard]] u16 quantise(f32 value, f32 low, f32 high) noexcept {
    const f32 span = high - low;
    if (span <= 0.0F) {
        return 0;
    }
    const f32 normalised = (value - low) / span;
    const f32 clamped = math::clamp(normalised, 0.0F, 1.0F);
    return static_cast<u16>(std::lround(clamped * kQuantiseMax));
}

[[nodiscard]] f32 dequantise(u16 value, f32 low, f32 high) noexcept {
    return low + ((static_cast<f32>(value) / kQuantiseMax) * (high - low));
}

[[nodiscard]] bool is_rotation(TrackKind kind) noexcept {
    return kind == TrackKind::Rotation;
}

/// `normalize(Quat)` asserts on a zero-length quaternion, and a dequantised key can be one when a
/// track was authored with a degenerate rotation. The identity is the honest answer for it.
[[nodiscard]] Quat safe_normalize(const Quat& q) noexcept {
    const f32 len_sq = length_squared(q);
    if (len_sq <= math::kSmallLength) {
        return Quat::identity();
    }
    return normalize(q);
}

[[nodiscard]] PackedKey pack(const TrackDesc& track, const RawKey& key) noexcept {
    PackedKey out;
    out.time = key.time;
    if (is_rotation(track.kind)) {
        out.c[0] = quantise(key.value.x, -1.0F, 1.0F);
        out.c[1] = quantise(key.value.y, -1.0F, 1.0F);
        out.c[2] = quantise(key.value.z, -1.0F, 1.0F);
        out.c[3] = quantise(key.value.w, -1.0F, 1.0F);
        return out;
    }
    out.c[0] = quantise(key.value.x, track.range_min.x, track.range_max.x);
    out.c[1] = quantise(key.value.y, track.range_min.y, track.range_max.y);
    out.c[2] = quantise(key.value.z, track.range_min.z, track.range_max.z);
    return out;
}

[[nodiscard]] Vec4 unpack(const TrackDesc& track, const PackedKey& key) noexcept {
    if (is_rotation(track.kind)) {
        Quat rotation{dequantise(key.c[0], -1.0F, 1.0F), dequantise(key.c[1], -1.0F, 1.0F),
                      dequantise(key.c[2], -1.0F, 1.0F), dequantise(key.c[3], -1.0F, 1.0F)};
        rotation = safe_normalize(rotation);
        return Vec4{rotation.x, rotation.y, rotation.z, rotation.w};
    }
    return Vec4{dequantise(key.c[0], track.range_min.x, track.range_max.x),
                dequantise(key.c[1], track.range_min.y, track.range_max.y),
                dequantise(key.c[2], track.range_min.z, track.range_max.z), 0.0F};
}

[[nodiscard]] Vec4 blend_values(TrackKind kind, Interpolation interpolation, Vec4 a, Vec4 b,
                                f32 t) noexcept {
    if (interpolation == Interpolation::Step) {
        return a;
    }
    if (is_rotation(kind)) {
        const Quat from{a.x, a.y, a.z, a.w};
        const Quat to{b.x, b.y, b.z, b.w};
        const Quat result = slerp(from, to, t);
        return Vec4{result.x, result.y, result.z, result.w};
    }
    return Vec4{a.x + ((b.x - a.x) * t), a.y + ((b.y - a.y) * t), a.z + ((b.z - a.z) * t),
                a.w + ((b.w - a.w) * t)};
}

/// The angle between two unit quaternions, in degrees.
[[nodiscard]] f32 angle_degrees(Vec4 a, Vec4 b) noexcept {
    const f32 d = std::fabs((a.x * b.x) + (a.y * b.y) + (a.z * b.z) + (a.w * b.w));
    const f32 clamped = math::clamp(d, -1.0F, 1.0F);
    return 2.0F * std::acos(clamped) * (180.0F / math::kPi);
}

[[nodiscard]] f32 largest_component_error(Vec4 a, Vec4 b) noexcept {
    const f32 dx = std::fabs(a.x - b.x);
    const f32 dy = std::fabs(a.y - b.y);
    const f32 dz = std::fabs(a.z - b.z);
    return math::max(dx, math::max(dy, dz));
}

/// The tolerance a track's kind is measured against, in the track's own units.
[[nodiscard]] f32 tolerance_for(TrackKind kind, const CompressionSettings& settings) noexcept {
    switch (kind) {
        case TrackKind::Translation:
            return settings.translation_tolerance_mm * 0.001F;  // millimetres to world units
        case TrackKind::Rotation:
            return settings.rotation_tolerance_degrees;
        case TrackKind::Scale:
            return settings.scale_tolerance;
        case TrackKind::Curve:
        case TrackKind::Property:
            return settings.curve_tolerance;
    }
    return settings.curve_tolerance;
}

[[nodiscard]] f32 deviation(TrackKind kind, Vec4 a, Vec4 b) noexcept {
    return is_rotation(kind) ? angle_degrees(a, b) : largest_component_error(a, b);
}

[[nodiscard]] Vec4 interpolate_raw(TrackKind kind, Interpolation interpolation, const RawKey& a,
                                   const RawKey& b, f32 time) noexcept {
    const f32 span = b.time - a.time;
    const f32 t = span > 0.0F ? math::clamp((time - a.time) / span, 0.0F, 1.0F) : 0.0F;
    return blend_values(kind, interpolation, a.value, b.value, t);
}

}  // namespace

// --- ClipCursor ---------------------------------------------------------------------------------

ClipCursor::ClipCursor(Allocator& allocator) noexcept : keys_(allocator) {}

Status ClipCursor::reset(u32 track_count) noexcept {
    if (Status sized = keys_.resize(track_count); !sized) {
        return sized;
    }
    for (u32& key : keys_) {
        key = 0;
    }
    reset_counters();
    return ok();
}

u32 ClipCursor::at(u32 track) const noexcept {
    return track < keys_.size() ? keys_[track] : 0;
}

void ClipCursor::set(u32 track, u32 key) noexcept {
    if (track < keys_.size()) {
        keys_[track] = key;
    }
}

// --- EventBuffer --------------------------------------------------------------------------------

EventBuffer::EventBuffer(Allocator& allocator) noexcept : events_(allocator) {}

Status EventBuffer::push(const EmittedEvent& event) noexcept {
    return events_.push_back(event);
}

void EventBuffer::clear() noexcept {
    events_.clear();
    suppressed_ = 0;
}

// --- Clip: authoring ----------------------------------------------------------------------------

Clip::Clip(Allocator& allocator) noexcept
    : tracks_(allocator),
      raw_(allocator),
      raw_first_(allocator),
      raw_count_(allocator),
      packed_(allocator),
      markers_(allocator),
      events_(allocator) {}

Expected<u32, Error> Clip::add_track(const TrackDesc& desc) noexcept {
    const auto index = static_cast<u32>(tracks_.size());
    if (Status pushed = tracks_.push_back(desc); !pushed) {
        return make_unexpected(pushed.error());
    }
    if (Status pushed = raw_first_.push_back(static_cast<u32>(raw_.size())); !pushed) {
        return make_unexpected(pushed.error());
    }
    if (Status pushed = raw_count_.push_back(0); !pushed) {
        return make_unexpected(pushed.error());
    }
    tracks_[index].first_key = 0;
    tracks_[index].key_count = 0;
    compressed_ = false;
    return index;
}

Expected<u32, Error> Clip::add_joint_track(TrackKind kind, u16 joint,
                                           Interpolation interpolation) noexcept {
    if (kind != TrackKind::Translation && kind != TrackKind::Rotation && kind != TrackKind::Scale) {
        return make_unexpected(Error{ErrorCode::InvalidArgument,
                                     "a joint track is a translation, rotation or scale", 0});
    }
    TrackDesc desc;
    desc.kind = kind;
    desc.joint = joint;
    desc.interpolation = interpolation;
    return add_track(desc);
}

Expected<u32, Error> Clip::add_curve_track(Name name, Interpolation interpolation) noexcept {
    TrackDesc desc;
    desc.kind = TrackKind::Curve;
    desc.name = name;
    desc.interpolation = interpolation;
    return add_track(desc);
}

Expected<u32, Error> Clip::add_property_track(Name name, const PropertyBinding& binding,
                                              Interpolation interpolation) noexcept {
    TrackDesc desc;
    desc.kind = TrackKind::Property;
    desc.name = name;
    desc.binding = binding;
    desc.interpolation = interpolation;
    return add_track(desc);
}

Status Clip::add_key(u32 track, f32 time, Vec4 value) noexcept {
    if (track >= tracks_.size()) {
        return fail(ErrorCode::OutOfRange, "no such track");
    }
    if (track + 1 != tracks_.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "a track's keys are appended before the next track is added; authoring out of "
                    "order would interleave two tracks' keys in one array");
    }
    const TrackDesc& desc = tracks_[track];
    if (raw_count_[track] != 0 && raw_[raw_.size() - 1].time > time) {
        return fail(ErrorCode::InvalidArgument, "keys are appended in increasing time order");
    }
    RawKey key;
    key.time = time;
    key.value = value;
    if (is_rotation(desc.kind) && raw_count_[track] != 0) {
        // "neighbouring keys hemisphere-aligned at import so no long-way rotation occurs".
        const RawKey& previous = raw_[raw_.size() - 1];
        const f32 d = (previous.value.x * key.value.x) + (previous.value.y * key.value.y) +
                      (previous.value.z * key.value.z) + (previous.value.w * key.value.w);
        if (d < 0.0F) {
            key.value = Vec4{-key.value.x, -key.value.y, -key.value.z, -key.value.w};
        }
    }
    if (Status pushed = raw_.push_back(key); !pushed) {
        return pushed;
    }
    ++raw_count_[track];
    compressed_ = false;
    return ok();
}

Status Clip::add_marker(Name name, f32 time) noexcept {
    return markers_.push_back(ClipMarker{name, time});
}

Status Clip::add_event(Name name, f32 time, f32 parameter) noexcept {
    return events_.push_back(ClipEvent{name, time, parameter});
}

// --- Clip: compression --------------------------------------------------------------------------

namespace {

/// Whether the straight segment from `anchor` to `next` reproduces every key between them within
/// the tolerance. The predicate the fitter drops a key on.
[[nodiscard]] bool segment_reproduces(const TrackDesc& track, Span<const RawKey> keys, usize anchor,
                                      usize next, f32 tolerance) noexcept {
    for (usize probe = anchor + 1; probe < next; ++probe) {
        const Vec4 approximated = interpolate_raw(track.kind, track.interpolation, keys[anchor],
                                                  keys[next], keys[probe].time);
        if (deviation(track.kind, approximated, keys[probe].value) > tolerance) {
            return false;
        }
    }
    return true;
}

/// The keys the fitter kept, as a forward greedy pass: a key is dropped when the straight line
/// between its neighbours reproduces it within the tolerance.
void fit_track(const TrackDesc& track, Span<const RawKey> keys, f32 tolerance,
               Array<u32>& kept) noexcept {
    kept.clear();
    if (keys.empty()) {
        return;
    }
    if (Status pushed = kept.push_back(0); !pushed) {
        return;
    }
    // CONSTANT-TRACK COLLAPSING, first: a track that never leaves the tolerance of its own first
    // key is one key, not two. Most of a skeleton's tracks are this — a joint that only rotates
    // still carries a translation and a scale track — so it is the compression that pays for
    // itself.
    bool constant = true;
    for (const RawKey& key : keys) {
        if (deviation(track.kind, keys[0].value, key.value) > tolerance) {
            constant = false;
            break;
        }
    }
    if (constant) {
        return;
    }
    usize anchor = 0;
    for (usize index = 1; index + 1 < keys.size(); ++index) {
        if (!segment_reproduces(track, keys, anchor, index + 1, tolerance)) {
            if (Status pushed = kept.push_back(static_cast<u32>(index)); !pushed) {
                return;
            }
            anchor = index;
        }
    }
    if (keys.size() > 1) {
        if (Status pushed = kept.push_back(static_cast<u32>(keys.size() - 1)); !pushed) {
            return;
        }
    }
}

void widen(Vec3& low, Vec3& high, Vec4 value, bool first) noexcept {
    if (first) {
        low = Vec3{value.x, value.y, value.z};
        high = low;
        return;
    }
    low.x = math::min(low.x, value.x);
    low.y = math::min(low.y, value.y);
    low.z = math::min(low.z, value.z);
    high.x = math::max(high.x, value.x);
    high.y = math::max(high.y, value.y);
    high.z = math::max(high.z, value.z);
}

}  // namespace

Status Clip::compress(const CompressionSettings& settings) noexcept {
    report_ = CompressionReport{};
    report_.tracks = static_cast<u32>(tracks_.size());

    Array<PackedKey> packed(packed_.allocator());
    Array<u32> kept(packed_.allocator());

    for (usize index = 0; index < tracks_.size(); ++index) {
        TrackDesc& track = tracks_[index];
        const Span<const RawKey> keys(raw_.data() + raw_first_[index], raw_count_[index]);
        report_.keys_before += raw_count_[index];

        bool first = true;
        for (const RawKey& key : keys) {
            widen(track.range_min, track.range_max, key.value, first);
            first = false;
        }

        const f32 tolerance = tolerance_for(track.kind, settings);
        fit_track(track, keys, tolerance, kept);
        // "constant-track collapsing": one kept key means the track never moves.
        track.constant = kept.size() <= 1;

        track.first_key = static_cast<u32>(packed.size());
        for (const u32 key_index : kept) {
            if (Status pushed = packed.push_back(pack(track, keys[key_index])); !pushed) {
                return pushed;
            }
        }
        track.key_count = static_cast<u32>(kept.size());
        if (track.constant) {
            ++report_.constant_tracks;
        }
        report_.keys_after += track.key_count;
    }

    packed_ = std::move(packed);
    compressed_ = true;

    // The error is MEASURED against the authored keys, not estimated from the tolerance: a fitter
    // that kept the wrong key would otherwise report the tolerance it was asked for.
    ClipCursor cursor(packed_.allocator());
    if (Status sized = cursor.reset(static_cast<u32>(tracks_.size())); !sized) {
        return sized;
    }
    for (usize index = 0; index < tracks_.size(); ++index) {
        const TrackDesc& track = tracks_[index];
        for (u32 key = 0; key < raw_count_[index]; ++key) {
            const RawKey& authored = raw_[raw_first_[index] + key];
            const Vec4 stored = sample_track(static_cast<u32>(index), authored.time, cursor);
            const f32 error = deviation(track.kind, stored, authored.value);
            switch (track.kind) {
                case TrackKind::Translation:
                    report_.worst_translation_mm =
                        math::max(report_.worst_translation_mm, error * 1000.0F);
                    break;
                case TrackKind::Rotation:
                    report_.worst_rotation_degrees =
                        math::max(report_.worst_rotation_degrees, error);
                    break;
                case TrackKind::Scale:
                    report_.worst_scale = math::max(report_.worst_scale, error);
                    break;
                case TrackKind::Curve:
                case TrackKind::Property:
                    report_.worst_curve = math::max(report_.worst_curve, error);
                    break;
            }
        }
    }

    const f32 before = static_cast<f32>(report_.keys_before) * static_cast<f32>(sizeof(RawKey));
    const f32 after = static_cast<f32>(report_.keys_after) * static_cast<f32>(sizeof(PackedKey));
    report_.ratio = after > 0.0F ? before / after : 1.0F;
    return ok();
}

// --- Clip: sampling -----------------------------------------------------------------------------

namespace {

/// The key index at or before `time`, advancing the cursor forward or searching when it is behind.
[[nodiscard]] u32 locate(Span<const PackedKey> keys, f32 time, u32 hint,
                         ClipCursor& cursor) noexcept {
    if (keys.size() <= 1) {
        return 0;
    }
    u32 index = hint < keys.size() ? hint : 0;
    if (keys[index].time > time) {
        cursor.note_search();
        usize low = 0;
        usize high = keys.size() - 1;
        while (low < high) {
            const usize middle = (low + high + 1) / 2;
            if (keys[middle].time <= time) {
                low = middle;
            } else {
                high = middle - 1;
            }
        }
        return static_cast<u32>(low);
    }
    while (index + 1 < keys.size() && keys[index + 1].time <= time) {
        ++index;
        cursor.note_step();
    }
    return index;
}

}  // namespace

Vec4 Clip::sample_track(u32 track, f32 time, ClipCursor& cursor) const noexcept {
    if (track >= tracks_.size()) {
        return Vec4{0.0F, 0.0F, 0.0F, 0.0F};
    }
    const TrackDesc& desc = tracks_[track];
    if (desc.key_count == 0) {
        return Vec4{0.0F, 0.0F, 0.0F, 0.0F};
    }
    const Span<const PackedKey> keys(packed_.data() + desc.first_key, desc.key_count);
    if (keys.size() == 1) {
        return unpack(desc, keys[0]);
    }
    const u32 index = locate(keys, time, cursor.at(track), cursor);
    cursor.set(track, index);
    if (index + 1 >= keys.size()) {
        return unpack(desc, keys[keys.size() - 1]);
    }
    const PackedKey& a = keys[index];
    const PackedKey& b = keys[index + 1];
    const f32 span = b.time - a.time;
    const f32 t = span > 0.0F ? math::clamp((time - a.time) / span, 0.0F, 1.0F) : 0.0F;
    return blend_values(desc.kind, desc.interpolation, unpack(desc, a), unpack(desc, b), t);
}

Status Clip::sample(f32 time, const JointMask& mask, ClipCursor& cursor, Span<Transform> out,
                    SampleStats& stats) const noexcept {
    if (!compressed_) {
        return fail(ErrorCode::InvalidArgument,
                    "a clip is sampled from its compressed form; call compress() at cook time");
    }
    if (cursor.empty()) {
        if (Status sized = cursor.reset(static_cast<u32>(tracks_.size())); !sized) {
            return sized;
        }
    }
    const f32 wrapped = wrap(time);
    u32 touched = 0;
    for (usize index = 0; index < tracks_.size(); ++index) {
        const TrackDesc& track = tracks_[index];
        if (track.joint == kInvalidJoint) {
            continue;
        }
        if (track.joint >= out.size() || !mask.test(track.joint)) {
            // POSE DEPENDENCY ANALYSIS, at the only place it can be observed: the track is not
            // read at all. "lower-body joints of that layer's clips are never read, and they SHALL
            // NOT be sampled."
            ++stats.tracks_skipped;
            continue;
        }
        const Vec4 value = sample_track(static_cast<u32>(index), wrapped, cursor);
        ++stats.tracks_read;
        Transform& target = out[track.joint];
        switch (track.kind) {
            case TrackKind::Translation:
                target.translation = Vec3{value.x, value.y, value.z};
                break;
            case TrackKind::Rotation:
                target.rotation = safe_normalize(Quat{value.x, value.y, value.z, value.w});
                break;
            case TrackKind::Scale:
                target.scale = Vec3{value.x, value.y, value.z};
                break;
            case TrackKind::Curve:
            case TrackKind::Property:
                break;
        }
        ++touched;
    }
    stats.joints_written += touched;
    return ok();
}

bool Clip::sample_curve(Name curve, f32 time, ClipCursor& cursor, f32& out) const noexcept {
    const u32 track = find_named_track(TrackKind::Curve, curve);
    if (track == kNoTrack) {
        return false;
    }
    out = sample_track(track, wrap(time), cursor).x;
    return true;
}

u32 Clip::write_properties(f32 time, ClipCursor& cursor, Span<f32* const> slots) const noexcept {
    const f32 wrapped = wrap(time);
    u32 written = 0;
    for (usize index = 0; index < tracks_.size(); ++index) {
        if (tracks_[index].kind != TrackKind::Property || index >= slots.size()) {
            continue;
        }
        f32* slot = slots[index];
        if (slot == nullptr) {
            continue;
        }
        *slot = sample_track(static_cast<u32>(index), wrapped, cursor).x;
        ++written;
    }
    return written;
}

u32 Clip::find_joint_track(TrackKind kind, u16 joint) const noexcept {
    for (usize index = 0; index < tracks_.size(); ++index) {
        if (tracks_[index].kind == kind && tracks_[index].joint == joint) {
            return static_cast<u32>(index);
        }
    }
    return kNoTrack;
}

u32 Clip::find_named_track(TrackKind kind, Name name) const noexcept {
    for (usize index = 0; index < tracks_.size(); ++index) {
        if (tracks_[index].kind == kind && tracks_[index].name == name) {
            return static_cast<u32>(index);
        }
    }
    return kNoTrack;
}

f32 Clip::wrap(f32 time) const noexcept {
    if (duration_ <= 0.0F) {
        return 0.0F;
    }
    switch (loop_) {
        case LoopMode::None:
            return math::clamp(time, 0.0F, duration_);
        case LoopMode::Loop: {
            const f32 cycles = std::floor(time / duration_);
            return time - (cycles * duration_);
        }
        case LoopMode::PingPong: {
            const f32 period = duration_ * 2.0F;
            const f32 cycles = std::floor(time / period);
            const f32 within = time - (cycles * period);
            return within <= duration_ ? within : period - within;
        }
    }
    return time;
}

f32 Clip::time_at_phase(f32 phase) const noexcept {
    return wrap(phase * duration_);
}

bool Clip::marker_time(Name marker, f32& out) const noexcept {
    for (const ClipMarker& entry : markers_) {
        if (entry.name == marker) {
            out = entry.time;
            return true;
        }
    }
    return false;
}

RootDelta Clip::root_delta(f32 from, f32 to) const noexcept {
    RootDelta delta;
    if (!has_root_motion() || !compressed_) {
        return delta;
    }
    ClipCursor cursor(packed_.allocator());
    if (Status sized = cursor.reset(static_cast<u32>(tracks_.size())); !sized) {
        return delta;
    }
    const u32 translation = find_joint_track(TrackKind::Translation, root_joint_);
    const u32 rotation = find_joint_track(TrackKind::Rotation, root_joint_);

    // A wrapped interval is two intervals: the tail of the clip and the head of the next cycle.
    // Composing them here is what makes "root motion SHALL be correctly composed" hold across a
    // loop boundary rather than losing a cycle's worth of travel.
    const f32 start = wrap(from);
    const f32 end = wrap(to);
    const bool wrapped = end < start;

    auto accumulate = [&](f32 a, f32 b) noexcept {
        if (translation != kNoTrack) {
            const Vec4 first = sample_track(translation, a, cursor);
            const Vec4 second = sample_track(translation, b, cursor);
            delta.translation = delta.translation +
                                Vec3{second.x - first.x, second.y - first.y, second.z - first.z};
        }
        if (rotation != kNoTrack) {
            const Vec4 first = sample_track(rotation, a, cursor);
            const Vec4 second = sample_track(rotation, b, cursor);
            const Quat q0 = safe_normalize(Quat{first.x, first.y, first.z, first.w});
            const Quat q1 = safe_normalize(Quat{second.x, second.y, second.z, second.w});
            delta.rotation = safe_normalize(q1 * conjugate(q0) * delta.rotation);
        }
    };

    if (wrapped) {
        accumulate(start, duration_);
        accumulate(0.0F, end);
    } else {
        accumulate(start, end);
    }
    delta.distance = length(delta.translation);

    const u32 contacts = find_named_track(TrackKind::Curve, Name::find("contacts"));
    if (contacts != kNoTrack) {
        delta.contacts = static_cast<u8>(std::lround(sample_track(contacts, end, cursor).x));
    }
    return delta;
}

// --- Events -------------------------------------------------------------------------------------

namespace {

/// Emit the events of `(from, to]` on one pass over the clip's timeline, in time order.
[[nodiscard]] Status emit_span(const Clip& clip, u32 instance, f32 from, f32 to, EventPolicy policy,
                               EventBuffer& buffer) noexcept {
    u32 suppressed = 0;
    for (const ClipEvent& event : clip.events()) {
        if (event.time <= from || event.time > to) {
            continue;
        }
        if (policy == EventPolicy::Suppress) {
            ++suppressed;
            continue;
        }
        EmittedEvent emitted;
        emitted.instance = instance;
        emitted.name = event.name;
        emitted.normalised_time = clip.duration() > 0.0F ? event.time / clip.duration() : 0.0F;
        emitted.parameter = event.parameter;
        if (Status pushed = buffer.push(emitted); !pushed) {
            return pushed;
        }
    }
    buffer.note_suppressed(suppressed);
    return ok();
}

}  // namespace

Status emit_events(const Clip& clip, u32 instance, f32 from, f32 to, EventPolicy policy,
                   EventBuffer& buffer) noexcept {
    const f32 duration = clip.duration();
    if (duration <= 0.0F || to == from) {
        return ok();
    }
    if (clip.loop_mode() != LoopMode::Loop) {
        return emit_span(clip, instance, math::min(from, to), math::max(from, to), policy, buffer);
    }
    // "WHEN a tick advances past several events THEN all of them SHALL be emitted in time order" —
    // including a step long enough to cross the whole clip more than once.
    f32 cursor = from;
    const f32 target = to;
    while (cursor < target) {
        const f32 wrapped = clip.wrap(cursor);
        const f32 remaining = target - cursor;
        const f32 to_end = duration - wrapped;
        const f32 step = remaining < to_end ? remaining : to_end;
        if (Status emitted = emit_span(clip, instance, wrapped, wrapped + step, policy, buffer);
            !emitted) {
            return emitted;
        }
        cursor += step;
        if (step <= 0.0F) {
            break;
        }
    }
    return ok();
}

Status resolve_properties(const Clip& clip, PropertyResolverFn resolver, void* user,
                          Array<f32*>& slots, Array<UnresolvedBinding>& unresolved) noexcept {
    if (resolver == nullptr) {
        return fail(ErrorCode::InvalidArgument, "a property resolver is required");
    }
    if (Status sized = slots.resize(clip.track_count()); !sized) {
        return sized;
    }
    const Span<const TrackDesc> tracks = clip.tracks();
    for (usize index = 0; index < tracks.size(); ++index) {
        slots[index] = nullptr;
        if (tracks[index].kind != TrackKind::Property) {
            continue;
        }
        f32* slot = resolver(tracks[index].binding, user);
        if (slot == nullptr) {
            // "the binding SHALL be reported as unresolved and skipped, and the clip's other tracks
            // SHALL still play".
            if (Status pushed = unresolved.push_back(
                    UnresolvedBinding{clip.name(), tracks[index].name, tracks[index].binding});
                !pushed) {
                return pushed;
            }
            continue;
        }
        slots[index] = slot;
    }
    return ok();
}

}  // namespace cy::animation
