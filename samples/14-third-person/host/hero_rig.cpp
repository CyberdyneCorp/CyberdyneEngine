// SPDX-License-Identifier: MIT
// The hero's rig: authored, cooked, loaded. See hero_rig.h.

#include "hero_rig.h"

#include <cy/animation/clip.h>
#include <cy/animation/cooked.h>
#include <cy/animation/skeleton.h>
#include <cy/core/assets/package.h>
#include <cy/core/math/quat.h>
#include <cy/core/memory/ownership.h>
#include <cy/graph/cybergraph.h>
#include <cy/graph/lower_pose.h>

#include <cmath>
#include <cstdio>
#include <numbers>
#include <string_view>
#include <utility>

namespace sample::tps {
namespace {

using cy::AssetId;
using cy::f32;
using cy::Name;
using cy::Quat;
using cy::Status;
using cy::u16;
using cy::u32;
using cy::Vec3;
using cy::Vec4;
namespace animation = cy::animation;
namespace graph = cy::graph;

/// Fixed ids beside the RTS sample's `0x13A0...`: the records are cooked again on every run.
constexpr cy::u64 kNamespace = 0x14A0'0000'0000'0001ULL;
constexpr AssetId kSkeletonId{kNamespace, 1};
constexpr AssetId kProgramId{kNamespace, 2};

constexpr HeroJointRow kSkeleton[kHeroJoints] = {
    {"root", animation::kInvalidJoint, Vec3{0.0F, 0.0F, 0.0F}},
    {"pelvis", kRoot, Vec3{0.0F, 0.95F, 0.0F}},
    {"spine", kPelvis, Vec3{0.0F, 0.12F, 0.0F}},
    {"chest", kSpine, Vec3{0.0F, 0.24F, 0.0F}},
    {"neck", kChest, Vec3{0.0F, 0.22F, 0.0F}},
    {"head", kNeck, Vec3{0.0F, 0.10F, 0.0F}},
    {"l_shoulder", kChest, Vec3{-0.19F, 0.17F, 0.0F}},
    {"l_elbow", kShoulderL, Vec3{0.0F, -0.28F, 0.0F}},
    {"l_wrist", kElbowL, Vec3{0.0F, -0.25F, 0.0F}},
    {"r_shoulder", kChest, Vec3{0.19F, 0.17F, 0.0F}},
    {"r_elbow", kShoulderR, Vec3{0.0F, -0.28F, 0.0F}},
    {"r_wrist", kElbowR, Vec3{0.0F, -0.25F, 0.0F}},
    // The knees sit a touch forward of the hip-ankle line, so a two-bone solve knows which way
    // they bend: forward, toward −Z, foot placement's `pole`.
    {"l_hip", kPelvis, Vec3{-0.10F, -0.05F, 0.0F}},
    {"l_knee", kHipL, Vec3{0.0F, -0.41F, -0.02F}},
    {"l_ankle", kKneeL, Vec3{0.0F, -0.41F, 0.02F}},
    {"l_toe", kAnkleL, Vec3{0.0F, -0.06F, -0.13F}},
    {"r_hip", kPelvis, Vec3{0.10F, -0.05F, 0.0F}},
    {"r_knee", kHipR, Vec3{0.0F, -0.41F, -0.02F}},
    {"r_ankle", kKneeR, Vec3{0.0F, -0.41F, 0.02F}},
    {"r_toe", kAnkleR, Vec3{0.0F, -0.06F, -0.13F}},
};

constexpr f32 kTau = 6.28318531F;
constexpr f32 kKeyRate = 30.0F;
constexpr f32 kPelvisHeight = 0.95F;

/// One sample of a clip: each joint's rotation as Euler angles (x pitch, y yaw, z roll, applied
/// yaw-pitch-roll) and the pelvis' translation.
struct ClipPose {
    Vec3 euler[kHeroJoints] = {};
    Vec3 pelvis{0.0F, kPelvisHeight, 0.0F};
};

using PoseFn = void (*)(f32 phase, ClipPose& out);

/// A smooth bump of height one at `centre`, zero beyond `width` from it, on a cycle.
[[nodiscard]] f32 bump(f32 phase, f32 centre, f32 width) noexcept {
    f32 distance = std::fabs(phase - centre);
    distance = std::fmin(distance, 1.0F - distance);
    if (distance >= width) {
        return 0.0F;
    }
    const f32 t = distance / width;
    return 0.5F * (1.0F + std::cos(t * std::numbers::pi_v<f32>));
}

[[nodiscard]] f32 wrap01(f32 phase) noexcept {
    return phase - std::floor(phase);
}

[[nodiscard]] f32 smooth(f32 t) noexcept {
    const f32 c = std::fmin(std::fmax(t, 0.0F), 1.0F);
    return c * c * (3.0F - (2.0F * c));
}

/// How a leg moves through a stride: swing, knee flex and ankle by phase, contact at zero.
struct Stride {
    f32 hip_swing = 0.42F;
    f32 hip_bias = -0.02F;
    f32 knee_swing = 0.95F;
    f32 knee_load = 0.18F;
    f32 knee_rest = 0.10F;
    f32 push_off = 0.35F;
};

void leg(const Stride& stride, f32 phase, u16 hip, u16 knee, u16 ankle, ClipPose& out) noexcept {
    out.euler[hip].x = (stride.hip_swing * std::cos(kTau * phase)) + stride.hip_bias;
    out.euler[knee].x = -(stride.knee_rest + (stride.knee_swing * bump(phase, 0.76F, 0.22F)) +
                          (stride.knee_load * bump(phase, 0.1F, 0.09F)));
    out.euler[ankle].x = (0.12F * bump(phase, 0.0F, 0.12F)) -
                         (stride.push_off * bump(phase, 0.6F, 0.11F)) +
                         (0.10F * bump(phase, 0.82F, 0.12F));
}

void walk_pose(f32 p, ClipPose& out) noexcept {
    const Stride stride;
    leg(stride, p, kHipL, kKneeL, kAnkleL, out);
    leg(stride, wrap01(p + 0.5F), kHipR, kKneeR, kAnkleR, out);
    const f32 swing = std::cos(kTau * p);
    out.pelvis.y = kPelvisHeight - 0.015F + (0.015F * std::cos(2.0F * kTau * (p - 0.25F)));
    out.euler[kPelvis].y = 0.07F * swing;
    out.euler[kSpine].x = -0.04F;
    out.euler[kChest].y = -0.10F * swing;
    out.euler[kHead].y = 0.03F * swing;
    out.euler[kShoulderL] = Vec3{-0.32F * swing, 0.0F, -0.08F};
    out.euler[kShoulderR] = Vec3{0.32F * swing, 0.0F, 0.08F};
    out.euler[kElbowL].x = 0.28F + (0.12F * std::fmax(0.0F, -swing));
    out.euler[kElbowR].x = 0.28F + (0.12F * std::fmax(0.0F, swing));
}

void run_pose(f32 p, ClipPose& out) noexcept {
    Stride stride;
    stride.hip_swing = 0.72F;
    stride.hip_bias = 0.08F;
    stride.knee_swing = 1.55F;
    stride.knee_load = 0.35F;
    stride.knee_rest = 0.22F;
    stride.push_off = 0.5F;
    leg(stride, p, kHipL, kKneeL, kAnkleL, out);
    leg(stride, wrap01(p + 0.5F), kHipR, kKneeR, kAnkleR, out);
    const f32 swing = std::cos(kTau * p);
    out.pelvis.y = kPelvisHeight - 0.06F + (0.03F * std::cos(2.0F * kTau * (p - 0.3F)));
    out.euler[kPelvis].y = 0.10F * swing;
    out.euler[kSpine].x = -0.20F;
    out.euler[kChest].y = -0.16F * swing;
    out.euler[kNeck].x = 0.12F;
    out.euler[kShoulderL] = Vec3{-0.75F * swing, 0.0F, -0.12F};
    out.euler[kShoulderR] = Vec3{0.75F * swing, 0.0F, 0.12F};
    out.euler[kElbowL].x = 1.45F + (0.15F * swing);
    out.euler[kElbowR].x = 1.45F - (0.15F * swing);
}

void idle_pose(f32 p, ClipPose& out) noexcept {
    const f32 breath = std::sin(kTau * p);
    const f32 shift = std::sin(kTau * p * 0.5F);
    out.pelvis.y = kPelvisHeight - 0.012F + (0.004F * breath);
    out.pelvis.x = 0.012F * shift;
    out.euler[kPelvis].z = 0.02F * shift;
    out.euler[kSpine].z = -0.02F * shift;
    out.euler[kChest].x = 0.025F * breath;
    out.euler[kHead].x = -0.02F * breath;
    out.euler[kShoulderL] = Vec3{0.03F * breath, 0.0F, -0.10F};
    out.euler[kShoulderR] = Vec3{0.03F * breath, 0.0F, 0.10F};
    out.euler[kElbowL].x = 0.18F;
    out.euler[kElbowR].x = 0.18F;
    for (const u16 hip : {kHipL, kHipR}) {
        out.euler[hip].x = 0.04F;
    }
    for (const u16 knee : {kKneeL, kKneeR}) {
        out.euler[knee].x = -0.08F;
    }
    for (const u16 ankle : {kAnkleL, kAnkleR}) {
        out.euler[ankle].x = 0.04F;
    }
}

void jump_pose(f32 p, ClipPose& out) noexcept {
    const f32 s = smooth(p * 1.6F);
    out.pelvis.y = kPelvisHeight + (0.04F * s);
    out.euler[kSpine].x = -0.12F * s;
    out.euler[kHipL].x = 0.95F * s;
    out.euler[kHipR].x = 0.55F * s;
    out.euler[kKneeL].x = -1.35F * s;
    out.euler[kKneeR].x = -0.95F * s;
    out.euler[kAnkleL].x = -0.35F * s;
    out.euler[kAnkleR].x = -0.30F * s;
    out.euler[kShoulderL] = Vec3{1.1F * s, 0.0F, -0.35F * s};
    out.euler[kShoulderR] = Vec3{0.7F * s, 0.0F, 0.35F * s};
    out.euler[kElbowL].x = 0.5F * s;
    out.euler[kElbowR].x = 0.7F * s;
}

void fall_pose(f32 p, ClipPose& out) noexcept {
    const f32 flail = std::sin(kTau * p);
    out.euler[kSpine].x = -0.06F;
    out.euler[kHipL].x = 0.40F + (0.12F * flail);
    out.euler[kHipR].x = -0.10F - (0.12F * flail);
    out.euler[kKneeL].x = -0.65F + (0.08F * flail);
    out.euler[kKneeR].x = -0.45F - (0.08F * flail);
    out.euler[kAnkleL].x = -0.2F;
    out.euler[kAnkleR].x = -0.2F;
    out.euler[kShoulderL] = Vec3{0.25F, 0.0F, -1.05F - (0.15F * flail)};
    out.euler[kShoulderR] = Vec3{0.25F, 0.0F, 1.05F - (0.15F * flail)};
    out.euler[kElbowL].x = 0.45F;
    out.euler[kElbowR].x = 0.45F;
}

void land_pose(f32 p, ClipPose& out) noexcept {
    const f32 crouch = p < 0.3F ? smooth(p / 0.3F) : 1.0F - smooth((p - 0.3F) / 0.7F);
    out.pelvis.y = kPelvisHeight - (0.13F * crouch);
    out.euler[kSpine].x = -0.22F * crouch;
    for (const u16 hip : {kHipL, kHipR}) {
        out.euler[hip].x = 0.6F * crouch;
    }
    for (const u16 knee : {kKneeL, kKneeR}) {
        out.euler[knee].x = -1.1F * crouch;
    }
    for (const u16 ankle : {kAnkleL, kAnkleR}) {
        out.euler[ankle].x = 0.5F * crouch;
    }
    out.euler[kShoulderL] = Vec3{0.35F * crouch, 0.0F, -0.25F * crouch};
    out.euler[kShoulderR] = Vec3{0.35F * crouch, 0.0F, 0.25F * crouch};
    out.euler[kElbowL].x = 0.5F * crouch;
    out.euler[kElbowR].x = 0.5F * crouch;
}

/// One state: its clip, how long it lasts, whether it loops, how it moves, and where its feet fall
/// as a fraction of the cycle (none for a clip whose feet do not).
struct StateRow {
    const char* state;
    const char* clip;
    f32 duration;
    bool looping;
    PoseFn pose;
    bool strides;
};

constexpr StateRow kStates[] = {
    {"locomotion", "hero_idle", 2.4F, true, &idle_pose, true},
    {"locomotion", "hero_walk", 1.1F, true, &walk_pose, true},
    {"locomotion", "hero_run", 0.7F, true, &run_pose, true},
    {"jump", "hero_jump", 0.45F, false, &jump_pose, false},
    {"fall", "hero_fall", 0.8F, true, &fall_pose, false},
    {"land", "hero_land", 0.35F, false, &land_pose, false},
};
constexpr u32 kClipCount = sizeof(kStates) / sizeof(kStates[0]);
constexpr AssetId kClipIds[kClipCount] = {{kNamespace, 10}, {kNamespace, 11}, {kNamespace, 12},
                                          {kNamespace, 13}, {kNamespace, 14}, {kNamespace, 15}};

[[nodiscard]] Vec4 rotation_of(Vec3 euler) noexcept {
    const Quat q = Quat::from_euler_yxz(euler);
    return Vec4{q.x, q.y, q.z, q.w};
}

[[nodiscard]] Status author_clip(animation::Clip& clip, const StateRow& row) noexcept {
    clip.set_name(Name::intern(row.clip));
    clip.set_duration(row.duration);
    clip.set_loop_mode(row.looping ? animation::LoopMode::Loop : animation::LoopMode::None);
    clip.set_sample_rate_hint(kKeyRate);
    const auto frames = static_cast<u32>(std::lround(row.duration * kKeyRate)) + 1U;
    const auto time_of = [&](u32 frame) noexcept {
        return frame + 1U == frames ? row.duration : static_cast<f32>(frame) / kKeyRate;
    };
    const auto pose_at = [&](u32 frame) noexcept {
        ClipPose pose;
        row.pose(time_of(frame) / row.duration, pose);
        return pose;
    };
    // The pelvis' translation, then a rotation track for every joint the clip moves. A track's keys
    // are authored before the next track is added.
    cy::Expected<u32, cy::Error> pelvis = clip.add_joint_track(
        animation::TrackKind::Translation, kPelvis, animation::Interpolation::Linear);
    if (!pelvis) {
        return Status{cy::make_unexpected(pelvis.error())};
    }
    for (u32 frame = 0; frame < frames; ++frame) {
        const Vec3 at = pose_at(frame).pelvis;
        if (Status added = clip.add_key(*pelvis, time_of(frame), Vec4{at.x, at.y, at.z, 0.0F});
            !added) {
            return added;
        }
    }
    for (u16 joint = kPelvis; joint < kHeroJoints; ++joint) {
        bool moves = false;
        for (u32 frame = 0; frame < frames && !moves; ++frame) {
            const Vec3 euler = pose_at(frame).euler[joint];
            moves = euler.x != 0.0F || euler.y != 0.0F || euler.z != 0.0F;
        }
        if (!moves) {
            continue;
        }
        cy::Expected<u32, cy::Error> track = clip.add_joint_track(
            animation::TrackKind::Rotation, joint, animation::Interpolation::Spherical);
        if (!track) {
            return Status{cy::make_unexpected(track.error())};
        }
        for (u32 frame = 0; frame < frames; ++frame) {
            if (Status added =
                    clip.add_key(*track, time_of(frame), rotation_of(pose_at(frame).euler[joint]));
                !added) {
                return added;
            }
        }
    }
    if (row.strides) {
        // MARKERS for the sync group — the left foot down at the start, the right at half way —
        // and a FOOTSTEP at each, its parameter the foot.
        const f32 half = row.duration * 0.5F;
        const std::pair<const char*, f32> markers[] = {{"l_down", 0.0F}, {"r_down", half}};
        for (const auto& [name, time] : markers) {
            if (Status added = clip.add_marker(Name::intern(name), time); !added) {
                return added;
            }
        }
        if (std::string_view(row.clip) != "hero_idle") {
            if (Status added = clip.add_event(Name::intern("footstep"), 0.02F * row.duration, 0.0F);
                !added) {
                return added;
            }
            if (Status added =
                    clip.add_event(Name::intern("footstep"), half + (0.02F * row.duration), 1.0F);
                !added) {
                return added;
            }
        }
    }
    return clip.compress(animation::CompressionSettings{});
}

// --- The program --------------------------------------------------------------------------------

[[nodiscard]] graph::Literal literal_name(const char* value) noexcept {
    graph::Literal literal;
    literal.type = Name::intern("name");
    literal.text = Name::intern(value);
    return literal;
}

[[nodiscard]] graph::Literal literal_number(f32 value) noexcept {
    graph::Literal literal;
    literal.type = Name::intern("float");
    literal.value = graph::Immediate::scalar(value);
    return literal;
}

[[nodiscard]] graph::Literal literal_bool(bool value) noexcept {
    graph::Literal literal;
    literal.type = Name::intern("bool");
    literal.value.mask = value ? 1U : 0U;
    return literal;
}

[[nodiscard]] graph::Literal literal_vec2(f32 x, f32 y) noexcept {
    graph::Literal literal;
    literal.type = Name::intern("vec2");
    literal.value.x = x;
    literal.value.y = y;
    return literal;
}

[[nodiscard]] graph::Literal literal_vec3(Vec3 value) noexcept {
    graph::Literal literal;
    literal.type = Name::intern("vec3");
    literal.value.x = value.x;
    literal.value.y = value.y;
    literal.value.z = value.z;
    return literal;
}

/// Authors a pose graph one node at a time, keeping the first failure.
class Authoring {
public:
    Authoring(graph::Graph& graph) noexcept : graph_(&graph) {}

