#include <cy/import/fbx_clip.h>

#include <cy/core/math/quat.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/scope.h>

#include <ufbx.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#if defined(CY_IMPORT_ANIMATION)
#    include <cy/animation/clip.h>
#    include <cy/animation/skeleton.h>
#endif

namespace cy::import {
namespace {

// --- Little-endian record primitives -------------------------------------------------------------
//
// The same shape `model.cpp` writes a cooked material with, repeated rather than shared because
// those are file-local by construction and a cooked record's writer should be readable beside its
// reader. A cooked payload is a pure function of its input: no padding, no host byte order, no
// `Name` index — a `Name` is a position in THIS process's intern table and means nothing in a file,
// so every name below is stored as its text and re-interned by whoever reads it.

/// A cursor over a payload that cannot run off the end.
///
/// A cooked clip carries three counted arrays and a table of counted strings, so every read is a
/// length the file supplied — which is precisely the shape that turns a truncated or hostile
/// payload into an out-of-bounds read. `ok_` latches false on the first overrun and every later
/// read answers zero, so the reader checks once at the end rather than after every field.
class Reader {
public:
    explicit Reader(Span<const u8> payload) noexcept : data_(payload) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }

    [[nodiscard]] u16 u16_value() noexcept {
        if (!take(2)) {
            return 0;
        }
        return static_cast<u16>(static_cast<u16>(data_[at_ - 2]) |
                                static_cast<u16>(static_cast<u16>(data_[at_ - 1]) << 8U));
    }

    [[nodiscard]] u32 u32_value() noexcept {
        if (!take(4)) {
            return 0;
        }
        u32 value = 0;
        for (u32 index = 0; index < 4; ++index) {
            value |= static_cast<u32>(data_[at_ - 4 + index]) << (index * 8U);
        }
        return value;
    }

    [[nodiscard]] f32 f32_value() noexcept {
        const u32 bits = u32_value();
        f32 value = 0.0F;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    [[nodiscard]] std::string text_value() noexcept {
        const u32 length = u32_value();
        if (!take(length)) {
            return {};
        }
        return std::string(reinterpret_cast<const char*>(data_.data() + at_ - length), length);
    }

    /// Refuse a count that could not possibly fit in what is left, BEFORE reserving for it. A
    /// four-byte payload claiming four billion tracks would otherwise ask for a gigabyte.
    [[nodiscard]] bool plausible(u32 count, usize bytes_each) noexcept {
        if (!ok_) {
            return false;
        }
        ok_ = static_cast<u64>(count) * bytes_each <= data_.size() - at_;
        return ok_;
    }

private:
    [[nodiscard]] bool take(usize bytes) noexcept {
        if (!ok_ || data_.size() - at_ < bytes) {
            ok_ = false;
            return false;
        }
        at_ += bytes;
        return true;
    }

    Span<const u8> data_;
    usize at_ = 0;
    bool ok_ = true;
};

/// The bytes one track's fixed part occupies, for `Reader::plausible`.
constexpr usize kTrackRecordBytes = (usize{6} * 4) + (usize{6} * 4);
/// A key is a time and four quantised components.
constexpr usize kKeyRecordBytes = 4 + (usize{4} * 2);

}  // namespace

Status read_cooked_clip(Span<const u8> payload, CookedClip& out) noexcept {
    Reader reader(payload);
    if (reader.u32_value() != kCookedClipVersion) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked clip record at a version this build does not read");
    }
    out.name = reader.text_value();
    out.duration = reader.f32_value();
    out.loop_mode = static_cast<u8>(reader.u32_value());
    out.sample_rate_hint = reader.f32_value();
    out.root_motion_joint = static_cast<u16>(reader.u32_value());

