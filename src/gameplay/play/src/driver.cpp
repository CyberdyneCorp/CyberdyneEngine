// One command vocabulary, two localities. See cy/gameplay/play/driver.h for the argument.

#include <cy/gameplay/play/driver.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace cy::gameplay {
namespace {

namespace ser = scene::serialization;

/// The height of the node an authoring identity names, read out of a `.cyworld`.
///
/// Both localities answer `translation_y` from the AUTHORED WORLD rather than from the ECS, and
/// that is deliberate: a play session publishes its simulated placements back into the authored
/// world every tick, so the authored world is the one surface both processes hold the same shape
/// of. Reading the local one from the ECS and the remote one from its published placements would
/// compare two different quantities and call the agreement a result.
[[nodiscard]] Expected<f32, Error> authored_height(const ser::World& world, u64 identity) noexcept {
    for (const ser::WorldNode& node : world.nodes()) {
        if (!node.live || node.identity != identity) {
            continue;
        }
        Transform placement;
        if (!ser::transform_of(world, node, placement)) {
            return fail(ErrorCode::NotFound, "that node carries no Transform this build can read");
        }
        return placement.translation.y;
    }
    return fail(ErrorCode::NotFound, "no live node in the world carries that identity");
}

/// A 32-bit IEEE-754 value from the eight hexadecimal digits the protocol carries it as.
[[nodiscard]] Expected<f32, Error> float_from_hex(std::string_view text) noexcept {
    if (text.size() != 8) {
        return fail(ErrorCode::InvalidArgument,
                    "a float crosses the play protocol as exactly eight hexadecimal digits");
    }
    char digits[9];
    std::memcpy(digits, text.data(), 8);
    digits[8] = '\0';
    char* end = nullptr;
    const unsigned long bits = std::strtoul(digits, &end, 16);
    if (end != digits + 8) {
        return fail(ErrorCode::InvalidArgument, "that is not a hexadecimal float");
    }
    const auto narrowed = static_cast<u32>(bits);
    f32 value = 0.0F;
    std::memcpy(&value, &narrowed, sizeof(value));
    return value;
}

/// The unsigned value of `key=<digits>` in a whitespace-separated reply, or nothing.
[[nodiscard]] Expected<u64, Error> field_of(std::string_view reply, std::string_view key) noexcept {
    usize at = 0;
    while (at < reply.size()) {
        const usize end = reply.find(' ', at);
        const std::string_view token =
            reply.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        if (token.size() > key.size() + 1 && token.starts_with(key) && token[key.size()] == '=') {
            const std::string_view digits = token.substr(key.size() + 1);
            char number[32];
            if (digits.empty() || digits.size() >= sizeof(number)) {
                return fail(ErrorCode::InvalidArgument, "that field's value is not a number");
            }
            std::memcpy(number, digits.data(), digits.size());
            number[digits.size()] = '\0';
            char* stop = nullptr;
            const unsigned long long value = std::strtoull(number, &stop, 10);
            if (*stop != '\0') {
                return fail(ErrorCode::InvalidArgument, "that field's value is not a number");
            }
            return static_cast<u64>(value);
        }
        if (end == std::string_view::npos) {
            break;
        }
        at = end + 1;
    }
    return fail(ErrorCode::NotFound,
                "the runtime host's reply is missing a field this build needs");
}

}  // namespace

// --- LocalPlayDriver ---------------------------------------------------------------------------

LocalPlayDriver::LocalPlayDriver(PlaySession& session, const PlayConfiguration& configuration,
                                 ser::World& authored) noexcept
    : session_(&session), configuration_(configuration), authored_(&authored) {}

Status LocalPlayDriver::enter() noexcept {
    return session_->enter(configuration_);
}
Status LocalPlayDriver::tick() noexcept {
    return session_->tick();
}
Status LocalPlayDriver::pause() noexcept {
    return session_->pause();
}
Status LocalPlayDriver::resume() noexcept {
    return session_->resume();
}
Status LocalPlayDriver::step_tick() noexcept {
    return session_->step_tick();
}
Status LocalPlayDriver::step_frame() noexcept {
    return session_->step_frame();
}
Status LocalPlayDriver::stop() noexcept {
    return session_->stop();
}

Expected<f32, Error> LocalPlayDriver::translation_y(u64 identity) noexcept {
    return authored_height(*authored_, identity);
}

