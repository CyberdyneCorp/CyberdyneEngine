#include "character.h"

#include <cy/animation/retarget.h>
#include <cy/animation/retarget_build.h>
#include <cy/core/assets/path.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/scalar.h>
#include <cy/core/math/transform.h>
#include <cy/import/animation_bridge.h>
#include <cy/import/fbx.h>
#include <cy/import/fbx_clip.h>
#include <cy/import/fbx_skeleton.h>
#include <cy/import/gltf.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string_view>

namespace cy::sample::character {
namespace {

using import::CookedClip;
using import::CookedClipKey;
using import::CookedClipTrack;
using import::ImportedSkeleton;

/// The codec's quantisation constant, from `src/animation/src/clip.cpp`.
///
/// DUPLICATED, and that is a cost worth naming rather than hiding behind a helper. `Clip` exposes
/// its stored keys — `keys()`'s own comment says a cooked clip IS those bytes — but exposes no way
/// to put them back, because the only thing that can produce the quantised form is `compress()`.
/// So a consumer that reads a cooked clip has to dequantise it and re-author, and the constant it
/// divides by has to match the one that multiplied. The alternative is a `Clip::adopt_packed()`
/// this artefact would be the first and only caller of, in a module it does not own.
constexpr f32 kQuantiseMax = 65535.0F;

[[nodiscard]] f32 dequantise(u16 value, f32 low, f32 high) noexcept {
    return low + ((static_cast<f32>(value) / kQuantiseMax) * (high - low));
}

/// One stored key, back as the value `add_key` takes. The inverse of `pack()` in clip.cpp, written
/// against the same two cases: a rotation over the fixed [-1, 1] component range, and everything
/// else over the track's own derived range.
[[nodiscard]] Vec4 unpack_key(const CookedClipTrack& track, const CookedClipKey& key) noexcept {
    if (static_cast<animation::TrackKind>(track.kind) == animation::TrackKind::Rotation) {
        const Quat rotation{
            dequantise(key.components[0], -1.0F, 1.0F), dequantise(key.components[1], -1.0F, 1.0F),
            dequantise(key.components[2], -1.0F, 1.0F), dequantise(key.components[3], -1.0F, 1.0F)};
        const f32 length_sq = length_squared(rotation);
        const Quat unit = length_sq <= math::kSmallLength ? Quat::identity() : normalize(rotation);
        return Vec4{unit.x, unit.y, unit.z, unit.w};
    }
    return Vec4{dequantise(key.components[0], track.range_min.x, track.range_max.x),
                dequantise(key.components[1], track.range_min.y, track.range_max.y),
                dequantise(key.components[2], track.range_min.z, track.range_max.z), 0.0F};
}

[[nodiscard]] Status read_file(const std::string& path, std::vector<u8>& out) noexcept {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return fail(ErrorCode::NotFound, "a source file could not be opened");
    }
    const std::streamoff size = file.tellg();
    if (size <= 0) {
        return fail(ErrorCode::InvalidArgument, "a source file is empty");
    }
    file.seekg(0);
    out.resize(static_cast<usize>(size));
    if (!file.read(reinterpret_cast<char*>(out.data()), size)) {
        return fail(ErrorCode::Internal, "a source file could not be read to its end");
    }
    return ok();
}

/// The sub-asset whose name begins with `prefix`, or null.
[[nodiscard]] const import::SubAsset* find_prefixed(const import::ImportResult& result,
                                                    std::string_view prefix) noexcept {
    for (const import::SubAsset& produced : result.assets()) {
        if (produced.view().starts_with(prefix)) {
            return &produced;
        }
    }
    return nullptr;
}

/// The full-detail mesh, which is the one sub-asset of kind `Mesh` whose name carries no `/lod`
/// suffix. `emit_mesh_with_lods` writes the source mesh first and then its reductions under
/// `<name>/lod<n>`, so a consumer that took the first `Mesh` would be right by accident and wrong
/// the day the order changed.
[[nodiscard]] const import::SubAsset* find_full_mesh(const import::ImportResult& result) noexcept {
    for (const import::SubAsset& produced : result.assets()) {
        const std::string_view name = produced.view();
        if (produced.kind == assets::AssetKind::Mesh &&
            name.find("/lod") == std::string_view::npos) {
            return &produced;
        }
    }
    return nullptr;
}

[[nodiscard]] Span<const u8> payload_of(const import::SubAsset& produced) noexcept {
    return {produced.payload.data(), produced.payload.size()};
}

/// Run the FBX importer over one file's bytes, with the schema's defaults.
///
/// THE DEFAULTS ARE THE POINT. Every option this import turns on — `import-animations`,
/// `animation-sample-rate`, the two tolerances — is the value a project gets without configuring
/// anything, so what this artefact demonstrates is the pipeline as it ships rather than the
/// pipeline as it can be tuned. `request.resolver` is null because the four sources reference
/// textures this artefact does not shade with; `fbx.cpp` skips the texture loop when there is no
/// resolver rather than dereferencing one.
[[nodiscard]] Status import_file(const std::string& path, std::string_view virtual_path,
                                 Span<const u8> bytes, import::ImportResult& out,
                                 SourceReport& report) noexcept {
    report.file = path;
    Expected<assets::VirtualPath, Error> source = assets::VirtualPath::normalise(virtual_path);
    if (!source) {
        return make_unexpected(source.error());
    }
    import::FbxImporter importer;
    import::ImportRequest request;
    request.source = *source;
    request.bytes = bytes;
    if (Status imported = importer.import(request, out); !imported) {
        return imported;
    }
    if (out.has_errors()) {
        return fail(ErrorCode::InvalidArgument, "the importer reported an error on a source file");
    }
    for (const import::SubAsset& produced : out.assets()) {
        switch (produced.kind) {
            case assets::AssetKind::Mesh:
                ++report.meshes;
                break;
            case assets::AssetKind::Material:
                ++report.materials;
                break;
            case assets::AssetKind::Animation:
                if (produced.view().starts_with(import::kSkeletonSubAssetPrefix)) {
                    ++report.skeletons;
                } else {
                    ++report.animations;
                }
                break;
            default:
                break;
        }
    }
    report.warnings = static_cast<u32>(out.warning_count());
    return ok();
}

/// Turn a cooked clip back into a runtime one, authored against `joint_count` joints.
///
/// The keys are dequantised and re-authored rather than adopted, for the reason `kQuantiseMax`'s
/// comment gives. What that costs is one extra pass of the codec: `compress()` runs on keys that
/// have already been fitted and quantised once, so the second fit keeps every key the first one did
/// (they already lie on their own straight segments) and the second quantisation lands on the same
/// grid. It is measured — `report.worst_rotation_degrees` is the SECOND pass's measurement — rather
/// than argued.
[[nodiscard]] Status build_clip(const CookedClip& cooked, u16 joint_count, Name name,
                                const animation::CompressionSettings& settings,
                                animation::Clip& out) noexcept {
    out.set_name(name);
    out.set_duration(cooked.duration);
    out.set_loop_mode(static_cast<animation::LoopMode>(cooked.loop_mode));
    out.set_sample_rate_hint(cooked.sample_rate_hint);
    if (cooked.root_motion_joint != 0xFFFFU) {
        out.set_root_motion_joint(cooked.root_motion_joint);
    }
    for (const CookedClipTrack& track : cooked.tracks) {
        // A curve or property track addresses no joint. The FBX importer produces neither, so this
        // is a refusal to invent one rather than a branch with a body.
        if (track.joint >= joint_count) {
            continue;
        }
        const Expected<u32, Error> index =
            out.add_joint_track(static_cast<animation::TrackKind>(track.kind), track.joint,
                                static_cast<animation::Interpolation>(track.interpolation));
        if (!index) {
            return make_unexpected(index.error());
        }
        // `add_key` refuses any track but the last one added, so a track's keys are all appended
        // before the next track is opened. That is why this is a nested loop and not two passes.
        for (u32 key = 0; key < track.key_count; ++key) {
            const usize at = static_cast<usize>(track.first_key) + key;
            if (at >= cooked.keys.size()) {
                return fail(ErrorCode::OutOfRange,
                            "a cooked clip track addresses a key it has not");
            }
            if (Status added =
                    out.add_key(*index, cooked.keys[at].time, unpack_key(track, cooked.keys[at]));
                !added) {
                return added;
            }
        }
    }
    return out.compress(settings);
}

/// Where a joint's bone segment runs: from the joint to each of its children, in model space.
///
/// A joint with no children — every `_End` terminator on a Mixamo rig, and the fingertips — has no
/// segment and is bound to as a POINT. That is the right answer for a terminator: it is a marker
/// for where the bone above it ends, not a bone.
struct BoneSegment {
    Vec3 from{0.0F, 0.0F, 0.0F};
    Vec3 to{0.0F, 0.0F, 0.0F};
    bool is_point = true;
};

/// The squared distance from `point` to the segment, and where along it the foot fell.
[[nodiscard]] f32 distance_squared_to(const BoneSegment& bone, Vec3 point) noexcept {
    const Vec3 to_point = point - bone.from;
    if (bone.is_point) {
        return length_squared(to_point);
    }
    const Vec3 axis = bone.to - bone.from;
    const f32 axis_length_sq = length_squared(axis);
    if (axis_length_sq <= 1e-12F) {
        return length_squared(to_point);
    }
    const f32 t = math::clamp(dot(to_point, axis) / axis_length_sq, 0.0F, 1.0F);
    const Vec3 foot = bone.from + (axis * t);
    return length_squared(point - foot);
}

/// One segment per joint, from the joint to the MEAN of its children.
///
/// The mean rather than one segment per child: a shoulder with three children would otherwise
/// attract three times the weight of a joint with one, and a T-posed character's arms would win
/// every contest against its chest. A joint with no children — every `_End` terminator on a Mixamo
/// rig, and every fingertip — stays a point, which is the right answer for a marker that says where
/// the bone above it ends.
void build_bone_segments(const animation::Skeleton& skeleton,
                         std::vector<BoneSegment>& out) noexcept {
    const u16 joints = skeleton.joint_count();
    const Span<const Transform> rest = skeleton.bind_model();
    std::vector<Vec3> child_sum(joints, Vec3{0.0F, 0.0F, 0.0F});
    std::vector<u32> child_count(joints, 0);
    for (u16 joint = 0; joint < joints; ++joint) {
        const u16 parent = skeleton.joints()[joint].parent;
        if (parent != animation::kInvalidJoint && parent < joints) {
            child_sum[parent] = child_sum[parent] + rest[joint].translation;
            ++child_count[parent];
        }
    }
    out.assign(joints, BoneSegment{});
    for (u16 joint = 0; joint < joints; ++joint) {
        out[joint].from = rest[joint].translation;
        out[joint].is_point = child_count[joint] == 0;
        if (!out[joint].is_point) {
            out[joint].to = child_sum[joint] * (1.0F / static_cast<f32>(child_count[joint]));
        }
    }
}

/// The four influences one vertex takes, before they are packed.
struct NearestBones {
    u16 joint[4] = {0, 0, 0, 0};
    f32 distance_sq[4] = {math::kInfinity, math::kInfinity, math::kInfinity, math::kInfinity};
};

/// The four nearest bones to `point`, kept by insertion because four is four: a heap for four
/// elements is longer to read and no faster.
[[nodiscard]] NearestBones nearest_bones(const std::vector<BoneSegment>& bones,
                                         Vec3 point) noexcept {
    NearestBones nearest;
    for (u16 joint = 0; joint < static_cast<u16>(bones.size()); ++joint) {
        const f32 distance_sq = distance_squared_to(bones[joint], point);
        for (u32 slot = 0; slot < 4U; ++slot) {
            if (distance_sq >= nearest.distance_sq[slot]) {
                continue;
            }
            for (u32 shift = 3; shift > slot; --shift) {
                nearest.distance_sq[shift] = nearest.distance_sq[shift - 1];
                nearest.joint[shift] = nearest.joint[shift - 1];
            }
            nearest.distance_sq[slot] = distance_sq;
            nearest.joint[slot] = joint;
            break;
        }
    }
    return nearest;
}

/// Turn four distances into four weight BYTES that sum to 255 exactly.
///
/// INVERSE DISTANCE TO THE FOURTH POWER, which is sharp enough that the nearest bone dominates and
/// soft enough that a joint bends rather than creases. A first power blends the whole skeleton into
/// every vertex and gives a character that moves like jelly; a very high power is rigid binding,
/// which creases visibly at every elbow. Four is the exponent this artefact's picture was judged
/// at, and it is a heuristic rather than a derivation.
///
/// The sum is 255 because `skin_dispatch.h` is explicit that the dispatch does NOT renormalise, and
/// that a stream which does not sum to 255 is a content defect it will faithfully reproduce as a
/// darker or brighter vertex. The rounding residue therefore goes to the nearest bone's lane rather
/// than being dropped.
void weight_bytes(const NearestBones& nearest, u8 (&out)[4]) noexcept {
    // A tenth of a millimetre: without it a vertex sitting exactly on a bone takes infinite weight
    // and the normalisation below divides infinity by infinity.
    constexpr f32 kEpsilon = 1.0e-4F;
    f32 weights[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    f32 total = 0.0F;
    for (u32 slot = 0; slot < 4U; ++slot) {
        if (nearest.distance_sq[slot] >= math::kInfinity) {
            continue;
        }
        const f32 distance = std::sqrt(nearest.distance_sq[slot]) + kEpsilon;
        const f32 squared = distance * distance;
        weights[slot] = 1.0F / (squared * squared);
        total += weights[slot];
    }
    if (total <= 0.0F) {
        weights[0] = 1.0F;
        total = 1.0F;
    }
    u32 assigned = 0;
    for (u32 slot = 0; slot < 4U; ++slot) {
        const auto quantised =
            static_cast<u32>((weights[slot] / total) * 255.0F);  // truncates, deliberately
        out[slot] = static_cast<u8>(quantised);
        assigned += quantised;
    }
    out[0] = static_cast<u8>(out[0] + (255U - assigned));
}

}  // namespace

Name motion_name(Motion motion) noexcept {
    switch (motion) {
        case Motion::Idle:
            return Name::intern("idle");
        case Motion::Walk:
            return Name::intern("walk");
        case Motion::Run:
            return Name::intern("run");
        case Motion::Die:
            return Name::intern("die");
        case Motion::Count:
            break;
    }
    return Name::intern("idle");
}

CharacterSources default_sources(std::string_view directory) noexcept {
    CharacterSources sources;
    sources.directory = directory;
    if (!sources.directory.empty() && sources.directory.back() != '/') {
        sources.directory += '/';
    }
    // The four Mixamo exports this artefact was written against, by the names Mixamo gives them.
    sources.files[static_cast<u32>(Motion::Idle)] = "Breathing Idle.fbx";
    sources.files[static_cast<u32>(Motion::Walk)] = "Walking.fbx";
    sources.files[static_cast<u32>(Motion::Run)] = "Running.fbx";
    sources.files[static_cast<u32>(Motion::Die)] = "Dying Backwards.fbx";
    sources.mesh_from = Motion::Walk;
    return sources;
}

Character::Character(Allocator& allocator_in) noexcept
    : allocator(&allocator_in),
      skeleton(allocator_in),
      positions(allocator_in),
      frames(allocator_in),
      indices(allocator_in),
      influences(allocator_in) {}

Status derive_influences(const animation::Skeleton& skeleton, Span<const Vec3> vertices,
                         Array<render::geometry::GpuSkinInfluence>& out,
                         SkinReport& report) noexcept {
    if (!skeleton.finalized()) {
        return fail(ErrorCode::InvalidArgument,
                    "a bind needs the skeleton's model-space rest pose, which finalize() computes");
    }
    if (Status sized = out.resize(vertices.size()); !sized) {
        return sized;
    }
    std::vector<BoneSegment> bones;
    build_bone_segments(skeleton, bones);

    std::vector<bool> used(skeleton.joint_count(), false);
    report.vertices = static_cast<u32>(vertices.size());
    report.worst_bind_distance = 0.0F;

    for (usize vertex = 0; vertex < vertices.size(); ++vertex) {
        const NearestBones nearest = nearest_bones(bones, vertices[vertex]);
        report.worst_bind_distance =
            math::max(report.worst_bind_distance, std::sqrt(nearest.distance_sq[0]));

        u8 indices[4] = {0, 0, 0, 0};
        u8 bytes[4] = {0, 0, 0, 0};
        weight_bytes(nearest, bytes);
        for (u32 slot = 0; slot < 4U; ++slot) {
            indices[slot] = static_cast<u8>(nearest.joint[slot]);
            if (bytes[slot] != 0) {
                used[nearest.joint[slot]] = true;
            }
        }
        out[vertex] = render::geometry::skin_influence(indices, bytes);
    }

    report.bones_used = 0;
    for (const bool bone_used : used) {
        report.bones_used += bone_used ? 1U : 0U;
    }
    return ok();
}

namespace {

/// Copy the cooked mesh into the character: positions, the normal-tangent frame and the indices.
///
/// The FRAME is uploaded because `SkinPass` is created with `with_frames` and would refuse an empty
/// span; NOTHING READS IT BACK — character.slang derives its normal from the screen-space
/// derivatives of the world position, for the reason that file's header gives. Writing the
/// importer's own tangent where there is one, and a placeholder where there is not, keeps that
/// honest rather than inventing a basis a consumer might trust.
[[nodiscard]] Status read_mesh(const import::ImportResult& result, Character& out) noexcept {
    const import::SubAsset* mesh_asset = find_full_mesh(result);
    if (mesh_asset == nullptr) {
        return fail(ErrorCode::NotFound, "the file the mesh was to come from produced none");
    }
    import::MeshData mesh;
    if (Status read = import::read_cooked_mesh(payload_of(*mesh_asset), mesh); !read) {
        return read;
    }
    if (mesh.normals.empty()) {
        return fail(ErrorCode::InvalidArgument, "the cooked mesh carries no normals");
    }
    if (Status sized = out.positions.resize(mesh.positions.size()); !sized) {
        return sized;
    }
    if (Status sized = out.frames.resize(mesh.positions.size()); !sized) {
        return sized;
    }
    if (Status sized = out.indices.resize(mesh.indices.size()); !sized) {
        return sized;
    }
    for (usize vertex = 0; vertex < mesh.positions.size(); ++vertex) {
        out.positions[vertex] = mesh.positions[vertex];
        const Vec3 normal = mesh.normals[vertex];
        const Vec3 tangent =
            vertex < mesh.tangents.size()
                ? Vec3{mesh.tangents[vertex].x, mesh.tangents[vertex].y, mesh.tangents[vertex].z}
                : Vec3{1.0F, 0.0F, 0.0F};
        const f32 sign = vertex < mesh.tangents.size() ? mesh.tangents[vertex].w : 1.0F;
        out.frames[vertex] = render::pack_normal_tangent(normal, tangent, sign);
    }
    for (usize index = 0; index < mesh.indices.size(); ++index) {
        out.indices[index] = mesh.indices[index];
    }
    return ok();
}

/// Import one source file and produce the skeleton and the clip it carries.
///
/// `mesh_out` is filled only for the file the mesh comes from; the other three carry no mesh at
/// all, which is what an animation-only export from a character library IS.
[[nodiscard]] Status load_one(const CharacterSources& sources, Motion motion,
                              animation::Skeleton& skeleton, animation::SkeletonProfile& humanoid,
                              animation::Clip& clip, Character* mesh_out,
                              SourceReport& report) noexcept {
    const std::string path = sources.directory + sources.files[static_cast<u32>(motion)];
    std::vector<u8> bytes;
    if (Status read = read_file(path, bytes); !read) {
        report.file = path;
        return read;
    }

    import::ImportResult result;
    const std::string virtual_path =
        std::string("models/") + sources.files[static_cast<u32>(motion)];
    if (Status imported = import_file(path, virtual_path,
                                      Span<const u8>(bytes.data(), bytes.size()), result, report);
        !imported) {
        return imported;
    }

    const import::SubAsset* skeleton_asset = find_prefixed(result, import::kSkeletonSubAssetPrefix);
    if (skeleton_asset == nullptr) {
        return fail(ErrorCode::NotFound,
                    "a source file produced no skeleton; step 7 imports one from every node that "
                    "carries a bone attribute, so a file without one is not a rig");
    }
    ImportedSkeleton record;
    if (Status read = import::read_cooked_skeleton(payload_of(*skeleton_asset), record); !read) {
        return read;
    }
    if (Status built = import::build_runtime_skeleton(record, skeleton, humanoid); !built) {
        return built;
    }
    skeleton.set_name(Name::intern(record.name));
    report.joints = skeleton.joint_count();
    report.humanoid_mapped = humanoid.mapped_count();

    const import::SubAsset* clip_asset = find_prefixed(result, "animation/");
    if (clip_asset == nullptr) {
        return fail(ErrorCode::NotFound,
                    "a source file produced no animation; a Mixamo export carries exactly one "
                    "stack that animates anything, and step 8 skips a stack whose every track the "
                    "codec collapsed to one key");
    }
    CookedClip cooked;
    if (Status read = import::read_cooked_clip(payload_of(*clip_asset), cooked); !read) {
        return read;
    }
    report.duration = cooked.duration;
    report.tracks = static_cast<u32>(cooked.tracks.size());
    report.keys = static_cast<u32>(cooked.keys.size());

    const animation::CompressionSettings settings;
    if (Status rebuilt =
            build_clip(cooked, skeleton.joint_count(), motion_name(motion), settings, clip);
        !rebuilt) {
        return rebuilt;
    }

    if (mesh_out == nullptr) {
        return ok();
    }
    return read_mesh(result, *mesh_out);
}

}  // namespace

Status load_character(Allocator& allocator, const CharacterSources& sources,
                      Character& out) noexcept {
    out.clips.reserve(kMotionCount);

    // --- The character's own rig and mesh, from the one file that has both.
    if (Status loaded = load_one(sources, sources.mesh_from, out.skeleton, out.humanoid,
                                 out.clips.emplace_back(allocator), &out,
                                 out.sources[static_cast<u32>(sources.mesh_from)]);
        !loaded) {
        return loaded;
    }
    out.sources[static_cast<u32>(sources.mesh_from)].final_tracks = out.clips[0].track_count();
    out.sources[static_cast<u32>(sources.mesh_from)].final_keys =
        static_cast<u32>(out.clips[0].keys().size());
    out.sources[static_cast<u32>(sources.mesh_from)].worst_rotation_degrees =
        out.clips[0].report().worst_rotation_degrees;

    // The clip for the file the rig came from needs no retarget: it is already indexed against
    // these joints. Moving it into its own slot is what makes `clips[motion]` the whole of the
    // lookup everything downstream does.
    std::vector<animation::Clip> ordered;
    ordered.reserve(kMotionCount);
    for (u32 motion = 0; motion < kMotionCount; ++motion) {
        ordered.emplace_back(allocator);
    }
    ordered[static_cast<u32>(sources.mesh_from)] = std::move(out.clips[0]);
    out.clips = std::move(ordered);

    // --- The three animation-only files, retargeted onto that rig.
    for (u32 index = 0; index < kMotionCount; ++index) {
        const auto motion = static_cast<Motion>(index);
        if (motion == sources.mesh_from) {
            continue;
        }
        SourceReport& report = out.sources[index];

        animation::Skeleton source_rig(allocator);
        animation::SkeletonProfile source_humanoid;
        animation::Clip source_clip(allocator);
        if (Status loaded = load_one(sources, motion, source_rig, source_humanoid, source_clip,
                                     nullptr, report);
            !loaded) {
            return loaded;
        }

        // MEASURED BEFORE IT IS RECONCILED. The four exports are the same 65-joint hierarchy and do
        // NOT share a rest pose — step 7 reads `ufbx_node::local_transform`, which is a bind pose
        // in a file that carries a skin and the node's authored default in a file that does not.
        // The retarget's rest reconciliation is exactly what absorbs that, and a reader should see
        // the number it absorbed rather than take the claim on faith.
        const animation::RigMatch match = animation::compare_rigs(source_rig, out.skeleton);
        report.rest_difference_degrees = match.worst_rest_rotation_degrees;
        report.rest_difference_metres = match.worst_rest_translation;
        report.retargeted = true;

        animation::RetargetProfile profile(allocator);
        animation::RetargetBuildReport retarget;
        if (Status built = animation::build_retarget_profile(
                source_rig, source_humanoid, out.skeleton, out.humanoid, profile, retarget);
            !built) {
            return built;
        }
        report.retarget_pairs = retarget.pairs;
        report.height_scale = profile.height_scale();

        // A DEFECT IN `bake_clip`, WORKED AROUND HERE AND REPORTED RATHER THAN HIDDEN.
        //
        // `bake_clip` resamples `floor(duration * rate) + 1` frames, so its LAST sample is taken at
        // exactly `clip.duration()`. `Clip::sample` wraps that time by the source clip's own loop
        // mode, and `wrap()` under `LoopMode::Loop` sends `duration` to ZERO — so the last key of
        // every baked clip is the source's FIRST frame. On a cyclic clip that is invisible, because
        // frame zero and frame N of a walk cycle are the same pose. On a death it is a character
        // that falls over for 4.6 seconds and then stands up in one frame, which is exactly what
        // this artefact's own `worst joint step between two frames` measurement caught: 2.4 metres,
        // at the frame where the clock reached the clip's duration.
        //
        // FBX carries no loop flag, so step 8 gives every imported clip the runtime's default —
        // `LoopMode::Loop` — and this is where that default meets a resampler that reads the end of
        // the timeline. Declaring the SOURCE non-looping for the duration of the bake is correct
        // whatever the clip is: a resampler asking for the pose at the end of a clip wants the last
        // frame, never the first. The fix belongs in `bake_clip`, in a module this artefact does
        // not own, and is reported as a finding.
        source_clip.set_loop_mode(animation::LoopMode::None);

        // Baked rather than retargeted per frame: `retarget.h` says to prefer the bake for the
        // combinations a game ships, and a demonstration that plays four fixed clips on one rig is
        // as shipped as a combination gets. The sample rate is the source clip's own hint, so a
        // 30 Hz Mixamo export is baked at 30 Hz and not resampled to a number this file invented.
        const animation::CompressionSettings settings;
        animation::Clip baked(allocator);
        if (Status done =
                animation::bake_clip(allocator, profile, source_rig, out.skeleton, source_clip,
                                     source_clip.sample_rate_hint(), settings, baked);
            !done) {
            return done;
        }
        baked.set_name(motion_name(motion));
        baked.set_duration(source_clip.duration());
        // A DEATH DOES NOT LOOP, and `build_locomotion_graph` refuses a spec that says it does. FBX
        // carries no loop flag, so the runtime's own default — Loop — is what every imported clip
        // arrives with; deciding otherwise is this artefact's call and is made here, once.
        baked.set_loop_mode(motion == Motion::Die ? animation::LoopMode::None
                                                  : animation::LoopMode::Loop);
        report.final_tracks = baked.track_count();
        report.final_keys = static_cast<u32>(baked.keys().size());
        report.worst_rotation_degrees = baked.report().worst_rotation_degrees;
        out.clips[index] = std::move(baked);
    }

    // --- The skin. See the header comment: the weights are derived, not imported.
    if (Status bound =
            derive_influences(out.skeleton, out.positions.span(), out.influences, out.skin);
        !bound) {
        return bound;
    }
    out.skin.triangles = static_cast<u32>(out.indices.size() / 3U);

    Vec3 mesh_min{math::kInfinity, math::kInfinity, math::kInfinity};
    Vec3 mesh_max{-math::kInfinity, -math::kInfinity, -math::kInfinity};
    for (const Vec3& position : out.positions.span()) {
        mesh_min = cwise_min(mesh_min, position);
        mesh_max = cwise_max(mesh_max, position);
    }
    Vec3 rig_min{math::kInfinity, math::kInfinity, math::kInfinity};
    Vec3 rig_max{-math::kInfinity, -math::kInfinity, -math::kInfinity};
    for (const Transform& joint : out.skeleton.bind_model()) {
        rig_min = cwise_min(rig_min, joint.translation);
        rig_max = cwise_max(rig_max, joint.translation);
    }
    out.skin.mesh_min = mesh_min;
    out.skin.mesh_max = mesh_max;
    out.skin.rig_min = rig_min;
    out.skin.rig_max = rig_max;
    return ok();
}

}  // namespace cy::sample::character
