// SPDX-License-Identifier: MIT
// ABI 1.7's animation thunks against a fake backend. Issue #76 stage 4.
//
// What the thunks own: the phases — what a character does in `[N F]`, taking root motion in `[F]`,
// the frame's events and a joint's pose in `[N U]`, everything else read anywhere — the argument
// checks, `struct_size` both ways, the events' sizing pattern, the structural epoch, and
// `CY_NAME_HASH`. The animation itself is integration.game_backend_animation's.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/animation.h>
#include <cy/abi/game/services.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/ecs/world.h>
#include <cy/test/test.h>

#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>

namespace {

using cy::f32;
using cy::u32;
using cy::u64;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

class FakeAnimation final : public cy::abi::game::AnimationBackend {
public:
    u32 attached = 0;
    u32 plays = 0;
    u32 triggers = 0;
    f32 last_seconds = -1.0F;
    f32 last_value = -1.0F;
    CyAnimatorDesc last_desc{};
    CyRootMotionMode last_mode = CY_ROOT_MOTION_IGNORE;
    CyAnimationEvent events_[3] = {{7, 11, 0.25F, 0.0F}, {7, 12, 0.5F, 1.0F}, {9, 11, 0.75F, 0.0F}};
    u32 event_count = 3;

    CyResult attach(CyEntity, const CyAnimatorDesc& desc) noexcept override {
        ++attached;
        last_desc = desc;
        return CY_RESULT_OK;
    }
    CyResult detach(CyEntity) noexcept override { return CY_RESULT_OK; }
    CyResult play(CyEntity, const char*, f32 seconds) noexcept override {
        ++plays;
        last_seconds = seconds;
        return CY_RESULT_OK;
    }
    CyResult stop(CyEntity, f32 seconds) noexcept override {
        last_seconds = seconds;
        return CY_RESULT_OK;
    }
    CyResult set_parameter(CyEntity, const char*, f32 value) noexcept override {
        last_value = value;
        return CY_RESULT_OK;
    }
    CyResult fire_trigger(CyEntity, const char*) noexcept override {
        ++triggers;
        return CY_RESULT_OK;
    }
    CyResult parameter(CyEntity, const char*, f32& out) const noexcept override {
        out = 0.75F;
        return CY_RESULT_OK;
    }
    CyResult state(CyEntity, CyAnimatorState& out) const noexcept override {
        out.flags = CY_ANIMATOR_BLENDING;
        out.state = 1;
        out.target = 2;
        out.blend_weight = 0.5F;
        out.state_time = 3.0F;
        return CY_RESULT_OK;
    }
    cy::Span<const CyAnimationEvent> events() const noexcept override {
        return {events_, event_count};
    }
    CyResult root_motion(CyEntity, CyRootMotion& out) const noexcept override {
        out.translation[2] = -1.0F;
        out.travelled[2] = -4.0F;
        return CY_RESULT_OK;
    }
    CyResult take_root_motion(CyEntity, CyRootMotion& out) noexcept override {
        out.translation[0] = 2.0F;
        return CY_RESULT_OK;
    }
    CyResult set_root_motion(CyEntity, CyRootMotionMode mode) noexcept override {
        last_mode = mode;
        return CY_RESULT_OK;
    }
    CyResult joint_pose(CyEntity, const char*, CyPose& out) const noexcept override {
        out.position[1] = 1.5F;
        out.rotation[3] = 1.0F;
        return CY_RESULT_OK;
    }
};

struct Fixture {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    FakeAnimation animation;
    Fixture() noexcept { host.game.animation = &animation; }
    [[nodiscard]] CyEngine engine() noexcept { return &host; }
};

}  // namespace

