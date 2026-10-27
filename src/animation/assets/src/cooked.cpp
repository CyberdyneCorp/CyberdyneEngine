// SPDX-License-Identifier: MIT
// The cooked animation records. See cy/animation/cooked.h.

#include <cy/animation/cooked.h>

#include "byte_io.h"

#include <initializer_list>

namespace cy::animation {
namespace {

using cooked::Reader;
using cooked::Writer;
namespace pose = graph::pose;

constexpr u32 kUnmapped = 0xFFFFFFFFU;
/// A joint record after its name: the parent, ten floats of bind pose, the bone level.
constexpr u64 kJointRecordBytes = u64{4} + (u64{10} * 4U) + 4U;
constexpr u64 kTrackRecordBytes = u64{12} * 4U;
constexpr u64 kKeyRecordBytes = u64{4} + (u64{4} * 2U);

[[nodiscard]] Status finished(const Writer& writer) noexcept {
    return writer.ok() ? ok() : fail(ErrorCode::OutOfMemory, "a cooked record would not grow");
}

void write_transform(Writer& writer, const Transform& value) noexcept {
    writer.f32_value(value.translation.x);
    writer.f32_value(value.translation.y);
    writer.f32_value(value.translation.z);
    writer.f32_value(value.rotation.x);
    writer.f32_value(value.rotation.y);
    writer.f32_value(value.rotation.z);
    writer.f32_value(value.rotation.w);
    writer.f32_value(value.scale.x);
    writer.f32_value(value.scale.y);
    writer.f32_value(value.scale.z);
}

[[nodiscard]] Transform read_transform(Reader& reader) noexcept {
    Transform value;
    value.translation = Vec3{reader.f32_value(), reader.f32_value(), reader.f32_value()};
    value.rotation =
        Quat{reader.f32_value(), reader.f32_value(), reader.f32_value(), reader.f32_value()};
    value.scale = Vec3{reader.f32_value(), reader.f32_value(), reader.f32_value()};
    return value;
}

void write_mask(Writer& writer, const JointMask& mask) noexcept {
    for (u32 word = 0; word < pose::kMaxJoints / 64U; ++word) {
        writer.u64_value(mask.word(word));
    }
}

[[nodiscard]] JointMask read_mask(Reader& reader) noexcept {
    JointMask mask;
    for (u32 word = 0; word < pose::kMaxJoints / 64U; ++word) {
        const u64 bits = reader.u64_value();
        for (u32 bit = 0; bit < 64U; ++bit) {
            if (((bits >> bit) & 1U) != 0U) {
                mask.set((word * 64U) + bit);
            }
        }
    }
    return mask;
}

}  // namespace

// --- Skeleton ------------------------------------------------------------------------------------

Status encode_skeleton(const Skeleton& skeleton, const SkeletonProfile& humanoid,
                       Array<u8>& out) noexcept {
    if (skeleton.joint_count() == 0) {
        return fail(ErrorCode::InvalidArgument, "a skeleton with no joints deforms nothing");
    }
    Writer writer(out);
    writer.u32_value(kCookedSkeletonVersion);
    writer.u32_value(skeleton.joint_count());
    // The record's name is counted bytes after the count, the importer's layout: the length, then
    // the name, with the joints after it.
    writer.text(skeleton.name().text());
    for (const Joint& joint : skeleton.joints()) {
        writer.text(joint.name.text());
        writer.u32_value(joint.parent == kInvalidJoint ? kUnmapped : joint.parent);
        write_transform(writer, joint.bind_local);
        writer.u32_value(joint.dropped_at);
    }
    for (u32 standard = 0; standard < static_cast<u32>(HumanoidJoint::Count); ++standard) {
        const u16 joint = humanoid.resolve(static_cast<HumanoidJoint>(standard));
        writer.u32_value(joint == kInvalidJoint ? kUnmapped : joint);
    }
    return finished(writer);
}

Status decode_skeleton(Span<const u8> payload, Skeleton& out, SkeletonProfile& humanoid) noexcept {
    if (out.joint_count() != 0) {
        return fail(ErrorCode::InvalidArgument, "a skeleton is decoded into an empty one");
    }
    Reader reader(payload);
    if (reader.u32_value() != kCookedSkeletonVersion) {
        return fail(ErrorCode::Unsupported, "a cooked skeleton from another format version");
    }
    const u32 count = reader.u32_value();
    if (count == 0 || count > kMaxJoints || !reader.plausible(count, kJointRecordBytes)) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked skeleton with no joints, more than a pose mask addresses, or fewer "
                    "bytes than it claims joints");
    }
    out.set_name(reader.name());
    for (u32 index = 0; index < count && reader.ok(); ++index) {
        const Name name = reader.name();
        const u32 parent = reader.u32_value();
        const Transform bind = read_transform(reader);
        const u32 dropped_at = reader.u32_value();
        if (!reader.ok()) {
            break;
        }
        if (dropped_at == 0 || dropped_at > kBoneLodLevels) {
            return fail(ErrorCode::InvalidArgument,
                        "a cooked skeleton with a bone level of detail outside the range");
        }
        // `add_joint` refuses a parent that does not precede its child, which is the order rule
        // the record promises.
        const Expected<u16, Error> added =
            out.add_joint(name, parent == kUnmapped ? kInvalidJoint : static_cast<u16>(parent),
                          bind, static_cast<u8>(dropped_at));
        if (!added) {
            return Status{make_unexpected(added.error())};
        }
    }
    for (u32 standard = 0; standard < static_cast<u32>(HumanoidJoint::Count) && reader.ok();
         ++standard) {
        const u32 joint = reader.u32_value();
        if (joint == kUnmapped) {
            continue;
        }
        if (joint >= count) {
            return fail(ErrorCode::InvalidArgument,
                        "a cooked skeleton whose humanoid profile names a joint it does not have");
        }
        humanoid.map(static_cast<HumanoidJoint>(standard), static_cast<u16>(joint));
    }
    if (!reader.finished()) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked skeleton whose payload does not match its header");
    }
    return out.finalize();
}

