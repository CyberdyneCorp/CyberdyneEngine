// samples/09b-animated-character — M8.d's artefact: four Mixamo files, one character, one video.
//
// ================================================================================================
// WHAT THIS PROGRAM CLAIMS, AND WHAT IT DOES NOT
// ================================================================================================
//
// IT CLAIMS: four FBX files go through `tools/import/`'s steps 7 and 8 into a `cy::animation`
// skeleton and four clips; three of those clips are RETARGETED onto the fourth's rig by
// `retarget_build.h`'s measured correspondence; a four-state machine compiled by
// `cy::graph::pose::compile_locomotion` drives idle to walk to run to a death; every frame's pose
// is published into a `cy::animation::PoseWorld`; and the bone matrices that come out of it move
// vertices in a COMPUTE PASS on a real device, whose output buffer a rasteriser then draws. Nothing
// on the CPU writes the vertices that appear in the picture.
//
// IT DOES NOT CLAIM that the skin weights are the artist's — they are derived, and `character.h`
// says so at the top and in the report this program prints — nor that a skinned mesh goes through
// `cy::rendering::pipeline`'s forward frame, which `stage.h` explains it cannot yet.
//
// ================================================================================================
// THE TIMESTEP IS FIXED, AND THAT IS A REPRODUCIBILITY CLAIM
// ================================================================================================
//
// Every frame advances the animation by exactly 1/`--fps` seconds and the camera by the same, so
// two runs of this program produce the same pose at the same frame index. There is no wall clock
// anywhere in the loop. That is what makes the video a thing a reviewer can regenerate and compare
// rather than a recording of one afternoon — and it is the same argument
// `determinism-and-replay` makes about a simulation tick.
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
// The take runs to thirteen seconds because the death clip is 4.6 s long and CLAMPS on its last
// frame rather than looping. Cutting at ten would show a character mid-fall and leave a viewer to
// guess what happened to it.

#include <cy/animation/evaluate.h>
#include <cy/animation/lod.h>
#include <cy/animation/pose_world.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/graph/locomotion.h>

#include "character.h"
#include "stage.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
        std::printf("    %-5s %s\n", motion_name(static_cast<Motion>(index)).c_str(),
                    report.file.c_str());
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
    std::printf(
        "\n  skin  DERIVED, NOT IMPORTED — see samples/09b-animated-character/character.h\n");
    std::printf("    vertices=%u triangles=%u bones given weight=%u\n", skin.vertices,
                skin.triangles, skin.bones_used);
    std::printf("    worst distance from a vertex to its nearest bone: %.1f mm\n",
                static_cast<f64>(skin.worst_bind_distance * 1000.0F));
    std::printf("    mesh bounds  (%.3f %.3f %.3f) to (%.3f %.3f %.3f)\n",
                static_cast<f64>(skin.mesh_min.x), static_cast<f64>(skin.mesh_min.y),
                static_cast<f64>(skin.mesh_min.z), static_cast<f64>(skin.mesh_max.x),
                static_cast<f64>(skin.mesh_max.y), static_cast<f64>(skin.mesh_max.z));
    std::printf("    rig bounds   (%.3f %.3f %.3f) to (%.3f %.3f %.3f)\n",
                static_cast<f64>(skin.rig_min.x), static_cast<f64>(skin.rig_min.y),
                static_cast<f64>(skin.rig_min.z), static_cast<f64>(skin.rig_max.x),
                static_cast<f64>(skin.rig_max.y), static_cast<f64>(skin.rig_max.z));
}

/// Build the spec the four imported clips describe. The durations are the clips' own, because
/// `ClipRef::duration` comes from the GRAPH and `LocomotionDriver::clip_time` is what reads it —
/// a spec that guessed would wrap the wrong clip at the wrong moment.
[[nodiscard]] pose::LocomotionSpec spec_for(const Character& character) noexcept {
    pose::LocomotionSpec spec;
    spec.name = Name::intern("locomotion");
    const auto describe = [&](Motion motion) noexcept {
        pose::LocomotionClip clip;
        clip.clip = motion_name(motion);
        clip.duration = character.clips[static_cast<u32>(motion)].duration();
        clip.looping = motion != Motion::Die;
        return clip;
    };
    spec.idle = describe(Motion::Idle);
    spec.walk = describe(Motion::Walk);
    spec.run = describe(Motion::Run);
    spec.die = describe(Motion::Die);
    return spec;
}

