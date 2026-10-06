// samples/09b-animated-character — M8.d's artefact: four Mixamo files, one character, one video.
//
// ================================================================================================
// WHAT THIS PROGRAM CLAIMS, AND WHAT IT DOES NOT
// ================================================================================================
//
// IT CLAIMS: four FBX files go through `tools/import/`'s steps 7 and 8; the character's skeleton,
// its four clips — three of them RETARGETED onto the fourth file's rig by `retarget_build.h`'s
// measured correspondence — and its compiled four-state locomotion program are COOKED into the
// records a game ships (`cook_locomotion_set`, the `animation` build-graph producer's work); those
// records are LOADED BY ASSET ID through the asset system and `AnimationLibrary`; one entity
// carries an `Animator`, and the engine's `AnimationSystem` advances it in the fixed step and
// evaluates it in `Stage::Animation` of a real `runtime::Simulation`, publishing into the system's
// `PoseWorld`; and the bone matrices that come out of it move vertices in a COMPUTE PASS on a real
// device, whose output buffer a rasteriser then draws. Nothing on the CPU writes the vertices that
// appear in the picture, and nothing in this file advances, evaluates or publishes a pose: the
// game's only per-frame animation code is the one request it raises.
//
// IT DOES NOT CLAIM that the skin weights are the artist's on an old cache entry — `character.h`
// says which were used, and the report this program prints names it.
//
// SINCE ISSUE #76 STAGE 3 the character is drawn by the ENGINE'S FORWARD FRAME: the pose world's
// dirty range fills `skinning::SkinnedScene`'s device pose buffer, one compute pass skins it, and
// `FramePipelines`' skinned variants draw it in the depth prepass, the directional shadow and the
// opaque pass, lit and shadowed like everything else the frame draws. `stage.h` says how.
//
// ================================================================================================
// THE TIMESTEP IS FIXED, AND THAT IS A REPRODUCIBILITY CLAIM
// ================================================================================================
//
// The simulation runs at `--fps` ticks a second, one tick a frame, and the camera moves by the same
// step, so two runs of this program produce the same pose at the same frame index. There is no
// wall clock anywhere in the loop: the animation is a function of the simulation's tick count,
// which is the rule `AnimationSystem` is written to. That is what makes the video a thing a
// reviewer can regenerate and compare rather than a recording of one afternoon.
//
// ================================================================================================
// THE STATE SCHEDULE
// ================================================================================================
//
// Requests, in seconds. The machine honours a request only from a state that has an edge for it,
// which is why the return from the run goes THROUGH the walk: `locomotion.h` records that there is
// deliberately no run-to-idle edge, so a character decelerates rather than teleporting to a stand.
//
//   0.0  idle     the entry state
//   2.0  walk
//   4.5  run
//   7.0  walk     decelerating, through the edge that exists
//   8.0  die      outranks locomotion and cannot be outranked; the death has no outgoing edge
//
// The take runs to thirteen seconds because the death clip is 4.6 s long and HOLDS on its last
// frame rather than looping. Cutting at ten would show a character mid-fall and leave a viewer to
// guess what happened to it.

#include <cy/animation/animation_system.h>
#include <cy/animation/library.h>
#include <cy/core/assets/asset_system.h>
#include <cy/core/assets/package.h>
#include <cy/core/assets/vfs.h>
#include <cy/core/jobs/async.h>
#include <cy/core/jobs/job_system.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/graph/locomotion.h>
#include <cy/runtime/simulation.h>

#include "character.h"
#include "stage.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace {

using namespace cy;
using namespace cy::sample::character;
namespace pose = cy::graph::pose;

Allocator& allocator() noexcept {
    return system_allocator(MemoryDomain::Animation);
}

/// One entry of the schedule above.
struct Cue {
    f32 at_seconds = 0.0F;
    pose::LocomotionState state = pose::LocomotionState::Idle;
};

constexpr Cue kSchedule[] = {
    {0.0F, pose::LocomotionState::Idle}, {2.0F, pose::LocomotionState::Walk},
    {4.5F, pose::LocomotionState::Run},  {7.0F, pose::LocomotionState::Walk},
    {8.0F, pose::LocomotionState::Die},
};

struct Options {
    std::string sources = "/home/leonardo/Downloads";
    std::string frames;
    u32 width = 960;
    u32 height = 540;
    u32 fps = 30;
    f32 seconds = 13.0F;
    /// Which frame is written a second time under `--still`. Frame 180 is six seconds in, which is
    /// the middle of the run — the moment in the take where the most of the body is furthest from
    /// its rest pose, and therefore the one frame that carries the most of the claim on its own.
    u32 still_frame = 180;
    std::string still;
};

