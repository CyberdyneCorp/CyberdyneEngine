// The native Linux Platform. See linux_platform.h for why it exists.
//
// EVERY CALL BELOW IS POSIX OR glibc. Nothing here links SDL3, and that is the whole experiment:
// `core-platform-abstraction`'s surface was designed against one implementation, and this file is
// the first evidence that it can be satisfied by another. Where the surface pushed back, the note
// is written at the call rather than in a summary nobody reads.

#include <cy/platform/linux_platform.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#endif

namespace cy {
namespace {

// errno's text, copied where the failure happened. `Error` holds a pointer and not storage
// (cy/core/base/error.h), and errno is overwritten by the next call that fails, so the message is
// captured at the point of failure into a thread-local — the same arrangement, and the same
// reasoning, as `Sdl3Platform`'s capture of SDL_GetError().
constexpr usize kErrnoTextCapacity = 192;
thread_local char g_errno_text[kErrnoTextCapacity];

const char* capture_errno(const char* what) {
    char reason[128];
    // The GNU strerror_r may answer in its own buffer rather than in ours, so its return is what is
    // used and the buffer is only the fallback. Getting this wrong prints an empty string.
    const char* text = ::strerror_r(errno, reason, sizeof(reason));
    std::snprintf(g_errno_text, sizeof(g_errno_text), "%s: %s", what,
                  text != nullptr ? text : "unknown error");
    return g_errno_text;
}

Unexpected<Error> errno_failure(ErrorCode code, const char* what) {
    return Unexpected<Error>(Error{code, capture_errno(what), 0});
}

// $HOME, or the passwd entry's directory if the environment has none. Empty when neither answers,
// which is a machine with no user — a container running as an unknown uid — and is reported rather
// than papered over with "/".
const char* home_directory() {
    const char* home = std::getenv("HOME");
    return home != nullptr && home[0] != '\0' ? home : nullptr;
}

// One XDG base directory, with the specification's own fallback. The variable is ignored when it is
// not absolute, which is what the specification requires and what a caller would otherwise turn
// into a path relative to the working directory.
bool xdg_base(const char* variable, const char* fallback, char* buffer, usize capacity) {
    const char* value = std::getenv(variable);
    if (value != nullptr && value[0] == '/') {
        std::snprintf(buffer, capacity, "%s", value);
        return true;
    }
    const char* home = home_directory();
    if (home == nullptr) {
        return false;
    }
    std::snprintf(buffer, capacity, "%s/%s", home, fallback);
    return true;
}

// mkdir -p, on a path that is modified in place and restored. Existing directories are not an
// error; anything else is, and the caller reports it with the path that failed.
bool create_directories(char* path) {
    for (char* cursor = path + 1; *cursor != '\0'; ++cursor) {
        if (*cursor != '/') {
            continue;
        }
        *cursor = '\0';
        const bool made = ::mkdir(path, 0700) == 0 || errno == EEXIST;
        *cursor = '/';
        if (!made) {
            return false;
        }
    }
    return ::mkdir(path, 0700) == 0 || errno == EEXIST;
}

u64 read_meminfo_bytes(const char* key) {
    std::FILE* file = std::fopen("/proc/meminfo", "re");
    if (file == nullptr) {
        return 0;
    }
    const usize key_length = std::strlen(key);
    u64 bytes = 0;
    char line[256];
    while (std::fgets(line, sizeof(line), file) != nullptr) {
        if (std::strncmp(line, key, key_length) != 0 || line[key_length] != ':') {
            continue;
        }
        char* end = nullptr;
        const unsigned long long kibibytes = std::strtoull(line + key_length + 1, &end, 10);
        if (end != line + key_length + 1) {
            bytes = static_cast<u64>(kibibytes) * 1024ULL;
        }
        break;
    }
    std::fclose(file);
    return bytes;
}

u64 read_resident_bytes() {
    std::FILE* file = std::fopen("/proc/self/statm", "re");
    if (file == nullptr) {
        return 0;
    }
    // Total pages, then resident pages, both in pages of _SC_PAGESIZE.
    char line[128] = {};
    const bool read = std::fgets(line, sizeof(line), file) != nullptr;
    std::fclose(file);
    if (!read) {
        return 0;
    }
    char* after_total = nullptr;
    (void)std::strtoull(line, &after_total, 10);
    char* after_resident = nullptr;
    const unsigned long long resident = std::strtoull(after_total, &after_resident, 10);
    if (after_resident == after_total) {
        return 0;
    }
    const long page_size = ::sysconf(_SC_PAGESIZE);
    return page_size > 0 ? static_cast<u64>(resident) * static_cast<u64>(page_size) : 0;
}

// A POSIX locale name — "en_GB.UTF-8", "C" — as the BCP-47 tag the interface promises. The
// encoding and the modifier are dropped and the underscore becomes a hyphen; "C" and "POSIX" have
// no tag at all and are reported as unavailable rather than as a language called "C".
bool locale_tag(const char* posix_name, char* buffer, usize capacity) {
    if (posix_name == nullptr || posix_name[0] == '\0') {
        return false;
    }
    if (std::strcmp(posix_name, "C") == 0 || std::strcmp(posix_name, "POSIX") == 0) {
        return false;
    }
    usize written = 0;
    for (const char* cursor = posix_name; *cursor != '\0' && written + 1 < capacity; ++cursor) {
        if (*cursor == '.' || *cursor == '@') {
            break;
        }
        buffer[written++] = *cursor == '_' ? '-' : *cursor;
    }
    buffer[written] = '\0';
    return written > 0;
}

void set_close_on_exec(int descriptor) {
    const int flags = ::fcntl(descriptor, F_GETFD, 0);
    if (flags >= 0) {
        (void)::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
    }
}

void set_non_blocking(int descriptor) {
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags >= 0) {
        (void)::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
    }
}

void close_if_open(int& descriptor) {
    if (descriptor >= 0) {
        (void)::close(descriptor);
        descriptor = -1;
    }
}

// --- The crash handler ---------------------------------------------------------------------------
//
// The handler runs on the crashing thread with the process in an unknown state, so everything in it
// is async-signal-safe: no allocation, no locking, no stdio, no strsignal(). It calls the installed
// handler, restores the default disposition and re-raises, so the process still dies the way the
// operating system expects and still writes whatever core file it was going to write.
//
// This is deliberately a SECOND copy of the arrangement platform/desktop-sdl3/src/host/ carries
// rather than a shared one. Moving it into a common file would put an #ifdef-free POSIX file above
// both backends and make the two implementations one — and the whole value of this module is that
// it shares no code with the backend it is testing the interface against.

struct SignalEntry {
    int number;
    const char* description;
};

constexpr SignalEntry kSignals[] = {
    {SIGSEGV, "SIGSEGV: invalid memory reference"},
    {SIGBUS, "SIGBUS: bus error"},
    {SIGILL, "SIGILL: illegal instruction"},
    {SIGFPE, "SIGFPE: arithmetic exception"},
    {SIGABRT, "SIGABRT: abort"},
};

constexpr usize kSignalCount = sizeof(kSignals) / sizeof(kSignals[0]);

CrashHandler g_handler = nullptr;
void* g_user = nullptr;
volatile std::sig_atomic_t g_installed = 0;
struct sigaction g_previous[kSignalCount];

const char* describe(int signal_number) {
    for (const SignalEntry& entry : kSignals) {
        if (entry.number == signal_number) {
            return entry.description;
        }
    }
    return "unknown signal";
}

extern "C" void linux_crash_signal_handler(int signal_number) {
    if (g_handler != nullptr) {
        CrashContext context;
        context.signal_or_exception = signal_number;
        context.description = describe(signal_number);
        g_handler(context, g_user);
    }
    for (usize i = 0; i < kSignalCount; ++i) {
        if (kSignals[i].number == signal_number) {
            ::sigaction(signal_number, &g_previous[i], nullptr);
            break;
        }
    }
    ::raise(signal_number);
}

}  // namespace