// --- Clip ----------------------------------------------------------------------------------------

Status encode_clip(const Clip& clip, Span<const Name> joints, Array<u8>& out) noexcept {
    if (!clip.compressed()) {
        return fail(ErrorCode::InvalidArgument, "a clip is cooked from its compressed form");
    }
    const bool annotated = !clip.markers().empty() || !clip.events().empty();
    Writer writer(out);
    writer.u32_value(annotated ? kCookedClipEventsVersion : kCookedClipVersion);
    writer.name(clip.name());
    writer.f32_value(clip.duration());
    writer.u32_value(static_cast<u32>(clip.loop_mode()));
    writer.f32_value(clip.sample_rate_hint());
    writer.u32_value(clip.root_motion_joint());
    writer.u32_value(static_cast<u32>(joints.size()));
    for (const Name& joint : joints) {
        writer.name(joint);
    }
    writer.u32_value(clip.track_count());
    for (const TrackDesc& track : clip.tracks()) {
        writer.u32_value(static_cast<u32>(track.kind));
        writer.u32_value(static_cast<u32>(track.interpolation));
        writer.u32_value(track.joint);
        writer.u32_value(track.constant ? 1U : 0U);
        writer.u32_value(track.first_key);
        writer.u32_value(track.key_count);
        writer.f32_value(track.range_min.x);
        writer.f32_value(track.range_min.y);
        writer.f32_value(track.range_min.z);
        writer.f32_value(track.range_max.x);
        writer.f32_value(track.range_max.y);
        writer.f32_value(track.range_max.z);
    }
    writer.u32_value(static_cast<u32>(clip.keys().size()));
    for (const PackedKey& key : clip.keys()) {
        writer.f32_value(key.time);
        for (const u16 component : key.c) {
            writer.u16_value(component);
        }
    }
    if (annotated) {
        writer.u32_value(static_cast<u32>(clip.markers().size()));
        for (const ClipMarker& marker : clip.markers()) {
            writer.name(marker.name);
            writer.f32_value(marker.time);
        }
        writer.u32_value(static_cast<u32>(clip.events().size()));
        for (const ClipEvent& event : clip.events()) {
            writer.name(event.name);
            writer.f32_value(event.time);
            writer.f32_value(event.parameter);
        }
    }
    return finished(writer);
}