/// `strtol` and `strtod` rather than `atoi` and `atof`, which report no conversion error at all: a
/// `--fps banana` would silently become zero and the run would divide by it.
[[nodiscard]] bool read_u32(const char* text, u32& out) noexcept {
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value <= 0) {
        return false;
    }
    out = static_cast<u32>(value);
    return true;
}

[[nodiscard]] bool read_f32(const char* text, f32& out) noexcept {
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0') {
        return false;
    }
    out = static_cast<f32>(value);
    return true;
}

/// One pass over the command line, with the "did this flag match, and did its value parse" question
/// answered in one place.
///
/// A cursor rather than a chain of `else if` blocks, because every one of those blocks is the same
/// four lines — take the next argument, refuse a missing one, convert it, refuse a bad conversion —
/// and nine copies of four lines is where the ninth quietly forgets the third.
class Cursor {
public:
    Cursor(int argc, char** argv) noexcept : argc_(argc), argv_(argv) {}

    [[nodiscard]] bool advance() noexcept { return ++index_ < argc_; }
    [[nodiscard]] std::string_view current() const noexcept { return argv_[index_]; }
    /// True once a flag was recognised but its value was missing or would not convert.
    [[nodiscard]] bool failed() const noexcept { return failed_; }

    [[nodiscard]] bool text(std::string_view name, std::string& out) noexcept {
        const char* value = value_for(name);
        if (value == nullptr) {
            return matched_;
        }
        out = value;
        return true;
    }

    [[nodiscard]] bool number(std::string_view name, u32& out) noexcept {
        const char* value = value_for(name);
        if (value == nullptr) {
            return matched_;
        }
        failed_ = failed_ || !read_u32(value, out);
        return true;
    }

    [[nodiscard]] bool number(std::string_view name, f32& out) noexcept {
        const char* value = value_for(name);
        if (value == nullptr) {
            return matched_;
        }
        failed_ = failed_ || !read_f32(value, out);
        return true;
    }

private:
    /// The value after `name`, or null. `matched_` separates "this was not the flag" from "it was
    /// the flag and it had no value", which the callers above return as a match so that the next
    /// flag is not also tried against it.
    [[nodiscard]] const char* value_for(std::string_view name) noexcept {
        matched_ = false;
        if (current() != name) {
            return nullptr;
        }
        matched_ = true;
        if (index_ + 1 >= argc_) {
            failed_ = true;
            return nullptr;
        }
        return argv_[++index_];
    }

    int argc_ = 0;
    char** argv_ = nullptr;
    int index_ = 0;
    bool matched_ = false;
    bool failed_ = false;
};

[[nodiscard]] bool parse(int argc, char** argv, Options& out) noexcept {
    Cursor cursor(argc, argv);
    while (cursor.advance()) {
        const bool recognised =
            cursor.text("--sources", out.sources) || cursor.text("--frames", out.frames) ||
            cursor.text("--still", out.still) || cursor.number("--still-frame", out.still_frame) ||
            cursor.number("--width", out.width) || cursor.number("--height", out.height) ||
            cursor.number("--fps", out.fps) || cursor.number("--seconds", out.seconds);
        if (!recognised) {
            const std::string_view argument = cursor.current();
            std::fprintf(stderr, "unknown argument: %.*s\n", static_cast<int>(argument.size()),
                         argument.data());
            return false;
        }
        if (cursor.failed()) {
            return false;
        }
    }
    // Even dimensions, because 4:2:0 chroma subsampling cannot encode an odd width or height and a
    // video that fails to encode after four hundred frames have been rendered is a bad way to learn
    // it. Rounded down rather than refused: the caller asked for a size, not for a lecture.
    out.width &= ~1U;
    out.height &= ~1U;
    return out.width > 0 && out.height > 0 && out.fps > 0 && out.seconds > 0.0F;
}