Expected<PlayObservation, Error> LocalPlayDriver::observe() noexcept {
    const PlayReport& report = session_->report();
    PlayObservation observation;
    observation.ticks = report.ticks;
    observation.stepped_ticks = report.stepped_ticks;
    observation.stepped_frames = report.stepped_frames;
    observation.entities = report.entities;
    observation.bodies = report.bodies;
    observation.restored_exactly = report.restored_exactly;
    return observation;
}

// --- ProcessPlayDriver -------------------------------------------------------------------------

ProcessPlayDriver::ProcessPlayDriver(RuntimeProcess& process, Allocator& allocator) noexcept
    : process_(&process), allocator_(&allocator) {}

Status ProcessPlayDriver::command(std::string_view request) noexcept {
    Array<char> reply(*allocator_);
    if (Status spoke = process_->request(request, reply); !spoke) {
        return spoke;
    }
    const std::string_view answer(reply.data(), reply.size());
    if (answer == "ok") {
        return ok();
    }
    // A refusal carries the child's own words, because the refusal is the interesting half: a
    // remote runtime that cannot step a frame says so, and a caller reading "refused" with no
    // reason cannot tell that from a crash.
    //
    // The message is a literal because `cy::Error` holds a `const char*`; what the child said goes
    // in the log the caller keeps, not in the error's storage.
    return fail(ErrorCode::Unsupported,
                "separate-process: the runtime host refused a command from the live bridge");
}

Status ProcessPlayDriver::enter() noexcept {
    return command("enter");
}
Status ProcessPlayDriver::tick() noexcept {
    return command("tick");
}
Status ProcessPlayDriver::pause() noexcept {
    return command("pause");
}
Status ProcessPlayDriver::resume() noexcept {
    return command("resume");
}
Status ProcessPlayDriver::step_tick() noexcept {
    return command("step-tick");
}
Status ProcessPlayDriver::step_frame() noexcept {
    return command("step-frame");
}
Status ProcessPlayDriver::stop() noexcept {
    return command("stop");
}

Expected<f32, Error> ProcessPlayDriver::translation_y(u64 identity) noexcept {
    char request[64];
    (void)std::snprintf(request, sizeof(request), "translation-y %llu",
                        static_cast<unsigned long long>(identity));

    Array<char> reply(*allocator_);
    if (Status spoke = process_->request(request, reply); !spoke) {
        return make_unexpected(spoke.error());
    }
    const std::string_view answer(reply.data(), reply.size());
    constexpr std::string_view kPrefix = "value ";
    if (answer.size() <= kPrefix.size() || !answer.starts_with(kPrefix)) {
        return fail(ErrorCode::NotFound,
                    "separate-process: the runtime host has no such node in its world");
    }
    return float_from_hex(answer.substr(kPrefix.size()));
}

Expected<PlayObservation, Error> ProcessPlayDriver::observe() noexcept {
    Array<char> reply(*allocator_);
    if (Status spoke = process_->request("report", reply); !spoke) {
        return make_unexpected(spoke.error());
    }
    const std::string_view answer(reply.data(), reply.size());
    if (!answer.starts_with("report")) {
        return fail(ErrorCode::Internal, "separate-process: that is not a report");
    }

    PlayObservation observation;
    // Every field is required. A reply missing one is a peer built from a different commit, and
    // filling the gap with a zero would turn that into a comparison that quietly passes.
    struct Binding {
        std::string_view key;
        u64* slot;
    };
    u64 ticks = 0;
    u64 stepped_ticks = 0;
    u64 stepped_frames = 0;
    u64 entities = 0;
    u64 bodies = 0;
    u64 restored = 0;
    const Binding bindings[] = {
        {"ticks", &ticks},
        {"stepped_ticks", &stepped_ticks},
        {"stepped_frames", &stepped_frames},
        {"entities", &entities},
        {"bodies", &bodies},
        {"restored_exactly", &restored},
    };
    for (const Binding& binding : bindings) {
        const Expected<u64, Error> value = field_of(answer, binding.key);
        if (!value) {
            return make_unexpected(value.error());
        }
        *binding.slot = *value;
    }
    observation.ticks = ticks;
    observation.stepped_ticks = stepped_ticks;
    observation.stepped_frames = stepped_frames;
    observation.entities = static_cast<u32>(entities);
    observation.bodies = static_cast<u32>(bodies);
    observation.restored_exactly = restored != 0;
    return observation;
}

}  // namespace cy::gameplay
