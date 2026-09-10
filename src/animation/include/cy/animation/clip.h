#pragma once
// Animation clips: tracks, keys, error-bounded compression, cursored sampling, markers, events,
// curves, and the root motion track. M8.b task 5.1.
//
// ================================================================================================
// THE FORMAT IS ENGINE-OWNED AND THE CODEC SITS BEHIND AN INTERFACE
// ================================================================================================
//
// `animation-and-skinning` — "Owned format, integrated codec": "the clip asset format SHALL be
// engine-owned, and any integrated codec SHALL sit behind an engine interface so it can be
// replaced". `Clip` is the format. `compress()` is the interface, and what it does today —
// per-track ranges, constant collapsing, curve fitting to an error tolerance and 16-bit
// quantisation — is one implementation of it. A different codec replaces the body of `compress()`
// and the reader in `sample()`; nothing above this header changes.
//
// TOLERANCES ARE IN WORLD UNITS, NOT QUALITY NUMBERS. "expressed as error tolerances in world units
// (translation in millimetres, rotation in degrees) rather than opaque quality numbers, and the
// achieved compression ratio and worst-case error SHALL be reported." `CompressionReport` carries
// both, measured against the authored keys rather than estimated.
//
// SAMPLING IS CURSORED. "WHEN a clip plays forward THEN sampling SHALL advance the cursor rather
// than searching from the start." `ClipCursor` holds one key index per track; a forward step is a
// short scan from where the last one ended, and only a backward jump costs a search.
//
// WHAT IS NOT HERE, AND IS NOT PRETENDED TO BE. Clip STREAMING — "metadata and the active window
// resident" — is a residency policy over `core-assets-and-io`, and the M8.b row of the roadmap does
// not name it. There is no partial residency in this file and no `sample()` that can report an
// underrun; a caller who needs one is not silently given a stall-free path that never stalls
// because everything is always resident.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/transform.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/allocator.h>
#include <cy/core/memory/array.h>
#include <cy/core/reflect/ids.h>
#include <cy/core/values/name.h>

#include <cy/animation/skeleton.h>

