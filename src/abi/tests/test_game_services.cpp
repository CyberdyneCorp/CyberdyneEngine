// SPDX-License-Identifier: MIT
// ABI 1.3's game services: the rules every entry shares. `add-swift-game-api`.
//
// The per-service suites (test_game_<group>.cpp, one per implementer) test their thunks against
// fake backends. This file tests what they all stand on — the table carries every 1.3 entry, the
// phase check refuses what it should and nothing else, and `struct_size` is honoured in both
// directions — so a group's suite can take those as given rather than re-proving them seven times.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/services.h>
#include <cy/abi/game/ui.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstddef>
#include <cstring>

namespace {

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

}  // namespace

CY_TEST_CASE("the 1.3 table carries every game-service entry, after every 1.2 entry") {
    const CyInterface& iface = table();
    CY_CHECK_GE(iface.header.abi_minor, 3U);
    CY_CHECK_EQ(iface.header.table_size, sizeof(CyInterface));

    // Appended, never inserted: the first 1.3 entry comes straight after the last 1.2 one.
    CY_CHECK_EQ(offsetof(CyInterface, time_get),
                offsetof(CyInterface, service_poll) + sizeof(void*));

    // Every entry is set. A null here is a table initialiser that fell out of step with the header.
    const bool entries[] = {
        iface.time_get != nullptr,
        iface.input_find_action != nullptr,
        iface.input_action_state != nullptr,
        iface.input_action_state_by_name != nullptr,
        iface.input_pointer != nullptr,
        iface.input_modifiers != nullptr,
        iface.input_find_context != nullptr,
        iface.input_push_context != nullptr,
        iface.input_pop_context != nullptr,
        iface.camera_active != nullptr,
        iface.camera_view != nullptr,
        iface.camera_screen_to_ray != nullptr,
        iface.camera_world_to_screen != nullptr,
        iface.camera_set_target != nullptr,
        iface.camera_set_pose != nullptr,
        iface.camera_clear_pose != nullptr,
        iface.physics_raycast != nullptr,
        iface.physics_raycast_all != nullptr,
        iface.physics_shape_cast != nullptr,
        iface.physics_overlap != nullptr,
        iface.nav_find_path != nullptr,
        iface.nav_request_path != nullptr,
        iface.nav_poll_path != nullptr,
        iface.nav_cancel_path != nullptr,
        iface.nav_agent_configure != nullptr,
        iface.nav_agent_move_to != nullptr,
        iface.nav_agent_stop != nullptr,
        iface.nav_agent_state != nullptr,
        iface.audio_find_cue != nullptr,
        iface.audio_play != nullptr,
        iface.audio_stop != nullptr,
        iface.audio_voice_playing != nullptr,
        iface.audio_find_bus != nullptr,
        iface.audio_set_bus_volume != nullptr,
        iface.spawn_resolve != nullptr,
        iface.spawn_instantiate != nullptr,
        iface.spawn_instantiate_many != nullptr,
        iface.spawn_destroy != nullptr,
    };
    CY_CHECK_EQ(sizeof(entries) / sizeof(entries[0]), 38U);
    for (const bool set : entries) {
        CY_CHECK(set);
    }
    // The 1.4 VFX entries follow the complete 1.3 game-service prefix.
    CY_CHECK_EQ(offsetof(CyInterface, spawn_destroy) + sizeof(void*),
                offsetof(CyInterface, vfx_effect_parameter_set));
    // And the 1.5 entries follow the 1.4 ones.
    CY_CHECK_EQ(offsetof(CyInterface, vfx_effect_parameter_get) + sizeof(void*),
                offsetof(CyInterface, register_system));
}

