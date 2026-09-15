// The second runtime process. See cy/gameplay/play/launcher.h for the argument.

#include <cy/core/assets/file.h>
#include <cy/core/memory/system_allocator.h>
#include <cy/gameplay/play/launcher.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace cy::gameplay {
namespace {

/// The executable suffix this platform's binaries carry. A resolution rule that forgot it would
/// find nothing on Windows and would report the launcher absent on a build that has one.
#if defined(_WIN32)
constexpr std::string_view kExecutableSuffix = ".exe";
#else
// Written as an explicit empty view rather than defaulted, so the two branches read as two answers
// to one question.
constexpr std::string_view kExecutableSuffix = std::string_view();
#endif

/// The largest path this resolution will build. A path longer than this is a broken installation
/// rather than a case to grow a buffer for, and it is reported as one.
constexpr usize kPathCapacity = 4096;

/// Append the bytes of `text` to `out`. Extracted because every assembly below is a sequence of
/// these and a hand-written loop per step is where an off-by-one lives.
[[nodiscard]] Status append_text(Array<char>& out, std::string_view text) noexcept {
    return out.append(Span<const char>(text.data(), text.size()));
}

/// Append `text` and then a NUL, so the result can be handed to a C interface.
[[nodiscard]] Status append_terminated(Array<char>& out, std::string_view text) noexcept {
    if (Status added = append_text(out, text); !added) {
        return added;
    }
    return out.push_back('\0');
}

/// The directory part of `path`, including the trailing separator. Empty when there is none, which
/// means "the working directory" and is the right answer for a bare executable name.
[[nodiscard]] std::string_view directory_of(std::string_view path) noexcept {
    for (usize index = path.size(); index > 0; --index) {
        const char character = path[index - 1];
        if (character == '/' || character == '\\') {
            return path.substr(0, index);
        }
    }
    return {};
}

}  // namespace

Status runtime_host_path(const Platform& platform, Array<char>& out) noexcept {
    char buffer[kPathCapacity];
    const Expected<usize, Error> length = platform.executable_path(buffer, sizeof(buffer));
    if (!length) {
        return make_unexpected(length.error());
    }
    if (*length == 0 || *length >= sizeof(buffer)) {
        return fail(ErrorCode::Internal,
                    "the platform reported an executable path this build cannot work from");
    }

    out.clear();
    const std::string_view directory = directory_of(std::string_view(buffer, *length));
    if (Status added = append_text(out, directory); !added) {
        return added;
    }
    if (Status added = append_text(out, kRuntimeHostBinary); !added) {
        return added;
    }
    return append_terminated(out, kExecutableSuffix);
}

bool runtime_launcher_available(const Platform& platform) noexcept {
    // The allocator is the resolution's own and lives no longer than the question: a query about a
    // build must not need a caller to supply storage, or the callers that have none — `mode.h`'s
    // no-argument support query is one — could not ask it.
    Array<char> path(system_allocator(MemoryDomain::World));
    if (!runtime_host_path(platform, path)) {
        return false;
    }
    // The file, on disk, now. This is the whole point of the change: a build whose host binary was
    // never built, was deleted, or was renamed reports the mode unavailable instead of claiming a
    // launcher it does not have.
    return assets::fs::exists(path.data());
}

PlayModeSupport play_mode_support(const Platform& platform) noexcept {
    PlayModeSupport support = play_mode_support();
    support.runtime_launcher = runtime_launcher_available(platform);
    return support;
}

// --- RuntimeProcess ----------------------------------------------------------------------------

RuntimeProcess::RuntimeProcess(Allocator& allocator) noexcept
    : allocator_(&allocator), path_(allocator), pending_(allocator) {}

RuntimeProcess::~RuntimeProcess() {
    if (handle_ == 0) {
        return;
    }
    // A destructor that left a child running would leak a process per failed test, and a runtime
    // that outlives the thing that launched it is the supervision failure this class is named for.
    if (!reaped_) {
        (void)platform_->terminate_process(handle_, true);
        (void)platform_->wait_process(handle_);
    }
    platform_->release_process(handle_);
    handle_ = 0;
}

std::string_view RuntimeProcess::binary_path() const noexcept {
    if (path_.size() <= 1) {
        return {};
    }
    // The array holds a trailing NUL that is not part of the path.
    return {path_.data(), path_.size() - 1};
}