    void node(graph::NodeKey key, const char* type) noexcept {
        keep(graph_->add_node(key, Name::intern(type)));
    }
    void property(graph::NodeKey key, const char* name, const graph::Literal& value) noexcept {
        keep(graph_->set_property(key, Name::intern(name), value));
    }
    void connect(graph::NodeKey from, graph::NodeKey to, const char* pin) noexcept {
        keep(graph_->connect(from, Name::intern("pose"), to, Name::intern(pin)));
    }
    /// A clip node playing `row`'s clip on a clock of its own.
    void clip(graph::NodeKey key, const StateRow& row, const char* clock) noexcept {
        node(key, "pose.clip");
        property(key, "clip", literal_name(row.clip));
        property(key, "duration", literal_number(row.duration));
        property(key, "loop", literal_bool(row.looping));
        property(key, "time_parameter", literal_name(clock));
    }
    /// Foot placement on both legs over pose `input`, faded by `feet`, into `state`.
    void feet(graph::NodeKey key, graph::NodeKey input) noexcept {
        node(key, "pose.foot_ik");
        const std::pair<const char*, const char*> joints[] = {
            {"pelvis", "pelvis"},   {"hip_l", "l_hip"},           {"knee_l", "l_knee"},
            {"ankle_l", "l_ankle"}, {"hip_r", "r_hip"},           {"knee_r", "r_knee"},
            {"ankle_r", "r_ankle"}, {"weight_parameter", "feet"},
        };
        for (const auto& [name, value] : joints) {
            property(key, name, literal_name(value));
        }
        property(key, "ankle_height", literal_number(0.08F));
        // The hips go down at most a riser and a bit: on a stair a walk's trailing foot can be
        // three treads below, and a hero that crouched to reach it would climb squatting.
        property(key, "max_pelvis_drop", literal_number(0.2F));
        property(key, "pole", literal_vec3(Vec3{0.0F, 0.0F, -1.0F}));
        property(key, "smoothing", literal_number(0.06F));
        connect(input, key, "a");
    }
    void state(graph::NodeKey key, const char* name, graph::NodeKey input) noexcept {
        node(key, "pose.state");
        property(key, "name", literal_name(name));
        connect(input, key, "pose");
    }

