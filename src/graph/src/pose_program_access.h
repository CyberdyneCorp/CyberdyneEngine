// SPDX-License-Identifier: MIT
#pragma once
// Write access to a `PoseProgram`, shared by the two translation units that build one: the compiler
// (lower_pose.cpp) and the loader's assembly of a cooked program (pose_program.cpp). Private to
// src/graph/src.

#include <cy/graph/lower_pose.h>

namespace cy::graph::pose {

/// Write access to a `PoseProgram`, which has none in public: a compiled program is shared by every
/// character at run time.
class PoseProgramAccess {
public:
    [[nodiscard]] static Array<PoseInstruction>& code(PoseProgram& program) noexcept {
        return program.code_;
    }
    [[nodiscard]] static Array<PoseState>& states(PoseProgram& program) noexcept {
        return program.states_;
    }
    [[nodiscard]] static Array<Transition>& transitions(PoseProgram& program) noexcept {
        return program.transitions_;
    }
    [[nodiscard]] static Array<ClipRef>& clips(PoseProgram& program) noexcept {
        return program.clips_;
    }
    [[nodiscard]] static Array<JointMask>& masks(PoseProgram& program) noexcept {
        return program.masks_;
    }
    [[nodiscard]] static Array<Name>& parameters(PoseProgram& program) noexcept {
        return program.parameters_;
    }
    [[nodiscard]] static Array<SyncGroup>& sync_groups(PoseProgram& program) noexcept {
        return program.sync_;
    }
    [[nodiscard]] static Array<SyncMarker>& markers(PoseProgram& program) noexcept {
        return program.markers_;
    }
    [[nodiscard]] static DebugMap& debug(PoseProgram& program) noexcept { return program.debug_; }
    static void set_name(PoseProgram& program, Name name) noexcept { program.name_ = name; }
    static void set_entry(PoseProgram& program, u16 state) noexcept {
        program.entry_state_ = state;
    }
    static void set_joints(PoseProgram& program, u32 joints) noexcept { program.joints_ = joints; }
    static void set_digest(PoseProgram& program, u64 digest) noexcept { program.digest_ = digest; }
};

}  // namespace cy::graph::pose