namespace {

[[nodiscard]] Status read_tracks(Reader& reader, Array<TrackDesc>& out) noexcept {
    const u32 count = reader.u32_value();
    if (!reader.plausible(count, kTrackRecordBytes)) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked clip record claims more tracks than it carries bytes for");
    }
    for (u32 index = 0; index < count && reader.ok(); ++index) {
        TrackDesc track;
        const u32 kind = reader.u32_value();
        const u32 interpolation = reader.u32_value();
        if (kind > static_cast<u32>(TrackKind::Property) ||
            interpolation > static_cast<u32>(Interpolation::Spherical)) {
            return fail(ErrorCode::InvalidArgument,
                        "a cooked clip track of a kind or interpolation this build does not know");
        }
        track.kind = static_cast<TrackKind>(kind);
        track.interpolation = static_cast<Interpolation>(interpolation);
        track.joint = static_cast<u16>(reader.u32_value());
        track.constant = reader.u32_value() != 0U;
        track.first_key = reader.u32_value();
        track.key_count = reader.u32_value();
        track.range_min = Vec3{reader.f32_value(), reader.f32_value(), reader.f32_value()};
        track.range_max = Vec3{reader.f32_value(), reader.f32_value(), reader.f32_value()};
        if (Status pushed = out.push_back(track); !pushed) {
            return pushed;
        }
    }
    return ok();
}

[[nodiscard]] Status read_keys(Reader& reader, Array<PackedKey>& out) noexcept {
    const u32 count = reader.u32_value();
    if (!reader.plausible(count, kKeyRecordBytes)) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked clip record claims more keys than it carries bytes for");
    }
    if (Status sized = out.resize(count); !sized) {
        return sized;
    }
    for (PackedKey& key : out) {
        key.time = reader.f32_value();
        for (u16& component : key.c) {
            component = reader.u16_value();
        }
    }
    return ok();
}

[[nodiscard]] Status read_annotations(Reader& reader, Clip& out) noexcept {
    const u32 markers = reader.u32_value();
    if (!reader.plausible(markers, 8)) {
        return fail(ErrorCode::InvalidArgument, "a cooked clip claims more markers than it holds");
    }
    for (u32 index = 0; index < markers && reader.ok(); ++index) {
        const Name name = reader.name();
        if (Status added = out.add_marker(name, reader.f32_value()); !added) {
            return added;
        }
    }
    const u32 events = reader.u32_value();
    if (!reader.plausible(events, 12)) {
        return fail(ErrorCode::InvalidArgument, "a cooked clip claims more events than it holds");
    }
    for (u32 index = 0; index < events && reader.ok(); ++index) {
        const Name name = reader.name();
        const f32 time = reader.f32_value();
        if (Status added = out.add_event(name, time, reader.f32_value()); !added) {
            return added;
        }
    }
    return ok();
}

}  // namespace

Status decode_clip(Span<const u8> payload, Clip& out, Array<Name>& joints) noexcept {
    Reader reader(payload);
    const u32 version = reader.u32_value();
    if (version != kCookedClipVersion && version != kCookedClipEventsVersion) {
        return fail(ErrorCode::Unsupported, "a cooked clip from another format version");
    }
    out.set_name(reader.name());
    out.set_duration(reader.f32_value());
    const u32 loop = reader.u32_value();
    if (loop > static_cast<u32>(LoopMode::PingPong)) {
        return fail(ErrorCode::InvalidArgument, "a cooked clip with a loop mode this build lacks");
    }
    out.set_loop_mode(static_cast<LoopMode>(loop));
    out.set_sample_rate_hint(reader.f32_value());
    out.set_root_motion_joint(static_cast<u16>(reader.u32_value()));

    const u32 joint_count = reader.u32_value();
    if (!reader.plausible(joint_count, 4)) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked clip record claims more joints than it carries bytes for");
    }
    joints.clear();
    for (u32 index = 0; index < joint_count && reader.ok(); ++index) {
        if (Status pushed = joints.push_back(reader.name()); !pushed) {
            return pushed;
        }
    }

    Array<TrackDesc> tracks(joints.allocator());
    Array<PackedKey> keys(joints.allocator());
    if (Status read = read_tracks(reader, tracks); !read) {
        return read;
    }
    if (Status read = read_keys(reader, keys); !read) {
        return read;
    }
    if (version == kCookedClipEventsVersion) {
        if (Status read = read_annotations(reader, out); !read) {
            return read;
        }
    }
    if (!reader.finished()) {
        return fail(ErrorCode::InvalidArgument, "a cooked clip record ends before its contents do");
    }
    return out.adopt_compressed(tracks.span(), keys.span());
}

