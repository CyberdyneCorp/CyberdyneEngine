#pragma once
// The shared fixture for CyberGameplay's suites. Section 4.4.
//
// WHAT IS ABSENT IS THE SUBJECT. There is no `ecs::World` here, no renderer, no audio device, no
// input server. `gameplay-framework` — "Headless operation": the framework is "**fully functional
// with no renderer, no audio, no interface, and no GPU**. This SHALL be a requirement rather than a
// build configuration." These suites are what that requirement looks like when it is kept: they run
// the real command stream, the real validation and the real control registry, and none of them can
// reach a device because this module links nothing that has one.

#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/command.h>
#include <cy/gameplay/context.h>
#include <cy/gameplay/control.h>
#include <cy/gameplay/random.h>
#include <cy/test/test.h>

namespace cy::gameplay_test {

using namespace cy::gameplay;

using cy::ecs::Entity;

inline cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

/// A stand-in for an entity the ECS would have issued. Generation 1 so that `valid()` is true —
/// generation 0 is the null entity, which is what an unset field means.
[[nodiscard]] inline Entity entity(u32 index) noexcept {
    return Entity::make(index, 1);
}

/// A session, a control registry and a command stream, wired the way a game wires them.
struct Fixture {
    explicit Fixture(u64 seed = 0x5EEDULL) noexcept
        : session(allocator(), seed), control(allocator()), commands(allocator(), control) {}

    [[nodiscard]] GameplayContext context() noexcept {
        GameplayContext ctx;
        ctx.session = &session;
        ctx.services = &session.services();
        ctx.commands = &commands;
        ctx.at.tick = tick;
        return ctx;
    }

    GameSession session;
    ControlRegistry control;
    CommandStream commands;
    u64 tick = 0;
};

/// The payload most cases use: a movement delta, small and trivially copyable, which is what a
/// command payload has to be.
struct MoveIntent {
    i32 dx = 0;
    i32 dy = 0;
};

}  // namespace cy::gameplay_test
