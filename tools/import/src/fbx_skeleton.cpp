#include <cy/import/fbx_skeleton.h>

#include <ufbx.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <utility>
#include <vector>

namespace cy::import {
namespace {

// --- The byte writer and reader the record shares ------------------------------------------------
//
// Spelled here rather than shared with `gltf.cpp` for the reason that file's own copy exists: four
// lines of shifting is not a dependency worth taking between two payload formats that may version
// independently.

void put_u32(Array<u8>& out, u32 value) noexcept {
    for (u32 index = 0; index < 4; ++index) {
        (void)out.push_back(static_cast<u8>((value >> (index * 8U)) & 0xFFU));
    }
}

void put_f32(Array<u8>& out, f32 value) noexcept {
    u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    put_u32(out, bits);
}

[[nodiscard]] u32 get_u32(const u8* data) noexcept {
    return static_cast<u32>(data[0]) | (static_cast<u32>(data[1]) << 8U) |
           (static_cast<u32>(data[2]) << 16U) | (static_cast<u32>(data[3]) << 24U);
}

[[nodiscard]] f32 get_f32(const u8* data) noexcept {
    const u32 bits = get_u32(data);
    f32 value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

[[nodiscard]] std::string_view view_of(const ufbx_string& text) noexcept {
    return {text.data, text.length};
}

// --- Names ---------------------------------------------------------------------------------------

/// The part of a joint name after the last ':'.
///
/// FBX namespaces a node as `namespace:name`, and Mixamo uses `mixamorig:` or `mixamorig1:` — the
/// digit appears the moment a rig is re-exported. Matching on the suffix is what makes one table
/// serve both, and it is also what makes a rig namespaced by a studio's own prefix map.
[[nodiscard]] std::string_view strip_namespace(std::string_view name) noexcept {
    const usize colon = name.rfind(':');
    return colon == std::string_view::npos ? name : name.substr(colon + 1);
}

[[nodiscard]] bool ends_with_text(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

[[nodiscard]] bool contains_text(std::string_view text, std::string_view needle) noexcept {
    return text.find(needle) != std::string_view::npos;
}

// --- The humanoid table --------------------------------------------------------------------------
//
// "WHEN a skeleton is assigned a humanoid profile THEN engine systems SHALL address joints by
// standard names regardless of the source rig's naming."
//
// TWO VOCABULARIES, AND BOTH ARE NAMED. The first is Mixamo's, because that is what the files this
// step was written against use and because Mixamo's naming is FIXED — it is generated, not
// authored, so a table is a complete description of it rather than a guess. The second is the
// engine's own spelling from `HumanoidJoint`, so a rig authored against this engine's vocabulary
// maps without a translation nobody would think to write.
//
// WHAT IS DELIBERATELY NOT HERE: fuzzy matching, case folding, and the dozen other studio
// conventions (`Bip01 L UpperArm`, `joint_L_arm`, `DEF-upper_arm.L`). A mapping that is nearly
// right is worse than one that is absent, because an unmapped joint is visible and a misplaced
// elbow is not. `asset-import-pipeline` says the profile is remapped "if configured" — a configured
// mapping, authored per project, is what the general case wants, and this table is what the
// generated case gets for free.

struct HumanoidName {
    std::string_view name;
    u16 standard;
};

// clang-format off
constexpr HumanoidName kHumanoidNames[] = {
    // Mixamo, `mixamorig[N]:` stripped. Spine1 is the chest: the engine's profile has Spine and
    // Chest and no upper chest, so Mixamo's third spine joint stays unmapped rather than being
    // forced into a slot that means something else.
    {"Hips", 1},           {"Spine", 2},          {"Spine1", 3},         {"Neck", 4},
    {"Head", 5},           {"LeftShoulder", 6},   {"LeftArm", 7},        {"LeftForeArm", 8},
    {"LeftHand", 9},       {"RightShoulder", 10}, {"RightArm", 11},      {"RightForeArm", 12},
    {"RightHand", 13},     {"LeftUpLeg", 14},     {"LeftLeg", 15},       {"LeftFoot", 16},
    {"LeftToeBase", 17},   {"RightUpLeg", 18},    {"RightLeg", 19},      {"RightFoot", 20},
    {"RightToeBase", 21},
    // The engine's own spelling, from `HumanoidJoint`. `Hips`, `Spine`, `Neck`, `Head`,
    // `LeftShoulder`, `LeftHand`, `RightShoulder`, `RightHand` and `LeftFoot`/`RightFoot` are
    // spelled identically in both vocabularies and appear once, above.
    {"Root", 0},           {"Chest", 3},          {"LeftUpperArm", 7},   {"LeftLowerArm", 8},
    {"RightUpperArm", 11}, {"RightLowerArm", 12}, {"LeftUpperLeg", 14},  {"LeftLowerLeg", 15},
    {"LeftToes", 17},      {"RightUpperLeg", 18}, {"RightLowerLeg", 19}, {"RightToes", 21},
};
// clang-format on

// --- The bone level-of-detail heuristic
// ------------------------------------------------------------
//
// "nested subsets of joints, ordered so that detail joints — facial, finger, twist, and accessory
// chains — are dropped first."
//
// ONE TIER, AND THE REASON IS THAT A SECOND ONE WOULD BE INVENTED. What a rig can lose first is a
// property of the rig, and the only part of it a NAME can tell you with certainty is the part that
// deforms nothing measurable at distance: a finger, a twist joint, and a chain terminator that
// exists to give its parent a direction. Dropping a head, a toe or a clavicle at some level is a
// judgement about a particular game's camera distances, and an importer that made it silently would
// be authoring level-of-detail policy from a file name.
//
// So: fingers, twist joints and `_End` terminators leave at level 1, and everything else survives
// every level. A project that wants more says so per joint, which is what `dropped_at` is for and
// what the `.meta`'s per-node options are for.
[[nodiscard]] u8 bone_lod_of(std::string_view stripped) noexcept {
    constexpr std::string_view kFingers[] = {"Thumb", "Index", "Middle", "Ring", "Pinky"};
    if (contains_text(stripped, "Hand")) {
        for (const std::string_view finger : kFingers) {
            if (contains_text(stripped, finger)) {
                return 1;
            }
        }
    }
    if (contains_text(stripped, "Twist") || contains_text(stripped, "twist")) {
        return 1;
    }
    // A terminator: Mixamo's `HeadTop_End` and `LeftToe_End` are the direction of the joint above
    // them and influence no vertex.
    if (ends_with_text(stripped, "_End")) {
        return 1;
    }
    return kBoneLodLevels;
}

// --- The joint set -------------------------------------------------------------------------------

/// Which ufbx nodes are joints.
///
/// A bone attribute is the direct answer, and the skin's own clusters are the authoritative one: a
/// node a skin deforms through IS a joint whether or not the exporter gave it a bone attribute.
/// Ancestors are then added, because an ancestor's transform is part of every descendant's bind
/// pose — a skeleton that skipped the node its root hangs from would place the whole rig somewhere
/// else. The scene root is excluded: it is ufbx's own node, not the file's, and `fbx.cpp`'s step-10
/// walk excludes it for the same reason.
void mark_joints(const ufbx_scene& scene, std::vector<bool>& marked) noexcept {
    for (usize index = 0; index < scene.nodes.count; ++index) {
        const ufbx_node* node = scene.nodes.data[index];
        if (node != nullptr && !node->is_root && node->bone != nullptr) {
            marked[node->typed_id] = true;
        }
    }
    for (usize index = 0; index < scene.skin_clusters.count; ++index) {
        const ufbx_skin_cluster* cluster = scene.skin_clusters.data[index];
        if (cluster != nullptr && cluster->bone_node != nullptr && !cluster->bone_node->is_root) {
            marked[cluster->bone_node->typed_id] = true;
        }
    }
    // The ancestors, in a second pass so the first one does not walk a chain it is still growing.
    for (usize index = 0; index < scene.nodes.count; ++index) {
        const ufbx_node* node = scene.nodes.data[index];
        if (node == nullptr || !marked[node->typed_id]) {
            continue;
        }
        for (const ufbx_node* parent = node->parent;
             parent != nullptr && !parent->is_root && !marked[parent->typed_id];
             parent = parent->parent) {
            marked[parent->typed_id] = true;
        }
    }
}

/// The rotation that sends a Z-up frame to the engine's Y-up one. `fbx.cpp`'s `z_up_to_y_up`, and
/// it is applied in the same place for the same reason: on the roots, once, so the hierarchy
/// carries it down rather than every joint carrying its own copy of a frame change.
[[nodiscard]] Quat z_up_to_y_up() noexcept {
    return Quat::from_axis_angle(Vec3{1.0f, 0.0f, 0.0f}, -1.5707963268f);
}

[[nodiscard]] Transform bind_local_of(const ufbx_node& node, const SkeletonImportOptions& options,
                                      bool is_root) noexcept {
    const ufbx_transform& local = node.local_transform;
    Transform bind;
    bind.translation =
        Vec3{static_cast<f32>(local.translation.x), static_cast<f32>(local.translation.y),
             static_cast<f32>(local.translation.z)} *
        options.scale;
    bind.rotation = Quat{static_cast<f32>(local.rotation.x), static_cast<f32>(local.rotation.y),
                         static_cast<f32>(local.rotation.z), static_cast<f32>(local.rotation.w)};
    bind.scale = Vec3{static_cast<f32>(local.scale.x), static_cast<f32>(local.scale.y),
                      static_cast<f32>(local.scale.z)};
    if (is_root && options.up_override == "z-up") {
        const Quat rotation = z_up_to_y_up();
        bind.translation = rotation * bind.translation;
        bind.rotation = rotation * bind.rotation;
    }
    return bind;
}

/// The depth-first walk, from the scene root's children, that puts a parent before its children.
///
/// `scene->nodes` is empirically in this order in every file measured, and ufbx documents nowhere
/// that it must be. `Skeleton::add_joint` REFUSES a parent index that is not already present, so
/// resting on the undocumented order would turn a ufbx version bump into a rig that will not load.
void walk_joints(const ufbx_scene& scene, const std::vector<bool>& marked,
                 const SkeletonImportOptions& options, ImportedSkeleton& out) {
    std::vector<i32> joint_of_node(scene.nodes.count, -1);
    std::vector<const ufbx_node*> stack;
    for (usize child = scene.root_node->children.count; child > 0; --child) {
        stack.push_back(scene.root_node->children.data[child - 1]);
    }
    while (!stack.empty()) {
        const ufbx_node* node = stack.back();
        stack.pop_back();
        if (node == nullptr) {
            continue;
        }
        if (marked[node->typed_id]) {
            const ufbx_node* parent = node->parent;
            const i32 parent_joint =
                parent != nullptr && !parent->is_root ? joint_of_node[parent->typed_id] : -1;

            ImportedJoint joint;
            joint.name = std::string(view_of(node->name));
            joint.parent = parent_joint;
            joint.bind_local = bind_local_of(*node, options, parent_joint < 0);
            joint.dropped_at = bone_lod_of(strip_namespace(joint.name));

            const auto index = static_cast<i32>(out.joints.size());
            // FIRST IN WALK ORDER WINS. A rig with two nodes named `LeftHand` has one of them
            // playing the part, and which one is decided by the file's own hierarchy rather than by
            // whichever the loop happened to reach last — so two imports of one file agree.
            const u16 standard = humanoid_joint_of(joint.name);
            if (standard != kUnmappedHumanoidJoint && out.humanoid.resolve(standard) < 0) {
                out.humanoid.map(standard, index);
            }
            joint_of_node[node->typed_id] = index;
            out.joints.push_back(std::move(joint));
        }
        for (usize child = node->children.count; child > 0; --child) {
            stack.push_back(node->children.data[child - 1]);
        }
    }
}

/// Make the bone levels nested subsets, which `Skeleton::finalize()` refuses a skeleton for not
/// being. One forward pass suffices precisely because the joints are parent-before-child.
void clamp_bone_levels(ImportedSkeleton& skeleton) noexcept {
    for (ImportedJoint& joint : skeleton.joints) {
        if (joint.parent < 0) {
            continue;
        }
        const u8 parent_level = skeleton.joints[static_cast<usize>(joint.parent)].dropped_at;
        joint.dropped_at = std::min(joint.dropped_at, parent_level);
    }
}

/// Claim the `Root` slot for the skeleton's own root joint when no node is named for it.
///
/// `RetargetProfile::build` derives its height scale from the ROOT chain's entries and leaves it at
/// 1.0 when that chain is empty, so a humanoid profile with no root is a retarget that silently
/// does not rescale. A Mixamo rig has no separate root node — the hips ARE the root — and `Root`
/// and `Hips` resolving to one index is exactly what that rig is.
void map_root_slot(ImportedSkeleton& skeleton) noexcept {
    if (skeleton.humanoid.resolve(0) >= 0 || skeleton.joints.empty()) {
        return;
    }
    // The walk emits a root first, so joint 0 is one by construction; the loop says so rather than
    // assuming it.
    for (usize index = 0; index < skeleton.joints.size(); ++index) {
        if (skeleton.joints[index].parent < 0) {
            skeleton.humanoid.map(0, static_cast<i32>(index));
            return;
        }
    }
}

/// The sub-asset stem: the root joint's name with its namespace stripped.
///
/// Derived from something an ARTIST controls rather than from the file's ordering, which is what
/// `importer.h` requires of a sub-asset name — it is what an `AssetId` is bound to across
/// re-imports. The namespace goes because it is the half an exporter rewrites: the same rig comes
/// back as `mixamorig:Hips` from one export and `mixamorig1:Hips` from the next, and a name that
/// moved between them would re-mint the id and orphan every reference.
[[nodiscard]] std::string stem_of(const ImportedSkeleton& skeleton) {
    for (const ImportedJoint& joint : skeleton.joints) {
        if (joint.parent < 0) {
            return std::string(strip_namespace(joint.name));
        }
    }
    return {};
}

}  // namespace

// --- ImportedSkeleton ----------------------------------------------------------------------------

i32 ImportedSkeleton::find(std::string_view joint_name) const noexcept {
    for (usize index = 0; index < joints.size(); ++index) {
        if (joints[index].name == joint_name) {
            return static_cast<i32>(index);
        }
    }
    return -1;
}

u16 humanoid_joint_of(std::string_view joint_name) noexcept {
    const std::string_view stripped = strip_namespace(joint_name);
    for (const HumanoidName& entry : kHumanoidNames) {
        if (entry.name == stripped) {
            return entry.standard;
        }
    }
    return kUnmappedHumanoidJoint;
}

// --- The cooked skeleton payload -----------------------------------------------------------------

Status write_cooked_skeleton(const ImportedSkeleton& skeleton, Array<u8>& out) noexcept {
    if (skeleton.joints.empty()) {
        return fail(ErrorCode::InvalidArgument, "a skeleton with no joints deforms nothing");
    }
    if (skeleton.joints.size() > kMaxSkeletonJoints) {
        return fail(ErrorCode::OutOfRange,
                    "a skeleton may not carry more joints than a pose mask addresses");
    }
    if (skeleton.name.size() > 0xFFFFU) {
        return fail(ErrorCode::OutOfRange, "a skeleton name longer than the record allows");
    }

    put_u32(out, kCookedSkeletonVersion);
    put_u32(out, static_cast<u32>(skeleton.joints.size()));
    put_u32(out, static_cast<u32>(skeleton.name.size()));
    if (Status appended = out.append(Span<const u8>(
            reinterpret_cast<const u8*>(skeleton.name.data()), skeleton.name.size()));
        !appended) {
        return appended;
    }

    for (usize index = 0; index < skeleton.joints.size(); ++index) {
        const ImportedJoint& joint = skeleton.joints[index];
        if (joint.name.size() > 0xFFFFU) {
            return fail(ErrorCode::OutOfRange, "a joint name longer than the record allows");
        }
        // `std::cmp_greater_equal` rather than a cast: the joint index is signed because -1 is
        // the root, the loop counter is not, and a cast is how a sign-comparison bug is usually
        // written down rather than avoided.
        if (std::cmp_greater_equal(joint.parent, index)) {
            // The invariant is checked HERE rather than only where the record is read, so a bug in
            // the walk cannot reach a package and fail at load time in somebody else's build.
            return fail(ErrorCode::InvalidArgument,
                        "a joint's parent must precede it, so the array is ordered parent before "
                        "child");
        }
        put_u32(out, static_cast<u32>(joint.name.size()));
        if (Status appended = out.append(
                Span<const u8>(reinterpret_cast<const u8*>(joint.name.data()), joint.name.size()));
            !appended) {
            return appended;
        }
        put_u32(out, static_cast<u32>(joint.parent));
        put_f32(out, joint.bind_local.translation.x);
        put_f32(out, joint.bind_local.translation.y);
        put_f32(out, joint.bind_local.translation.z);
        put_f32(out, joint.bind_local.rotation.x);
        put_f32(out, joint.bind_local.rotation.y);
        put_f32(out, joint.bind_local.rotation.z);
        put_f32(out, joint.bind_local.rotation.w);
        put_f32(out, joint.bind_local.scale.x);
        put_f32(out, joint.bind_local.scale.y);
        put_f32(out, joint.bind_local.scale.z);
        put_u32(out, joint.dropped_at);
    }
    // The humanoid profile, one slot per `HumanoidJoint` ordinal, after the joints because it
    // indexes them.
    for (const i32 joint : skeleton.humanoid.joints) {
        put_u32(out, joint < 0 ? 0xFFFFFFFFU : static_cast<u32>(joint));
    }
    return ok();
}

namespace {

/// How many bytes a joint's record holds after its name: the parent, the ten floats of the bind
/// pose, and the bone level. The name length precedes the name and is counted by the caller.
constexpr usize kJointPayloadBytes = 4 + (10 * 4) + 4;

/// Decode one joint's fixed fields from `data`, which the caller has already bounds-checked
/// against `kJointPayloadBytes`.
void read_joint_fields(const u8* data, ImportedJoint& joint) noexcept {
    joint.parent = static_cast<i32>(get_u32(data));
    joint.bind_local.translation = Vec3{get_f32(data + 4), get_f32(data + 8), get_f32(data + 12)};
    joint.bind_local.rotation =
        Quat{get_f32(data + 16), get_f32(data + 20), get_f32(data + 24), get_f32(data + 28)};
    joint.bind_local.scale = Vec3{get_f32(data + 32), get_f32(data + 36), get_f32(data + 40)};
    joint.dropped_at = static_cast<u8>(get_u32(data + 44));
}

/// The three refusals `skeleton.h` would otherwise make, made here where the record can name the
/// joint that is wrong rather than at load time where it cannot. `earlier` holds the joints already
/// read, which is what makes the nested-subset rule checkable in one forward pass.
[[nodiscard]] Status check_joint(const ImportedJoint& joint, usize index,
                                 const std::vector<ImportedJoint>& earlier) noexcept {
    if (joint.parent < -1 || std::cmp_greater_equal(joint.parent, index)) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked skeleton whose joints are not ordered parent before child");
    }
    if (joint.dropped_at == 0 || joint.dropped_at > kBoneLodLevels) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked skeleton with a bone level of detail outside the range; a joint "
                    "dropped at level 0 is in no level at all");
    }
    if (joint.parent >= 0 &&
        earlier[static_cast<usize>(joint.parent)].dropped_at < joint.dropped_at) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked skeleton whose bone levels are not nested subsets: a joint survives "
                    "a level its parent is dropped at");
    }
    return ok();
}

}  // namespace