void print_sources(const Character& character) noexcept {
    std::printf("\n  source files\n");
    for (u32 index = 0; index < kMotionCount; ++index) {
        const SourceReport& report = character.sources[index];
        std::printf("    %-5s %s\n", motion_name(static_cast<Motion>(index)), report.file.c_str());
        std::printf("          imported  mesh=%u material=%u skeleton=%u animation=%u warning=%u\n",
                    report.meshes, report.materials, report.skeletons, report.animations,
                    report.warnings);
        std::printf("          rig       joints=%u humanoid=%u/22\n", report.joints,
                    report.humanoid_mapped);
        std::printf("          clip      duration=%.3fs tracks=%u keys=%u\n",
                    static_cast<f64>(report.duration), report.tracks, report.keys);
        if (report.retargeted) {
            std::printf(
                "          retarget  pairs=%u height_scale=%.4f  rest difference before it: "
                "%.2f deg, %.1f mm\n",
                report.retarget_pairs, static_cast<f64>(report.height_scale),
                static_cast<f64>(report.rest_difference_degrees),
                static_cast<f64>(report.rest_difference_metres * 1000.0F));
        } else {
            std::printf("          retarget  none — this file's rig IS the character's\n");
        }
        std::printf("          played    tracks=%u keys=%u worst rotation error=%.3f deg\n",
                    report.final_tracks, report.final_keys,
                    static_cast<f64>(report.worst_rotation_degrees));
    }
}

void print_skin(const SkinReport& skin) noexcept {
    // WHICH SOURCE THE WEIGHTS CAME FROM IS THE FIRST LINE, because a picture of a skinned
    // character looks the same either way and the report is the only place a reader can tell. M11.b
    // gave `MeshData` joint and weight arrays and taught both model importers to fill them; the
    // derived bind stays as the fallback for a cooked mesh that predates the format version.
    std::printf(skin.imported
                    ? "\n  skin  IMPORTED — the artist's weights, out of the file\n"
                    : "\n  skin  DERIVED, NOT IMPORTED — the cooked mesh carried no bindings; see "
                      "samples/09b-animated-character/character.h\n");
    std::printf("    vertices=%u triangles=%u bones given weight=%u\n", skin.vertices,
                skin.triangles, skin.bones_used);
    if (!skin.imported) {
        std::printf("    worst distance from a vertex to its nearest bone: %.1f mm\n",
                    static_cast<f64>(skin.worst_bind_distance * 1000.0F));
    }
    std::printf("    mesh bounds  (%.3f %.3f %.3f) to (%.3f %.3f %.3f)\n",
                static_cast<f64>(skin.mesh_min.x), static_cast<f64>(skin.mesh_min.y),
                static_cast<f64>(skin.mesh_min.z), static_cast<f64>(skin.mesh_max.x),
                static_cast<f64>(skin.mesh_max.y), static_cast<f64>(skin.mesh_max.z));
    std::printf("    rig bounds   (%.3f %.3f %.3f) to (%.3f %.3f %.3f)\n",
                static_cast<f64>(skin.rig_min.x), static_cast<f64>(skin.rig_min.y),
                static_cast<f64>(skin.rig_min.z), static_cast<f64>(skin.rig_max.x),
                static_cast<f64>(skin.rig_max.y), static_cast<f64>(skin.rig_max.z));
}

/// Which state the schedule asks for at `seconds`.
[[nodiscard]] pose::LocomotionState requested_at(f32 seconds) noexcept {
    pose::LocomotionState wanted = kSchedule[0].state;
    for (const Cue& cue : kSchedule) {
        if (seconds + 1e-4F >= cue.at_seconds) {
            wanted = cue.state;
        }
    }
    return wanted;
}

/// What the take measured, and what the gates at the end of `run()` read.
struct Summary {
    u16 states_entered[pose::kLocomotionStateCount] = {};
    /// The largest distance any joint covered between two consecutive frames, and where. A
    /// character frozen in ONE animated pose — the failure a still photograph cannot distinguish
    /// from a working one — has this at zero.
    f32 worst_step = 0.0F;
    u32 worst_step_frame = 0;
    /// The largest distance any joint ever sat from its own rest placement. A character stuck in
    /// its bind pose has this at zero.
    f32 worst_departure = 0.0F;
    /// How far the hips got from where they started.
    f32 furthest = 0.0F;
    /// How many times each double-buffered range changed. A range that never alternates is one half
    /// of a pair being read every frame.
    u32 vertex_offsets_seen = 0;
    u32 pose_offsets_seen = 0;
    u32 validation_errors = 0;
    u32 frames = 0;
    /// Frames in which the forward frame did not draw the character from the skinning output in
    /// all three of its depth prepass, shadow and opaque passes.
    u32 frames_not_drawn = 0;
    /// The most matrices one frame copied to the device pose buffer. The pose world's dirty range
    /// is one skeleton's current half here; anything larger is a copy of more than changed.
    u32 most_uploaded = 0;
};

