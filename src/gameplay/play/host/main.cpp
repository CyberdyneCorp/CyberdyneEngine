// cy_play_runtime_host — THE SECOND RUNTIME PROCESS. M11.b tasks 0.5 and 3.1.
//
// ================================================================================================
// WHAT THIS BINARY IS
// ================================================================================================
//
// `live-editing` (Play modes) gives `SeparateProcess` a runtime location — *"A second runtime
// process"* — and `editor-architecture` (Play mode, "Standalone play") says why: *"the game SHALL
// launch as a separate process with the debugger attached, so editor-specific state cannot mask
// bugs"*.
//
// This is that process. It reads a `.cyworld`, builds a `cy::gameplay::PlaySession` over it, and is
// driven, one line at a time, by whatever launched it — `cy::gameplay::ProcessPlayDriver`, over the
// protocol `cy/gameplay/play/launcher.h` writes down.
//
// ================================================================================================
// WHY THE ISOLATION CLAIM IS STRUCTURAL RATHER THAN ASSERTED
// ================================================================================================
//
// *"editor-only state SHALL be absent from the runtime, so editor-specific behaviour cannot mask a
// defect."* A boolean on a capability table cannot carry that claim; a LINK LIST can. This
// executable's dependencies in `src/gameplay/play/CMakeLists.txt` are the engine's runtime side and
// nothing else — there is no `cy::abi`, no editor bridge, no editor services, and the editor is a
// Rust application in another workspace that this binary could not link if it wanted to. Editor
// state is absent from it because there is no expression in it that could reach any.
//
// ================================================================================================
// IT WRITES NOTHING BUT REPLIES TO STANDARD OUTPUT
// ================================================================================================
//
// Standard output is the protocol's channel; standard error is where anything a human should read
// goes, and the launcher deliberately leaves it inherited so that a child's complaint lands in the
// parent's log rather than in a buffer the protocol never drains. A stray `printf` here would be
// read by the parent as a reply to whatever it last asked, so there is exactly one function that
// writes to standard output and it is `reply`.

#include <cy/core/assets/file.h>
#include <cy/core/memory/array.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/play/driver.h>
#include <cy/gameplay/play/launcher.h>
#include <cy/gameplay/play/session.h>
#include <cy/platform/sdl3_platform.h>
#include <cy/scene/serialization/worldfile.h>
#include <cy/servers/physics/reference/server.h>
#include <cy_reflect_generated_scene.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