namespace cy::animation {

enum class TrackKind : u8 {
    Translation = 0,
    Rotation,
    Scale,
    /// A named float track evaluated alongside the pose, readable by gameplay, audio and materials.
    Curve,
    /// A reflected field, addressed by `TypeId` and `FieldId` so a rename does not break the clip.
    Property,
};

enum class Interpolation : u8 {
    Step = 0,
    Linear,
    /// Authored as cubic. **The stored form is linear between the keys the fitter kept**, and no
    /// tangent is carried: `add_key` takes a value and nothing else. That is a real limitation of
    /// this codec rather than a property of the format — the tolerance is measured against the
    /// authored keys either way, so a cubic track keeps more of them — and a codec that stores
    /// tangents replaces the body of `compress()` and the reader in `sample_track()`.
    Cubic,
    /// Shortest-arc quaternion interpolation. Rotation tracks are stored hemisphere-aligned so no
    /// long-way rotation occurs.
    Spherical,
};

enum class LoopMode : u8 {
    None = 0,
    Loop,
    PingPong,
};

/// A reflected field a property track writes. Stored as identity, never as a name path.
struct PropertyBinding {
    reflect::TypeId type;
    reflect::FieldId field;
    /// Which target of the instance the field belongs to — a component slot, a material parameter
    /// index. Opaque to this module and handed back to the resolver unchanged.
    u32 target = 0;
};

/// Error tolerances, in world units. `animation-and-skinning` states them this way deliberately.
struct CompressionSettings {
    /// Millimetres.
    f32 translation_tolerance_mm = 0.1F;
    /// Degrees.
    f32 rotation_tolerance_degrees = 0.1F;
    /// Unitless, on a scale factor.
    f32 scale_tolerance = 0.001F;
    /// Unitless, on a curve or property value.
    f32 curve_tolerance = 0.001F;
};

/// What compression achieved, measured rather than estimated.
struct CompressionReport {
    u32 tracks = 0;
    u32 constant_tracks = 0;
    u32 keys_before = 0;
    u32 keys_after = 0;
    /// Authored key bytes divided by stored key bytes. Greater than one means smaller.
    f32 ratio = 1.0F;
    /// The largest deviation any sampled position showed from the authored curve, in millimetres.
    f32 worst_translation_mm = 0.0F;
    /// The largest angular deviation, in degrees.
    f32 worst_rotation_degrees = 0.0F;
    f32 worst_scale = 0.0F;
    f32 worst_curve = 0.0F;
};

/// A named point used to align playback when blending clips of differing length.
struct ClipMarker {
    Name name;
    f32 time = 0.0F;
};

/// A named trigger on a clip's timeline.
struct ClipEvent {
    Name name;
    f32 time = 0.0F;
    f32 parameter = 0.0F;
};

/// One track's description. Keys live in the clip's shared key array.
struct TrackDesc {
    TrackKind kind = TrackKind::Translation;
    Interpolation interpolation = Interpolation::Linear;
    /// The joint a TRS track drives; `kInvalidJoint` for a curve or property track.
    u16 joint = kInvalidJoint;
    /// The name of a curve track. Empty otherwise.
    Name name;
    /// The field a property track writes.
    PropertyBinding binding;
    u32 first_key = 0;
    u32 key_count = 0;
    /// Collapsed to a single key by compression.
    bool constant = false;
    /// The quantisation range. Rotation tracks use the fixed [-1, 1] component range and leave
    /// these at their defaults.
    Vec3 range_min{0.0F, 0.0F, 0.0F};
    Vec3 range_max{0.0F, 0.0F, 0.0F};
};

/// One stored key: a time and four 16-bit components. Twelve bytes.
struct PackedKey {
    f32 time = 0.0F;
    u16 c[4] = {0, 0, 0, 0};
};

static_assert(sizeof(PackedKey) == 12, "a stored key is a time and four quantised components");

/// One authored key, before compression.
struct RawKey {
    f32 time = 0.0F;
    Vec4 value{0.0F, 0.0F, 0.0F, 0.0F};
};

/// Where sampling left off, one key index per track.
///
/// Owned by the instance, not by the clip: a clip is shared by every character playing it, and a
/// cursor is where one of them is.
class ClipCursor {
public:
    explicit ClipCursor(Allocator& allocator) noexcept;

    [[nodiscard]] Status reset(u32 track_count) noexcept;
    [[nodiscard]] u32 at(u32 track) const noexcept;
    void set(u32 track, u32 key) noexcept;
    [[nodiscard]] bool empty() const noexcept { return keys_.empty(); }

    /// How many keys the last sample had to step over. The measurement behind "forward playback is
    /// O(1) amortised": a forward step over a 600-key clip visits one or two, a backward jump
    /// visits none because it binary-searches.
    [[nodiscard]] u32 steps() const noexcept { return steps_; }
    [[nodiscard]] u32 searches() const noexcept { return searches_; }
    void note_step() noexcept { ++steps_; }
    void note_search() noexcept { ++searches_; }
    void reset_counters() noexcept {
        steps_ = 0;
        searches_ = 0;
    }

private:
    Array<u32> keys_;
    u32 steps_ = 0;
    u32 searches_ = 0;
};

/// What one clip sample touched. `joints_written` is the measurement behind pose dependency
/// analysis: a masked-out joint is not merely blended away, its tracks are never read.
struct SampleStats {
    u32 tracks_read = 0;
    u32 tracks_skipped = 0;
    u32 joints_written = 0;
    u32 curves_read = 0;
};

/// The root motion of one interval, extracted rather than applied to the skeleton.
struct RootDelta {
    Vec3 translation{0.0F, 0.0F, 0.0F};
    Quat rotation = Quat::identity();
    /// The motion curve: distance travelled over the interval, in world units.
    f32 distance = 0.0F;
    /// Contact states, bit 0 left and bit 1 right, from the clip's contact curve where it has one.
    u8 contacts = 0;
};

/// An animation clip.
///
/// Authored by `add_track` and `add_key`, then `compress()`. Sampling reads the compressed form
/// only, so a clip that was never compressed samples nothing and says so.
class Clip {
public:
    explicit Clip(Allocator& allocator) noexcept;