    const u32 joints = reader.u32_value();
    if (!reader.plausible(joints, 4)) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked clip record claims more joints than it "
                    "carries bytes for");
    }
    out.joint_names.clear();
    out.joint_names.reserve(joints);
    for (u32 index = 0; index < joints; ++index) {
        out.joint_names.push_back(reader.text_value());
    }

    const u32 tracks = reader.u32_value();
    if (!reader.plausible(tracks, kTrackRecordBytes)) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked clip record claims more tracks than it carries bytes for");
    }
    out.tracks.clear();
    out.tracks.reserve(tracks);
    for (u32 index = 0; index < tracks; ++index) {
        CookedClipTrack track;
        track.kind = static_cast<u8>(reader.u32_value());
        track.interpolation = static_cast<u8>(reader.u32_value());
        track.joint = static_cast<u16>(reader.u32_value());
        track.constant = reader.u32_value() != 0U;
        track.first_key = reader.u32_value();
        track.key_count = reader.u32_value();
        track.range_min = Vec3{reader.f32_value(), reader.f32_value(), reader.f32_value()};
        track.range_max = Vec3{reader.f32_value(), reader.f32_value(), reader.f32_value()};
        out.tracks.push_back(track);
    }

    const u32 keys = reader.u32_value();
    if (!reader.plausible(keys, kKeyRecordBytes)) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked clip record claims more keys than it carries bytes for");
    }
    out.keys.clear();
    out.keys.reserve(keys);
    for (u32 index = 0; index < keys; ++index) {
        CookedClipKey key;
        key.time = reader.f32_value();
        for (u16& component : key.components) {
            component = reader.u16_value();
        }
        out.keys.push_back(key);
    }

    if (!reader.ok()) {
        return fail(ErrorCode::InvalidArgument, "a cooked clip record ends before its contents do");
    }
    return ok();
}

}  // namespace cy::import

#if !defined(CY_IMPORT_ANIMATION)

namespace cy::import {

// `-D CY_ANIMATION=OFF` removed `src/animation/` from the build, which `animation-and-skinning`
// requires the runtime to be removable by. There is then no `Clip` to author and no codec to
// compress with — so step 8 is NOT REACHED, and the honest thing is to say so once, as information
// rather than as a warning about the file. `asset-import-pipeline` draws exactly that line: "A step
// skipped for that reason is not a warning about the file and SHALL NOT be reported as one."
Status import_fbx_animations(const ufbx_scene& scene, const FbxClipOptions& options,
                             Span<const std::string_view> joint_names, std::string_view source_path,
                             SubAssetNames& names, ImportResult& out,
                             FbxClipReport& report) noexcept {
    (void)joint_names;
    (void)names;
    report.stacks = static_cast<u32>(scene.anim_stacks.count);
    if (!options.import_animations || report.stacks == 0) {
        return ok();
    }
    return out.report(ImportSeverity::Info, "animation-runtime-absent",
                      "this build was configured with CY_ANIMATION=OFF, so there is no clip codec "
                      "to compress with and step 8 was not reached; the animation stacks in this "
                      "file were read by nothing",
                      source_path);
}

}  // namespace cy::import

#else

