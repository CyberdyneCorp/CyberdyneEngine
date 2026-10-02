#include "character.h"

#include <cy/animation/cooked.h>
#include <cy/core/assets/path.h>
#include <cy/core/math/quat.h>
#include <cy/core/math/scalar.h>
#include <cy/core/math/transform.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/import/fbx.h>
#include <cy/import/fbx_skeleton.h>
#include <cy/import/gltf.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string_view>

namespace cy::sample::character {
namespace {

using import::ImportResult;
using import::SubAsset;

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

const char* motion_name(Motion motion) noexcept {
    switch (motion) {
        case Motion::Idle:
            return "idle";
        case Motion::Walk:
            return "walk";
        case Motion::Run:
            return "run";
        case Motion::Die:
            return "die";
        case Motion::Count:
            break;
    }
    return "idle";
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

Status convert_influences(Span<const import::SkinInfluence> influences, u32 joint_count,
                          Array<render::geometry::GpuSkinInfluence>& out,
                          SkinReport& report) noexcept {
    if (Status sized = out.resize(influences.size()); !sized) {
        return sized;
    }
    std::vector<bool> used(joint_count, false);
    report.vertices = static_cast<u32>(influences.size());
    report.imported = true;
    report.worst_bind_distance = 0.0F;

    for (usize vertex = 0; vertex < influences.size(); ++vertex) {
        const import::SkinInfluence& influence = influences[vertex];
        u8 indices[4] = {0, 0, 0, 0};
        u8 bytes[4] = {0, 0, 0, 0};
        // Largest remainder, so the four bytes sum to 255 exactly. Four independent roundings of a
        // partition of one sum to 254 or 256 about half the time, and `skin_dispatch.h` calls a
        // stream that does not sum to 255 a content defect the dispatch faithfully reproduces.
        f32 remainder[4] = {0.0F, 0.0F, 0.0F, 0.0F};
        u32 assigned = 0;
        for (u32 lane = 0; lane < 4U; ++lane) {
            if (influence.joints[lane] > 0xFFU) {
                return fail(ErrorCode::OutOfRange,
                            "a joint index past 255, which VertexStream::Skin's one-byte bone "
                            "index cannot address");
            }
            indices[lane] = static_cast<u8>(influence.joints[lane]);
            const f32 scaled = influence.weights[lane] * 255.0F;
            const f32 floored = std::floor(scaled);
            bytes[lane] = static_cast<u8>(floored);
            remainder[lane] = scaled - floored;
            assigned += bytes[lane];
        }
        while (assigned < 255U) {
            u32 best = 0;
            for (u32 lane = 1; lane < 4U; ++lane) {
                best = remainder[lane] > remainder[best] ? lane : best;
            }
            if (bytes[best] == 0xFFU) {
                break;
            }
            ++bytes[best];
            ++assigned;
            remainder[best] = -1.0F;
        }
        for (u32 lane = 0; lane < 4U; ++lane) {
            if (bytes[lane] != 0 && influence.joints[lane] < joint_count) {
                used[influence.joints[lane]] = true;
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
    // The artist's bindings, when the cooked mesh has them. Kept rather than converted here
    // because the conversion needs the skeleton's joint count, and the skeleton is built later.
    if (mesh.skin.size() == mesh.positions.size()) {
        if (Status sized = out.imported_skin.resize(mesh.skin.size()); !sized) {
            return sized;
        }
        for (usize vertex = 0; vertex < mesh.skin.size(); ++vertex) {
            out.imported_skin[vertex] = mesh.skin[vertex];
        }
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

/// Import one source file, and count what it produced.
[[nodiscard]] Status import_one(const CharacterSources& sources, Motion motion,
                                ImportResult& result, SourceReport& report) noexcept {
    const std::string path = sources.directory + sources.files[static_cast<u32>(motion)];
    std::vector<u8> bytes;
    if (Status read = read_file(path, bytes); !read) {
        report.file = path;
        return read;
    }
    const std::string virtual_path =
        std::string("models/") + sources.files[static_cast<u32>(motion)];
    return import_file(path, virtual_path, Span<const u8>(bytes.data(), bytes.size()), result,
                       report);
}

/// The source's own rig, as step 7 recorded it: what the report prints beside the clip.
[[nodiscard]] Status report_rig(const ImportResult& result, SourceReport& report) noexcept {
    const SubAsset* skeleton_asset = find_prefixed(result, import::kSkeletonSubAssetPrefix);
    if (skeleton_asset == nullptr) {
        return fail(ErrorCode::NotFound,
                    "a source file produced no skeleton; step 7 imports one from every node that "
                    "carries a bone attribute, so a file without one is not a rig");
    }
    animation::Skeleton rig(system_allocator(MemoryDomain::Animation));
    animation::SkeletonProfile humanoid;
    if (Status read = animation::decode_skeleton(payload_of(*skeleton_asset), rig, humanoid);
        !read) {
        return read;
    }
    report.joints = rig.joint_count();
    report.humanoid_mapped = humanoid.mapped_count();
    return ok();
}

}  // namespace

Status load_character(Allocator& allocator, const CharacterSources& sources,
                      Character& out) noexcept {
    (void)allocator;
    ImportResult imports[kMotionCount];
    for (u32 index = 0; index < kMotionCount; ++index) {
        const auto motion = static_cast<Motion>(index);
        if (Status imported = import_one(sources, motion, imports[index], out.sources[index]);
            !imported) {
            return imported;
        }
        if (Status reported = report_rig(imports[index], out.sources[index]); !reported) {
            return reported;
        }
    }
    if (Status read = read_mesh(imports[static_cast<u32>(sources.mesh_from)], out); !read) {
        return read;
    }

    // THE COOK. The character's skeleton is the mesh file's; the other three clips are retargeted
    // onto it and baked, because their rest poses differ from it by up to 29.89 degrees; the death
    // holds its last frame, which FBX has no flag to say; and the machine is compiled. The same
    // function is the `animation` build-graph producer's work.
    import::AnimationCookSpec spec;
    spec.rig = &imports[static_cast<u32>(sources.mesh_from)];
    for (u32 index = 0; index < kMotionCount; ++index) {
        const auto motion = static_cast<Motion>(index);
        spec.clips.push_back(import::AnimationClipSource{&imports[index], motion_name(motion),
                                                         motion != Motion::Die});
    }
    if (Status cooked = import::cook_locomotion_set(spec, out.cooked); !cooked) {
        return cooked;
    }
    for (u32 index = 0; index < kMotionCount; ++index) {
        const import::CookedAnimationClip& clip = out.cooked.clips[index];
        SourceReport& report = out.sources[index];
        report.duration = clip.duration;
        report.tracks = clip.source_tracks;
        report.keys = clip.source_keys;
        report.retargeted = clip.retargeted;
        report.rest_difference_degrees = clip.rest_difference_degrees;
        report.rest_difference_metres = clip.rest_difference_metres;
        report.retarget_pairs = clip.retarget_pairs;
        report.height_scale = clip.height_scale;
        report.final_tracks = clip.tracks;
        report.final_keys = clip.keys;
        report.worst_rotation_degrees = clip.worst_rotation_degrees;
    }
    if (Status decoded =
            animation::decode_skeleton(out.cooked.skeleton.span(), out.skeleton, out.humanoid);
        !decoded) {
        return decoded;
    }

    // --- The skin. The artist's weights when the cooked mesh carries them — which it has since
    // M11.b taught both model importers to fill `MeshData::skin` — and the derived bind when it
    // does not, which is what an older cache entry is. Never in silence either way: `imported`
    // reaches the printed report.
    if (!out.imported_skin.empty()) {
        if (Status bound = convert_influences(out.imported_skin.span(), out.skeleton.joint_count(),
                                              out.influences, out.skin);
            !bound) {
            return bound;
        }
    } else if (Status bound =
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
