#ifndef CY_IMPORT_FBX_CLIP_H
#define CY_IMPORT_FBX_CLIP_H
// Step 8 of model import for FBX: animations, sampled out of ufbx and cooked through
// `cy::animation::Clip`'s own codec. M8.d.
//
// ================================================================================================
// WHY THIS IS A SEPARATE TRANSLATION UNIT AND NOT MORE OF fbx.cpp
// ================================================================================================
//
// `fbx.cpp` is one linear function whose shape IS the ten-step sequence `asset-import-pipeline`
// fixes, and step 8 is the only step that reaches outside `tools/import/` for its target: it
// authors a `cy::animation::Clip`, compresses it with that class's tolerances, and writes the
// result out. Keeping it here means `fbx.cpp` gains one delimited call and keeps its shape, and it
// means the animation dependency is one file's rather than the importer's — `-D CY_ANIMATION=OFF`
// removes `src/animation/` from the build entirely, and this file is where that removal is
// absorbed (see `kFbxClipsAvailable`).
//
// ================================================================================================
// WHAT WAS HERE BEFORE, AND WHY IT WAS WORSE THAN AN ERROR
// ================================================================================================
//
// Until M8.d the FBX importer read no animation at all, and said so under the `skipped-rig`
// diagnostic — but only when the file also carried a skin, a blend shape or MORE THAN ONE
// animation stack. A Mixamo animation export carries exactly one stack, no skin and no blend
// shape, so it satisfied none of those and imported to a prefab, in silence, with the animation
// the artist exported dropped and nobody told. That is the failure mode this file and the
// corrected condition in `fbx.cpp` exist to end: anything the importer drops is NAMED.
//
// ================================================================================================
// THE THREE DECISIONS A READER WILL WANT ARGUED
// ================================================================================================
//
// WHICH STACK. Every stack the file carries is imported, and a stack whose bake animates NOTHING —
// every channel of every node constant — is skipped and reported. That rule is what keeps a Mixamo
// character export honest without matching on a name: such a file carries `Take 001`, which holds
// only `filmboxTypeID` and `lockInfluenceWeights` and bakes to a flat pose, beside `mixamo.com`,
// which holds the walk. An importer that took `anim_stacks.data[0]` would cook the flat pose and
// drop the walk, and it would do it without a word.
//
// WHAT THE SUB-ASSET IS CALLED. `importer.h` requires a sub-asset's name to come from "something an
// artist controls ... rather than from something the file's ordering controls", because that name
// is what an `AssetId` is bound to across re-imports. A stack's name is not usable on its own here:
// every Mixamo export names its stack `mixamo.com`, so a whole library of animations would cook to
// one name. So: when a file yields exactly ONE clip the sub-asset takes the SOURCE FILE's stem,
// which is what the artist named when they exported it — `animation/Walking`. When it yields
// several, each takes its stack's name, because a file with several stacks is one an artist named
// them inside. The clip's own `Name` is always the stack's, which is what the file says it is.
//
// ROOT MOTION IS OPT-IN, AND THAT IS A STATEMENT ABOUT THE RUNTIME AND NOT ABOUT FBX.
// `Clip::set_root_motion_joint` designates the joint whose motion `Clip::root_delta` EXTRACTS —
// but `Clip::sample` writes that joint's translation track into the pose like any other track, so
// a clip that both designates a root joint and keeps its travel is applied twice by today's
// runtime: once as a pose and once as a delta. Until the runtime suppresses the designated joint,
// the default that is CORRECT with the runtime as it stands is "none", and `root-joint` is there
// for a caller who consumes `root_delta` and knows to ignore the pose. Neither choice is silent:
// both are a declared option, so both reach the derivation key.

#include <cy/core/base/expected.h>
#include <cy/core/base/types.h>
#include <cy/core/math/vec.h>
#include <cy/core/memory/array.h>
#include <cy/import/importer.h>
#include <cy/import/model.h>
#include <cy/import/options.h>

#include <string>
#include <string_view>
#include <vector>

/// ufbx's scene, named opaquely.
///
/// `deps/manifest.toml` records `src/fbx.cpp` as ufbx's interface and `tools/import/CMakeLists.txt`
/// keeps `cy::dep::ufbx` PRIVATE so that a consumer of `cy::import` cannot name a ufbx type. A
/// forward declaration keeps that true: this header compiles with no ufbx header in reach, and
/// only a translation unit that has already included `ufbx.h` can call the function below.
struct ufbx_scene;