    Clip(const Clip&) = delete;
    Clip& operator=(const Clip&) = delete;
    Clip(Clip&&) noexcept = default;
    Clip& operator=(Clip&&) noexcept = default;
    ~Clip() = default;

    void set_name(Name name) noexcept { name_ = name; }
    void set_duration(f32 seconds) noexcept { duration_ = seconds; }
    void set_loop_mode(LoopMode mode) noexcept { loop_ = mode; }
    void set_sample_rate_hint(f32 hertz) noexcept { sample_rate_ = hertz; }
    /// Designate the joint whose motion is EXTRACTED rather than applied. "the root joint SHALL not
    /// be moved by the animation."
    void set_root_motion_joint(u16 joint) noexcept { root_joint_ = joint; }

    [[nodiscard]] Name name() const noexcept { return name_; }
    [[nodiscard]] f32 duration() const noexcept { return duration_; }
    [[nodiscard]] LoopMode loop_mode() const noexcept { return loop_; }
    [[nodiscard]] f32 sample_rate_hint() const noexcept { return sample_rate_; }
    [[nodiscard]] u16 root_motion_joint() const noexcept { return root_joint_; }
    [[nodiscard]] bool has_root_motion() const noexcept { return root_joint_ != kInvalidJoint; }
    [[nodiscard]] bool compressed() const noexcept { return compressed_; }
    [[nodiscard]] u32 track_count() const noexcept { return static_cast<u32>(tracks_.size()); }
    [[nodiscard]] Span<const TrackDesc> tracks() const noexcept { return tracks_.span(); }
    [[nodiscard]] Span<const ClipMarker> markers() const noexcept { return markers_.span(); }
    [[nodiscard]] Span<const ClipEvent> events() const noexcept { return events_.span(); }
    [[nodiscard]] const CompressionReport& report() const noexcept { return report_; }

    // --- Authoring ------------------------------------------------------------------------------

    [[nodiscard]] Expected<u32, Error> add_joint_track(TrackKind kind, u16 joint,
                                                       Interpolation interpolation) noexcept;
    [[nodiscard]] Expected<u32, Error> add_curve_track(Name name,
                                                       Interpolation interpolation) noexcept;
    [[nodiscard]] Expected<u32, Error> add_property_track(Name name, const PropertyBinding& binding,
                                                          Interpolation interpolation) noexcept;
    /// Keys must be appended in increasing time order within a track.
    [[nodiscard]] Status add_key(u32 track, f32 time, Vec4 value) noexcept;
    [[nodiscard]] Status add_marker(Name name, f32 time) noexcept;
    [[nodiscard]] Status add_event(Name name, f32 time, f32 parameter = 0.0F) noexcept;

    /// Compress the authored keys. Idempotent in effect: calling it again recompresses from the
    /// authored keys, which are kept so the error report is measured rather than estimated.
    [[nodiscard]] Status compress(const CompressionSettings& settings) noexcept;

    // --- Sampling -------------------------------------------------------------------------------

    /// Sample the pose into `out`, writing only the joints in `mask`.
    ///
    /// `out` is indexed by joint and must be at least the skeleton's joint count. Joints the clip
    /// has no track for are left untouched, so a caller seeds `out` with the reference pose.
    [[nodiscard]] Status sample(f32 time, const JointMask& mask, ClipCursor& cursor,
                                Span<Transform> out, SampleStats& stats) const noexcept;

    /// Sample one named curve. The empty answer is `false`, so a caller can tell "zero" from "no
    /// such curve".
    [[nodiscard]] bool sample_curve(Name curve, f32 time, ClipCursor& cursor,
                                    f32& out) const noexcept;

    /// Write every property track's value through the resolved accessors in `slots`, which is
    /// parallel to `tracks()` and holds `nullptr` for a track that did not resolve.
    [[nodiscard]] u32 write_properties(f32 time, ClipCursor& cursor,
                                       Span<f32* const> slots) const noexcept;

    /// The root motion between two times on this clip's timeline, composed the way the character
    /// controller consumes it. `to` may be less than `from`, which is a wrapped loop.
    [[nodiscard]] RootDelta root_delta(f32 from, f32 to) const noexcept;