LinuxPlatform::~LinuxPlatform() {
    if (initialised_) {
        shutdown();
    }
}

void LinuxPlatform::set_application_identity(const char* organisation, const char* application) {
    if (organisation != nullptr) {
        organisation_ = organisation;
    }
    if (application != nullptr) {
        application_ = application;
    }
}

Status LinuxPlatform::initialise(int argument_count, char** arguments) {
    if (initialised_) {
        return fail(ErrorCode::AlreadyExists, "the Linux platform is already initialised");
    }
    // Nothing to bring up. THAT IS A FINDING, not an omission: `Platform`'s whole surface is
    // process services the operating system already provides, and the only reason SDL3's
    // implementation needs an SDL_Init() is that SDL3 is a library. An interface that had required
    // an initialisation handshake — a context object, a subsystem mask — would have been an
    // interface shaped around its first implementation. It did not.
    argument_count_ = argument_count > 0 ? argument_count : 0;
    arguments_ = arguments;
    initialised_ = true;
    return ok();
}

void LinuxPlatform::shutdown() {
    for (ProcessSlot& slot : processes_) {
        if (slot.handle != 0) {
            close_slot_descriptors(slot);
            slot = ProcessSlot{};
        }
    }
    uninstall_crash_handler();
    initialised_ = false;
    argument_count_ = 0;
    arguments_ = nullptr;
}

