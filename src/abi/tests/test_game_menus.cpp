// SPDX-License-Identifier: MIT
// ABI 1.17's thunks against fake backends: rebinding, the profile, the display settings, the
// player's files and tooltips.
//
// What the thunks own: the phase rule — every entry `[N U]` — the argument and range checks, a file
// name that is one plain name, the sized reads, `struct_size` both ways, and UNAVAILABLE with no
// backend bound or a backend from before 1.17. The adapters themselves are
// integration.game_backend_menus's.

#include <cy/abi/cy_abi.h>
#include <cy/abi/errors.h>
#include <cy/abi/game/input.h>
#include <cy/abi/game/menus.h>
#include <cy/abi/game/services.h>
#include <cy/abi/game/ui.h>
#include <cy/abi/host.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/test/test.h>

#include <cstring>
#include <string>
#include <string_view>

namespace {

using cy::u32;
using cy::u64;
using cy::usize;

const CyInterface& table() noexcept {
    const CyInterface* iface = cy_get_interface(CY_ABI_MAJOR, CY_ABI_MINOR);
    CY_REQUIRE(iface != nullptr);
    return *iface;
}

/// An input backend from before 1.17: it has the 1.3 methods and none of the new ones.
class OldInput : public cy::abi::game::InputBackend {
public:
    CyResult find_action(const char*, CyInputAction& out) noexcept override {
        out = 0;
        return CY_RESULT_OK;
    }
    CyResult action_state(u32, CyInputAction, CyInputActionState&) noexcept override {
        return CY_RESULT_OK;
    }
    CyResult pointer(u32, CyInputPointer&) noexcept override { return CY_RESULT_OK; }
    CyResult modifiers(u32, u32&) noexcept override { return CY_RESULT_OK; }
    CyResult find_context(const char*, CyInputContext&) noexcept override { return CY_RESULT_OK; }
    CyResult push_context(u32, CyInputContext, cy::i32) noexcept override { return CY_RESULT_OK; }
    CyResult pop_context(u32, CyInputContext) noexcept override { return CY_RESULT_OK; }
};

class FakeInput final : public OldInput {
public:
    u32 begun[5] = {};
    bool cancelled = false;
    std::string profile;

    CyResult rebind_begin(u32 user, CyInputAction action, u32 scheme, u32 slot,
                          u32 conflict) noexcept override {
        begun[0] = user;
        begun[1] = action;
        begun[2] = scheme;
        begun[3] = slot;
        begun[4] = conflict;
        return CY_RESULT_OK;
    }
    CyResult rebind_poll(u32, CyInputRebind& out) noexcept override {
        out.status = CY_INPUT_REBIND_APPLIED;
        out.action = begun[1];
        std::strncpy(out.control, "keyboard/g", sizeof(out.control) - 1);
        return CY_RESULT_OK;
    }
    CyResult rebind_cancel(u32) noexcept override {
        cancelled = true;
        return CY_RESULT_OK;
    }
    CyResult binding(u32, CyInputAction action, u32, u32, char (&out)[64]) noexcept override {
        if (action != 3U) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "no such binding");
        }
        std::strncpy(out, "gamepad/south", sizeof(out) - 1);
        return CY_RESULT_OK;
    }
    CyResult profile_save(u32, const char*& text, u32& size) noexcept override {
        text = profile.c_str();
        size = static_cast<u32>(profile.size());
        return CY_RESULT_OK;
    }
    CyResult profile_load(u32, const char* text, u64 size) noexcept override {
        profile.assign(text, static_cast<usize>(size));
        return CY_RESULT_OK;
    }
};