Status RuntimeProcess::launch(Platform& platform, const RuntimeLaunchRequest& request) noexcept {
    if (handle_ != 0) {
        return fail(ErrorCode::AlreadyExists, "this RuntimeProcess has already launched a child");
    }
    if (request.world_path.empty()) {
        return fail(ErrorCode::InvalidArgument,
                    "separate-process play needs a world for the second process to play");
    }
    platform_ = &platform;

    path_.clear();
    if (request.binary.empty()) {
        if (Status resolved = runtime_host_path(platform, path_); !resolved) {
            return resolved;
        }
    } else if (Status copied = append_terminated(path_, request.binary); !copied) {
        return copied;
    }
    if (!assets::fs::exists(path_.data())) {
        return fail(ErrorCode::NotFound,
                    "separate-process: the runtime host binary is not beside this executable — "
                    "this build cannot launch a second runtime process");
    }

    // The argument vector. The strings must outlive the spawn call, which is what these arrays are
    // for: a `std::string_view` the caller owns may be a slice of something temporary.
    Array<char> world(*allocator_);
    if (Status copied = append_terminated(world, request.world_path); !copied) {
        return copied;
    }
    Array<char> asset(*allocator_);
    if (Status copied = append_terminated(
            asset, request.asset_path.empty() ? request.world_path : request.asset_path);
        !copied) {
        return copied;
    }
    char capacity_text[32];
    (void)std::snprintf(capacity_text, sizeof(capacity_text), "%u", request.body_capacity);

    const char* arguments[10];
    usize argument_count = 0;
    arguments[argument_count++] = path_.data();
    arguments[argument_count++] = "--world";
    arguments[argument_count++] = world.data();
    arguments[argument_count++] = "--asset-path";
    arguments[argument_count++] = asset.data();
    arguments[argument_count++] = "--body-capacity";
    arguments[argument_count++] = capacity_text;
    if (request.wait_for_debugger) {
        arguments[argument_count++] = "--wait-for-debugger";
    }

    ProcessOptions options;
    options.arguments = arguments;
    options.argument_count = argument_count;
    // The channel. Without it the child is started and not addressable, which is the difference
    // between a process that ran and a play mode that works.
    options.piped_standard_streams = true;

    const Expected<ProcessHandle, Error> spawned = platform.spawn_process(options);
    if (!spawned) {
        return make_unexpected(spawned.error());
    }
    handle_ = *spawned;
    reaped_ = false;

    const Expected<i64, Error> observed = platform.process_id(handle_);
    if (!observed) {
        (void)kill();
        return make_unexpected(observed.error());
    }
    observed_pid_ = *observed;

    // THE HANDSHAKE. Version first, and the child's own idea of its process identifier with it.
    Array<char> reply(*allocator_);
    char hello[32];
    (void)std::snprintf(hello, sizeof(hello), "hello %u", kPlayProtocolVersion);
    if (Status spoke = this->request(hello, reply); !spoke) {
        (void)kill();
        return spoke;
    }

    const std::string_view welcome(reply.data(), reply.size());
    char expected[64];
    (void)std::snprintf(expected, sizeof(expected), "welcome %u pid=", kPlayProtocolVersion);
    const std::string_view prefix(expected);
    if (welcome.size() <= prefix.size() || !welcome.starts_with(prefix)) {
        (void)kill();
        return fail(ErrorCode::Unsupported,
                    "separate-process: the runtime host answered a protocol version this build "
                    "does not speak — the two sides were built from different commits");
    }

    // `strtoll` over a view that is not NUL-terminated would read past it, so the digits are copied
    // into a bounded buffer first.
    const std::string_view digits = welcome.substr(prefix.size());
    char number[32];
    if (digits.empty() || digits.size() >= sizeof(number)) {
        (void)kill();
        return fail(ErrorCode::Internal,
                    "separate-process: the runtime host reported no usable process identifier");
    }
    std::memcpy(number, digits.data(), digits.size());
    number[digits.size()] = '\0';
    reported_pid_ = static_cast<i64>(std::strtoll(number, nullptr, 10));

    // THE CHECK THAT CANNOT BE SATISFIED IN ONE PROCESS. The child says which process it is; the
    // operating system, asked independently by this process, says the same. An implementation that
    // answered this protocol from a local object would have to produce an identifier the operating
    // system agrees belongs to a child it spawned, and there is none to produce.
    if (reported_pid_ == 0 || reported_pid_ != observed_pid_) {
        (void)kill();
        return fail(ErrorCode::Internal,
                    "separate-process: the process that answered is not the process that was "
                    "launched — the identifier it reported is not the one the operating system "
                    "gave this launch");
    }
    return ok();
}