    [[nodiscard]] const Status& status() const noexcept { return status_; }

private:
    void keep(const Status& step) noexcept {
        if (status_ && !step) {
            status_ = step;
        }
    }

    graph::Graph* graph_;
    Status status_ = cy::ok();
};

/// The four states. Ascending state keys, so locomotion — the lowest — is the entry state.
[[nodiscard]] cy::Expected<graph::pose::PoseProgram, cy::Error> compile_program(
    cy::Allocator& allocator) noexcept {
    graph::NodeRegistry registry(allocator);
    if (Status registered = graph::pose::register_pose_nodes(registry); !registered) {
        return cy::make_unexpected(registered.error());
    }
    graph::Graph authored(allocator, Name::intern("hero"));
    Authoring author(authored);

    // LOCOMOTION: idle, walk and run on a synchronised one-dimensional space over `speed`.
    constexpr graph::NodeKey kSpace = 100;
    author.node(kSpace, "pose.blend1d");
    author.property(kSpace, "x_parameter", literal_name("speed"));
    author.property(kSpace, "sync", literal_bool(true));
    const f32 positions[3] = {0.0F, kWalkSpeed, kRunSpeed};
    for (u32 sample = 0; sample < 3U; ++sample) {
        char clock[32];
        (void)std::snprintf(clock, sizeof(clock), "clock_%s", kStates[sample].clip);
        char pin[8];
        (void)std::snprintf(pin, sizeof(pin), "s%u", sample);
        const graph::NodeKey clip = 101U + sample;
        author.clip(clip, kStates[sample], clock);
        author.connect(clip, kSpace, pin);
        author.property(kSpace, pin, literal_vec2(positions[sample], 0.0F));
    }
    author.feet(110, kSpace);
    author.state(10, "locomotion", 110);

    // JUMP, FALL, LAND: a clip each; the landing through foot placement.
    for (u32 index = 3; index < kClipCount; ++index) {
        const StateRow& row = kStates[index];
        char clock[32];
        (void)std::snprintf(clock, sizeof(clock), "clock_%s", row.clip);
        const graph::NodeKey clip = 200U + (index * 10U);
        author.clip(clip, row, clock);
        graph::NodeKey pose = clip;
        if (std::string_view(row.state) == "land") {
            author.feet(clip + 1U, clip);
            pose = clip + 1U;
        }
        author.state(10U + (static_cast<graph::NodeKey>(index) * 10U), row.state, pose);
    }
    if (!author.status()) {
        return cy::make_unexpected(author.status().error());
    }
    authored.resolve(registry);
    graph::DiagnosticSink sink(allocator);
    cy::Expected<graph::pose::PoseProgram, cy::Error> program =
        graph::pose::compile_pose(authored, registry, kHeroJoints, sink);
    if (program && sink.errors() != 0) {
        return cy::fail(cy::ErrorCode::InvalidArgument, "the hero's pose graph did not compile");
    }
    return program;
}

}  // namespace

const HeroJointRow* hero_skeleton() noexcept {
    return kSkeleton;
}

Vec3 hero_bind_position(u16 joint) noexcept {
    Vec3 position{0.0F, 0.0F, 0.0F};
    for (u16 at = joint; at < kHeroJoints; at = kSkeleton[at].parent) {
        position = position + kSkeleton[at].offset;
    }
    return position;
}

HeroRig::HeroRig(cy::Allocator& allocator) noexcept : allocator_(&allocator) {}

HeroRig::~HeroRig() {
    shutdown();
}

void HeroRig::shutdown() noexcept {
    rig_ = nullptr;
    library_.reset();
    if (started_) {
        assets_.shutdown();
        async_.stop();
        workers_.shutdown();
        started_ = false;
    }
}

const cy::animation::AnimationLibraryStats& HeroRig::stats() const noexcept {
    static const cy::animation::AnimationLibraryStats kNone{};
    return library_ ? library_->stats() : kNone;
}

Status HeroRig::store(AssetId id, const cy::Array<cy::u8>& bytes) noexcept {
    cy::Expected<cy::assets::VirtualPath, cy::Error> path = cy::assets::package_entry_path(id, {});
    if (!path) {
        return Status{cy::make_unexpected(path.error())};
    }
    ++records_;
    return files_.write(*path, bytes.data(), bytes.size());
}

Status HeroRig::cook() noexcept {
    animation::Skeleton skeleton(*allocator_);
    skeleton.set_name(Name::intern("hero"));
    for (const HeroJointRow& row : kSkeleton) {
        if (cy::Expected<u16, cy::Error> added = skeleton.add_joint(
                Name::intern(row.name), row.parent, cy::Transform::from_translation(row.offset));
            !added) {
            return Status{cy::make_unexpected(added.error())};
        }
    }
    if (Status finalized = skeleton.finalize(); !finalized) {
        return finalized;
    }
    cy::Array<cy::u8> bytes(*allocator_);
    if (Status encoded = animation::encode_skeleton(skeleton, animation::SkeletonProfile{}, bytes);
        !encoded) {
        return encoded;
    }
    if (Status stored = store(kSkeletonId, bytes); !stored) {
        return stored;
    }
    Name joints[kHeroJoints];
    for (u16 joint = 0; joint < kHeroJoints; ++joint) {
        joints[joint] = Name::intern(kSkeleton[joint].name);
    }
    for (u32 index = 0; index < kClipCount; ++index) {
        animation::Clip clip(*allocator_);
        if (Status authored = author_clip(clip, kStates[index]); !authored) {
            return authored;
        }
        bytes.clear();
        if (Status encoded =
                animation::encode_clip(clip, cy::Span<const Name>(joints, kHeroJoints), bytes);
            !encoded) {
            return encoded;
        }
        if (Status stored = store(kClipIds[index], bytes); !stored) {
            return stored;
        }
    }
    cy::Expected<graph::pose::PoseProgram, cy::Error> program = compile_program(*allocator_);
    if (!program) {
        return Status{cy::make_unexpected(program.error())};
    }
    bytes.clear();
    if (Status encoded = animation::encode_program(*program, bytes); !encoded) {
        return encoded;
    }
    return store(kProgramId, bytes);
}

Status HeroRig::load() noexcept {
    cy::jobs::JobSystemConfig config;
    config.worker_count = 1;
    if (Status started = workers_.start(config); !started) {
        return started;
    }
    if (Status started = async_.start(workers_); !started) {
        return started;
    }
    cy::Expected<cy::UniquePtr<cy::assets::MemoryMount>, cy::Error> memory =
        cy::make_unique<cy::assets::MemoryMount>(*allocator_, "cooked-hero");
    if (!memory) {
        return Status{cy::make_unexpected(memory.error())};
    }
    if (cy::Expected<cy::assets::MountId, cy::Error> mounted =
            files_.mount_owned(std::move(*memory), cy::assets::mount_priority::kMemory);
        !mounted) {
        return Status{cy::make_unexpected(mounted.error())};
    }
    if (Status started = assets_.start(workers_, async_, files_, cy::assets::AssetSystemConfig{});
        !started) {
        return started;
    }
    started_ = true;
    if (Status cooked = cook(); !cooked) {
        return cooked;
    }
    library_ = std::make_unique<cy::animation::AnimationLibrary>(*allocator_, assets_);
    const cy::animation::RigAssets wanted{kSkeletonId, kProgramId,
                                          cy::Span<const AssetId>(kClipIds, kClipCount)};
    cy::Expected<const cy::animation::AnimationRig*, cy::Error> rig = library_->rig(wanted);
    if (!rig) {
        return Status{cy::make_unexpected(rig.error())};
    }
    rig_ = *rig;
    if (rig_->unresolved_joints() != 0) {
        return cy::fail(cy::ErrorCode::NotFound,
                        "the hero's program names a joint the skeleton does not have");
    }
    return cy::ok();
}

}  // namespace sample::tps
