// SPDX-License-Identifier: MIT
#pragma once
// The four-state locomotion machine over the fixture's clips, bound into a rig. Shared by the
// clock cases and the animation system's suite. Issue #76.
//
// `aim` stands in for the idle, the walk is the walk and the run, and a third walk is the death,
// so the death travels the root a metre over its one second and a death that restarted would
// visibly come back.

#include <cy/animation/evaluate.h>
#include <cy/graph/locomotion.h>

#include "fixture.h"

#include <utility>

namespace cy::animation::testing {

struct LocomotionRig {
    explicit LocomotionRig(Allocator& memory) noexcept
        : skeleton(memory),
          idle(memory),
          walk(memory),
          run(memory),
          die(memory),
          program(memory),
          table(memory),
          rig(memory) {}

    LocomotionRig(const LocomotionRig&) = delete;
    LocomotionRig& operator=(const LocomotionRig&) = delete;

    [[nodiscard]] Status build(LoopMode death_mode = LoopMode::None) noexcept {
        if (Status built = build_biped(skeleton); !built) {
            return built;
        }
        if (Status built = build_aim(idle); !built) {
            return built;
        }
        if (Status built = build_walk(walk); !built) {
            return built;
        }
        if (Status built = build_walk(run); !built) {
            return built;
        }
        if (Status built = build_walk(die); !built) {
            return built;
        }
        idle.set_name(Name::intern("idle"));
        run.set_name(Name::intern("run"));
        die.set_name(Name::intern("die"));
        die.set_loop_mode(death_mode);

        graph::pose::LocomotionSpec spec;
        spec.name = Name::intern("locomotion");
        spec.idle = graph::pose::LocomotionClip{idle.name(), idle.duration(), true};
        spec.walk = graph::pose::LocomotionClip{walk.name(), walk.duration(), true};
        spec.run = graph::pose::LocomotionClip{run.name(), run.duration(), true};
        spec.die = graph::pose::LocomotionClip{die.name(), die.duration(), false};
        graph::DiagnosticSink sink(allocator());
        Expected<graph::pose::PoseProgram, Error> compiled =
            graph::pose::compile_locomotion(allocator(), spec, kJointCount, sink);
        if (!compiled) {
            return Status{make_unexpected(compiled.error())};
        }
        program = std::move(*compiled);

        if (Status sized = table.resize(program.clips().size()); !sized) {
            return sized;
        }
        const Clip* clips[] = {&idle, &walk, &run, &die};
        for (usize index = 0; index < program.clips().size(); ++index) {
            table[index] = nullptr;
            for (const Clip* clip : clips) {
                if (clip->name() == program.clips()[index].name) {
                    table[index] = clip;
                }
            }
        }
        return rig.bind(skeleton, program, table.span());
    }

    Skeleton skeleton;
    Clip idle;
    Clip walk;
    Clip run;
    Clip die;
    graph::pose::PoseProgram program;
    Array<const Clip*> table;
    AnimationRig rig;
};

/// Raise exactly one locomotion request, the way `LocomotionDriver::request` does, through any
/// setter that takes a parameter name and a value.
template <class Setter>
[[nodiscard]] Status request_state(graph::pose::LocomotionState state, Setter&& set) noexcept {
    for (u32 index = 0; index < graph::pose::kLocomotionStateCount; ++index) {
        const auto each = static_cast<graph::pose::LocomotionState>(index);
        if (Status done = set(graph::pose::locomotion_request(each), each == state ? 1.0F : 0.0F);
            !done) {
            return done;
        }
    }
    return ok();
}

}  // namespace cy::animation::testing