class FakeVideo final : public cy::abi::game::VideoBackend {
public:
    CyVideoSettings current{sizeof(CyVideoSettings), 1280,          720, CY_VIDEO_QUALITY_HIGH,
                            CY_VIDEO_UPSCALE_NATIVE, CY_VIDEO_VSYNC};
    u32 applied = 0;
    CyResult settings(CyVideoSettings& out) const noexcept override {
        out = current;
        return CY_RESULT_OK;
    }
    CyResult apply(const CyVideoSettings& settings) noexcept override {
        current = settings;
        ++applied;
        return CY_RESULT_OK;
    }
    CyResult modes(CyVideoMode* out, u32 capacity, u32& count) const noexcept override {
        count = 3;
        const CyVideoMode all[3] = {{1280, 720}, {1920, 1080}, {2560, 1440}};
        for (u32 index = 0; out != nullptr && index < capacity && index < 3; ++index) {
            out[index] = all[index];
        }
        return CY_RESULT_OK;
    }
};

class FakeFiles final : public cy::abi::game::UserDataBackend {
public:
    std::string name;
    std::string bytes;
    CyResult write(const char* file, const void* data, u64 size) noexcept override {
        name = file;
        bytes.assign(static_cast<const char*>(data), static_cast<usize>(size));
        return CY_RESULT_OK;
    }
    CyResult read(const char* file, void* buffer, u64 capacity, u64& size) noexcept override {
        if (name != file) {
            return cy::abi::report(CY_RESULT_NOT_FOUND, "no such file");
        }
        size = bytes.size();
        if (buffer != nullptr) {
            std::memcpy(buffer, bytes.data(),
                        static_cast<usize>(capacity < size ? capacity : size));
        }
        return CY_RESULT_OK;
    }
};

struct Bound {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    FakeInput input;
    FakeVideo video;
    FakeFiles files;
    Bound() {
        host.game.input = &input;
        host.game.video = &video;
        host.game.user_data = &files;
    }
    [[nodiscard]] CyEngine engine() noexcept { return &host; }
};

}  // namespace