/// Fold one frame's pose into the measurements, and keep it for the next frame's comparison.
///
/// The joints' model-space places are recovered from the published skinning matrices — the matrix
/// carries the bind pose out, so multiplying it back in gives where the joint is — because the pose
/// is the system's and the matrices are what it publishes.
void measure(const Character& character, Span<const Mat4> matrices, Array<Vec3>& places,
             Array<Vec3>& previous, u32 frame, bool have_previous, Summary& out) noexcept {
    const u16 joints = character.skeleton.joint_count();
    for (u16 joint = 0; joint < joints; ++joint) {
        places[joint] =
            (matrices[joint] * character.skeleton.bind_model()[joint].to_matrix()).translation();
        const Vec3 rest = character.skeleton.bind_model()[joint].translation;
        out.worst_departure = math::max(out.worst_departure, length(places[joint] - rest));
        if (have_previous) {
            const f32 step = length(places[joint] - previous[joint]);
            if (step > out.worst_step) {
                out.worst_step = step;
                out.worst_step_frame = frame;
            }
        }
        previous[joint] = places[joint];
    }
}

/// Where the camera stands at `seconds`, looking at a point that follows the hips.
///
/// The ground is STATIC, so what a viewer sees sliding underneath a travelling character is the
/// character's own motion — the only way a fixed framing can show locomotion at all. The follow is
/// smoothed because the hips sway laterally within a stride and a camera nailed to them would make
/// the world wobble; the slow quarter-turn over the whole take is what gives the picture parallax,
/// so a viewer can see a solid in a space rather than a flat animation.
[[nodiscard]] Shot compose(Vec3 follow, f32 seconds) noexcept {
    Shot shot;
    const f32 angle = 0.55F + (seconds * 0.075F);
    const f32 distance = 3.1F;
    shot.target = Vec3{follow.x, 1.00F, follow.z};
    shot.eye = Vec3{shot.target.x + (std::sin(angle) * distance), 1.35F,
                    shot.target.z + (std::cos(angle) * distance)};
    shot.fov_y_radians = 0.80F;
    return shot;
}