// The process's own identifier, which no portable C++ interface offers. Two branches, in one
// function, in the one binary that needs it — the alternative is a `Platform` call that would exist
// solely so that this file could avoid an `#ifdef`, and `Platform` reports facts about CHILDREN.
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace {

namespace ser = cy::scene::serialization;

[[nodiscard]] long long own_process_id() noexcept {
#if defined(_WIN32)
    return static_cast<long long>(::GetCurrentProcessId());
#else
    return static_cast<long long>(::getpid());
#endif
}

cy::Allocator& allocator() noexcept {
    return cy::system_allocator(cy::MemoryDomain::World);
}

/// The one function that writes to standard output. Flushed every time: a reply sitting in this
/// process's buffer is a parent blocked reading a pipe, which presents as a hang rather than as an
/// error.
void reply(const std::string& line) {
    std::fputs(line.c_str(), stdout);
    std::fputc('\n', stdout);
    (void)std::fflush(stdout);
}

/// A 32-bit float as the eight hexadecimal digits of its bits. See `launcher.h` for why a decimal
/// would weaken the claim the comparison makes.
[[nodiscard]] std::string hex_of(cy::f32 value) {
    cy::u32 bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    char text[16];
    (void)std::snprintf(text, sizeof(text), "%08x", bits);
    return text;
}

/// What the host was asked to run.
struct Arguments {
    std::string world_path;
    /// The path the editor opens the world by. Every node identity is derived from it, so a host
    /// that guessed it would hold a world whose nodes the editor cannot name.
    std::string asset_path;
    cy::u32 body_capacity = 64;
    bool wait_for_debugger = false;
    bool valid = false;
    const char* complaint = "";
};

[[nodiscard]] Arguments parse(int count, char** values) {
    Arguments parsed;
    for (int index = 1; index < count; ++index) {
        const std::string_view argument = values[index];
        if (argument == "--wait-for-debugger") {
            parsed.wait_for_debugger = true;
            continue;
        }
        if (index + 1 >= count) {
            parsed.complaint = "an option was given no value";
            return parsed;
        }
        const char* value = values[++index];
        if (argument == "--world") {
            parsed.world_path = value;
        } else if (argument == "--asset-path") {
            parsed.asset_path = value;
        } else if (argument == "--body-capacity") {
            parsed.body_capacity = static_cast<cy::u32>(std::strtoul(value, nullptr, 10));
        } else {
            parsed.complaint = "an option this build does not know";
            return parsed;
        }
    }
    if (parsed.world_path.empty()) {
        parsed.complaint = "--world is required: a runtime process with no world simulates nothing";
        return parsed;
    }
    if (parsed.asset_path.empty()) {
        parsed.asset_path = parsed.world_path;
    }
    parsed.valid = true;
    return parsed;
}

/// The world, the schema and the physics this process plays with. Built once, before the first
/// command, so that a world that cannot be read is a refused handshake rather than a refused
/// `enter` halfway through a session.
class Host {
public:
    [[nodiscard]] bool open(const Arguments& arguments) {
        cy::Array<cy::u8> bytes(allocator());
        if (!cy::assets::fs::read_whole(arguments.world_path.c_str(), bytes)) {
            complaint_ = "the world file could not be read";
            return false;
        }
        text_.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());

        if (!cy::reflect::register_scene_types(registry_) ||
            !ser::build_authoring_schema(registry_, schema_)) {
            complaint_ = "the engine's scene types could not be registered";
            return false;
        }
        if (!ser::read_world(text_, arguments.asset_path, world_) ||
            !ser::resolve_against(world_, schema_)) {
            complaint_ = "the world file is not one this build can resolve";
            return false;
        }

        const cy::Expected<cy::physics::PhysicsServer*, cy::Error> made =
            cy::physics::reference::create_server(allocator());
        if (!made) {
            complaint_ = "the reference physics backend could not be created";
            return false;
        }
        server_ = *made;
        if (!server_->initialize()) {
            complaint_ = "the reference physics backend could not be initialised";
            return false;
        }

        session_ = new (std::nothrow) cy::gameplay::PlaySession(allocator(), world_);
        if (session_ == nullptr) {
            complaint_ = "the play session could not be allocated";
            return false;
        }
        configuration_.physics = server_;
        configuration_.schema = &schema_;
        configuration_.body_capacity = arguments.body_capacity;
        // The mode this session serves is the one whose runtime location is "a second runtime
        // process", because this IS that process. Its availability is MEASURED — the same
        // resolution the launcher used, run here against this binary's own directory — rather than
        // asserted by the fact that something launched us.
        configuration_.mode = cy::gameplay::PlayMode::SeparateProcess;
        configuration_.support = cy::gameplay::play_mode_support(platform_);
        // THE ONE BIT ONLY THIS BINARY SETS. `PlaySession::enter` refuses `SeparateProcess` without
        // it, so a session built in the process that did the launching cannot call itself the
        // second process. This is that process; see `PlayModeSupport::hosted_runtime_process`.
        configuration_.support.hosted_runtime_process = true;
        return true;
    }

    ~Host() {
        delete session_;
        if (server_ != nullptr) {
            server_->shutdown();
            cy::physics::reference::destroy_server(server_, allocator());
        }
    }

    Host() = default;
    Host(const Host&) = delete;
    Host& operator=(const Host&) = delete;
    Host(Host&&) = delete;
    Host& operator=(Host&&) = delete;

    [[nodiscard]] cy::Status initialise_platform(int count, char** values) {
        return platform_.initialise(count, values);
    }
    void shutdown_platform() { platform_.shutdown(); }

    [[nodiscard]] const char* complaint() const { return complaint_; }
    [[nodiscard]] cy::gameplay::PlaySession& session() { return *session_; }
    [[nodiscard]] const cy::gameplay::PlayConfiguration& configuration() const {
        return configuration_;
    }
    [[nodiscard]] ser::World& world() { return world_; }

private:
    cy::Sdl3Platform platform_;
    std::string text_;
    cy::reflect::TypeRegistry registry_;
    ser::World world_{allocator()};
    ser::AuthoringSchema schema_{allocator()};
    cy::physics::PhysicsServer* server_ = nullptr;
    cy::gameplay::PlaySession* session_ = nullptr;
    cy::gameplay::PlayConfiguration configuration_;
    const char* complaint_ = "";
};

/// The height of one authored node, by the identity the editor knows it by.
[[nodiscard]] bool height_of(const ser::World& world, cy::u64 identity, cy::f32& out) {
    for (const ser::WorldNode& node : world.nodes()) {
        if (!node.live || node.identity != identity) {
            continue;
        }
        cy::Transform placement;
        if (!ser::transform_of(world, node, placement)) {
            return false;
        }
        out = placement.translation.y;
        return true;
    }
    return false;
}