bool clip_matches_skeleton(const Clip& clip, Span<const Name> joints, const Skeleton& skeleton,
                           u16& out_joint) noexcept {
    for (const TrackDesc& track : clip.tracks()) {
        if (track.joint == kInvalidJoint) {
            continue;
        }
        const bool named = track.joint < joints.size() && track.joint < skeleton.joint_count() &&
                           joints[track.joint] == skeleton.joints()[track.joint].name;
        if (!named) {
            out_joint = track.joint;
            return false;
        }
    }
    return true;
}

// --- Program -------------------------------------------------------------------------------------

namespace {

[[nodiscard]] bool has_modifiers(const pose::PoseProgram& program) noexcept {
    return !program.blend_spaces().empty() || !program.foot_placements().empty() ||
           !program.aims().empty();
}

void write_modifiers(Writer& writer, const pose::PoseProgram& program) noexcept {
    writer.u32_value(static_cast<u32>(program.blend_spaces().size()));
    for (const pose::BlendSpace& space : program.blend_spaces()) {
        writer.u16_value(static_cast<u16>((space.dimensions << 8U) | (space.sync ? 1U : 0U)));
        writer.u16_value(space.x_param);
        writer.u16_value(space.y_param);
        writer.u16_value(space.phase_param);
        writer.u32_value(space.first_sample);
        writer.u32_value(space.sample_count);
        writer.u32_value(space.first_triangle);
        writer.u32_value(space.triangle_count);
        writer.u64_value(space.origin);
    }
    writer.u32_value(static_cast<u32>(program.blend_samples().size()));
    for (const pose::BlendSample& sample : program.blend_samples()) {
        writer.f32_value(sample.x);
        writer.f32_value(sample.y);
        writer.u16_value(sample.value);
    }
    writer.u32_value(static_cast<u32>(program.blend_triangles().size()));
    for (const pose::BlendTriangle& triangle : program.blend_triangles()) {
        for (const u16 vertex : triangle.sample) {
            writer.u16_value(vertex);
        }
    }
    writer.u32_value(static_cast<u32>(program.foot_placements().size()));
    for (const pose::FootPlacementRef& foot : program.foot_placements()) {
        writer.name(foot.pelvis);
        for (const auto& leg : foot.leg) {
            for (const Name& joint : leg) {
                writer.name(joint);
            }
        }
        for (const f32 value : {foot.ankle_height, foot.probe_above, foot.probe_below,
                                foot.max_pelvis_drop, foot.max_foot_raise, foot.smoothing,
                                foot.max_tilt, foot.pole[0], foot.pole[1], foot.pole[2]}) {
            writer.f32_value(value);
        }
        writer.u64_value(foot.origin);
    }
    writer.u32_value(static_cast<u32>(program.aims().size()));
    for (const pose::AimRef& aim : program.aims()) {
        writer.u32_value(aim.joint_count);
        for (u32 joint = 0; joint < aim.joint_count; ++joint) {
            writer.name(aim.joints[joint]);
        }
        for (const f32 value : {aim.axis[0], aim.axis[1], aim.axis[2], aim.max_angle}) {
            writer.f32_value(value);
        }
        for (const u16 parameter : aim.target_param) {
            writer.u16_value(parameter);
        }
        writer.u64_value(aim.origin);
    }
}

}  // namespace