namespace cy::import {
namespace {

// --- The write side, and why it lives BEHIND the guard rather than beside the reader -------------
//
// `read_cooked_clip` is declared unconditionally, because a tool that inspects a package must be
// able to read a clip out of one whether or not THIS build can author a clip. Writing is the other
// way round: nothing without `cy::animation::Clip` can produce the quantised form these helpers
// serialise, so a build configured with `-D CY_ANIMATION=OFF` compiles them into a translation unit
// that never calls them — which `-Werror=unused-function` correctly refuses. Keeping them here is
// what makes this file's claim to compile in both configurations true rather than intended.

void put_u16(Array<u8>& out, u16 value) noexcept {
    for (u32 shift = 0; shift < 2; ++shift) {
        (void)out.push_back(static_cast<u8>((value >> (shift * 8U)) & 0xFFU));
    }
}

void put_u32(Array<u8>& out, u32 value) noexcept {
    for (u32 shift = 0; shift < 4; ++shift) {
        (void)out.push_back(static_cast<u8>((value >> (shift * 8U)) & 0xFFU));
    }
}

void put_f32(Array<u8>& out, f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    put_u32(out, bits);
}

void put_text(Array<u8>& out, std::string_view text) noexcept {
    put_u32(out, static_cast<u32>(text.size()));
    for (const char letter : text) {
        (void)out.push_back(static_cast<u8>(letter));
    }
}

[[nodiscard]] std::string_view view_of(const ufbx_string& text) noexcept {
    return {text.data, text.length};
}

/// The stem of a source path: the file's own name without its directory or its extension.
///
/// It is what an artist typed when they exported the file, and — see `fbx_clip.h` — it is the only
/// thing in a Mixamo export that distinguishes one animation from another, since every one of them
/// names its stack `mixamo.com`.
[[nodiscard]] std::string_view stem_of(std::string_view path) noexcept {
    const usize slash = path.find_last_of('/');
    const std::string_view file = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const usize dot = file.find_last_of('.');
    return dot == std::string_view::npos || dot == 0 ? file : file.substr(0, dot);
}

/// The joint table a clip's tracks are indexed by: one ufbx node per joint index, parent first.
struct JointTable {
    std::vector<const ufbx_node*> nodes;
    /// The joint's name as the COOKED RECORD carries it: step 7's when it supplied one, the source
    /// node's own otherwise. Not read from `nodes` at write time, because a supplied joint that
    /// this scene has no node for still occupies its index and still has to be named.
    std::vector<std::string_view> names;
    std::vector<u16> parents;
    /// `ufbx_node::typed_id` to joint index, or -1. Sized to the scene's node count, so a baked
    /// node's `typed_id` indexes it directly.
    std::vector<i32> index_of_node;
};

/// Walk the hierarchy depth first, parent before child, offering every node to `accept`.
///
/// THE ORDER IS THE ONE fbx.cpp's STEP 10 ALREADY USES, and that is the whole reason this is a
/// walk rather than a scan of `scene->nodes`. `Skeleton::add_joint` refuses a parent index that is
/// not strictly smaller than the child's, and ufbx documents no ordering for `scene->nodes` — it is
/// depth-ascending in every Mixamo file measured, which is a property of today's implementation
/// and not a promise. Walking the tree makes the guarantee ours.
template <typename Accept>
void walk_hierarchy(const ufbx_scene& scene, JointTable& table, Accept accept) {
    table.index_of_node.assign(scene.nodes.count, -1);
    std::vector<const ufbx_node*> stack_nodes;
    std::vector<u16> stack_parents;
    for (usize child = scene.root_node->children.count; child > 0; --child) {
        stack_nodes.push_back(scene.root_node->children.data[child - 1]);
        stack_parents.push_back(animation::kInvalidJoint);
    }
    while (!stack_nodes.empty()) {
        const ufbx_node* node = stack_nodes.back();
        const u16 parent = stack_parents.back();
        stack_nodes.pop_back();
        stack_parents.pop_back();
        if (node == nullptr) {
            continue;
        }

        u16 joint = parent;
        if (accept(*node) && table.nodes.size() < animation::kMaxJoints) {
            joint = static_cast<u16>(table.nodes.size());
            table.nodes.push_back(node);
            table.names.push_back(view_of(node->name));
            table.parents.push_back(parent);
            if (node->typed_id < table.index_of_node.size()) {
                table.index_of_node[node->typed_id] = static_cast<i32>(joint);
            }
        }

        for (usize child = node->children.count; child > 0; --child) {
            stack_nodes.push_back(node->children.data[child - 1]);
            // A node the table skipped passes its own parent down, so a bone under a non-bone
            // group is still parented to the bone above it rather than losing its chain.
            stack_parents.push_back(joint);
        }
    }
}

/// Build the joint table, either from the names step 7 numbered or by deriving one.
///
/// THE FALLBACK IS NOT A CONVENIENCE. The first derivation rule — a joint is a node carrying a bone
/// attribute — is the one every skeleton extractor uses, so a derived table and a supplied one
/// agree on a rig. But an FBX that animates plain nodes carries no bone attribute at all: a camera
/// move, a prop, a door, and every hand-written test document. Refusing to import that file's
/// animation because nothing in it declared itself a bone would be the same silent drop this file
/// exists to end, so the second rule takes every node.
void build_joint_table(const ufbx_scene& scene, Span<const std::string_view> supplied,
                       JointTable& table) {
    if (!supplied.empty()) {
        // Step 7's joints, resolved through the ONE identity the two steps share: the source node's
        // own name. The walk is the same depth-first one, so where a file names two nodes alike the
        // first in hierarchy order wins — deterministically, and the same way twice.
        JointTable every;
        walk_hierarchy(scene, every, [](const ufbx_node&) { return true; });

        table.index_of_node.assign(scene.nodes.count, -1);
        table.nodes.reserve(supplied.size());
        for (const std::string_view wanted : supplied) {
            const ufbx_node* found = nullptr;
            for (const ufbx_node* candidate : every.nodes) {
                if (found == nullptr && view_of(candidate->name) == wanted) {
                    found = candidate;
                }
            }
            // A name step 7 numbered that this scene has no node for cannot happen from the same
            // file, and a null entry would mis-align every joint index after it — so the slot is
            // kept and left empty rather than skipped.
            table.nodes.push_back(found);
            table.names.push_back(wanted);
            table.parents.push_back(animation::kInvalidJoint);
            if (found != nullptr && found->typed_id < table.index_of_node.size()) {
                table.index_of_node[found->typed_id] = static_cast<i32>(table.nodes.size() - 1);
            }
        }
        for (usize index = 0; index < table.nodes.size(); ++index) {
            const ufbx_node* node = table.nodes[index];
            const ufbx_node* parent = node != nullptr ? node->parent : nullptr;
            if (parent != nullptr && parent->typed_id < table.index_of_node.size()) {
                const i32 mapped = table.index_of_node[parent->typed_id];
                table.parents[index] =
                    mapped >= 0 ? static_cast<u16>(mapped) : animation::kInvalidJoint;
            }
        }
        return;
    }

    walk_hierarchy(scene, table, [](const ufbx_node& node) { return node.bone != nullptr; });
    if (!table.nodes.empty()) {
        return;
    }
    table.nodes.clear();
    table.names.clear();
    table.parents.clear();
    walk_hierarchy(scene, table, [](const ufbx_node&) { return true; });
}

/// The rotation that sends a Z-up frame to the engine's Y-up one. `fbx.cpp` states the argument;
/// this is the same rotation, applied in the same place — to the transforms of the scene root's own
/// children, so the hierarchy carries it down exactly once.
[[nodiscard]] Quat z_up_to_y_up() noexcept {
    return Quat::from_axis_angle(Vec3{1.0F, 0.0F, 0.0F}, -1.5707963268F);
}

[[nodiscard]] Vec3 to_vec3(const ufbx_vec3& value) noexcept {
    return Vec3{static_cast<f32>(value.x), static_cast<f32>(value.y), static_cast<f32>(value.z)};
}

[[nodiscard]] Quat to_quat(const ufbx_quat& value) noexcept {
    return Quat{static_cast<f32>(value.x), static_cast<f32>(value.y), static_cast<f32>(value.z),
                static_cast<f32>(value.w)};
}

/// Author one joint's three channels onto the clip.
///
/// THE ORDER IS FORCED BY THE CLIP, not chosen: `Clip::add_key` refuses any track but the
/// last-added one, because every track's keys live in one shared array. So a track is opened and
/// filled before the next is opened, which is why this takes one baked node at a time rather than
/// streaming ufbx's curves channel by channel.
[[nodiscard]] Status author_joint(const ufbx_baked_node& baked, u16 joint, bool root_level,
                                  const FbxClipOptions& options, animation::Clip& clip) noexcept {
    const Quat frame = z_up_to_y_up();
    const bool rotate = options.z_up_override && root_level;

    if (baked.translation_keys.count != 0) {
        Expected<u32, Error> track = clip.add_joint_track(animation::TrackKind::Translation, joint,
                                                          animation::Interpolation::Linear);
        if (!track) {
            return make_unexpected(track.error());
        }
        for (usize index = 0; index < baked.translation_keys.count; ++index) {
            const ufbx_baked_vec3& key = baked.translation_keys.data[index];
            Vec3 value = to_vec3(key.value) * options.scale;
            value = rotate ? frame * value : value;
            if (Status added = clip.add_key(track.value(), static_cast<f32>(key.time),
                                            Vec4{value.x, value.y, value.z, 0.0F});
                !added) {
                return added;
            }
        }
    }

    if (baked.rotation_keys.count != 0) {
        Expected<u32, Error> track = clip.add_joint_track(animation::TrackKind::Rotation, joint,
                                                          animation::Interpolation::Spherical);
        if (!track) {
            return make_unexpected(track.error());
        }
        for (usize index = 0; index < baked.rotation_keys.count; ++index) {
            const ufbx_baked_quat& key = baked.rotation_keys.data[index];
            Quat value = to_quat(key.value);
            value = rotate ? frame * value : value;
            if (Status added = clip.add_key(track.value(), static_cast<f32>(key.time),
                                            Vec4{value.x, value.y, value.z, value.w});
                !added) {
                return added;
            }
        }
    }

    if (baked.scale_keys.count != 0) {
        Expected<u32, Error> track = clip.add_joint_track(animation::TrackKind::Scale, joint,
                                                          animation::Interpolation::Linear);
        if (!track) {
            return make_unexpected(track.error());
        }
        for (usize index = 0; index < baked.scale_keys.count; ++index) {
            const ufbx_baked_vec3& key = baked.scale_keys.data[index];
            const Vec3 value = to_vec3(key.value);
            if (Status added = clip.add_key(track.value(), static_cast<f32>(key.time),
                                            Vec4{value.x, value.y, value.z, 0.0F});
                !added) {
                return added;
            }
        }
    }
    return ok();
}

/// The joint whose motion gameplay extracts, or `kInvalidJoint`.
///
/// The first root of the JOINT hierarchy — not of the node hierarchy — that the clip actually
/// translates. A Mixamo rig's `Hips` is exactly that, and a rig whose travel lives on a dedicated
/// root node is too. A joint whose translation track collapsed to a constant is not: a clip that
/// does not move its root has no root motion to extract, and designating one would make
/// `root_delta` report zero for ever rather than saying there is none.
[[nodiscard]] u16 root_motion_joint_of(const animation::Clip& clip,
                                       const JointTable& table) noexcept {
    for (usize joint = 0; joint < table.nodes.size(); ++joint) {
        if (table.parents[joint] != animation::kInvalidJoint) {
            continue;
        }
        const u32 track =
            clip.find_joint_track(animation::TrackKind::Translation, static_cast<u16>(joint));
        if (track != 0xFFFFFFFFU && !clip.tracks()[track].constant) {
            return static_cast<u16>(joint);
        }
    }
    return animation::kInvalidJoint;
}

/// Write the compressed clip as the cooked payload.
///
/// THE JOINT NAMES RIDE WITH IT and the reason is in `fbx_clip.h`: a track addresses a joint by
/// index, `AnimationRig::bind` checks only counts, and a clip bound to the wrong rig drives the
/// wrong bones in silence. The names are what a loader can check or rebind by.
[[nodiscard]] Status write_cooked_clip(const animation::Clip& clip, const JointTable& table,
                                       Array<u8>& out) noexcept {
    put_u32(out, kCookedClipVersion);
    put_text(out, clip.name().text());
    put_f32(out, clip.duration());
    put_u32(out, static_cast<u32>(clip.loop_mode()));
    put_f32(out, clip.sample_rate_hint());
    put_u32(out, clip.root_motion_joint());

    put_u32(out, static_cast<u32>(table.names.size()));
    for (const std::string_view joint : table.names) {
        put_text(out, joint);
    }

    put_u32(out, clip.track_count());
    for (const animation::TrackDesc& track : clip.tracks()) {
        put_u32(out, static_cast<u32>(track.kind));
        put_u32(out, static_cast<u32>(track.interpolation));
        put_u32(out, track.joint);
        put_u32(out, track.constant ? 1U : 0U);
        put_u32(out, track.first_key);
        put_u32(out, track.key_count);
        put_f32(out, track.range_min.x);
        put_f32(out, track.range_min.y);
        put_f32(out, track.range_min.z);
        put_f32(out, track.range_max.x);
        put_f32(out, track.range_max.y);
        put_f32(out, track.range_max.z);
    }

    put_u32(out, static_cast<u32>(clip.keys().size()));
    for (const animation::PackedKey& key : clip.keys()) {
        put_f32(out, key.time);
        for (const u16 component : key.c) {
            put_u16(out, component);
        }
    }
    return ok();
}

/// Everything one stack becomes, or the reason it became nothing.
struct StackOutcome {
    bool produced = false;
    /// Every track the codec collapsed to one key, which is a stack that animates nothing.
    bool constant = false;
    Array<u8> payload;
    std::string clip_name;
    animation::CompressionReport report;
};

[[nodiscard]] Status import_stack(const ufbx_scene& scene, const ufbx_anim_stack& stack,
                                  const JointTable& table, const FbxClipOptions& options,
                                  ufbx_baked_anim& bake, StackOutcome& outcome) noexcept {
    animation::Clip clip(current_allocator());
    outcome.clip_name = std::string(view_of(stack.name));
    clip.set_name(Name::intern(outcome.clip_name));
    clip.set_sample_rate_hint(options.sample_rate);
    // `playback_duration` is the stack's span, and `trim_start_time` has already moved the keys so
    // that the first sits at zero. A stack with no span still gets a positive duration, because a
    // clip whose duration is zero divides by it when a time is wrapped.
    const f64 duration = bake.playback_duration > 0.0 ? bake.playback_duration : bake.key_time_max;
    clip.set_duration(duration > 0.0 ? static_cast<f32>(duration) : 1.0F / options.sample_rate);

    for (usize joint = 0; joint < table.nodes.size(); ++joint) {
        const ufbx_node* node = table.nodes[joint];
        const ufbx_baked_node* baked =
            node != nullptr ? ufbx_find_baked_node_by_typed_id(&bake, node->typed_id) : nullptr;
        if (baked == nullptr) {
            continue;
        }
        const bool root_level = node->parent == scene.root_node;
        if (Status authored =
                author_joint(*baked, static_cast<u16>(joint), root_level, options, clip);
            !authored) {
            return authored;
        }
    }

    animation::CompressionSettings settings;
    settings.translation_tolerance_mm = options.translation_tolerance_mm;
    settings.rotation_tolerance_degrees = options.rotation_tolerance_degrees;
    // Scale and curve tolerances are NOT options. Nothing in an FBX bake produces a curve track,
    // and a scale track that a rig animates at all is animated far above a thousandth — so
    // exposing two more settings would be two more ways to change the cooked bytes for a
    // difference nobody can see. They are the codec's own defaults, stated here so that a reader
    // looking for them finds the decision rather than an omission.
    settings.scale_tolerance = 0.001F;
    settings.curve_tolerance = 0.001F;
    if (Status compressed = clip.compress(settings); !compressed) {
        return compressed;
    }
    outcome.report = clip.report();

    // A stack every one of whose tracks collapsed to a single key animates NOTHING within the
    // tolerances this import declared. That is how a Mixamo character export's `Take 001` — which
    // holds `filmboxTypeID` and `lockInfluenceWeights` and bakes to a flat pose — is told from the
    // `mixamo.com` stack beside it that holds the walk, without matching on either name. The caller
    // reports it; it is never dropped in silence.
    outcome.constant = outcome.report.tracks == outcome.report.constant_tracks;
    if (outcome.constant) {
        return ok();
    }

    if (options.root_motion == "root-joint") {
        clip.set_root_motion_joint(root_motion_joint_of(clip, table));
    }
    if (Status written = write_cooked_clip(clip, table, outcome.payload); !written) {
        return written;
    }
    outcome.produced = true;
    return ok();
}

}  // namespace

Status import_fbx_animations(const ufbx_scene& scene, const FbxClipOptions& options,
                             Span<const std::string_view> joint_names, std::string_view source_path,
                             SubAssetNames& names, ImportResult& out,
                             FbxClipReport& report) noexcept {
    report.stacks = static_cast<u32>(scene.anim_stacks.count);
    if (!options.import_animations || report.stacks == 0) {
        return ok();
    }

    JointTable table;
    build_joint_table(scene, joint_names, table);
    report.joints = static_cast<u32>(table.nodes.size());
    if (table.nodes.empty()) {
        return ok();
    }
    // `Skeleton::add_joint` refuses past 256 joints because the joint mask is a fixed `u64[4]`, so
    // a clip indexed against more of them could never be bound to the skeleton it came from.
    // Refusing loudly is the only honest answer: truncating would cook a clip whose tracks drive
    // the wrong bones, which no later stage could detect.
    if (table.nodes.size() >= animation::kMaxJoints) {
        report.too_many_joints = true;
        return out.report(ImportSeverity::Warning, "rig-too-large",
                          "this rig addresses more joints than a pose mask holds, so no animation "
                          "was imported from it",
                          source_path);
    }

    // ufbx's bake is deterministic given these settings, and every setting that moves a key is
    // either a declared option or a constant named right here. `trim_start_time` is the one that is
    // not negotiable: a clip's timeline starts at zero, and a stack exported over frames [30, 60]
    // must not cook to a clip whose first key sits at one second.
    ufbx_bake_opts bake_opts = {};
    bake_opts.trim_start_time = true;
    bake_opts.resample_rate = options.sample_rate;
    // A source already keyed at or above the requested rate is left alone rather than resampled a
    // second time — which is what every exporter that bakes its own output produces, Mixamo's
    // included.
    bake_opts.minimum_sample_rate = options.sample_rate;
    bake_opts.max_keyframe_segments = 32;
    bake_opts.key_reduction_enabled = options.key_reduction;
    // Rotations are stored hemisphere-aligned and sampled spherically by the clip codec, which is
    // the assumption this flag names.
    bake_opts.key_reduction_rotation = options.key_reduction;
    bake_opts.key_reduction_threshold = 1.0e-6;
    bake_opts.key_reduction_passes = 4;

    // EVERY STACK IS BAKED BEFORE ANY IS NAMED, because the naming rule depends on how many clips
    // the FILE yields: one clip takes the source's own stem, several take their stacks' names. See
    // `fbx_clip.h` for why a stack name alone will not do.
    std::vector<StackOutcome> outcomes;
    outcomes.reserve(scene.anim_stacks.count);
    for (usize index = 0; index < scene.anim_stacks.count; ++index) {
        const ufbx_anim_stack& stack = *scene.anim_stacks.data[index];
        ufbx_error error = {};
        ufbx_baked_anim* bake = ufbx_bake_anim(&scene, stack.anim, &bake_opts, &error);
        if (bake == nullptr) {
            char description[192] = {};
            (void)ufbx_format_error(description, sizeof(description), &error);
            if (Status reported = out.report(ImportSeverity::Warning, "unbakeable-animation",
                                             description, view_of(stack.name));
                !reported) {
                return reported;
            }
            continue;
        }

        // A node the bake animates that the joint table has no index for is animation this import
        // is about to DROP. Counting it here is what lets the caller name it; the number must never
        // be non-zero without a diagnostic.
        for (usize baked = 0; baked < bake->nodes.count; ++baked) {
            const u32 typed_id = bake->nodes.data[baked].typed_id;
            if (typed_id >= table.index_of_node.size() || table.index_of_node[typed_id] < 0) {
                ++report.unmapped_nodes;
            }
        }

        StackOutcome outcome;
        const Status imported = import_stack(scene, stack, table, options, *bake, outcome);
        ufbx_free_baked_anim(bake);
        if (!imported) {
            return imported;
        }
        outcomes.push_back(std::move(outcome));
    }

    usize produced = 0;
    for (const StackOutcome& outcome : outcomes) {
        produced += outcome.produced ? 1U : 0U;
    }

    const std::string_view stem = stem_of(source_path);
    for (usize index = 0; index < outcomes.size(); ++index) {
        StackOutcome& outcome = outcomes[index];
        if (outcome.constant) {
            ++report.constant_stacks;
            if (Status reported =
                    out.report(ImportSeverity::Info, "constant-animation-stack",
                               "every track of this animation stack holds one value for its whole "
                               "length, so it animates nothing and produced no clip",
                               outcome.clip_name);
                !reported) {
                return reported;
            }
            continue;
        }
        if (!outcome.produced) {
            continue;
        }

        const std::string_view derived = produced == 1 ? stem : std::string_view(outcome.clip_name);
        const std::string name = names.unique("animation/", derived, index);
        if (Status added =
                out.add(assets::AssetKind::Animation, name, std::move(outcome.payload), false);
            !added) {
            return added;
        }

        ++report.clips;
        report.tracks += outcome.report.tracks;
        report.keys_before += outcome.report.keys_before;
        report.keys_after += outcome.report.keys_after;
        report.worst_translation_mm =
            outcome.report.worst_translation_mm > report.worst_translation_mm
                ? outcome.report.worst_translation_mm
                : report.worst_translation_mm;
        report.worst_rotation_degrees =
            outcome.report.worst_rotation_degrees > report.worst_rotation_degrees
                ? outcome.report.worst_rotation_degrees
                : report.worst_rotation_degrees;
    }

    // "the achieved compression ratio and worst-case error SHALL be reported", and the numbers are
    // MEASURED by the codec against the authored keys rather than estimated from the settings.
    if (report.clips != 0) {
        char detail[ImportDiagnostic::kDetailCapacity] = {};
        (void)std::snprintf(detail, sizeof(detail),
                            "%u clip(s), %u tracks, %u keys from %u sampled; worst error %.3f mm "
                            "and %.3f degrees",
                            report.clips, report.tracks, report.keys_after, report.keys_before,
                            static_cast<f64>(report.worst_translation_mm),
                            static_cast<f64>(report.worst_rotation_degrees));
        if (Status reported =
                out.report(ImportSeverity::Info, "imported-animation", detail, source_path);
            !reported) {
            return reported;
        }
    }

    if (report.unmapped_nodes != 0) {
        char detail[ImportDiagnostic::kDetailCapacity] = {};
        (void)std::snprintf(detail, sizeof(detail),
                            "%u animated node(s) are outside this file's joint table, so their "
                            "motion was not imported",
                            report.unmapped_nodes);
        if (Status reported =
                out.report(ImportSeverity::Warning, "unmapped-animation", detail, source_path);
            !reported) {
            return reported;
        }
    }
    return ok();
}

}  // namespace cy::import

#endif  // CY_IMPORT_ANIMATION