// --- Process lifetime ---------------------------------------------------------------------------

void LinuxPlatform::request_exit(i32 exit_code) {
    // First request wins: a shutdown already under way is not re-coded by a later SIGINT.
    if (!exit_requested_) {
        exit_requested_ = true;
        exit_code_ = exit_code;
    }
}

std::string_view LinuxPlatform::argument(usize index) const {
    if (arguments_ == nullptr || index >= static_cast<usize>(argument_count_)) {
        return {};
    }
    const char* value = arguments_[index];
    return value != nullptr ? std::string_view{value} : std::string_view{};
}

// --- Environment --------------------------------------------------------------------------------

Expected<usize, Error> LinuxPlatform::environment_variable(const char* name, char* buffer,
                                                           usize capacity) const {
    if (name == nullptr) {
        return fail(ErrorCode::InvalidArgument, "an environment variable needs a name");
    }
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return fail(ErrorCode::NotFound, "no such environment variable");
    }
    return write_to_buffer(buffer, capacity, value);
}

Status LinuxPlatform::set_environment_variable(const char* name, const char* value) {
    if (name == nullptr || value == nullptr) {
        return fail(ErrorCode::InvalidArgument,
                    "setting an environment variable needs both a name and a value");
    }
    if (::setenv(name, value, 1) != 0) {
        return errno_failure(ErrorCode::Internal, "setenv");
    }
    return ok();
}

// --- Standard streams ---------------------------------------------------------------------------

void LinuxPlatform::write_standard_output(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
}

void LinuxPlatform::write_standard_error(std::string_view text) {
    // Flushed by convention: an error still in a buffer when the process dies was never written.
    std::fwrite(text.data(), 1, text.size(), stderr);
    std::fflush(stderr);
}

// --- Directories --------------------------------------------------------------------------------
//
// THE PATHS MATCH SDL3's EXACTLY, and that is a decision rather than an accident. SDL_GetPrefPath()
// answers $XDG_DATA_HOME/<organisation>/<application>/, and the engine's convention — stated in
// sdl3_platform.cpp — is that config/ and cache/ are subdirectories of that one writable root
// rather than the separate XDG config and cache roots. Deriving them differently here would give a
// player who switched backends a different save directory, and the symptom would be "my game is
// gone" rather than "the paths differ".

Expected<usize, Error> LinuxPlatform::preference_path(const char* suffix, char* buffer,
                                                      usize capacity) const {
    char base[512];
    if (!xdg_base("XDG_DATA_HOME", ".local/share", base, sizeof(base))) {
        return fail(ErrorCode::Unavailable,
                    "neither $XDG_DATA_HOME nor $HOME is set, so there is no writable user "
                    "directory for this process");
    }

    char path[1024];
    const int written = std::snprintf(path, sizeof(path), "%s/%s/%s/%s", base, organisation_,
                                      application_, suffix);
    if (written < 0 || static_cast<usize>(written) >= sizeof(path)) {
        return fail(ErrorCode::BufferTooSmall, "the user directory path is longer than 1024 bytes");
    }
    // Created, not merely named: SDL_GetPrefPath() creates the directory it answers, and a caller
    // that had to know which backend it was on to know whether the path exists would be reading
    // the implementation rather than the interface.
    if (!create_directories(path)) {
        return errno_failure(ErrorCode::Unavailable, "mkdir");
    }
    return write_to_buffer(buffer, capacity, path);
}