Status encode_program(const pose::PoseProgram& program, Array<u8>& out) noexcept {
    Writer writer(out);
    const bool modifiers = has_modifiers(program);
    writer.u32_value(kCookedProgramMagic);
    writer.u32_value(modifiers ? kCookedProgramModifiersVersion : kCookedProgramVersion);
    writer.name(program.name());
    writer.u32_value(program.joint_count());
    writer.u32_value(program.entry_state());
    writer.u64_value(program.digest());

    writer.u32_value(static_cast<u32>(program.parameters().size()));
    for (const Name& parameter : program.parameters()) {
        writer.name(parameter);
    }
    writer.u32_value(static_cast<u32>(program.clips().size()));
    for (const pose::ClipRef& clip : program.clips()) {
        writer.name(clip.name);
        writer.f32_value(clip.duration);
        writer.u32_value(clip.looping ? 1U : 0U);
        writer.u32_value(clip.first_marker);
        writer.u32_value(clip.marker_count);
    }
    writer.u32_value(static_cast<u32>(program.masks().size()));
    for (const JointMask& mask : program.masks()) {
        write_mask(writer, mask);
    }
    writer.u32_value(static_cast<u32>(program.code().size()));
    for (const pose::PoseInstruction& instruction : program.code()) {
        writer.u16_value(static_cast<u16>(instruction.op));
        writer.u16_value(instruction.a);
        writer.u16_value(instruction.b);
        writer.u16_value(instruction.clip);
        writer.u16_value(instruction.mask);
        writer.u16_value(instruction.time_param);
        writer.u16_value(instruction.weight_param);
        writer.u16_value(instruction.chain);
        write_mask(writer, instruction.required);
        writer.u64_value(instruction.origin);
    }
    writer.u32_value(static_cast<u32>(program.states().size()));
    for (const pose::PoseState& state : program.states()) {
        writer.name(state.name);
        writer.u16_value(state.root);
        writer.u16_value(state.sync_group);
        writer.u32_value(state.first_transition);
        writer.u32_value(state.transition_count);
        writer.u64_value(state.origin);
    }
    writer.u32_value(static_cast<u32>(program.transitions().size()));
    for (const pose::Transition& transition : program.transitions()) {
        writer.u16_value(transition.target_state);
        writer.u16_value(transition.condition_param);
        writer.f32_value(transition.duration);
        writer.u16_value(transition.priority);
        writer.u16_value(static_cast<u16>(transition.interruption));
        writer.u64_value(transition.origin);
    }
    writer.u32_value(static_cast<u32>(program.sync_groups().size()));
    for (const pose::SyncGroup& group : program.sync_groups()) {
        writer.name(group.name);
        writer.u32_value(group.first_marker);
        writer.u32_value(group.marker_count);
    }
    writer.u32_value(static_cast<u32>(program.markers().size()));
    for (const pose::SyncMarker& marker : program.markers()) {
        writer.name(marker.name);
        writer.f32_value(marker.time);
    }
    if (modifiers) {
        write_modifiers(writer, program);
    }
    return finished(writer);
}

namespace {

/// The decoded arrays, owned until `assemble_pose_program` copies them.
struct DecodedProgram {
    explicit DecodedProgram(Allocator& allocator) noexcept
        : code(allocator),
          states(allocator),
          transitions(allocator),
          clips(allocator),
          masks(allocator),
          parameters(allocator),
          sync_groups(allocator),
          markers(allocator),
          blend_spaces(allocator),
          blend_samples(allocator),
          blend_triangles(allocator),
          foot_placements(allocator),
          aims(allocator) {}