CY_TEST_CASE("the 1.5 table appends systems, nodes, bodies and characters after every 1.4 entry") {
    const CyInterface& iface = table();
    CY_CHECK_GE(iface.header.abi_minor, 5U);
    CY_CHECK_EQ(iface.header.table_size, sizeof(CyInterface));
    const bool entries[] = {
        iface.register_system != nullptr,      iface.node_find != nullptr,
        iface.physics_apply_force != nullptr,  iface.physics_apply_impulse != nullptr,
        iface.physics_apply_torque != nullptr, iface.physics_set_velocity != nullptr,
        iface.physics_get_velocity != nullptr, iface.character_create != nullptr,
        iface.character_destroy != nullptr,    iface.character_move != nullptr,
        iface.character_state != nullptr,
    };
    CY_CHECK_EQ(sizeof(entries) / sizeof(entries[0]), 11U);
    for (const bool set : entries) {
        CY_CHECK(set);
    }
    // Appended, never inserted: the first 1.5 entry is one pointer after the last 1.4 one, and the
    // last 1.5 entry is followed directly by the first 1.6 one, so nothing was declared and not
    // listed. test_game_ui.cpp holds the 1.6 entries to the end of the table.
    CY_CHECK_EQ(offsetof(CyInterface, register_system),
                offsetof(CyInterface, vfx_effect_parameter_get) + sizeof(void*));
    CY_CHECK_EQ(offsetof(CyInterface, character_state) + sizeof(void*),
                offsetof(CyInterface, ui_root));
}

CY_TEST_CASE("each scheduler stage runs in the phase cy_abi.h states") {
    using cy::abi::game::phase_of_stage;
    CY_CHECK_EQ(phase_of_stage(CY_STAGE_PRE_SIMULATION), CY_PHASE_FIXED_UPDATE);
    CY_CHECK_EQ(phase_of_stage(CY_STAGE_PHYSICS), CY_PHASE_FIXED_UPDATE);
    CY_CHECK_EQ(phase_of_stage(CY_STAGE_SIMULATION), CY_PHASE_FIXED_UPDATE);
    CY_CHECK_EQ(phase_of_stage(CY_STAGE_POST_SIMULATION), CY_PHASE_FIXED_UPDATE);
    CY_CHECK_EQ(phase_of_stage(CY_STAGE_FRAME), CY_PHASE_FRAME_UPDATE);
    CY_CHECK_EQ(phase_of_stage(CY_STAGE_ANIMATION), CY_PHASE_FRAME_UPDATE);
    CY_CHECK_EQ(phase_of_stage(CY_STAGE_UI), CY_PHASE_FRAME_UPDATE);
    CY_CHECK_EQ(phase_of_stage(CY_STAGE_RENDER), CY_PHASE_NONE);
}

CY_TEST_CASE("a new host starts with no game backend and in no phase") {
    cy::abi::Host host(cy::system_allocator(cy::MemoryDomain::Scripting));
    CY_CHECK(host.game.input == nullptr);
    CY_CHECK(host.game.camera == nullptr);
    CY_CHECK(host.game.physics == nullptr);
    CY_CHECK(host.game.navigation == nullptr);
    CY_CHECK(host.game.audio == nullptr);
    CY_CHECK(host.game.spawn == nullptr);
    CY_CHECK(host.game.scene == nullptr);
    CY_CHECK(host.game.bodies == nullptr);
    CY_CHECK(host.game.characters == nullptr);
    CY_CHECK(host.game.ui == nullptr);
    CY_CHECK_EQ(host.game.clock.phase, CY_PHASE_NONE);
    CY_CHECK_FALSE(host.game.clock.resimulating());
}

CY_TEST_CASE("the phase check refuses exactly the phases an entry does not list") {
    using namespace cy::abi::game;
    GameServices services;

    services.clock.phase = CY_PHASE_FIXED_UPDATE;
    cy::abi::clear_last_error();
    CY_CHECK_EQ(require_phase(services, kPhaseFixed | kPhaseFrame, "physics_raycast"),
                CY_RESULT_OK);
    // Allowed calls leave the last error alone, so a successful thunk does not report a failure.
    CY_CHECK_EQ(cy::abi::last_error_code(), CY_RESULT_OK);

    CY_CHECK_EQ(require_phase(services, kPhaseNone | kPhaseFrame, "input_pointer"),
                CY_RESULT_PERMISSION_DENIED);
    CY_CHECK_EQ(cy::abi::last_error_code(), CY_RESULT_PERMISSION_DENIED);
    // The message names the entry and the phase, so a Swift `CyberdyneError` says what to fix.
    CY_CHECK(std::strstr(cy::abi::last_error_message(), "input_pointer") != nullptr);
    CY_CHECK(std::strstr(cy::abi::last_error_message(), "fixed update") != nullptr);

    services.clock.phase = CY_PHASE_NONE;
    CY_CHECK_EQ(require_phase(services, kPhaseFixed, "nav_request_path"),
                CY_RESULT_PERMISSION_DENIED);
    CY_CHECK_EQ(require_phase(services, kPhaseAny, "time_get"), CY_RESULT_OK);

    services.clock.phase = CY_PHASE_FRAME_UPDATE;
    CY_CHECK_EQ(require_phase(services, kPhaseNone | kPhaseFixed, "spawn_instantiate"),
                CY_RESULT_PERMISSION_DENIED);
    CY_CHECK(std::strstr(cy::abi::last_error_message(), "frame update") != nullptr);
    cy::abi::clear_last_error();
}