/// The clip table `AnimationRig::bind` takes, parallel to `program.clips()` and matched BY NAME.
///
/// The matching is the caller's — `evaluate.h` says so — and it is the one place a program and the
/// content it names are joined. A null entry is legal and yields the reference pose, which is
/// exactly the silent failure this artefact must not ship: a missing clip would make the character
/// stand in its bind pose with nothing reporting it, so an unmatched name is an error here.
[[nodiscard]] Status build_clip_table(const pose::PoseProgram& program, Character& character,
                                      Array<const animation::Clip*>& out) noexcept {
    if (Status sized = out.resize(program.clips().size()); !sized) {
        return sized;
    }
    for (usize index = 0; index < program.clips().size(); ++index) {
        const Name wanted = program.clips()[index].name;
        out[index] = nullptr;
        for (u32 motion = 0; motion < kMotionCount; ++motion) {
            if (character.clips[motion].name() == wanted) {
                out[index] = &character.clips[motion];
                break;
            }
        }
        if (out[index] == nullptr) {
            return fail(ErrorCode::NotFound,
                        "the compiled program names a clip the import did not produce");
        }
    }
    return ok();
}

/// The clip a state samples, or null when its root is not a clip instruction.
///
/// `LocomotionDriver::write_clock` walks exactly this path privately; it is written out here
/// because this program owns its own clocks (see `StateClocks` below) and therefore has to reach
/// the same `ClipRef` the driver would have.
[[nodiscard]] const pose::ClipRef* clip_of_state(const pose::PoseProgram& program,
                                                 u16 state) noexcept {
    if (state >= program.states().size()) {
        return nullptr;
    }
    const pose::PoseValue root = program.states()[state].root;
    if (root == pose::kNoPoseValue || root >= program.code().size()) {
        return nullptr;
    }
    const pose::PoseInstruction& instruction = program.code()[root];
    if (instruction.op != pose::PoseOp::SampleClip || instruction.clip >= program.clips().size()) {
        return nullptr;
    }
    return &program.clips()[instruction.clip];
}

/// One clock per state, advanced by this program and never reset by the state machine.
///
/// WHY THIS EXISTS, AND IT IS A DEFECT WORKED AROUND RATHER THAN A PREFERENCE.
/// `LocomotionDriver::follow` derives a state's clip time from `PoseInstance::state_time`, and
/// `graph::pose::advance` sets `state_time = 0` at the instant a blend COMPLETES
/// (src/graph/src/lower_pose.cpp). During the blend the incoming state is the target and its clock
/// is driven from `blend_elapsed`, so it has already reached the blend's duration by the time the
/// blend ends — and then restarts at zero. The incoming clip therefore jumps backwards by one blend
/// duration on the frame it becomes the active state: 0.15 s of a 0.633 s run cycle, which this
/// program's own `worst joint step between two frames` measured at 802 mm in one thirtieth of a
/// second. `animation::advance`'s `reset_state_times` does the same thing by a different route.
///
/// A clock a state machine resets is a clock the HOST cannot keep continuous, and `locomotion.h` is
/// explicit that the clock is the host's: "nothing in the program advances it, because a clock the
/// program owned would be a clock every character shared". So this program advances its own, zeroes
/// a state's clock when that state becomes a blend TARGET — which is the moment it starts being
/// sampled — and leaves it alone when the blend completes. The fix belongs in src/graph/, which
/// this artefact does not own, and is reported as a finding.
class StateClocks {
public:
    void tick(const pose::PoseProgram& program, const pose::PoseInstance& machine,
              f32 dt) noexcept {
        if (machine.target != previous_target_ && machine.target < pose::kLocomotionStateCount) {
            elapsed_[machine.target] = 0.0F;
        }
        previous_target_ = machine.target;
        if (machine.state < pose::kLocomotionStateCount) {
            elapsed_[machine.state] += dt;
        }
        if (machine.target < pose::kLocomotionStateCount && machine.target != machine.state) {
            elapsed_[machine.target] += dt;
        }
        (void)program;
    }

    /// Write every state's clip time into `driver`, through `clip_time` so that the compiled
    /// `ClipRef::looping` is what decides whether a clock wraps or clamps.
    void publish(const pose::PoseProgram& program, pose::LocomotionDriver& driver) const noexcept {
        for (u32 index = 0; index < pose::kLocomotionStateCount; ++index) {
            const pose::ClipRef* reference = clip_of_state(program, static_cast<u16>(index));
            if (reference == nullptr) {
                continue;
            }
            driver.set_clock(static_cast<pose::LocomotionState>(index),
                             pose::clip_time(*reference, elapsed_[index]));
        }
    }

private:
    f32 elapsed_[pose::kLocomotionStateCount] = {};
    u16 previous_target_ = 0xFFFFU;
};

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

/// Everything one character needs to be evaluated and published, bound once.
///
/// A struct rather than a dozen locals in `run()`, because binding a rig is nine calls that each
/// fail differently and the loop that follows is the part worth reading.
struct Take {
    explicit Take(Allocator& memory) noexcept
        : table(memory),
          rig(memory),
          instance(memory),
          scratch(memory),
          driver(memory),
          world(memory),
          local(memory),
          model(memory),
          matrices(memory),
          previous_model(memory) {}