    Array<pose::PoseInstruction> code;
    Array<pose::PoseState> states;
    Array<pose::Transition> transitions;
    Array<pose::ClipRef> clips;
    Array<JointMask> masks;
    Array<Name> parameters;
    Array<pose::SyncGroup> sync_groups;
    Array<pose::SyncMarker> markers;
    Array<pose::BlendSpace> blend_spaces;
    Array<pose::BlendSample> blend_samples;
    Array<pose::BlendTriangle> blend_triangles;
    Array<pose::FootPlacementRef> foot_placements;
    Array<pose::AimRef> aims;
};

/// Read a counted table, one record at a time through `read`. `bytes` is the smallest a record can
/// be, which is what refuses a corrupt count before anything is sized by it.
template <class T, class Read>
[[nodiscard]] Status read_table(Reader& reader, u64 bytes, Array<T>& out, Read&& read) noexcept {
    const u32 count = reader.u32_value();
    if (!reader.plausible(count, bytes)) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked pose program claims more records than it carries bytes for");
    }
    if (Status sized = out.resize(count); !sized) {
        return sized;
    }
    for (T& record : out) {
        read(record);
    }
    return ok();
}

constexpr u64 kMaskBytes = u64{pose::kMaxJoints / 64U} * 8U;

[[nodiscard]] Status read_tables(Reader& reader, DecodedProgram& out) noexcept {
    Status read = read_table(reader, 4, out.parameters, [&](Name& name) { name = reader.name(); });
    if (read) {
        read = read_table(reader, 20, out.clips, [&](pose::ClipRef& clip) {
            clip.name = reader.name();
            clip.duration = reader.f32_value();
            clip.looping = reader.u32_value() != 0U;
            clip.first_marker = reader.u32_value();
            clip.marker_count = reader.u32_value();
        });
    }
    if (read) {
        read = read_table(reader, kMaskBytes, out.masks,
                          [&](JointMask& mask) { mask = read_mask(reader); });
    }
    if (read) {
        read = read_table(reader, 16 + kMaskBytes + 8, out.code,
                          [&](pose::PoseInstruction& instruction) {
                              instruction.op = static_cast<pose::PoseOp>(reader.u16_value());
                              instruction.a = reader.u16_value();
                              instruction.b = reader.u16_value();
                              instruction.clip = reader.u16_value();
                              instruction.mask = reader.u16_value();
                              instruction.time_param = reader.u16_value();
                              instruction.weight_param = reader.u16_value();
                              instruction.chain = reader.u16_value();
                              instruction.required = read_mask(reader);
                              instruction.origin = reader.u64_value();
                          });
    }
    if (read) {
        read = read_table(reader, 24, out.states, [&](pose::PoseState& state) {
            state.name = reader.name();
            state.root = reader.u16_value();
            state.sync_group = reader.u16_value();
            state.first_transition = reader.u32_value();
            state.transition_count = reader.u32_value();
            state.origin = reader.u64_value();
        });
    }
    if (read) {
        read = read_table(reader, 20, out.transitions, [&](pose::Transition& transition) {
            transition.target_state = reader.u16_value();
            transition.condition_param = reader.u16_value();
            transition.duration = reader.f32_value();
            transition.priority = reader.u16_value();
            const u16 interruption = reader.u16_value();
            transition.interruption = interruption <= static_cast<u16>(pose::Interruption::Any)
                                          ? static_cast<pose::Interruption>(interruption)
                                          : pose::Interruption::None;
            transition.origin = reader.u64_value();
        });
    }
    if (read) {
        read = read_table(reader, 12, out.sync_groups, [&](pose::SyncGroup& group) {
            group.name = reader.name();
            group.first_marker = reader.u32_value();
            group.marker_count = reader.u32_value();
        });
    }
    if (read) {
        read = read_table(reader, 8, out.markers, [&](pose::SyncMarker& marker) {
            marker.name = reader.name();
            marker.time = reader.f32_value();
        });
    }
    return read;
}