/// One command, answered. Returns false when the command was `quit`.
[[nodiscard]] bool serve(Host& host, std::string_view command, bool& waiting_for_debugger) {
    const auto answer = [](const cy::Status& status) {
        // A refusal is reported as a refusal and carries the engine's own message, because the
        // capability that differs by mode — a frame step a remote transport cannot honour — reaches
        // the editor through exactly this path, and "refused" with no reason cannot be told from a
        // crash.
        if (status) {
            reply("ok");
        } else {
            reply(std::string("refused ") + status.error().message);
        }
    };

    if (command == "quit") {
        reply("bye");
        return false;
    }
    if (command == "resume-launch") {
        waiting_for_debugger = false;
        reply("ok");
        return true;
    }
    if (waiting_for_debugger) {
        reply("refused this process was launched stopped for a debugger; send resume-launch");
        return true;
    }

    if (command == "enter") {
        answer(host.session().enter(host.configuration()));
        return true;
    }
    if (command == "tick") {
        answer(host.session().tick());
        return true;
    }
    if (command == "pause") {
        answer(host.session().pause());
        return true;
    }
    if (command == "resume") {
        answer(host.session().resume());
        return true;
    }
    if (command == "step-tick") {
        answer(host.session().step_tick());
        return true;
    }
    if (command == "step-frame") {
        answer(host.session().step_frame());
        return true;
    }
    if (command == "stop") {
        answer(host.session().stop());
        return true;
    }
    if (command == "report") {
        const cy::gameplay::PlayReport& report = host.session().report();
        char line[256];
        (void)std::snprintf(line, sizeof(line),
                            "report ticks=%llu stepped_ticks=%llu stepped_frames=%llu "
                            "entities=%u bodies=%u restored_exactly=%u",
                            static_cast<unsigned long long>(report.ticks),
                            static_cast<unsigned long long>(report.stepped_ticks),
                            static_cast<unsigned long long>(report.stepped_frames),
                            report.entities, report.bodies, report.restored_exactly ? 1U : 0U);
        reply(line);
        return true;
    }

    constexpr std::string_view kHeight = "translation-y ";
    if (command.size() > kHeight.size() && command.substr(0, kHeight.size()) == kHeight) {
        const std::string digits(command.substr(kHeight.size()));
        const auto identity = static_cast<cy::u64>(std::strtoull(digits.c_str(), nullptr, 10));
        cy::f32 height = 0.0F;
        if (!height_of(host.world(), identity, height)) {
            reply("refused no live node in this world carries that identity");
            return true;
        }
        reply("value " + hex_of(height));
        return true;
    }

    // Named rather than ignored, for the same reason `play_mode_of` refuses a word it does not
    // know: a peer built from a different commit must be told, not answered approximately.
    reply("refused this build does not know that command");
    return true;
}

}  // namespace

int main(int argument_count, char** arguments) {
    const Arguments parsed = parse(argument_count, arguments);
    if (!parsed.valid) {
        std::fprintf(stderr, "cy_play_runtime_host: %s\n", parsed.complaint);
        return 2;
    }

    Host host;
    if (!host.initialise_platform(argument_count, arguments)) {
        std::fprintf(stderr, "cy_play_runtime_host: the platform could not be initialised\n");
        return 3;
    }
    if (!host.open(parsed)) {
        std::fprintf(stderr, "cy_play_runtime_host: %s\n", host.complaint());
        host.shutdown_platform();
        return 4;
    }

    // THE HANDSHAKE, and it is the first thing on the wire. The version, so that a peer from
    // another commit is refused by number; and this process's own identifier, so that the launcher
    // can check the operating system agrees the answers are coming from the process it spawned.
    char welcome[128];
    bool waiting_for_debugger = parsed.wait_for_debugger;
    bool greeted = false;

    char line[1024];
    while (std::fgets(line, sizeof(line), stdin) != nullptr) {
        std::string_view command(line);
        while (!command.empty() && (command.back() == '\n' || command.back() == '\r')) {
            command.remove_suffix(1);
        }

        if (!greeted) {
            char expected[32];
            (void)std::snprintf(expected, sizeof(expected), "hello %u",
                                cy::gameplay::kPlayProtocolVersion);
            if (command != expected) {
                reply("refused this runtime host speaks a different protocol version");
                host.shutdown_platform();
                return 5;
            }
            (void)std::snprintf(welcome, sizeof(welcome), "welcome %u pid=%lld",
                                cy::gameplay::kPlayProtocolVersion, own_process_id());
            reply(welcome);
            greeted = true;
            continue;
        }

        if (!serve(host, command, waiting_for_debugger)) {
            break;
        }
    }

    host.shutdown_platform();
    return 0;
}