    Take(const Take&) = delete;
    Take& operator=(const Take&) = delete;

    [[nodiscard]] Status prepare(const pose::PoseProgram& program, Character& character) noexcept {
        if (Status built = build_clip_table(program, character, table); !built) {
            return built;
        }
        if (Status bound = rig.bind(character.skeleton, program, table.span()); !bound) {
            return bound;
        }
        if (Status prepared = instance.prepare(rig); !prepared) {
            return prepared;
        }
        if (Status prepared = scratch.prepare(rig); !prepared) {
            return prepared;
        }
        if (Status bound = driver.bind(program); !bound) {
            return bound;
        }
        const u16 joints = character.skeleton.joint_count();
        Expected<animation::PoseHandle, Error> added = world.add(joints);
        if (!added) {
            return make_unexpected(added.error());
        }
        handle = *added;
        // The previous frame's model pose is kept for the one measurement that proves the claim:
        // how far a joint moved between two frames. See `Summary::worst_step`.
        if (!local.resize(joints) || !model.resize(joints) || !matrices.resize(joints) ||
            !previous_model.resize(joints)) {
            return fail(ErrorCode::OutOfMemory, "the pose buffers would not size");
        }
        return ok();
    }

    Array<const animation::Clip*> table;
    animation::AnimationRig rig;
    animation::AnimationInstance instance;
    animation::PoseScratch scratch;
    pose::LocomotionDriver driver;
    animation::PoseWorld world;
    animation::PoseHandle handle;
    Array<Transform> local;
    Array<Transform> model;
    Array<Mat4> matrices;
    Array<Transform> previous_model;
};

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
};