Status read_cooked_skeleton(Span<const u8> payload, ImportedSkeleton& out) noexcept {
    constexpr usize kHeaderBytes = 12;
    constexpr usize kJointNameLengthBytes = 4;
    constexpr usize kProfileBytes = usize{kHumanoidJointCount} * 4;
    if (payload.size() < kHeaderBytes) {
        return fail(ErrorCode::InvalidArgument, "shorter than a cooked skeleton header");
    }
    if (get_u32(payload.data()) != kCookedSkeletonVersion) {
        return fail(ErrorCode::Unsupported, "a cooked skeleton from another format version");
    }
    const usize count = get_u32(payload.data() + 4);
    if (count == 0 || count > kMaxSkeletonJoints) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked skeleton with no joints, or with more than a pose mask addresses");
    }
    const usize name_length = get_u32(payload.data() + 8);
    if (kHeaderBytes + name_length > payload.size()) {
        return fail(ErrorCode::InvalidArgument, "a cooked skeleton that ends inside its name");
    }

    out.name.assign(reinterpret_cast<const char*>(payload.data() + kHeaderBytes), name_length);
    out.joints.clear();
    out.joints.reserve(count);

    usize cursor = kHeaderBytes + name_length;
    for (usize index = 0; index < count; ++index) {
        if (cursor + kJointNameLengthBytes > payload.size()) {
            return fail(ErrorCode::InvalidArgument, "a cooked skeleton that ends inside a joint");
        }
        const usize joint_name_length = get_u32(payload.data() + cursor);
        cursor += kJointNameLengthBytes;
        if (cursor + joint_name_length + kJointPayloadBytes > payload.size()) {
            return fail(ErrorCode::InvalidArgument, "a cooked skeleton that ends inside a joint");
        }
        ImportedJoint joint;
        joint.name.assign(reinterpret_cast<const char*>(payload.data() + cursor),
                          joint_name_length);
        cursor += joint_name_length;
        read_joint_fields(payload.data() + cursor, joint);
        cursor += kJointPayloadBytes;

        if (Status checked = check_joint(joint, index, out.joints); !checked) {
            return checked;
        }
        out.joints.push_back(std::move(joint));
    }
    if (cursor + kProfileBytes != payload.size()) {
        return fail(ErrorCode::InvalidArgument,
                    "a cooked skeleton whose payload does not match its header");
    }
    out.humanoid = HumanoidProfile{};
    for (u16 standard = 0; standard < kHumanoidJointCount; ++standard) {
        const u32 joint = get_u32(payload.data() + cursor);
        cursor += 4;
        if (joint == 0xFFFFFFFFU) {
            continue;
        }
        if (joint >= count) {
            return fail(ErrorCode::InvalidArgument,
                        "a cooked skeleton whose humanoid profile names a joint it does not have");
        }
        out.humanoid.map(standard, static_cast<i32>(joint));
    }
    return ok();
}