CY_TEST_CASE("a phase scope sets the phase and restores the previous one, nested") {
    using namespace cy::abi::game;
    GameClock clock;
    {
        const PhaseScope fixed(clock, CY_PHASE_FIXED_UPDATE);
        CY_CHECK_EQ(clock.phase, CY_PHASE_FIXED_UPDATE);
        {
            const PhaseScope frame(clock, CY_PHASE_FRAME_UPDATE);
            CY_CHECK_EQ(clock.phase, CY_PHASE_FRAME_UPDATE);
        }
        CY_CHECK_EQ(clock.phase, CY_PHASE_FIXED_UPDATE);
    }
    CY_CHECK_EQ(clock.phase, CY_PHASE_NONE);
}

CY_TEST_CASE("a sized struct read in is whole, whatever size the caller compiled") {
    using namespace cy::abi::game;
    CyTime in{};
    in.phase = CY_PHASE_FIXED_UPDATE;
    in.tick = 42;
    in.flags = CY_TIME_PAUSED;

    // Zero is "the size this header declares".
    CyTime out{};
    in.struct_size = 0;
    CY_CHECK(read_sized(in, out));
    CY_CHECK_EQ(out.struct_size, sizeof(CyTime));
    CY_CHECK_EQ(out.tick, 42U);
    CY_CHECK_EQ(out.flags, CY_TIME_PAUSED);

    // An older, shorter caller: only its prefix is read and the rest is zero, not stale.
    in.struct_size = static_cast<uint32_t>(offsetof(CyTime, fixed_delta));
    std::memset(&out, 0xAB, sizeof(out));
    CY_CHECK(read_sized(in, out));
    CY_CHECK_EQ(out.struct_size, sizeof(CyTime));
    CY_CHECK_EQ(out.tick, 42U);
    CY_CHECK_EQ(out.flags, 0U);

    // A size too small to hold even itself is malformed rather than old.
    in.struct_size = 2;
    CY_CHECK_FALSE(read_sized(in, out));
}

CY_TEST_CASE("a sized struct written out stops at the caller's size and says how much it wrote") {
    using namespace cy::abi::game;
    CyTime value{};
    value.struct_size = sizeof(CyTime);
    value.tick = 7;
    value.flags = CY_TIME_RESIMULATING;

    CyTime whole{};
    whole.struct_size = 0;
    CY_CHECK(write_sized(whole, value));
    CY_CHECK_EQ(whole.struct_size, sizeof(CyTime));
    CY_CHECK_EQ(whole.flags, CY_TIME_RESIMULATING);

    // A caller that knows only up to `tick` gets exactly that, and nothing past it is touched.
    CyTime shorter{};
    std::memset(&shorter, 0xCD, sizeof(shorter));
    const auto prefix = static_cast<uint32_t>(offsetof(CyTime, fixed_delta));
    shorter.struct_size = prefix;
    CY_CHECK(write_sized(shorter, value));
    CY_CHECK_EQ(shorter.struct_size, prefix);
    CY_CHECK_EQ(shorter.tick, 7U);
    CY_CHECK_EQ(shorter.flags, 0xCDCDCDCDU);

    // A larger caller — a module built against a later minor — is written this build's size.
    CyTime larger{};
    larger.struct_size = sizeof(CyTime) + 16U;
    CY_CHECK(write_sized(larger, value));
    CY_CHECK_EQ(larger.struct_size, sizeof(CyTime));

    CyTime malformed{};
    malformed.struct_size = 1;
    CY_CHECK_FALSE(write_sized(malformed, value));
    CY_CHECK_EQ(malformed.tick, 0U);
}