namespace cy::import {

/// Whether this build can import animation at all.
///
/// False when `-D CY_ANIMATION=OFF` removed `src/animation/`, which `animation-and-skinning`
/// requires the runtime to be removable by. The importer then reports that step 8 was not reached
/// instead of pretending it was, which is the same distinction `asset-import-pipeline` draws for a
/// format that cannot express a step.
#if defined(CY_IMPORT_ANIMATION)
inline constexpr bool kFbxClipsAvailable = true;
#else
inline constexpr bool kFbxClipsAvailable = false;
#endif

/// The cooked clip payload's format version. Moved when the layout changes.
inline constexpr u32 kCookedClipVersion = 1;

/// The steps an FBX import reaches once animations are imported: `kHierarchyModelSteps` plus 8.
///
/// Its own constant rather than an edit to `kHierarchyModelSteps`, which `model.h` shares with the
/// glTF and OBJ importers precisely so that one importer growing a step does not silently enlarge
/// another's claim. Step 7 — skeletons — is not in it: a clip's tracks are addressed by joint
/// INDEX, and this file mints those indices for itself from the node hierarchy without producing a
/// skeleton sub-asset.
inline constexpr ModelImportStepSet kAnimatedHierarchyModelSteps =
    static_cast<ModelImportStepSet>(kHierarchyModelSteps | step_bit(ModelImportStep::Animations));

/// What `animation-root-motion` may be set to.
inline constexpr std::string_view kRootMotionChoices[] = {"none", "root-joint"};

/// The option specs step 8 adds to the FBX importer's schema, spliced onto `kFbxOptions` by
/// `fbx_options()`.
///
/// EVERY ONE OF THEM CHANGES THE COOKED BYTES, which is why each is a declared option and not a
/// constant somebody tunes later: `options.h` calls a setting that changes the output and cannot
/// reach the derivation key the one defect a cook cache cannot survive. `ufbx_bake_opts` has a
/// dozen fields of exactly that kind, and the ones this importer does not expose are hard-coded in
/// `fbx_clip.cpp` with the reason written beside them. They live HERE, beside the code that reads
/// them, so that a change to what a setting means and a change to how it is described are one edit.
inline constexpr OptionSpec kFbxAnimationOptions[] = {
    {"import-animations",
     OptionType::Bool,
     OptionValue::of_bool(true),
     "Whether to produce animation clips from the file's animation stacks. Turning meshes and "
     "materials off and leaving this on is the fast path for animation iteration, which is what "
     "an animation-only export from a character library is.",
     {},
     0.0,
     0.0},
    {"animation-sample-rate",
     OptionType::Float,
     OptionValue::of_float(30.0),
     "Samples per second used to bake a curve that is not already resampled, and the clip's own "
     "sample rate hint. A source exported at or above this rate is not resampled again, so raising "
     "it does not re-sample an already-baked export; lowering it does.",
     {},
     1.0,
     240.0},
    {"animation-key-reduction",
     OptionType::Bool,
     OptionValue::of_bool(true),
     "Whether the baker drops keys a linear segment already reproduces, before the engine's own "
     "error-bounded curve fitting runs. Off imports every sampled frame, which is slower and "
     "larger and is the way to tell a baking artefact from a fitting one.",
     {},
     0.0,
     0.0},
    {"animation-translation-tolerance-mm",
     OptionType::Float,
     OptionValue::of_float(0.1),
     "How far, in millimetres, the curve fit may deviate from the sampled translation. The codec "
     "then quantises what the fit kept, and that step adds its own error on top — so the figure "
     "the import report names is MEASURED against the authored keys and can exceed this one. It "
     "is the dial that governs how many keys survive, not a guarantee about the last micrometre.",
     {},
     0.0001,
     100.0},
    {"animation-rotation-tolerance-degrees",
     OptionType::Float,
     OptionValue::of_float(0.1),
     "How far, in degrees, the curve fit may deviate from the sampled rotation. Quantising a "
     "rotation to four 16-bit components over a fixed [-1, 1] range costs about a twentieth of a "
     "degree by itself, so the measured worst case composes the two and sits a little above this "
     "number; below roughly 0.05 degrees this dial stops buying accuracy and only buys keys.",
     {},
     0.0001,
     45.0},
    {"animation-root-motion", OptionType::Enumeration, OptionValue::of_enumeration("none"),
     "Whether a clip designates the joint whose motion gameplay extracts. 'root-joint' nominates "
     "the first root of the joint hierarchy that is translated by the clip. It is off by default "
     "because the runtime's sampler still writes that joint's translation into the pose as well as "
     "reporting it as a delta, so a designated clip is applied twice until that is closed.",
     Span<const std::string_view>(kRootMotionChoices), 0.0, 0.0},
};

/// What step 8 was configured with. Filled from the options above by the caller.
struct FbxClipOptions {
    bool import_animations = true;
    /// The same uniform scale the mesh and node paths apply, so a skeleton's travel and its mesh
    /// stay the same size. Translation keys are in metres before it, because ufbx converted them.
    f32 scale = 1.0F;
    /// True when `source-up-axis` overrode the file's declaration with `z-up`. The rotation is then
    /// applied to the keys of a joint whose parent is the scene root, which is exactly where
    /// `fbx.cpp` applies it to that node's static transform.
    bool z_up_override = false;
    /// `ufbx_bake_opts::resample_rate`, and the clip's sample rate hint.
    f32 sample_rate = 30.0F;
    /// `ufbx_bake_opts::key_reduction_enabled`, before the engine's own curve fitting runs.
    bool key_reduction = true;
    f32 translation_tolerance_mm = 0.1F;
    f32 rotation_tolerance_degrees = 0.1F;
    f32 scale_tolerance = 0.001F;
    /// "none" or "root-joint". See the header comment: the default is the choice that is correct
    /// with the runtime as it stands.
    std::string_view root_motion = "none";
};

/// What step 8 did, measured rather than estimated. Every field is reported by the caller.
struct FbxClipReport {
    /// Stacks the file carried.
    u32 stacks = 0;
    /// Clips produced.
    u32 clips = 0;
    /// Stacks whose bake animated nothing, skipped and named.
    u32 constant_stacks = 0;
    /// Joints the clips are indexed against.
    u32 joints = 0;
    u32 tracks = 0;
    u32 keys_before = 0;
    u32 keys_after = 0;
    f32 worst_translation_mm = 0.0F;
    f32 worst_rotation_degrees = 0.0F;
    /// Baked nodes no joint index covers — animation this import DROPPED, and the number that must
    /// never be non-zero without a diagnostic.
    u32 unmapped_nodes = 0;
    /// True when the rig is over `animation::kMaxJoints` and nothing could be imported.
    bool too_many_joints = false;
};

/// Import every animation stack in `scene` as its own `Animation` sub-asset.
///
/// `joint_names` is step 7's joint table, in ITS order: `ImportedSkeleton::joints[i].name`. A track
/// addresses a joint by INDEX, and the only index that means anything to a consumer is the one the
/// skeleton record numbered — so the two steps are joined by the one identity they share, the
/// source node's name, and never by an ordering each derived for itself.
///
/// AN EMPTY SPAN MEANS DERIVE A TABLE, by the depth-first walk `fbx_clip.cpp` describes. That is
/// what an FBX with animation and no rig needs — a camera move, a prop, a door — and it is also
/// what keeps this file honest when step 7 declines to produce a skeleton.
///
/// `source_path` is the request's source, whose stem names a single-clip file's sub-asset.
/// Diagnostics go to `out`; a returned error is about the machine, never about the file.
[[nodiscard]] Status import_fbx_animations(const ufbx_scene& scene, const FbxClipOptions& options,
                                           Span<const std::string_view> joint_names,
                                           std::string_view source_path, SubAssetNames& names,
                                           ImportResult& out, FbxClipReport& report) noexcept;

// --- The cooked clip record ----------------------------------------------------------------------

/// One track of a cooked clip, as `read_cooked_clip` hands it back.
struct CookedClipTrack {
    /// `cy::animation::TrackKind`, widened. Named by value rather than by type so that this header
    /// — and anything that inspects a package — compiles without the animation runtime.
    u8 kind = 0;
    /// `cy::animation::Interpolation`, widened.
    u8 interpolation = 1;
    /// The joint index, or `0xFFFF` for a curve or property track.
    u16 joint = 0xFFFFU;
    /// Collapsed to a single key by the codec.
    bool constant = false;
    u32 first_key = 0;
    u32 key_count = 0;
    /// The quantisation range the codec derived. Rotation tracks leave it zeroed and use the fixed
    /// [-1, 1] component range.
    Vec3 range_min{0.0F, 0.0F, 0.0F};
    Vec3 range_max{0.0F, 0.0F, 0.0F};
};

/// One stored key: a time in seconds and four 16-bit components, exactly as the codec left them.
struct CookedClipKey {
    f32 time = 0.0F;
    u16 components[4] = {0, 0, 0, 0};
};

/// A cooked clip, read back.
///
/// THE JOINT NAMES ARE PART OF THE RECORD, and that is the point of it. A `Clip`'s tracks address
/// joints by INDEX, and `AnimationRig::bind` checks only that the skeleton has at least as many
/// joints as the program addresses — so a clip cooked against one rig and bound to another drives
/// the wrong bones and nothing reports it. Carrying the names the clip was authored against is what
/// lets a loader rebind by name, or refuse.
struct CookedClip {
    std::string name;
    f32 duration = 0.0F;
    /// `cy::animation::LoopMode`, widened. FBX carries no loop flag, so this is the runtime's own
    /// default and not a statement about the file.
    u8 loop_mode = 1;
    f32 sample_rate_hint = 30.0F;
    /// The designated root-motion joint, or `0xFFFF`.
    u16 root_motion_joint = 0xFFFFU;
    std::vector<std::string> joint_names;
    std::vector<CookedClipTrack> tracks;
    std::vector<CookedClipKey> keys;
};

/// Read a cooked clip back. For the tests, and for a tool that inspects a package.
[[nodiscard]] Status read_cooked_clip(Span<const u8> payload, CookedClip& out) noexcept;

}  // namespace cy::import

#endif  // CY_IMPORT_FBX_CLIP_H