Status RuntimeProcess::read_line(Array<char>& line) noexcept {
    line.clear();
    i64 deadline = platform_->monotonic_nanoseconds() + kReplyDeadlineNanoseconds;
    for (;;) {
        // Anything already buffered first: a pipe splits where it likes, and a reply may have
        // arrived in the same read as the previous one.
        for (usize index = 0; index < pending_.size(); ++index) {
            if (pending_[index] != '\n') {
                continue;
            }
            if (Status copied = line.append(Span<const char>(pending_.data(), index)); !copied) {
                return copied;
            }
            // Keep what follows the newline; discarding it would lose the next reply.
            const usize remaining = pending_.size() - index - 1;
            for (usize move = 0; move < remaining; ++move) {
                pending_[move] = pending_[index + 1 + move];
            }
            if (Status trimmed = pending_.resize(remaining); !trimmed) {
                return trimmed;
            }
            return ok();
        }

        char buffer[512];
        const Expected<usize, Error> read =
            platform_->read_process_output(handle_, buffer, sizeof(buffer));
        if (!read) {
            return make_unexpected(read.error());
        }
        if (*read > 0) {
            if (Status kept = pending_.append(Span<const char>(buffer, *read)); !kept) {
                return kept;
            }
            // A byte arrived, so the child is answering: the deadline starts again from here rather
            // than from the request, or a long reply delivered in small pieces would time out.
            deadline = platform_->monotonic_nanoseconds() + kReplyDeadlineNanoseconds;
            continue;
        }

        // Nothing at this instant, which the platform deliberately does not distinguish from end of
        // stream. `poll_process` is the call whose subject is whether the child is alive, so it is
        // the one asked.
        const Expected<ProcessStatus, Error> alive = platform_->poll_process(handle_);
        if (!alive) {
            return make_unexpected(alive.error());
        }
        if (!alive->running) {
            return fail(ErrorCode::Unavailable,
                        "separate-process: the runtime host exited before it replied — the second "
                        "process died");
        }
        if (platform_->monotonic_nanoseconds() > deadline) {
            return fail(ErrorCode::Unavailable,
                        "separate-process: the runtime host is alive and has not answered — the "
                        "second process is unresponsive");
        }
        // A short sleep rather than a spin: the child is doing the work, and a busy parent on a
        // one-core runner would be taking the processor away from the thing it is waiting for.
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
}

Status RuntimeProcess::request(std::string_view command, Array<char>& reply) noexcept {
    if (handle_ == 0 || reaped_) {
        return fail(ErrorCode::Unavailable,
                    "separate-process: there is no second runtime process to drive");
    }

    Array<char> line(*allocator_);
    if (Status built = append_text(line, command); !built) {
        return built;
    }
    if (Status built = line.push_back('\n'); !built) {
        return built;
    }

    // A pipe may accept fewer bytes than asked for, so the write loops rather than assuming.
    usize written = 0;
    while (written < line.size()) {
        const Expected<usize, Error> sent = platform_->write_process_input(
            handle_, std::string_view(line.data() + written, line.size() - written));
        if (!sent) {
            return make_unexpected(sent.error());
        }
        if (*sent == 0) {
            return fail(ErrorCode::Unavailable,
                        "separate-process: the runtime host stopped accepting input");
        }
        written += *sent;
    }
    return read_line(reply);
}

Expected<i32, Error> RuntimeProcess::shutdown() noexcept {
    if (handle_ == 0) {
        return fail(ErrorCode::Unavailable, "no second runtime process was launched");
    }
    if (reaped_) {
        return exit_code_;
    }

    // Asked to leave rather than killed, so that the exit code means something: a child that
    // completed its work exits zero, and a child that was killed cannot be told from one that
    // crashed.
    Array<char> reply(*allocator_);
    (void)request("quit", reply);
    (void)platform_->close_process_input(handle_);

    const Expected<i32, Error> code = platform_->wait_process(handle_);
    if (!code) {
        return make_unexpected(code.error());
    }
    reaped_ = true;
    exit_code_ = *code;
    return exit_code_;
}

Status RuntimeProcess::kill() noexcept {
    if (handle_ == 0 || reaped_) {
        return ok();
    }
    if (Status killed = platform_->terminate_process(handle_, true); !killed) {
        return killed;
    }
    const Expected<i32, Error> code = platform_->wait_process(handle_);
    if (!code) {
        return make_unexpected(code.error());
    }
    reaped_ = true;
    exit_code_ = *code;
    return ok();
}

}  // namespace cy::gameplay