[[nodiscard]] Status read_modifiers(Reader& reader, DecodedProgram& out) noexcept {
    Status read = read_table(reader, 32, out.blend_spaces, [&](pose::BlendSpace& space) {
        const u16 shape = reader.u16_value();
        space.dimensions = static_cast<u8>(shape >> 8U);
        space.sync = (shape & 1U) != 0U;
        space.x_param = reader.u16_value();
        space.y_param = reader.u16_value();
        space.phase_param = reader.u16_value();
        space.first_sample = reader.u32_value();
        space.sample_count = reader.u32_value();
        space.first_triangle = reader.u32_value();
        space.triangle_count = reader.u32_value();
        space.origin = reader.u64_value();
    });
    if (read) {
        read = read_table(reader, 10, out.blend_samples, [&](pose::BlendSample& sample) {
            sample.x = reader.f32_value();
            sample.y = reader.f32_value();
            sample.value = reader.u16_value();
        });
    }
    if (read) {
        read = read_table(reader, 6, out.blend_triangles, [&](pose::BlendTriangle& triangle) {
            for (u16& vertex : triangle.sample) {
                vertex = reader.u16_value();
            }
        });
    }
    if (read) {
        read = read_table(reader, 76, out.foot_placements, [&](pose::FootPlacementRef& foot) {
            foot.pelvis = reader.name();
            for (auto& leg : foot.leg) {
                for (Name& joint : leg) {
                    joint = reader.name();
                }
            }
            for (f32* value : {&foot.ankle_height, &foot.probe_above, &foot.probe_below,
                               &foot.max_pelvis_drop, &foot.max_foot_raise, &foot.smoothing,
                               &foot.max_tilt, &foot.pole[0], &foot.pole[1], &foot.pole[2]}) {
                *value = reader.f32_value();
            }
            foot.origin = reader.u64_value();
        });
    }
    if (read) {
        read = read_table(reader, 34, out.aims, [&](pose::AimRef& aim) {
            const u32 joints = reader.u32_value();
            // Refused by `assemble_pose_program`; read no further than the table can hold.
            aim.joint_count = static_cast<u8>(joints <= pose::kMaxAimJoints ? joints : 0U);
            for (u32 joint = 0; joint < aim.joint_count; ++joint) {
                aim.joints[joint] = reader.name();
            }
            for (f32* value : {&aim.axis[0], &aim.axis[1], &aim.axis[2], &aim.max_angle}) {
                *value = reader.f32_value();
            }
            for (u16& parameter : aim.target_param) {
                parameter = reader.u16_value();
            }
            aim.origin = reader.u64_value();
        });
    }
    return read;
}

}  // namespace

Expected<pose::PoseProgram, Error> decode_program(Allocator& allocator,
                                                  Span<const u8> payload) noexcept {
    Reader reader(payload);
    if (reader.u32_value() != kCookedProgramMagic) {
        return fail(ErrorCode::InvalidArgument, "not a cooked pose program");
    }
    const u32 version = reader.u32_value();
    if (version != kCookedProgramVersion && version != kCookedProgramModifiersVersion) {
        return fail(ErrorCode::Unsupported, "a cooked pose program from another format version");
    }
    pose::PoseProgramParts parts;
    parts.name = reader.name();
    parts.joint_count = reader.u32_value();
    parts.entry_state = static_cast<u16>(reader.u32_value());
    parts.digest = reader.u64_value();

    DecodedProgram decoded(allocator);
    if (Status read = read_tables(reader, decoded); !read) {
        return make_unexpected(read.error());
    }
    if (version == kCookedProgramModifiersVersion) {
        if (Status read = read_modifiers(reader, decoded); !read) {
            return make_unexpected(read.error());
        }
    }
    if (!reader.finished()) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked pose program record ends before its contents do");
    }
    parts.code = decoded.code.span();
    parts.states = decoded.states.span();
    parts.transitions = decoded.transitions.span();
    parts.clips = decoded.clips.span();
    parts.masks = decoded.masks.span();
    parts.parameters = decoded.parameters.span();
    parts.sync_groups = decoded.sync_groups.span();
    parts.markers = decoded.markers.span();
    parts.blend_spaces = decoded.blend_spaces.span();
    parts.blend_samples = decoded.blend_samples.span();
    parts.blend_triangles = decoded.blend_triangles.span();
    parts.foot_placements = decoded.foot_placements.span();
    parts.aims = decoded.aims.span();
    return pose::assemble_pose_program(allocator, parts);
}

}  // namespace cy::animation