CY_TEST_CASE("animation: requests are simulation, reads are any phase, events are the frame's") {
    Fixture fixture;
    const CyInterface& iface = table();
    CyAnimatorDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.rig = "hero";
    CyAnimatorState state{};
    state.struct_size = sizeof(state);
    CyRootMotion motion{};
    motion.struct_size = sizeof(motion);
    CyPose pose{};
    u32 count = 0;
    f32 value = 0.0F;

    // Every request is refused in the frame phase.
    {
        const cy::abi::game::PhaseScope frame(fixture.host.game.clock, CY_PHASE_FRAME_UPDATE);
        CY_CHECK_EQ(iface.animation_attach(fixture.engine(), 4, &desc),
                    CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(iface.animation_detach(fixture.engine(), 4), CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(iface.animation_play(fixture.engine(), 4, "walk", 0.2F),
                    CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(iface.animation_stop(fixture.engine(), 4, 0.2F), CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(iface.animation_set_float(fixture.engine(), 4, "speed", 1.0F),
                    CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(iface.animation_set_bool(fixture.engine(), 4, "armed", true),
                    CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(iface.animation_fire_trigger(fixture.engine(), 4, "jump"),
                    CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(iface.animation_set_root_motion(fixture.engine(), 4, CY_ROOT_MOTION_EXTRACT),
                    CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(iface.animation_take_root_motion(fixture.engine(), 4, &motion),
                    CY_RESULT_PERMISSION_DENIED);
        // And every read answers.
        CY_CHECK_EQ(iface.animation_get_float(fixture.engine(), 4, "speed", &value), CY_RESULT_OK);
        CY_CHECK_EQ(iface.animation_state(fixture.engine(), 4, &state), CY_RESULT_OK);
        CY_CHECK_EQ(iface.animation_root_motion(fixture.engine(), 4, &motion), CY_RESULT_OK);
        CY_CHECK_EQ(iface.animation_events(fixture.engine(), nullptr, 0, &count), CY_RESULT_OK);
        CY_CHECK_EQ(iface.animation_joint_pose(fixture.engine(), 4, "hand", &pose), CY_RESULT_OK);
    }
    // The fixed step: requests answer, the frame's presentation does not.
    {
        const cy::abi::game::PhaseScope fixed(fixture.host.game.clock, CY_PHASE_FIXED_UPDATE);
        CY_CHECK_EQ(iface.animation_attach(fixture.engine(), 4, &desc), CY_RESULT_OK);
        CY_CHECK_EQ(iface.animation_play(fixture.engine(), 4, "walk", 0.2F), CY_RESULT_OK);
        CY_CHECK_EQ(iface.animation_take_root_motion(fixture.engine(), 4, &motion), CY_RESULT_OK);
        CY_CHECK_EQ(motion.translation[0], 2.0F);
        CY_CHECK_EQ(iface.animation_events(fixture.engine(), nullptr, 0, &count),
                    CY_RESULT_PERMISSION_DENIED);
        CY_CHECK_EQ(iface.animation_joint_pose(fixture.engine(), 4, "hand", &pose),
                    CY_RESULT_PERMISSION_DENIED);
    }
    // No phase: initialisation may attach and play, and take nothing.
    CY_CHECK_EQ(iface.animation_play(fixture.engine(), 4, "walk", 0.2F), CY_RESULT_OK);
    CY_CHECK_EQ(iface.animation_take_root_motion(fixture.engine(), 4, &motion),
                CY_RESULT_PERMISSION_DENIED);
    CY_CHECK_EQ(fixture.animation.plays, 2U);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("animation: the arguments a thunk refuses before a backend sees them") {
    Fixture fixture;
    const CyInterface& iface = table();
    CyAnimatorDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.rig = "hero";
    CY_CHECK_EQ(iface.animation_attach(nullptr, 4, &desc), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.animation_attach(fixture.engine(), CY_ENTITY_NULL, &desc),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.animation_attach(fixture.engine(), 4, nullptr), CY_RESULT_INVALID_ARGUMENT);
    CyAnimatorDesc broken = desc;
    broken.rig = nullptr;
    CY_CHECK_EQ(iface.animation_attach(fixture.engine(), 4, &broken), CY_RESULT_INVALID_ARGUMENT);
    broken = desc;
    broken.tier = 4;
    CY_CHECK_EQ(iface.animation_attach(fixture.engine(), 4, &broken), CY_RESULT_INVALID_ARGUMENT);
    broken = desc;
    broken.root_motion = 5;
    CY_CHECK_EQ(iface.animation_attach(fixture.engine(), 4, &broken), CY_RESULT_INVALID_ARGUMENT);
    broken = desc;
    broken.play_rate = -1.0F;
    CY_CHECK_EQ(iface.animation_attach(fixture.engine(), 4, &broken), CY_RESULT_INVALID_ARGUMENT);
    broken = desc;
    broken.struct_size = 2;
    CY_CHECK_EQ(iface.animation_attach(fixture.engine(), 4, &broken), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(fixture.animation.attached, 0U);

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 infinity = std::numeric_limits<f32>::infinity();
    CY_CHECK_EQ(iface.animation_play(fixture.engine(), 4, nullptr, 0.2F),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.animation_play(fixture.engine(), 4, "walk", -0.1F),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.animation_play(fixture.engine(), 4, "walk", nan), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.animation_stop(fixture.engine(), 4, infinity), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.animation_set_float(fixture.engine(), 4, "speed", nan),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.animation_set_float(fixture.engine(), 4, nullptr, 1.0F),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.animation_fire_trigger(fixture.engine(), 4, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.animation_get_float(fixture.engine(), 4, "speed", nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.animation_set_root_motion(fixture.engine(), 4, 5),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(fixture.animation.plays, 0U);

    // A zero crossfade is a cut, not a refusal; a bool reaches the backend as 1 or 0.
    CY_CHECK_EQ(iface.animation_play(fixture.engine(), 4, "walk", 0.0F), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.animation.last_seconds, 0.0F);
    CY_CHECK_EQ(iface.animation_set_bool(fixture.engine(), 4, "armed", true), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.animation.last_value, 1.0F);
    CY_CHECK_EQ(iface.animation_set_bool(fixture.engine(), 4, "armed", false), CY_RESULT_OK);
    CY_CHECK_EQ(fixture.animation.last_value, 0.0F);
    CY_CHECK_EQ(iface.animation_set_root_motion(fixture.engine(), 4, CY_ROOT_MOTION_CHARACTER),
                CY_RESULT_OK);
    CY_CHECK_EQ(fixture.animation.last_mode, CY_ROOT_MOTION_CHARACTER);

    // No backend bound: every entry is UNAVAILABLE, in every phase it is allowed in.
    fixture.host.game.animation = nullptr;
    CY_CHECK_EQ(iface.animation_play(fixture.engine(), 4, "walk", 0.2F), CY_RESULT_UNAVAILABLE);
    u32 count = 0;
    CY_CHECK_EQ(iface.animation_events(fixture.engine(), nullptr, 0, &count),
                CY_RESULT_UNAVAILABLE);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("animation: struct_size both ways, and the events' sizing pattern") {
    Fixture fixture;
    const CyInterface& iface = table();

    // A caller compiled against a shorter state reads only its prefix, and is told what it got.
    CyAnimatorState state{};
    state.struct_size = static_cast<u32>(offsetof(CyAnimatorState, target));
    state.blend_weight = 99.0F;
    CY_CHECK_EQ(iface.animation_state(fixture.engine(), 4, &state), CY_RESULT_OK);
    CY_CHECK_EQ(state.struct_size, static_cast<u32>(offsetof(CyAnimatorState, target)));
    CY_CHECK_EQ(state.state, 1U);
    CY_CHECK_EQ(state.blend_weight, 99.0F);
    state.struct_size = 0;
    CY_CHECK_EQ(iface.animation_state(fixture.engine(), 4, &state), CY_RESULT_OK);
    CY_CHECK_EQ(state.struct_size, static_cast<u32>(sizeof(CyAnimatorState)));
    CY_CHECK_EQ(state.blend_weight, 0.5F);
    CY_CHECK_EQ(state.target, 2U);
    state.struct_size = 2;
    CY_CHECK_EQ(iface.animation_state(fixture.engine(), 4, &state), CY_RESULT_INVALID_ARGUMENT);

    CyRootMotion motion{};
    motion.struct_size = static_cast<u32>(sizeof(CyRootMotion));
    CY_CHECK_EQ(iface.animation_root_motion(fixture.engine(), 4, &motion), CY_RESULT_OK);
    CY_CHECK_EQ(motion.travelled[2], -4.0F);

    // THE SIZING PATTERN: the count always, the events only when they fit, the question free.
    u32 count = 0;
    CY_CHECK_EQ(iface.animation_events(fixture.engine(), nullptr, 0, &count), CY_RESULT_OK);
    CY_CHECK_EQ(count, 3U);
    CyAnimationEvent two[2] = {};
    std::memset(two, 0xAB, sizeof(two));
    CY_CHECK_EQ(iface.animation_events(fixture.engine(), two, 2, &count),
                CY_RESULT_BUFFER_TOO_SMALL);
    CY_CHECK_EQ(count, 3U);
    CY_CHECK_EQ(two[0].entity, 0xABABABABABABABABULL);
    CyAnimationEvent three[3] = {};
    CY_CHECK_EQ(iface.animation_events(fixture.engine(), three, 3, &count), CY_RESULT_OK);
    CY_CHECK_EQ(three[1].name, 12U);
    CY_CHECK_EQ(three[2].entity, 9U);
    CY_CHECK_EQ(iface.animation_events(fixture.engine(), three, 3, nullptr),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(iface.animation_events(fixture.engine(), nullptr, 3, &count),
                CY_RESULT_INVALID_ARGUMENT);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("animation: CY_NAME_HASH is FNV-1a over the name's bytes") {
    // The published FNV-1a 64 vectors: the empty string is the offset basis.
    CY_CHECK_EQ(cy::abi::game::name_hash(""), CY_NAME_HASH_OFFSET);
    CY_CHECK_EQ(cy::abi::game::name_hash("a"), 0xaf63dc4c8601ec8cULL);
    CY_CHECK_EQ(cy::abi::game::name_hash("foobar"), 0x85944171f73967e8ULL);
    // Constant-evaluated, so a module's `static const` and the engine's agree at compile time.
    static_assert(cy::abi::game::name_hash("a") == 0xaf63dc4c8601ec8cULL);
    CY_CHECK_NE(cy::abi::game::name_hash("walk"), cy::abi::game::name_hash("run"));
}

CY_TEST_CASE("animation: attaching and detaching move the world's epoch") {
    Fixture fixture;
    cy::ecs::World ecs{cy::system_allocator(cy::MemoryDomain::Scripting)};
    CY_REQUIRE(ecs.initialize().has_value());
    cy::abi::World world{cy::system_allocator(cy::MemoryDomain::Scripting), ecs};
    fixture.host.bind_world(&world);
    const u64 before = world.epoch;
    CyAnimatorDesc desc{};
    desc.struct_size = sizeof(desc);
    desc.rig = "hero";
    CY_REQUIRE_EQ(table().animation_attach(fixture.engine(), 4, &desc), CY_RESULT_OK);
    CY_CHECK_EQ(world.epoch, before + 1);
    CY_REQUIRE_EQ(table().animation_play(fixture.engine(), 4, "walk", 0.2F), CY_RESULT_OK);
    CY_CHECK_EQ(world.epoch, before + 1);
    CY_REQUIRE_EQ(table().animation_detach(fixture.engine(), 4), CY_RESULT_OK);
    CY_CHECK_EQ(world.epoch, before + 2);
    fixture.host.bind_world(nullptr);
}