/// The asset ids the cooked records are stored under.
///
/// FIXED rather than minted, which a real cook would not do: `cy::assets::mint_asset_id` draws
/// random bits once and a sidecar records them. These records are cooked again on every run from
/// files outside the repository and nothing persists a reference to them, so a fixed id is the
/// reproducible choice — the same one samples/01-headless-host makes, and outside the reserved
/// placeholder namespace.
constexpr AssetId kSkeletonId{0x09B0'0000'0000'0001ULL, 1};
constexpr AssetId kProgramId{0x09B0'0000'0000'0001ULL, 2};
constexpr AssetId kClipIds[kMotionCount] = {
    {0x09B0'0000'0000'0001ULL, 10},
    {0x09B0'0000'0000'0001ULL, 11},
    {0x09B0'0000'0000'0001ULL, 12},
    {0x09B0'0000'0000'0001ULL, 13},
};

/// The asset system a game loads through, over one memory mount the cooked records are written
/// into. A shipped game mounts a package instead; the asset system, the ids and every load are the
/// same.
struct AssetHost {
    AssetHost() = default;
    ~AssetHost() {
        library.reset();
        assets.shutdown();
        async.stop();
        workers.shutdown();
    }
    AssetHost(const AssetHost&) = delete;
    AssetHost& operator=(const AssetHost&) = delete;

    [[nodiscard]] Status open(const Character& character) noexcept {
        jobs::JobSystemConfig config;
        config.worker_count = 2;
        if (Status started = workers.start(config); !started) {
            return started;
        }
        if (Status started = async.start(workers); !started) {
            return started;
        }
        Expected<UniquePtr<assets::MemoryMount>, Error> memory =
            make_unique<assets::MemoryMount>(allocator(), "cooked-character");
        if (!memory) {
            return Status{make_unexpected(memory.error())};
        }
        if (Expected<assets::MountId, Error> mounted =
                files.mount_owned(std::move(*memory), assets::mount_priority::kMemory);
            !mounted) {
            return Status{make_unexpected(mounted.error())};
        }
        if (Status started = assets.start(workers, async, files, assets::AssetSystemConfig{});
            !started) {
            return started;
        }
        Status put = store(kSkeletonId, character.cooked.skeleton);
        if (put) {
            put = store(kProgramId, character.cooked.program);
        }
        for (u32 index = 0; index < kMotionCount && put; ++index) {
            put = store(kClipIds[index], character.cooked.clips[index].bytes);
        }
        if (!put) {
            return put;
        }
        library = std::make_unique<animation::AnimationLibrary>(allocator(), assets);
        return ok();
    }

    [[nodiscard]] Status store(AssetId id, const Array<u8>& bytes) noexcept {
        Expected<assets::VirtualPath, Error> path = assets::package_entry_path(id, {});
        if (!path) {
            return Status{make_unexpected(path.error())};
        }
        return files.write(*path, bytes.data(), bytes.size());
    }

    jobs::JobSystem workers;
    jobs::AsyncService async;
    assets::VirtualFileSystem files;
    assets::AssetSystem assets;
    std::unique_ptr<animation::AnimationLibrary> library;
};

/// The rig, bound from asset ids, and the compiled clip table printed: `ClipRef::looping` is the
/// one thing about a clip the program carries that nothing else in this report would show, and a
/// death that arrived as `looping=yes` would play forever with the picture looking plausible
/// throughout.
[[nodiscard]] Expected<const animation::AnimationRig*, Error> bind_rig(AssetHost& host) noexcept {
    const animation::RigAssets wanted{kSkeletonId, kProgramId,
                                      Span<const AssetId>(kClipIds, kMotionCount)};
    Expected<const animation::AnimationRig*, Error> rig = host.library->rig(wanted);
    if (!rig) {
        std::fprintf(stderr, "the rig would not bind: %s\n", rig.error().message);
        return rig;
    }
    const pose::PoseProgram& program = (*rig)->program();
    for (const pose::ClipRef& reference : program.clips()) {
        std::printf("  clip ref  %-5s duration=%.3fs looping=%s\n", reference.name.c_str(),
                    static_cast<f64>(reference.duration), reference.looping ? "yes" : "no");
    }
    std::printf("\n  program   states=%u transitions=%u clips=%u parameters=%u joints=%u\n",
                static_cast<u32>(program.states().size()),
                static_cast<u32>(program.transitions().size()),
                static_cast<u32>(program.clips().size()),
                static_cast<u32>(program.parameters().size()), program.joint_count());
    std::printf(
        "  loaded    by asset id through the asset system: 1 skeleton, %u clips, 1 program\n",
        host.library->stats().clips);
    return rig;
}

/// The simulation the character lives in, with the engine's animation system installed: the tick
/// half in the fixed step and the pose half in `Stage::Animation`.
struct World {
    explicit World(u32 fps) noexcept : simulation(allocator(), config_for(fps)) {}

    [[nodiscard]] static runtime::SimulationConfig config_for(u32 fps) noexcept {
        runtime::SimulationConfig config;
        config.world_name = "animated-character";
        config.clock.rate = determinism::TickRate{fps, 1};
        config.clock.mode = determinism::TickMode::FixedStep;
        config.clock.fixed_ticks_per_frame = 1;
        return config;
    }

    [[nodiscard]] Status open(const animation::AnimationRig& rig) noexcept {
        if (Status initialized = simulation.initialize(); !initialized) {
            return initialized;
        }
        Expected<ecs::ComponentTypeId, Error> registered =
            animation::register_animator(simulation.world());
        if (!registered) {
            return Status{make_unexpected(registered.error())};
        }
        animator = *registered;
        system = std::make_unique<animation::AnimationSystem>(allocator(), simulation.world(),
                                                              animator, simulation.tree());
        if (Expected<animation::RigId, Error> added = system->add_rig(rig); !added) {
            return Status{make_unexpected(added.error())};
        }
        if (Expected<ecs::SystemId, Error> installed =
                system->install(simulation.schedule(), simulation.clock());
            !installed) {
            return Status{make_unexpected(installed.error())};
        }
        if (Status closed = simulation.finalize_registration(); !closed) {
            return closed;
        }
        // THE CHARACTER IS AN ENTITY WITH AN ANIMATOR, and that is all the game declares. Full
        // tier: one character, framed close.
        animation::Animator settings;
        settings.rig = 0;
        settings.tier = animation::LodTier::Full;
        const ecs::ComponentTypeId components[] = {animator};
        Expected<ecs::Entity, Error> created =
            simulation.world().create(Span<const ecs::ComponentTypeId>(components, 1));
        if (!created) {
            return Status{make_unexpected(created.error())};
        }
        character = *created;
        return simulation.world().set(character, animator, settings);
    }

    /// Raise exactly one request — what `LocomotionDriver::request` does — through the system.
    /// Before the first tick the instance does not exist yet, and the entry state is the idle the
    /// schedule asks for, so there is nothing to raise.
    [[nodiscard]] Status request(pose::LocomotionState wanted) const noexcept {
        if (system->instance(character) == nullptr) {
            return ok();
        }
        for (u32 index = 0; index < pose::kLocomotionStateCount; ++index) {
            const auto state = static_cast<pose::LocomotionState>(index);
            if (Status set = system->set_parameter(character, pose::locomotion_request(state),
                                                   state == wanted ? 1.0F : 0.0F);
                !set) {
                return set;
            }
        }
        return ok();
    }

    /// One frame of the simulation: its tick, then the frame stages.
    [[nodiscard]] Status frame() noexcept {
        const determinism::FrameTicks ticks = simulation.begin_frame(0);
        for (u32 tick = 0; tick < ticks.ticks; ++tick) {
            Expected<determinism::CommitRecord, Error> stepped = simulation.step(nullptr);
            if (!stepped) {
                return Status{make_unexpected(stepped.error())};
            }
        }
        if (Status framed = simulation.frame(ticks.alpha, nullptr); !framed) {
            return framed;
        }
        return system->last_error();
    }

    runtime::Simulation simulation;
    ecs::ComponentTypeId animator = ecs::kInvalidComponent;
    std::unique_ptr<animation::AnimationSystem> system;
    ecs::Entity character = ecs::kNoEntity;
};

/// One frame: the request, the simulation's tick and frame stages, and the shot.
[[nodiscard]] Status step(const Options& options, World& world, Stage& stage, u32 frame, f32 dt,
                          Vec3 hips, Vec3& follow, FrameReport& report) noexcept {
    const f32 seconds = static_cast<f32>(frame) * dt;
    animation::PoseWorld& poses = world.system->poses();
    const animation::PoseHandle handle = world.system->pose_of(world.character);
    follow = frame == 0 ? hips : follow + ((hips - follow) * 0.08F);
    const Shot shot = compose(follow, seconds);

    // The stage uploads the pose world's dirty range and reads `matrix_offset(handle)` itself,
    // every frame: the offset moves on every publish, which is what the double buffering IS.
    char path[512];
    (void)std::snprintf(path, sizeof(path), "%s/frame_%04u.png", options.frames.c_str(), frame);
    const char* target = options.frames.empty() ? nullptr : path;
    const char* still =
        frame == options.still_frame && !options.still.empty() ? options.still.c_str() : nullptr;
    if (Status taken = stage.shoot(poses, handle, shot, frame, target, report); !taken) {
        return taken;
    }
    // The still is the same frame drawn again: nothing was published since, so the upload is
    // empty and the skinning pass writes the other half from the same pose.
    if (still != nullptr) {
        FrameReport again;
        return stage.shoot(poses, handle, shot, frame, still, again);
    }
    return ok();
}

/// Every frame of the take.
[[nodiscard]] Status play(const Options& options, const Character& character, World& world,
                          Stage& stage, Summary& out) noexcept {
    const f32 dt = 1.0F / static_cast<f32>(options.fps);
    out.frames = static_cast<u32>(std::lround(options.seconds * static_cast<f32>(options.fps)));
    // Seeded from the first frame's hips inside `step`, so the camera does not sweep in from
    // the origin over the first second of the take.
    Vec3 follow{0.0F, 0.0F, 0.0F};
    FrameReport report;
    u32 last_vertex_offset = 0xFFFFFFFFU;
    u32 last_pose_offset = 0xFFFFFFFFU;
    Array<Vec3> places(allocator());
    Array<Vec3> previous(allocator());
    if (!places.resize(character.skeleton.joint_count()) ||
        !previous.resize(character.skeleton.joint_count())) {
        return fail(ErrorCode::OutOfMemory, "the measurement buffers would not size");
    }

    for (u32 frame = 0; frame < out.frames; ++frame) {
        // THE GAME'S WHOLE PER-FRAME ANIMATION CODE: one request. The system advances, evaluates
        // and publishes in the simulation's own stages.
        const f32 seconds = static_cast<f32>(frame) * dt;
        Status stepped = world.request(requested_at(seconds));
        if (stepped) {
            stepped = world.frame();
        }
        if (stepped) {
            const Span<const Mat4> matrices =
                world.system->poses().current(world.system->pose_of(world.character));
            measure(character, matrices, places, previous, frame, frame != 0, out);
            stepped = step(options, world, stage, frame, dt, places[0], follow, report);
        }
        if (!stepped) {
            std::fprintf(stderr, "frame %u failed: %s\n", frame, stepped.error().message);
            return stepped;
        }

        const u16 state = world.system->instance(world.character)->machine().state;
        if (state < pose::kLocomotionStateCount) {
            out.states_entered[state] = 1;
        }
        const Vec3 hips = places[0];
        out.furthest = math::max(out.furthest, std::sqrt((hips.x * hips.x) + (hips.z * hips.z)));
        if (report.vertex_offset != last_vertex_offset) {
            last_vertex_offset = report.vertex_offset;
            ++out.vertex_offsets_seen;
        }
        if (report.pose_offset != last_pose_offset) {
            last_pose_offset = report.pose_offset;
            ++out.pose_offsets_seen;
        }
        out.validation_errors = report.validation_errors;
        out.frames_not_drawn += report.skinned_draws == 3U ? 0U : 1U;
        out.most_uploaded = math::max(out.most_uploaded, report.uploaded_matrices);
        if ((frame % 30U) == 0U) {
            std::printf("    frame %3u  t=%5.2fs  state=%-5s  hips=(%.2f, %.2f, %.2f)\n", frame,
                        static_cast<f64>(seconds),
                        pose::locomotion_state_name(static_cast<pose::LocomotionState>(state)),
                        static_cast<f64>(hips.x), static_cast<f64>(hips.y),
                        static_cast<f64>(hips.z));
        }
    }
    return ok();
}

void print_summary(const Options& options, const Summary& summary, u32& out_visited) noexcept {
    std::printf("\n  frames    %u written to %s\n", summary.frames,
                options.frames.empty() ? "(nowhere — no --frames given)" : options.frames.c_str());
    std::printf("  states    ");
    out_visited = 0;
    for (u32 index = 0; index < pose::kLocomotionStateCount; ++index) {
        if (summary.states_entered[index] != 0) {
            std::printf("%s ",
                        pose::locomotion_state_name(static_cast<pose::LocomotionState>(index)));
            ++out_visited;
        }
    }
    std::printf("(%u of %u)\n", out_visited, pose::kLocomotionStateCount);
    // THE FRAME IS NAMED because a single large step is how a POP looks in a number, and the frame
    // index is what turns "something jumped" into "look at this transition". It found two, and both
    // are recorded in README.md.
    std::printf("  motion    worst joint step between two frames: %.1f mm, at frame %u\n",
                static_cast<f64>(summary.worst_step * 1000.0F), summary.worst_step_frame);
    std::printf("            worst joint departure from the rest pose: %.0f mm\n",
                static_cast<f64>(summary.worst_departure * 1000.0F));
    // REPORTED AND NOT GATED, because it is a property of the SOURCES rather than of this engine:
    // all four Mixamo exports are IN-PLACE takes, with the root's horizontal travel removed at
    // export. The character therefore runs on the spot and the chequerboard does not slide under
    // it. Nothing here fakes a forward velocity to make the picture look better — the camera
    // follows the hips because a clip that did travel would need it, and on these four it does
    // nothing. The death is the exception: the fall's displacement is part of the animation.
    std::printf(
        "  travel    the hips reached %.2f m from where they started — these are in-place\n"
        "            exports, so locomotion does not move them and this is not a gate\n",
        static_cast<f64>(summary.furthest));
    std::printf("  buffers   the skinned vertex range alternated %u times, the pose offset %u\n",
                summary.vertex_offsets_seen, summary.pose_offsets_seen);
    std::printf(
        "  frame     drawn by the forward frame's prepass, shadow and opaque passes in "
        "all but %u frames; at most %u matrices uploaded in one frame\n",
        summary.frames_not_drawn, summary.most_uploaded);
    std::printf("  validation errors: %u\n", summary.validation_errors);
}

/// EVERY ONE OF THESE IS A WAY THE PICTURE COULD LOOK RIGHT AND BE WRONG, so each is a non-zero
/// exit rather than a line in a log: a machine that never left idle, a pose that never left the
/// bind pose, a pose that moved once and then froze, a double-buffered range that never flipped, or
/// a frame the validation layer objected to.
[[nodiscard]] int gate(const Summary& summary, u32 visited) noexcept {
    int status = 0;
    if (visited != pose::kLocomotionStateCount) {
        std::fprintf(stderr, "GAP: the machine did not reach every state\n");
        status = 1;
    }
    if (summary.worst_departure < 0.10F) {
        std::fprintf(
            stderr,
            "GAP: no joint ever left its rest pose by 100 mm, so the clips are not driving "
            "the skeleton\n");
        status = 1;
    }
    if (summary.worst_step < 0.001F) {
        std::fprintf(stderr,
                     "GAP: no joint moved a millimetre between two frames, so the pose is frozen — "
                     "which is what a skinned character and a posed one look identical as\n");
        status = 1;
    }
    if (summary.vertex_offsets_seen < 2 || summary.pose_offsets_seen < 2) {
        std::fprintf(stderr,
                     "GAP: a double-buffered range never alternated, so one of the two buffers is "
                     "being read every frame\n");
        status = 1;
    }
    if (summary.validation_errors != 0) {
        std::fprintf(stderr, "GAP: %u Vulkan validation errors\n", summary.validation_errors);
        status = 1;
    }
    if (summary.frames_not_drawn != 0) {
        std::fprintf(stderr,
                     "GAP: %u frames did not draw the character from the skinning output in the "
                     "frame's prepass, shadow and opaque passes\n",
                     summary.frames_not_drawn);
        status = 1;
    }
    return status;
}

/// Import the four files and report what came of each. Separated from `run()` because a missing
/// source file is the ONE failure a person running this is likely to hit, and it deserves a message
/// that says where the program looked.
[[nodiscard]] bool load(Allocator& memory, const Options& options, Character& out) noexcept {
    const CharacterSources sources = default_sources(options.sources);
    if (Status loaded = load_character(memory, sources, out); !loaded) {
        std::fprintf(stderr, "\nthe character could not be loaded: %s\n", loaded.error().message);
        for (const SourceReport& source : out.sources) {
            if (!source.file.empty()) {
                std::fprintf(stderr, "  looked at %s\n", source.file.c_str());
            }
        }
        std::fprintf(stderr,
                     "\nthe four Mixamo exports live outside the repository; pass --sources with "
                     "the directory holding them.\n");
        return false;
    }
    print_sources(out);
    print_skin(out.skin);
    return true;
}

int run(const Options& options) noexcept {
    Allocator& memory = allocator();

    std::printf("cy_sample_animated-character\n");
    std::printf("  sources   %s\n", options.sources.c_str());
    std::printf("  capture   %ux%u at %u fps for %.2f s\n", options.width, options.height,
                options.fps, static_cast<f64>(options.seconds));

    Character character(memory);
    if (!load(memory, options, character)) {
        return 1;
    }
    AssetHost host;
    if (Status opened = host.open(character); !opened) {
        std::fprintf(stderr, "the asset system would not start: %s\n", opened.error().message);
        return 1;
    }
    Expected<const animation::AnimationRig*, Error> rig = bind_rig(host);
    if (!rig) {
        return 1;
    }
    World world(options.fps);
    if (Status opened = world.open(**rig); !opened) {
        std::fprintf(stderr, "the simulation would not start: %s\n", opened.error().message);
        return 1;
    }

    Stage stage(memory);
    if (Status opened = stage.open(options.width, options.height); !opened) {
        std::fprintf(stderr, "the stage would not open: %s\n", opened.error().message);
        return 1;
    }
    if (!stage.available()) {
        std::printf("\nno graphics device answered: %s\n", stage.absence());
        std::printf(
            "the import, the cook and the load all ran; only the picture needs a device.\n");
        return 2;
    }
    if (Status staged = stage.stage_character(character); !staged) {
        std::fprintf(stderr, "the character would not stage: %s\n", staged.error().message);
        return 1;
    }

    Summary summary;
    const Status played = play(options, character, world, stage, summary);
    u32 visited = 0;
    print_summary(options, summary, visited);
    const int status = played ? gate(summary, visited) : 1;
    stage.close();
    return status;
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse(argc, argv, options)) {
        std::fprintf(stderr,
                     "usage: cy_sample_animated-character [--sources <dir>] [--frames <dir>] "
                     "[--still <path>] [--still-frame <n>] [--width <n>] [--height <n>] "
                     "[--fps <n>] [--seconds <s>]\n");
        return 2;
    }
    return run(options);
}