// --- Step 7 --------------------------------------------------------------------------------------

Status import_fbx_skeleton(const ufbx_scene& scene, const SkeletonImportOptions& options,
                           SubAssetNames& names, ImportResult& out,
                           ImportedSkeleton& skeleton) noexcept {
    skeleton.joints.clear();
    skeleton.name.clear();
    skeleton.humanoid = HumanoidProfile{};
    if (scene.root_node == nullptr || scene.nodes.count == 0) {
        return ok();
    }

    std::vector<bool> marked(scene.nodes.count, false);
    mark_joints(scene, marked);
    walk_joints(scene, marked, options, skeleton);
    if (skeleton.joints.empty()) {
        // A static model. Not a warning: "A format that cannot express a step SHALL skip it and say
        // so in the import report", and a file that HAS no rig has nothing to say about one either.
        return ok();
    }

    if (skeleton.joints.size() > kMaxSkeletonJoints) {
        char detail[ImportDiagnostic::kDetailCapacity];
        (void)std::snprintf(
            detail, sizeof(detail),
            "this rig has %zu joints and a pose mask addresses %u; the skeleton was "
            "not imported, because truncating a hierarchy would deform the model "
            "rather than reduce it",
            skeleton.joints.size(), kMaxSkeletonJoints);
        skeleton.joints.clear();
        return out.report(ImportSeverity::Warning, "skeleton-too-large", detail, "skeleton");
    }

    clamp_bone_levels(skeleton);
    map_root_slot(skeleton);
    skeleton.name = names.unique(kSkeletonSubAssetPrefix, stem_of(skeleton), 0);

    Array<u8> payload;
    if (Status written = write_cooked_skeleton(skeleton, payload); !written) {
        return written;
    }

    u32 detail_joints = 0;
    for (const ImportedJoint& joint : skeleton.joints) {
        detail_joints += joint.dropped_at < kBoneLodLevels ? 1U : 0U;
    }
    char detail[ImportDiagnostic::kDetailCapacity];
    (void)std::snprintf(
        detail, sizeof(detail),
        "%zu joints, %u of the %u standard humanoid joints mapped, %u detail joints "
        "dropped at bone level 1",
        skeleton.joints.size(), skeleton.humanoid.mapped_count(),
        static_cast<u32>(kHumanoidJointCount), detail_joints);
    if (Status reported = out.report(ImportSeverity::Info, "skeleton", detail, skeleton.name);
        !reported) {
        return reported;
    }

    return out.add(assets::AssetKind::Animation, skeleton.name, std::move(payload), false);
}

}  // namespace cy::import