CY_TEST_CASE(
    "menus: a rebind reaches its backend, and a scheme or a policy out of range does not") {
    Bound bound;
    const CyInterface& abi = table();
    CY_CHECK_EQ(abi.input_rebind_begin(bound.engine(), 1, 3, CY_INPUT_SCHEME_GAMEPAD, 0,
                                       CY_INPUT_CONFLICT_SWAP),
                CY_RESULT_OK);
    CY_CHECK_EQ(bound.input.begun[0], 1U);
    CY_CHECK_EQ(bound.input.begun[2], static_cast<u32>(CY_INPUT_SCHEME_GAMEPAD));
    CY_CHECK_EQ(bound.input.begun[4], static_cast<u32>(CY_INPUT_CONFLICT_SWAP));
    bound.input.begun[1] = 0;
    CY_CHECK_EQ(abi.input_rebind_begin(bound.engine(), 0, 3, 2, 0, CY_INPUT_CONFLICT_SWAP),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(abi.input_rebind_begin(bound.engine(), 0, 3, 0, 0, 4), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(abi.input_rebind_begin(bound.engine(), 0, 3, 0, 256, 0),
                CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(bound.input.begun[1], 0U);  // none of the three reached the backend

    // Polled: the struct the caller sized, written whole.
    CyInputRebind rebind{};
    rebind.struct_size = sizeof(rebind);
    CY_REQUIRE_EQ(abi.input_rebind_poll(bound.engine(), 0, &rebind), CY_RESULT_OK);
    CY_CHECK_EQ(rebind.status, static_cast<u32>(CY_INPUT_REBIND_APPLIED));
    CY_CHECK_EQ(std::string_view(rebind.control), std::string_view("keyboard/g"));
    // An older caller's shorter struct: only its prefix is written.
    CyInputRebind short_one{};
    short_one.struct_size = 8;
    CY_REQUIRE_EQ(abi.input_rebind_poll(bound.engine(), 0, &short_one), CY_RESULT_OK);
    CY_CHECK_EQ(short_one.status, static_cast<u32>(CY_INPUT_REBIND_APPLIED));
    CY_CHECK_EQ(short_one.control[0], '\0');
    CY_CHECK_EQ(abi.input_rebind_poll(bound.engine(), 0, nullptr), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(abi.input_rebind_cancel(bound.engine(), 0), CY_RESULT_OK);
    CY_CHECK(bound.input.cancelled);
}

CY_TEST_CASE("menus: a binding and a profile are sized reads, and a profile round-trips") {
    Bound bound;
    const CyInterface& abi = table();
    u32 size = 0;
    CY_REQUIRE_EQ(
        abi.input_binding(bound.engine(), 0, 3, CY_INPUT_SCHEME_GAMEPAD, 0, nullptr, 0, &size),
        CY_RESULT_OK);
    CY_CHECK_EQ(size, 13U);
    char path[8] = {};
    CY_REQUIRE_EQ(abi.input_binding(bound.engine(), 0, 3, CY_INPUT_SCHEME_GAMEPAD, 0, path,
                                    sizeof(path), &size),
                  CY_RESULT_OK);
    CY_CHECK_EQ(std::string_view(path), std::string_view("gamepad"));  // cut to fit, terminated
    CY_CHECK_EQ(abi.input_binding(bound.engine(), 0, 9, 0, 0, nullptr, 0, &size),
                CY_RESULT_NOT_FOUND);
    CY_CHECK_EQ(abi.input_binding(bound.engine(), 0, 3, 0, 0, nullptr, 4, &size),
                CY_RESULT_INVALID_ARGUMENT);

    constexpr std::string_view kProfile = "cyinputprofile 1\n2 keyboard 0 keyboard/g Fire\n";
    CY_REQUIRE_EQ(abi.input_profile_load(bound.engine(), 0, kProfile.data(), kProfile.size()),
                  CY_RESULT_OK);
    char saved[128] = {};
    CY_REQUIRE_EQ(abi.input_profile_save(bound.engine(), 0, saved, sizeof(saved), &size),
                  CY_RESULT_OK);
    CY_CHECK_EQ(std::string_view(saved, size), kProfile);
    CY_CHECK_EQ(abi.input_profile_load(bound.engine(), 0, nullptr, 3), CY_RESULT_INVALID_ARGUMENT);
}

CY_TEST_CASE("menus: display settings are checked and normalised both ways") {
    Bound bound;
    const CyInterface& abi = table();
    CyVideoSettings settings{};
    settings.struct_size = sizeof(settings);
    CY_REQUIRE_EQ(abi.video_get_settings(bound.engine(), &settings), CY_RESULT_OK);
    CY_CHECK_EQ(settings.width, 1280U);
    settings.width = 1920;
    settings.height = 1080;
    settings.upscale = CY_VIDEO_UPSCALE_BALANCED;
    CY_REQUIRE_EQ(abi.video_set_settings(bound.engine(), &settings), CY_RESULT_OK);
    CY_CHECK_EQ(bound.video.current.upscale, static_cast<u32>(CY_VIDEO_UPSCALE_BALANCED));
    const u32 applied = bound.video.applied;
    CyVideoSettings wrong = settings;
    wrong.quality = 4;
    CY_CHECK_EQ(abi.video_set_settings(bound.engine(), &wrong), CY_RESULT_INVALID_ARGUMENT);
    wrong = settings;
    wrong.flags = 0x4U;
    CY_CHECK_EQ(abi.video_set_settings(bound.engine(), &wrong), CY_RESULT_INVALID_ARGUMENT);
    wrong = settings;
    wrong.width = 0;
    CY_CHECK_EQ(abi.video_set_settings(bound.engine(), &wrong), CY_RESULT_INVALID_ARGUMENT);
    CY_CHECK_EQ(bound.video.applied, applied);

    u32 count = 0;
    CY_REQUIRE_EQ(abi.video_modes(bound.engine(), nullptr, 0, &count), CY_RESULT_OK);
    CY_CHECK_EQ(count, 3U);
    CyVideoMode two[2] = {};
    CY_CHECK_EQ(abi.video_modes(bound.engine(), two, 2, &count), CY_RESULT_BUFFER_TOO_SMALL);
    CY_CHECK_EQ(count, 3U);
    CY_CHECK_EQ(two[1].width, 1920U);
}

CY_TEST_CASE("menus: a player's file is one plain name, written whole and read back sized") {
    Bound bound;
    const CyInterface& abi = table();
    constexpr std::string_view kBytes = "volume=0.5\n";
    CY_REQUIRE_EQ(abi.user_data_write(bound.engine(), "settings.cfg", kBytes.data(), kBytes.size()),
                  CY_RESULT_OK);
    for (const char* bad : {"../escape", "a/b", ".hidden", "", "a b", "x\\y"}) {
        CY_CHECK_EQ(abi.user_data_write(bound.engine(), bad, kBytes.data(), kBytes.size()),
                    CY_RESULT_INVALID_ARGUMENT);
    }
    CY_CHECK_EQ(bound.files.name, std::string("settings.cfg"));
    u64 size = 0;
    CY_REQUIRE_EQ(abi.user_data_read(bound.engine(), "settings.cfg", nullptr, 0, &size),
                  CY_RESULT_OK);
    CY_CHECK_EQ(size, kBytes.size());
    char four[4] = {};
    CY_CHECK_EQ(abi.user_data_read(bound.engine(), "settings.cfg", four, sizeof(four), &size),
                CY_RESULT_BUFFER_TOO_SMALL);
    CY_CHECK_EQ(std::string_view(four, 4), std::string_view("volu"));
    CY_CHECK_EQ(abi.user_data_read(bound.engine(), "missing.cfg", nullptr, 0, &size),
                CY_RESULT_NOT_FOUND);
}

CY_TEST_CASE("menus: without a backend, or with one from before 1.17, every entry is UNAVAILABLE") {
    cy::abi::Host host{cy::system_allocator(cy::MemoryDomain::Scripting)};
    const CyInterface& abi = table();
    CyVideoSettings settings{};
    settings.struct_size = sizeof(settings);
    u64 size = 0;
    CY_CHECK_EQ(abi.video_get_settings(&host, &settings), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(abi.user_data_read(&host, "a.cfg", nullptr, 0, &size), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(abi.input_rebind_cancel(&host, 0), CY_RESULT_UNAVAILABLE);
    CY_CHECK_EQ(abi.ui_widget_set_tooltip(&host, 1, "hi"), CY_RESULT_UNAVAILABLE);
    OldInput old;
    host.game.input = &old;
    CY_CHECK_EQ(abi.input_rebind_begin(&host, 0, 0, 0, 0, 0), CY_RESULT_UNAVAILABLE);
    CY_CHECK(std::string_view(cy::abi::last_error_message()).find("rebinding") !=
             std::string_view::npos);
}

CY_TEST_CASE("menus: every 1.17 entry is refused in a fixed step") {
    Bound bound;
    const CyInterface& abi = table();
    bound.host.game.clock.phase = CY_PHASE_FIXED_UPDATE;
    bound.input.begun[1] = 77;
    CyVideoSettings settings{};
    settings.struct_size = sizeof(settings);
    u64 size = 0;
    CY_CHECK_EQ(abi.input_rebind_begin(bound.engine(), 0, 3, 0, 0, 0), CY_RESULT_PERMISSION_DENIED);
    CY_CHECK_EQ(abi.input_profile_reset(bound.engine(), 0), CY_RESULT_PERMISSION_DENIED);
    CY_CHECK_EQ(abi.video_get_settings(bound.engine(), &settings), CY_RESULT_PERMISSION_DENIED);
    CY_CHECK_EQ(abi.user_data_read(bound.engine(), "a.cfg", nullptr, 0, &size),
                CY_RESULT_PERMISSION_DENIED);
    CY_CHECK_EQ(abi.ui_widget_set_tooltip(bound.engine(), 1, "x"), CY_RESULT_PERMISSION_DENIED);
    CY_CHECK_EQ(bound.input.begun[1], 77U);  // the refused rebind never reached the backend
}