/// Fold one frame's model pose into the measurements, and keep it for the next frame's comparison.
void measure(const Character& character, Take& take, u32 frame, bool have_previous,
             Summary& out) noexcept {
    const u16 joints = character.skeleton.joint_count();
    for (u16 joint = 0; joint < joints; ++joint) {
        const Vec3 rest = character.skeleton.bind_model()[joint].translation;
        out.worst_departure =
            math::max(out.worst_departure, length(take.model[joint].translation - rest));
        if (have_previous) {
            const f32 step =
                length(take.model[joint].translation - take.previous_model[joint].translation);
            if (step > out.worst_step) {
                out.worst_step = step;
                out.worst_step_frame = frame;
            }
        }
        take.previous_model[joint] = take.model[joint];
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

/// One frame: request, advance, clock, evaluate, publish, skin, draw.
[[nodiscard]] Status step(const Options& options, const pose::PoseProgram& program,
                          Character& character, Take& take, StateClocks& clocks, Stage& stage,
                          u32 frame, f32 dt, Vec3& follow, FrameReport& report) noexcept {
    const f32 seconds = static_cast<f32>(frame) * dt;

    // 1. The request. Exactly one is raised; the machine decides whether it has an edge for it.
    take.driver.request(requested_at(seconds));
    Span<f32> parameters = take.instance.parameters();
    for (usize index = 0; index < parameters.size(); ++index) {
        parameters[index] = take.driver.parameters()[index];
    }

    // 2. The deterministic half: the state machine, the clip clocks, root motion and events.
    if (Status advanced = animation::advance(take.rig, take.instance, dt, nullptr); !advanced) {
        return advanced;
    }

    // 3. THE CLOCKS ARE THIS PROGRAM'S, AND THERE ARE TWO REASONS.
    //
    //    `animation::advance` wraps every clip clock by that clip's DURATION whatever its loop mode
    //    is — `AnimationRig::bind` records a duration per time parameter and nothing there reads
    //    `LoopMode` or the compiled `ClipRef::looping` — so a death, which must stop on its last
    //    frame, restarts. `clip_time()` is the one place the compiled loop flag is acted on, and
    //    `StateClocks` is why the elapsed time handed to it is kept here rather than read back out
    //    of the state machine.
    //
    //    Overwriting AFTER the advance rather than instead of it keeps the state machine, the
    //    root-motion integration and the event emission on the runtime's own path.
    clocks.tick(program, take.instance.machine(), dt);
    clocks.publish(program, take.driver);
    for (usize index = 0; index < parameters.size(); ++index) {
        parameters[index] = take.driver.parameters()[index];
    }

    // 4. The pose. The seed is mandatory: only the joints a track wrote are copied out, so a joint
    //    no clip drives keeps whatever it was handed.
    character.skeleton.reference_pose(take.local.span());
    animation::EvaluationStats stats;
    if (Status evaluated =
            animation::evaluate(take.rig, take.instance, 0, take.scratch, take.local.span(), stats);
        !evaluated) {
        return evaluated;
    }

    // 5. Local to model to skinning matrices, published into the pose world. `matrix_offset` moves
    //    on every publish, which is what the double buffering IS.
    if (Status published =
            animation::publish_pose(character.skeleton, take.local.span(), 0, take.world,
                                    take.handle, take.model.span(), take.matrices.span());
        !published) {
        return published;
    }

    const Vec3 hips = take.model[0].translation;
    follow = frame == 0 ? hips : follow + ((hips - follow) * 0.08F);
    const Shot shot = compose(follow, seconds);
    const u32 pose_offset = take.world.matrix_offset(take.handle);

    char path[512];
    (void)std::snprintf(path, sizeof(path), "%s/frame_%04u.png", options.frames.c_str(), frame);
    const char* target = options.frames.empty() ? nullptr : path;
    if (Status taken = stage.shoot(take.world.matrices(), pose_offset, shot, frame, target, report);
        !taken) {
        return taken;
    }
    if (frame == options.still_frame && !options.still.empty()) {
        return stage.shoot(take.world.matrices(), pose_offset, shot, frame, options.still.c_str(),
                           report);
    }
    return ok();
}

/// Every frame of the take.
[[nodiscard]] Status play(const Options& options, const pose::PoseProgram& program,
                          Character& character, Take& take, Stage& stage, Summary& out) noexcept {
    const f32 dt = 1.0F / static_cast<f32>(options.fps);
    out.frames = static_cast<u32>(std::lround(options.seconds * static_cast<f32>(options.fps)));
    // Seeded from the first frame's hips inside `step`, so the camera does not sweep in from
    // the origin over the first second of the take.
    Vec3 follow{0.0F, 0.0F, 0.0F};
    StateClocks clocks;
    FrameReport report;
    u32 last_vertex_offset = 0xFFFFFFFFU;
    u32 last_pose_offset = 0xFFFFFFFFU;

    for (u32 frame = 0; frame < out.frames; ++frame) {
        if (Status stepped =
                step(options, program, character, take, clocks, stage, frame, dt, follow, report);
            !stepped) {
            std::fprintf(stderr, "frame %u failed: %s\n", frame, stepped.error().message);
            return stepped;
        }
        measure(character, take, frame, frame != 0, out);

        const u16 state = take.instance.machine().state;
        if (state < pose::kLocomotionStateCount) {
            out.states_entered[state] = 1;
        }
        const Vec3 hips = take.model[0].translation;
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
        if ((frame % 30U) == 0U) {
            std::printf("    frame %3u  t=%5.2fs  state=%-5s  hips=(%.2f, %.2f, %.2f)\n", frame,
                        static_cast<f64>(static_cast<f32>(frame) * dt),
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

/// Compile the four-state machine over the four imported clips, and print what it compiled to.
[[nodiscard]] Expected<pose::PoseProgram, Error> compile(Allocator& memory,
                                                         const Character& character) noexcept {
    graph::DiagnosticSink sink(memory);
    Expected<pose::PoseProgram, Error> compiled = pose::compile_locomotion(
        memory, spec_for(character), character.skeleton.joint_count(), sink);
    if (!compiled) {
        std::fprintf(stderr, "the locomotion graph did not compile: %s\n",
                     compiled.error().message);
        for (const graph::Diagnostic& entry : sink.entries()) {
            std::fprintf(stderr, "  %s\n", entry.message);
        }
        return compiled;
    }
    // THE COMPILED CLIP TABLE, printed because `ClipRef::looping` is the one thing about a clip the
    // compiled program carries that nothing else in this report would show — and a death that
    // arrived as `looping=yes` would play forever with the picture looking plausible throughout.
    for (const pose::ClipRef& reference : compiled->clips()) {
        std::printf("  clip ref  %-5s duration=%.3fs looping=%s\n", reference.name.c_str(),
                    static_cast<f64>(reference.duration), reference.looping ? "yes" : "no");
    }
    std::printf("\n  program   states=%u transitions=%u clips=%u parameters=%u joints=%u\n",
                static_cast<u32>(compiled->states().size()),
                static_cast<u32>(compiled->transitions().size()),
                static_cast<u32>(compiled->clips().size()),
                static_cast<u32>(compiled->parameters().size()), compiled->joint_count());
    return compiled;
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
    Expected<pose::PoseProgram, Error> program = compile(memory, character);
    if (!program) {
        return 1;
    }
    Take take(memory);
    if (Status prepared = take.prepare(*program, character); !prepared) {
        std::fprintf(stderr, "the rig would not bind: %s\n", prepared.error().message);
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
            "the import, the retarget and the pose program all ran; only the picture needs a "
            "device.\n");
        return 2;
    }
    if (Status staged = stage.stage_character(character); !staged) {
        std::fprintf(stderr, "the character would not stage: %s\n", staged.error().message);
        return 1;
    }

    Summary summary;
    const Status played = play(options, *program, character, take, stage, summary);
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