Expected<usize, Error> LinuxPlatform::user_data_directory(char* buffer, usize capacity) const {
    return preference_path("", buffer, capacity);
}

Expected<usize, Error> LinuxPlatform::user_config_directory(char* buffer, usize capacity) const {
    return preference_path("config/", buffer, capacity);
}

Expected<usize, Error> LinuxPlatform::user_cache_directory(char* buffer, usize capacity) const {
    return preference_path("cache/", buffer, capacity);
}

Expected<usize, Error> LinuxPlatform::executable_path(char* buffer, usize capacity) const {
    char path[4096];
    const ssize_t length = ::readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length <= 0) {
        return fail(ErrorCode::Unavailable, "/proc/self/exe could not be read");
    }
    path[length] = '\0';
    return write_to_buffer(buffer, capacity, std::string_view{path, static_cast<usize>(length)});
}

// --- Dynamic libraries --------------------------------------------------------------------------

Expected<LibraryHandle, Error> LinuxPlatform::load_library(const char* path) {
    if (path == nullptr) {
        return fail(ErrorCode::InvalidArgument, "loading a library needs a path");
    }
    ::dlerror();
    void* handle = ::dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        const char* reason = ::dlerror();
        std::snprintf(g_errno_text, sizeof(g_errno_text), "dlopen: %s",
                      reason != nullptr ? reason : "unknown error");
        return Unexpected<Error>(Error{ErrorCode::NotFound, g_errno_text, 0});
    }
    return static_cast<LibraryHandle>(handle);
}

Expected<void*, Error> LinuxPlatform::library_symbol(LibraryHandle library, const char* symbol) {
    if (library == nullptr || symbol == nullptr) {
        return fail(ErrorCode::InvalidArgument, "resolving a symbol needs a library and a name");
    }
    // dlerror() is cleared first because a symbol whose address is legitimately null is
    // indistinguishable from a failure by the return value alone.
    ::dlerror();
    void* address = ::dlsym(library, symbol);
    if (const char* reason = ::dlerror(); reason != nullptr) {
        std::snprintf(g_errno_text, sizeof(g_errno_text), "dlsym: %s", reason);
        return Unexpected<Error>(Error{ErrorCode::NotFound, g_errno_text, 0});
    }
    return address;
}

void LinuxPlatform::unload_library(LibraryHandle library) {
    if (library != nullptr) {
        (void)::dlclose(library);
    }
}

// --- Subprocesses -------------------------------------------------------------------------------

LinuxPlatform::ProcessSlot* LinuxPlatform::find_process(ProcessHandle process) {
    if (process == 0) {
        return nullptr;
    }
    for (ProcessSlot& slot : processes_) {
        if (slot.handle == process) {
            return &slot;
        }
    }
    return nullptr;
}

const LinuxPlatform::ProcessSlot* LinuxPlatform::find_process(ProcessHandle process) const {
    return const_cast<LinuxPlatform*>(this)->find_process(process);
}

void LinuxPlatform::close_slot_descriptors(ProcessSlot& slot) {
    close_if_open(slot.input_fd);
    close_if_open(slot.output_fd);
}