    /// The time on this clip that corresponds to `phase` — normalised, or aligned to a marker when
    /// `marker` names one this clip carries. Marker correspondence is what sync groups align by.
    [[nodiscard]] f32 time_at_phase(f32 phase) const noexcept;
    [[nodiscard]] bool marker_time(Name marker, f32& out) const noexcept;

    /// Wrap a time onto the clip's timeline under its loop mode.
    [[nodiscard]] f32 wrap(f32 time) const noexcept;

    /// The track index driving `joint` for `kind`, or `0xFFFFFFFF`.
    [[nodiscard]] u32 find_joint_track(TrackKind kind, u16 joint) const noexcept;
    [[nodiscard]] u32 find_named_track(TrackKind kind, Name name) const noexcept;

    /// Sample one track's four raw components. Exposed because retargeting and the pose cache both
    /// need a track's value without a pose buffer around it.
    [[nodiscard]] Vec4 sample_track(u32 track, f32 time, ClipCursor& cursor) const noexcept;

private:
    [[nodiscard]] Expected<u32, Error> add_track(const TrackDesc& desc) noexcept;

    Name name_;
    Array<TrackDesc> tracks_;
    /// The authored keys, and where each track's are. Kept after compression so the error report
    /// is measured against them and so a second `compress()` with different tolerances starts from
    /// the source rather than from the last result.
    Array<RawKey> raw_;
    Array<u32> raw_first_;
    Array<u32> raw_count_;
    Array<PackedKey> packed_;
    Array<ClipMarker> markers_;
    Array<ClipEvent> events_;
    CompressionReport report_;
    f32 duration_ = 1.0F;
    f32 sample_rate_ = 30.0F;
    LoopMode loop_ = LoopMode::Loop;
    u16 root_joint_ = kInvalidJoint;
    bool compressed_ = false;
};

/// One emitted animation event. `animation-and-skinning`: "Events SHALL be emitted as typed data
/// into an event buffer ... Events SHALL NOT invoke arbitrary callbacks scattered across objects."
struct EmittedEvent {
    u32 instance = 0;
    Name name;
    /// Where in the clip the event sits, normalised.
    f32 normalised_time = 0.0F;
    f32 parameter = 0.0F;
};

/// How an animation level of detail tier treats events.
enum class EventPolicy : u8 {
    /// Every crossing is emitted.
    Emit = 0,
    /// Crossings are counted and not emitted. "so a distant crowd does not emit thousands of
    /// footstep events".
    Suppress,
};

/// The buffer events are written into, and the counts a suppressed tier still reports.
class EventBuffer {
public:
    explicit EventBuffer(Allocator& allocator) noexcept;

    [[nodiscard]] Status push(const EmittedEvent& event) noexcept;
    void note_suppressed(u32 count) noexcept { suppressed_ += count; }
    void clear() noexcept;

    [[nodiscard]] Span<const EmittedEvent> events() const noexcept { return events_.span(); }
    [[nodiscard]] u32 suppressed() const noexcept { return suppressed_; }

private:
    Array<EmittedEvent> events_;
    u32 suppressed_ = 0;
};

/// Emit every event the interval `(from, to]` crosses, in time order, wrapping under the clip's
/// loop mode. A step that crosses the end of a looping clip several times emits each crossing.
[[nodiscard]] Status emit_events(const Clip& clip, u32 instance, f32 from, f32 to,
                                 EventPolicy policy, EventBuffer& buffer) noexcept;

/// The accessor a property track writes through, looked up once per instance.
using PropertyResolverFn = f32* (*)(const PropertyBinding& binding, void* user) noexcept;

/// A binding whose target no longer exists. Reported and skipped; the clip's other tracks play.
struct UnresolvedBinding {
    Name clip;
    Name track;
    PropertyBinding binding;
};

/// Resolve every property track once, filling `slots` parallel to `clip.tracks()`.
[[nodiscard]] Status resolve_properties(const Clip& clip, PropertyResolverFn resolver, void* user,
                                        Array<f32*>& slots,
                                        Array<UnresolvedBinding>& unresolved) noexcept;

}  // namespace cy::animation