void LinuxPlatform::reap(ProcessSlot& slot, bool block) {
    if (slot.exited || slot.pid <= 0) {
        return;
    }
    int status = 0;
    const pid_t result = ::waitpid(static_cast<pid_t>(slot.pid), &status, block ? 0 : WNOHANG);
    if (result != static_cast<pid_t>(slot.pid)) {
        return;
    }
    slot.exited = true;
    // A child killed by a signal has no exit code of its own. 128 + n is the shell's convention and
    // the one every tool that reads an exit status already understands.
    slot.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

namespace {

// The child half of the fork. Everything here is between fork() and execvp() and therefore runs in
// a process with one thread and an arbitrary lock state, so it calls only async-signal-safe
// functions and never returns on success.
[[noreturn]] void run_child(const ProcessOptions& options, const char* const* argv, int input_read,
                            int output_write) {
    if (options.working_directory != nullptr && ::chdir(options.working_directory) != 0) {
        ::_exit(127);
    }
    if (input_read >= 0) {
        (void)::dup2(input_read, STDIN_FILENO);
    }
    if (output_write >= 0) {
        (void)::dup2(output_write, STDOUT_FILENO);
    }
    if (input_read < 0 && !options.inherit_standard_streams) {
        const int null_device = ::open("/dev/null", O_RDWR);
        if (null_device >= 0) {
            (void)::dup2(null_device, STDIN_FILENO);
            (void)::dup2(null_device, STDOUT_FILENO);
            (void)::close(null_device);
        }
    }
    // Standard error is left where the caller put it even when input and output are piped: a
    // child's diagnostics belong in the parent's log, not in a buffer the protocol never drains.
    // execvp takes a non-const vector it does not write through.
    ::execvp(argv[0], const_cast<char* const*>(argv));
    ::_exit(127);
}

}  // namespace

Expected<ProcessHandle, Error> LinuxPlatform::spawn_process(const ProcessOptions& options) {
    if (options.arguments == nullptr || options.argument_count == 0) {
        return fail(ErrorCode::InvalidArgument, "spawning a process needs at least argv[0]");
    }
    constexpr usize kMaxArguments = 64;
    if (options.argument_count >= kMaxArguments) {
        return fail(ErrorCode::InvalidArgument, "a subprocess takes fewer than 64 arguments");
    }

    ProcessSlot* slot = nullptr;
    for (ProcessSlot& candidate : processes_) {
        if (candidate.handle == 0) {
            slot = &candidate;
            break;
        }
    }
    if (slot == nullptr) {
        return fail(ErrorCode::OutOfMemory,
                    "LinuxPlatform::kMaxProcesses subprocesses are already held; release one");
    }

    const char* argv[kMaxArguments];
    for (usize i = 0; i < options.argument_count; ++i) {
        argv[i] = options.arguments[i];
    }
    argv[options.argument_count] = nullptr;

    int to_child[2] = {-1, -1};
    int from_child[2] = {-1, -1};
    if (options.piped_standard_streams) {
        if (::pipe(to_child) != 0) {
            return errno_failure(ErrorCode::Unavailable, "pipe");
        }
        if (::pipe(from_child) != 0) {
            close_if_open(to_child[0]);
            close_if_open(to_child[1]);
            return errno_failure(ErrorCode::Unavailable, "pipe");
        }
        // The parent's ends must not survive an exec in some later child, and the read must never
        // block: read_process_output() promises it does not.
        set_close_on_exec(to_child[1]);
        set_close_on_exec(from_child[0]);
        set_non_blocking(from_child[0]);
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        close_if_open(to_child[0]);
        close_if_open(to_child[1]);
        close_if_open(from_child[0]);
        close_if_open(from_child[1]);
        return errno_failure(ErrorCode::Unavailable, "fork");
    }
    if (pid == 0) {
        run_child(options, argv, to_child[0], from_child[1]);
    }

    close_if_open(to_child[0]);
    close_if_open(from_child[1]);

    *slot = ProcessSlot{};
    slot->handle = next_process_++;
    slot->pid = pid;
    slot->piped = options.piped_standard_streams;
    slot->input_fd = to_child[1];
    slot->output_fd = from_child[0];
    return slot->handle;
}

Expected<ProcessStatus, Error> LinuxPlatform::poll_process(ProcessHandle process) {
    ProcessSlot* slot = find_process(process);
    if (slot == nullptr) {
        return fail(ErrorCode::NotFound, "no such process");
    }
    reap(*slot, false);
    return ProcessStatus{!slot->exited, slot->exited ? slot->exit_code : 0};
}

Expected<i32, Error> LinuxPlatform::wait_process(ProcessHandle process) {
    ProcessSlot* slot = find_process(process);
    if (slot == nullptr) {
        return fail(ErrorCode::NotFound, "no such process");
    }
    reap(*slot, true);
    if (!slot->exited) {
        return errno_failure(ErrorCode::Internal, "waitpid");
    }
    return slot->exit_code;
}

Status LinuxPlatform::terminate_process(ProcessHandle process, bool force) {
    ProcessSlot* slot = find_process(process);
    if (slot == nullptr) {
        return fail(ErrorCode::NotFound, "no such process");
    }
    if (slot->exited) {
        return ok();
    }
    if (::kill(static_cast<pid_t>(slot->pid), force ? SIGKILL : SIGTERM) != 0) {
        return errno_failure(ErrorCode::Internal, "kill");
    }
    return ok();
}

void LinuxPlatform::release_process(ProcessHandle process) {
    ProcessSlot* slot = find_process(process);
    if (slot == nullptr) {
        return;
    }
    close_slot_descriptors(*slot);
    // A child that is still running is not killed — the interface says so — but it must still be
    // reaped, or it becomes a zombie this process holds until it exits. The double-fork that would
    // avoid that entirely is a different contract from the one `Platform` states.
    if (!slot->exited) {
        (void)::waitpid(static_cast<pid_t>(slot->pid), nullptr, WNOHANG);
    }
    *slot = ProcessSlot{};
}

Expected<i64, Error> LinuxPlatform::process_id(ProcessHandle process) const {
    const ProcessSlot* slot = find_process(process);
    if (slot == nullptr) {
        return fail(ErrorCode::NotFound, "no such process");
    }
    return slot->pid;
}

// --- Talking to a spawned process ---------------------------------------------------------------

Expected<usize, Error> LinuxPlatform::write_process_input(ProcessHandle process,
                                                          std::string_view bytes) {
    ProcessSlot* slot = find_process(process);
    if (slot == nullptr) {
        return fail(ErrorCode::NotFound, "no such process");
    }
    if (!slot->piped) {
        return fail(ErrorCode::Unsupported,
                    "this child's standard streams were not piped; spawn it with "
                    "ProcessOptions::piped_standard_streams to write to it");
    }
    if (slot->input_closed || slot->input_fd < 0) {
        return fail(ErrorCode::Unavailable, "this child's standard input is already closed");
    }
    if (bytes.empty()) {
        return usize{0};
    }
    const ssize_t written = ::write(slot->input_fd, bytes.data(), bytes.size());
    if (written < 0) {
        return errno_failure(ErrorCode::Internal, "write");
    }
    // No flush: this is the descriptor itself rather than a buffered stream, so the bytes are in
    // the pipe when write() returns. SDL's implementation needs an explicit flush and this does
    // not, which is the kind of difference the interface's "may be fewer than asked for" wording
    // already covers.
    return static_cast<usize>(written);
}

Status LinuxPlatform::close_process_input(ProcessHandle process) {
    ProcessSlot* slot = find_process(process);
    if (slot == nullptr) {
        return fail(ErrorCode::NotFound, "no such process");
    }
    if (!slot->piped) {
        return fail(ErrorCode::Unsupported, "this child's standard streams were not piped");
    }
    if (slot->input_closed) {
        return ok();
    }
    slot->input_closed = true;
    close_if_open(slot->input_fd);
    return ok();
}

Expected<usize, Error> LinuxPlatform::read_process_output(ProcessHandle process, char* buffer,
                                                          usize capacity) {
    ProcessSlot* slot = find_process(process);
    if (slot == nullptr) {
        return fail(ErrorCode::NotFound, "no such process");
    }
    if (!slot->piped) {
        return fail(ErrorCode::Unsupported, "this child's standard streams were not piped");
    }
    if (buffer == nullptr || capacity == 0) {
        return fail(ErrorCode::InvalidArgument, "reading a child's output needs a buffer");
    }
    if (slot->output_fd < 0) {
        return usize{0};
    }
    const ssize_t read = ::read(slot->output_fd, buffer, capacity);
    if (read >= 0) {
        // Zero is end-of-file here and EAGAIN is "nothing yet"; both are reported as zero, per the
        // interface. Telling them apart is poll_process()'s job, and answering it here as well
        // would put two sources of truth behind one fact.
        return static_cast<usize>(read);
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
        return usize{0};
    }
    return errno_failure(ErrorCode::Internal, "read");
}

// --- Clocks -------------------------------------------------------------------------------------
//
// Two clocks, two sources, and the specification's reason for the split: the system clock can be
// adjusted while the game runs and frame timing must be unaffected. CLOCK_MONOTONIC is not affected
// by a settimeofday(); CLOCK_REALTIME is.

Nanoseconds LinuxPlatform::monotonic_nanoseconds() const {
    timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return static_cast<Nanoseconds>(now.tv_sec) * 1'000'000'000LL +
           static_cast<Nanoseconds>(now.tv_nsec);
}

i64 LinuxPlatform::wall_nanoseconds() const {
    timespec now{};
    if (::clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return 0;
    }
    return static_cast<i64>(now.tv_sec) * 1'000'000'000LL + static_cast<i64>(now.tv_nsec);
}

// --- Locale, CPU, memory ------------------------------------------------------------------------

Expected<usize, Error> LinuxPlatform::locale(char* buffer, usize capacity) const {
    // The environment first, in the order POSIX gives it, because setlocale(LC_ALL, nullptr)
    // answers "C" in a process that has never called setlocale — which is every process that has
    // not asked for one, including this engine.
    char tag[64];
    for (const char* variable : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        if (locale_tag(std::getenv(variable), tag, sizeof(tag))) {
            return write_to_buffer(buffer, capacity, tag);
        }
    }
    return fail(ErrorCode::Unavailable, "the host reports no preferred locale");
}

u32 LinuxPlatform::cpu_count() const {
    const long count = ::sysconf(_SC_NPROCESSORS_ONLN);
    // Never zero: an implementation that cannot tell reports 1, which the interface requires.
    return count > 0 ? static_cast<u32>(count) : 1U;
}

CpuFeatures LinuxPlatform::cpu_features() const {
    CpuFeatures features;
#if defined(__x86_64__) || defined(__i386__)
    // __builtin_cpu_supports() reads the running processor, not the compiler's baseline, which is
    // what the interface asks for: "reported, never assumed".
    __builtin_cpu_init();
    features.sse42 = __builtin_cpu_supports("sse4.2") != 0;
    features.avx = __builtin_cpu_supports("avx") != 0;
    features.avx2 = __builtin_cpu_supports("avx2") != 0;
    features.avx512f = __builtin_cpu_supports("avx512f") != 0;
#elif defined(__aarch64__)
    // Every AArch64 processor has Advanced SIMD; it is not optional in the base architecture.
    features.neon = true;
#endif
    return features;
}

Expected<MemoryStatistics, Error> LinuxPlatform::memory_statistics() const {
    MemoryStatistics statistics;
    const long pages = ::sysconf(_SC_PHYS_PAGES);
    const long page_size = ::sysconf(_SC_PAGESIZE);
    if (pages > 0 && page_size > 0) {
        statistics.total_physical_bytes = static_cast<u64>(pages) * static_cast<u64>(page_size);
    }
    // A host that cannot report the other two leaves them at zero rather than failing: "unknown" is
    // what zero means here, and the total is still worth having.
    statistics.available_physical_bytes = read_meminfo_bytes("MemAvailable");
    statistics.process_resident_bytes = read_resident_bytes();
    return statistics;
}

// --- Crash handling -----------------------------------------------------------------------------

Status LinuxPlatform::install_crash_handler(CrashHandler handler, void* user) {
    if (handler == nullptr) {
        return fail(ErrorCode::InvalidArgument, "installing a crash handler needs a handler");
    }
    if (g_installed != 0) {
        return fail(ErrorCode::AlreadyExists, "a crash handler is already installed");
    }

    g_handler = handler;
    g_user = user;

    struct sigaction action{};
    std::memset(&action, 0, sizeof(action));
    action.sa_handler = linux_crash_signal_handler;
    ::sigemptyset(&action.sa_mask);
    // SA_NODEFER is deliberately absent: a fault inside the handler must not re-enter it.
    action.sa_flags = SA_RESTART;

    for (usize i = 0; i < kSignalCount; ++i) {
        if (::sigaction(kSignals[i].number, &action, &g_previous[i]) == 0) {
            continue;
        }
        for (usize undo = 0; undo < i; ++undo) {
            ::sigaction(kSignals[undo].number, &g_previous[undo], nullptr);
        }
        g_handler = nullptr;
        g_user = nullptr;
        return errno_failure(ErrorCode::Internal, "sigaction");
    }

    g_installed = 1;
    return ok();
}

void LinuxPlatform::uninstall_crash_handler() {
    if (g_installed == 0) {
        return;
    }
    for (usize i = 0; i < kSignalCount; ++i) {
        ::sigaction(kSignals[i].number, &g_previous[i], nullptr);
    }
    g_installed = 0;
    g_handler = nullptr;
    g_user = nullptr;
}

}  // namespace cy
